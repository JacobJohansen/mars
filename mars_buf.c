// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

// Buf brick (just for demonstration)

//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>

#include "mars.h"

///////////////////////// own type definitions ////////////////////////

#include "mars_buf.h"

///////////////////////// own helper functions ////////////////////////

static inline int buf_hash(struct buf_brick *brick, loff_t pos)
{
	return (pos >> brick->backing_order) % MARS_BUF_HASH_MAX;
}

static struct buf_head *hash_find(struct buf_brick *brick, loff_t pos)
{
	int hash = buf_hash(brick, pos);
	struct list_head *start = &brick->cache_anchors[hash];
	struct list_head *tmp;
	struct buf_head *res;
	for (tmp = start->next; ; tmp = tmp->next) {
		if (tmp == start)
			return NULL;
		res = container_of(tmp, struct buf_head, bf_hash_head);
		if (res->bf_pos == pos)
			break;
	}
	return res;
}

static inline void hash_insert(struct buf_brick *brick, struct buf_head *elem)
{
	int hash = buf_hash(brick, elem->bf_pos);
	struct list_head *start = &brick->cache_anchors[hash];
	list_add(&elem->bf_hash_head, start);
}

static inline void free_buf(struct buf_brick *brick, struct buf_head *bf)
{
	free_pages((unsigned long)bf->bf_data, brick->backing_order);
	kfree(bf);
}

/* brick->buf_lock must be held
 */
static inline void prune_cache(struct buf_brick *brick)
{
	while (brick->alloc_count > brick->max_count) {
		struct buf_head *bf;
		if (list_empty(&brick->free_anchor))
			break;
		bf = container_of(brick->free_anchor.next, struct buf_head, bf_lru_head);
		list_del(&bf->bf_lru_head);
		brick->current_count--;
		brick->alloc_count--;
		
		spin_unlock_irq(&brick->buf_lock);
		free_buf(brick, bf);
		spin_lock_irq(&brick->buf_lock);
	}
}

static struct mars_io_object_layout *buf_init_object_layout(struct buf_output *output)
{
	const int layout_size = 1024;
	const int max_aspects = 16;
	struct mars_io_object_layout *res;
	int status;
	void *data = kzalloc(layout_size, GFP_KERNEL);
	if (!data) {
		MARS_ERR("emergency, cannot allocate object_layout!\n");
		return NULL;
	}
	res = mars_io_init_object_layout(data, layout_size, max_aspects, &mars_io_type);
	if (unlikely(!res)) {
		MARS_ERR("emergency, cannot init object_layout!\n");
		goto err_free;
	}
	
	status = output->ops->make_object_layout(output, (struct generic_object_layout*)res);
	if (unlikely(status < 0)) {
		MARS_ERR("emergency, cannot add aspects to object_layout!\n");
		goto err_free;
	}
	MARS_INF("OK, object_layout init succeeded.\n");
	return res;

err_free:
	kfree(res);
	return NULL;
}

static inline int get_info(struct buf_brick *brick)
{
	struct buf_input *input = brick->inputs[0];
	int status = GENERIC_INPUT_CALL(input, mars_get_info, &brick->base_info);
	if (status >= 0) {
		brick->got_info = 1;
	}
	return status;
}

/* Convert from arbitrary kernel address/length to struct page,
 * create bio from it, round up/down to full sectors.
 * return the length (may be smaller than requested)
 */
static int make_bio(struct buf_brick *brick, struct bio **_bio, void *data, int len, loff_t pos)
{
	unsigned long sector;
	int sector_offset;
	int page_offset;
	int bvec_count;
	int status;
	int i;
	struct page *page;
	struct bio *bio = NULL;
	struct block_device *bdev;

	if (unlikely(!brick->got_info)) {
		struct request_queue *q;
		status = get_info(brick);
		if (status < 0)
			goto out;
		bdev = brick->base_info.backing_file->f_mapping->host->i_sb->s_bdev;
		q = bdev_get_queue(bdev);
		brick->bvec_max = queue_max_hw_sectors(q) >> (PAGE_SHIFT - 9);
	} else {
		bdev = brick->base_info.backing_file->f_mapping->host->i_sb->s_bdev;
	}

	sector = pos << 9;                     // TODO: make dynamic
	sector_offset = pos & ((1 << 9) - 1);  // TODO: make dynamic
	// round down to start of first sector
	data -= sector_offset;
	len += sector_offset;
	pos -= sector_offset;
	page_offset = pos & (PAGE_SIZE - 1);
	bvec_count = len / PAGE_SIZE + 1;
	if (bvec_count > brick->bvec_max)
		bvec_count = brick->bvec_max;

	bio = bio_alloc(GFP_KERNEL, bvec_count);
	status = -ENOMEM;
	if (!bio)
		goto out;

	status = 0;
	for (i = 0; i < bvec_count && len > 0; i++) {
		int myrest = PAGE_SIZE - page_offset;
		int mylen = len;
		int iolen;
		int iooffset;
		if (mylen > myrest)
			mylen = myrest;
		// round last sector up
		iolen = mylen;
		iooffset = iolen % (1 << 9);          // TODO: make dynamic
		if (iooffset)
			iolen += (1 << 9) - iooffset; // TODO: make dynamic

		page = virt_to_page(data);

		bio->bi_io_vec[i].bv_page = page;
		bio->bi_io_vec[i].bv_len = iolen;
		bio->bi_io_vec[i].bv_offset = page_offset;

		data += mylen;
		len -= mylen;
		status += mylen;
		page_offset = 0;
	}

	bio->bi_vcnt = i;
	bio->bi_idx = 0;
	bio->bi_size = i * PAGE_SIZE;
	bio->bi_sector = sector;
	bio->bi_bdev = bdev;
	bio->bi_private = NULL; // must be filled in later
	bio->bi_end_io = NULL; // must be filled in later
	if (status >= sector_offset)
		status -= sector_offset;

out:
	*_bio = bio;
	return status;
}

////////////////// own brick / input / output operations //////////////////

static int buf_io(struct buf_output *output, struct mars_io_object *mio)
{
	struct buf_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, mars_io, mio);
}

static int buf_get_info(struct buf_output *output, struct mars_info *info)
{
	struct buf_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, mars_get_info, info);
}

static int buf_buf_get(struct buf_output *output, struct mars_buf_object **_mbuf, struct mars_buf_object_layout *buf_layout, loff_t pos, int len)
{
	struct buf_brick *brick = output->brick;
	void *data;
	struct mars_buf_object *mbuf;
	struct buf_mars_buf_aspect *mbuf_a;
	struct buf_head *bf;
	loff_t base_pos;
	int status = -EILSEQ;

	data = kzalloc(buf_layout->object_size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mbuf = mars_buf_construct(data, buf_layout);
	if (!mbuf)
		goto err_free;

	mbuf_a = buf_mars_buf_get_aspect(output, mbuf);
	if (!mbuf_a)
		goto err_free;
	
	spin_lock_init(&mbuf->buf_lock);
	mbuf->buf_pos = pos;
	

	base_pos = pos & ~((loff_t)brick->backing_size - 1);

	spin_lock_irq(&brick->buf_lock);
	bf = hash_find(brick, base_pos);
	if (!bf) {
		MARS_DBG("buf_get() hash nothing found\n");
		if (unlikely(list_empty(&brick->free_anchor))) {
			struct buf_head *test_bf;
			MARS_DBG("buf_get() alloc new buf_head\n");

			spin_unlock_irq(&brick->buf_lock);

			status = -ENOMEM;
			bf = kzalloc(sizeof(struct buf_head), GFP_KERNEL);
			if (!bf)
				goto err_free;

			bf->bf_data = (void*)__get_free_pages(GFP_KERNEL, brick->backing_order);
			if (!bf->bf_data)
				goto err_free2;

			bf->bf_brick = brick;
			atomic_set(&bf->bf_bio_count, 0);
			//INIT_LIST_HEAD(&bf->bf_mbuf_anchor);
			INIT_LIST_HEAD(&bf->bf_lru_head);
			INIT_LIST_HEAD(&bf->bf_hash_head);
			INIT_LIST_HEAD(&bf->bf_io_pending_anchor);
			INIT_LIST_HEAD(&bf->bf_again_write_pending_anchor);

			spin_lock_irq(&brick->buf_lock);
			brick->alloc_count++;
			/* during the open lock, somebody might have raced
			 * against us at the same base_pos...
			 */
			test_bf = hash_find(brick, base_pos);
			if (unlikely(test_bf)) {
				free_buf(brick, bf);
				bf = test_bf;
			}
		} else {
			bf = container_of(brick->free_anchor.next, struct buf_head, bf_lru_head);
		}
		MARS_DBG("buf_get() bf=%p\n", bf);
		bf->bf_pos = base_pos;
		bf->bf_flags = 0;
		atomic_set(&bf->bf_count, 0);

		hash_insert(brick, bf);
		brick->current_count++;
	}

	mbuf_a->bfa_bf = bf;
	atomic_inc(&bf->bf_count);
	MARS_DBG("buf_get() bf=%p initial bf_count=%d\n", bf, atomic_read(&bf->bf_count));

	list_del_init(&bf->bf_lru_head);

	mbuf->buf_flags = bf->bf_flags;
	spin_unlock_irq(&brick->buf_lock);

	mbuf->buf_data = bf->bf_data + (pos - base_pos);
	mbuf->buf_len = brick->backing_size - (pos - base_pos);

	*_mbuf = mbuf;
	if (len > mbuf->buf_len)
		len = mbuf->buf_len;
	return len;

err_free2:
	kfree(bf);
err_free:
	kfree(data);
	return status;
}

static int _buf_buf_put(struct buf_head *bf)
{
	struct buf_brick *brick;

	MARS_DBG("_buf_buf_put() bf=%p bf_count=%d\n", bf, atomic_read(&bf->bf_count));

	if (!atomic_dec_and_test(&bf->bf_count))
		return 0;

	MARS_DBG("_buf_buf_put() ZERO_COUNT\n");

	brick = bf->bf_brick;

	spin_lock_irq(&brick->buf_lock);

	if (atomic_read(&bf->bf_count) <= 0) {
		struct list_head *where = &brick->lru_anchor;
		BUG_ON(bf->bf_flags & (MARS_BUF_READING | MARS_BUF_WRITING));
		if (unlikely(!(bf->bf_flags & MARS_BUF_UPTODATE))) {
			list_del_init(&bf->bf_hash_head);
			brick->current_count--;
			where = &brick->free_anchor;
		}
		list_del(&bf->bf_lru_head);
		list_add(&bf->bf_lru_head, where);
	}

	// lru freeing
	while (brick->current_count > brick->max_count) {
		if (list_empty(&brick->lru_anchor))
			break;
		bf = container_of(brick->lru_anchor.prev, struct buf_head, bf_lru_head);
		list_del(&bf->bf_lru_head);
		list_del_init(&bf->bf_hash_head);
		brick->current_count--;
		list_add(&bf->bf_lru_head, &brick->free_anchor);
	}

	prune_cache(brick);

	spin_unlock_irq(&brick->buf_lock);

	return 0;
}

static int buf_buf_put(struct buf_output *output, struct mars_buf_object *mbuf)
{
	struct buf_mars_buf_aspect *mbuf_a;
	struct buf_head *bf;

	mbuf_a = buf_mars_buf_get_aspect(output, mbuf);
	if (!mbuf_a)
		return -EILSEQ;

	bf = mbuf_a->bfa_bf;
	MARS_DBG("buf_buf_put() mbuf=%p mbuf_a=%p bf=%p\n", mbuf, mbuf_a, bf);
	
	return _buf_buf_put(bf);
}

static int _buf_endio(struct mars_io_object *mio)
{
	struct bio *bio = mio->orig_bio;
	MARS_DBG("_buf_endio() mio=%p bio=%p\n", mio, bio);
	if (bio) {
		mio->orig_bio = NULL;
		if (!bio->bi_size) {
			bio_endio(bio, 0);
		} else {
			MARS_ERR("NYI: RETRY LOGIC %u\n", bio->bi_size);
			bio_endio(bio, -EIO);
		}
	} // else lower layers have already signalled the orig_bio

	kfree(mio);
	return 0;
}

static void _buf_bio_callback(struct bio *bio, int code);

static int _buf_make_bios(struct buf_brick *brick, struct buf_head *bf, void *start_data, loff_t start_pos, int start_len)
{
	while (start_len > 0) {
		struct buf_input *input = brick->inputs[0];
		struct mars_io_object *data;
		struct mars_io_object *mio;
		struct buf_mars_io_aspect *mio_a;
		struct bio *bio = NULL;
		int len;
		int status;

		if (unlikely(!brick->mio_layout)) {
			brick->mio_layout = buf_init_object_layout(brick->outputs[0]);
			if (!brick->mio_layout)
				return -ENOMEM;
		}

		data = kzalloc(brick->mio_layout->object_size, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		mio = mars_io_construct(data, brick->mio_layout);
		if (!mio)
			goto err_free;

		mio_a = buf_mars_io_get_aspect(brick->outputs[0], mio);
		if (!mio_a)
			goto err_free2;

		len = make_bio(brick, &bio, start_data, start_len, start_pos);
		if (len < 0 || !bio)
			goto err_free2;

		mio_a->mia_bf = bf;
		atomic_inc(&bf->bf_bio_count);
		bio->bi_private = mio_a;
		bio->bi_end_io = _buf_bio_callback;
		mio->orig_bio = bio;
		mio->mars_endio = _buf_endio;

		MARS_DBG("starting buf IO mio=%p bio=%p len=%d bf=%p bf_count=%d bf_bio_count=%d\n", mio, bio, len, bf, atomic_read(&bf->bf_count), atomic_read(&bf->bf_bio_count));
#if 1
		status = GENERIC_INPUT_CALL(input, mars_io, mio);
		if (status < 0)
			goto err_free3;
#else
		// fake IO for testing
		bio->bi_size = 0;
		mio->mars_endio(mio);
#endif

		start_data -= len;
		start_pos -= len;
		start_len -= len;
		continue;

	err_free3:
		atomic_dec(&bf->bf_bio_count);
		bio_put(bio);
	err_free2:
		kfree(mio);
	err_free:
		kfree(data);
		return -EIO;
	}
	return 0;
}

static void _buf_bio_callback(struct bio *bio, int code)
{
	struct buf_mars_io_aspect *mio_a;
	struct buf_head *bf;
	struct buf_brick *brick;
	void  *start_data = NULL;
	loff_t start_pos = 0;
	int    start_len = 0;
	int old_flags;

	mio_a = bio->bi_private;
	bf = mio_a->mia_bf;

	MARS_DBG("_buf_bio_callback() mio=%p bio=%p bf=%p bf_count=%d bf_bio_count=%d code=%d\n", mio_a->object, bio, bf, atomic_read(&bf->bf_count), atomic_read(&bf->bf_bio_count), code);

	if (unlikely(mio_a->mia_end_io_called)) {
		MARS_ERR("Oops, somebody called us twice on the same bio. I'm not amused.\n");
		msleep(5000);
		return;
	} else {
		mio_a->mia_end_io_called = true;
		bio_put(bio);
	}

	if (code < 0) {
		// this can race, but we don't worry about the exact error code
		bf->bf_bio_status = code;
	}

	if (!atomic_dec_and_test(&bf->bf_bio_count))
		return;

	MARS_DBG("_buf_bio_callback() ZERO_COUNT mio=%p bio=%p bf=%p code=%d\n", mio_a->object, bio, bf, code);

	brick = bf->bf_brick;

	spin_lock_irq(&brick->buf_lock);

	// signal success by calling all callbacks.
	while (!list_empty(&bf->bf_io_pending_anchor)) {
		struct buf_mars_buf_callback_aspect *mbuf_cb_a = container_of(bf->bf_io_pending_anchor.next, struct buf_mars_buf_callback_aspect, bfc_pending_head);
		struct mars_buf_callback_object *mbuf_cb = mbuf_cb_a->object;

		BUG_ON(mbuf_cb_a->bfc_bfa->bfa_bf != bf);
		mbuf_cb->cb_error = bf->bf_bio_status;
		list_del(&mbuf_cb_a->bfc_pending_head);
		/* drop normal refcount.
		 * full _buf_buf_put() not needed, see below. */
		atomic_dec(&bf->bf_count);
		MARS_DBG("_buf_bio_callback() bf=%p now bf_count=%d\n", bf, atomic_read(&bf->bf_count));

		spin_unlock_irq(&brick->buf_lock);

		mbuf_cb->cb_buf_endio(mbuf_cb);

		spin_lock_irq(&brick->buf_lock);
	}

	old_flags = bf->bf_flags;
	if (!bf->bf_bio_status && (old_flags & MARS_BUF_READING)) {
		bf->bf_flags |= MARS_BUF_UPTODATE;
	}

	// clear the flags. may be re-enabled later.
	bf->bf_flags &= ~(MARS_BUF_READING | MARS_BUF_WRITING);

	/* move pending jobs to work.
	 * this is in essence an automatic restart mechanism.
	 */
	while (!list_empty(&bf->bf_again_write_pending_anchor)) {
		struct buf_mars_buf_callback_aspect *mbuf_cb_a = container_of(bf->bf_again_write_pending_anchor.next, struct buf_mars_buf_callback_aspect, bfc_pending_head);
		struct mars_buf_object *mbuf = mbuf_cb_a->bfc_bfa->object;
		BUG_ON(mbuf_cb_a->bfc_bfa->bfa_bf != bf);
		list_del(&mbuf_cb_a->bfc_pending_head);
		list_add_tail(&mbuf_cb_a->bfc_pending_head, &bf->bf_io_pending_anchor);
		// re-enable flags
		bf->bf_flags |= MARS_BUF_WRITING;
		bf->bf_bio_status = 0;

		if (!start_len) {
			start_data = mbuf->buf_data;
			start_pos = mbuf->buf_pos;
			start_len = mbuf->buf_len;
		} else if (start_data != mbuf->buf_data ||
			  start_pos != mbuf->buf_pos ||
			  start_len != mbuf->buf_len) {
			start_data = bf->bf_data;
			start_pos = bf->bf_pos;
			start_len = brick->backing_size;
		}
	}

	spin_unlock_irq(&brick->buf_lock);

	if (start_len) {
		// in this case, the extra refcount is kept => nothing to do
		_buf_make_bios(brick, bf, start_data, start_pos, start_len);
	} else if (old_flags & (MARS_BUF_READING | MARS_BUF_WRITING)) {
		// drop extra refcount for pending IO
		_buf_buf_put(bf);
	}
}

static int buf_buf_io(struct buf_output *output, struct mars_buf_callback_object *mbuf_cb)
{
	struct buf_brick *brick = output->brick;
	struct mars_buf_object *mbuf = mbuf_cb->cb_mbuf;
	struct buf_mars_buf_aspect *mbuf_a;
	struct buf_mars_buf_callback_aspect *mbuf_cb_a;
	struct buf_head *bf;
	void  *start_data = NULL;
	loff_t start_pos = 0;
	int    start_len = 0;

	if (!mbuf) {
		MARS_ERR("internal problem: forgotten to supply mbuf\n");
		return -EILSEQ;
	}
	mbuf_cb_a = buf_mars_buf_callback_get_aspect(output, mbuf_cb);
	if (!mbuf_cb_a) {
		MARS_ERR("internal problem: mbuf_cb aspect does not work\n");
		return -EILSEQ;
	}

	mbuf_a = buf_mars_buf_get_aspect(output, mbuf);
	if (!mbuf_a) {
		MARS_ERR("internal problem: mbuf aspect does not work\n");
		return -EILSEQ;
	}

	mbuf_cb_a->bfc_bfa = mbuf_a;
	bf = mbuf_a->bfa_bf;

	spin_lock_irq(&brick->buf_lock);

	if (mbuf_cb->cb_rw) { // WRITE
		BUG_ON(bf->bf_flags & MARS_BUF_READING);
		if (!(bf->bf_flags & MARS_BUF_WRITING)) {
			bf->bf_flags |= MARS_BUF_WRITING;
			bf->bf_bio_status = 0;
			// grab an extra refcount for pending IO
			atomic_inc(&bf->bf_count);
			MARS_DBG("buf_buf_io() bf=%p extra bf_count=%d\n", bf, atomic_read(&bf->bf_count));
			start_data = mbuf->buf_data;
			start_pos = mbuf->buf_pos;
			start_len = mbuf->buf_len;
			list_add(&mbuf_cb_a->bfc_pending_head, &bf->bf_io_pending_anchor);
		} else {
			list_add(&mbuf_cb_a->bfc_pending_head, &bf->bf_again_write_pending_anchor);
			MARS_INF("postponing %lld %d\n", mbuf->buf_pos, mbuf->buf_len);
		}
	} else { // READ
		if (bf->bf_flags & (MARS_BUF_UPTODATE | MARS_BUF_WRITING)) {
			spin_unlock_irq(&brick->buf_lock);
			return mbuf_cb->cb_buf_endio(mbuf_cb);
		}
		if (!(bf->bf_flags & MARS_BUF_READING)) {
			bf->bf_flags |= MARS_BUF_READING;
			bf->bf_bio_status = 0;
			// grab an extra refcount for pending IO
			atomic_inc(&bf->bf_count);
			MARS_DBG("buf_buf_io() bf=%p extra bf_count=%d\n", bf, atomic_read(&bf->bf_count));
			start_data = (void*)((unsigned long)mbuf->buf_data & (brick->backing_size - 1));
			start_pos = mbuf->buf_pos & (brick->backing_size - 1);
			start_len = brick->backing_size;
		}
		list_add(&mbuf_cb_a->bfc_pending_head, &bf->bf_io_pending_anchor);
	}

	// grab normal refcount for each mbuf_cb
	atomic_inc(&bf->bf_count);
	MARS_DBG("buf_buf_io() bf=%p normal bf_count=%d\n", bf, atomic_read(&bf->bf_count));

	spin_unlock_irq(&brick->buf_lock);

	return _buf_make_bios(brick, bf, start_data, start_pos, start_len);
}

//////////////// object / aspect constructors / destructors ///////////////

static int buf_mars_io_aspect_init_fn(struct generic_aspect *_ini, void *_init_data)
{
	struct buf_mars_io_aspect *ini = (void*)_ini;
	ini->mia_bf = NULL;
	ini->mia_end_io_called = false;
	return 0;
}

static int buf_mars_buf_aspect_init_fn(struct generic_aspect *_ini, void *_init_data)
{
	struct buf_mars_buf_aspect *ini = (void*)_ini;
	ini->bfa_bf = NULL;
	return 0;
}

static int buf_mars_buf_callback_aspect_init_fn(struct generic_aspect *_ini, void *_init_data)
{
	struct buf_mars_buf_callback_aspect *ini = (void*)_ini;
	INIT_LIST_HEAD(&ini->bfc_pending_head);
	ini->bfc_bfa = NULL;
	return 0;
}

MARS_MAKE_STATICS(buf);

static int buf_make_object_layout(struct buf_output *output, struct generic_object_layout *object_layout)
{
	const struct generic_object_type *object_type = object_layout->object_type;
	int status;
	int aspect_size = 0;
	struct buf_brick *brick = output->brick;
	int i;

	if (object_type == &mars_io_type) {
		aspect_size = sizeof(struct buf_mars_io_aspect);
		status = buf_mars_io_add_aspect(output, object_layout, &buf_mars_io_aspect_type);
	} else if (object_type == &mars_buf_type) {
		aspect_size = sizeof(struct buf_mars_buf_aspect);
		status = buf_mars_buf_add_aspect(output, object_layout, &buf_mars_buf_aspect_type);
		if (status < 0)
			return status;
		return aspect_size;
	} else if (object_type == &mars_buf_callback_type) {
		aspect_size = sizeof(struct buf_mars_buf_callback_aspect);
		status = buf_mars_buf_callback_add_aspect(output, object_layout, &buf_mars_buf_callback_aspect_type);
		if (status < 0)
			return status;
		return aspect_size;
	} else {
		return 0;
	}

	if (status < 0)
		return status;

	for (i = 0; i < brick->type->max_inputs; i++) {
		struct buf_input *input = brick->inputs[i];
		if (input && input->connect) {
			int substatus = input->connect->ops->make_object_layout(input->connect, object_layout);
			if (substatus < 0)
				return substatus;
			aspect_size += substatus;
		}
	}

	return aspect_size;
}

////////////////////// brick constructors / destructors ////////////////////

static int buf_brick_construct(struct buf_brick *brick)
{
	int i;
	brick->backing_order = 5; // TODO: make this configurable
	brick->backing_size = PAGE_SIZE << brick->backing_order;
	brick->max_count = 32; // TODO: make this configurable
	brick->current_count = 0;
	brick->alloc_count = 0;
	spin_lock_init(&brick->buf_lock);
	INIT_LIST_HEAD(&brick->free_anchor);
	INIT_LIST_HEAD(&brick->lru_anchor);
	for (i = 0; i < MARS_BUF_HASH_MAX; i++) {
		INIT_LIST_HEAD(&brick->cache_anchors[i]);
	}
	return 0;
}

static int buf_output_construct(struct buf_output *output)
{
	return 0;
}

///////////////////////// static structs ////////////////////////

static struct buf_brick_ops buf_brick_ops = {
};

static struct buf_output_ops buf_output_ops = {
	.make_object_layout = buf_make_object_layout,
	.mars_io = buf_io,
	.mars_get_info = buf_get_info,
	.mars_buf_get = buf_buf_get,
	.mars_buf_put = buf_buf_put,
	.mars_buf_io = buf_buf_io,
};

static const struct buf_input_type buf_input_type = {
	.type_name = "buf_input",
	.input_size = sizeof(struct buf_input),
};

static const struct buf_input_type *buf_input_types[] = {
	&buf_input_type,
};

static const struct buf_output_type buf_output_type = {
	.type_name = "buf_output",
	.output_size = sizeof(struct buf_output),
	.master_ops = &buf_output_ops,
	.output_construct = &buf_output_construct,
};

static const struct buf_output_type *buf_output_types[] = {
	&buf_output_type,
};

const struct buf_brick_type buf_brick_type = {
	.type_name = "buf_brick",
	.brick_size = sizeof(struct buf_brick),
	.max_inputs = 1,
	.max_outputs = 1,
	.master_ops = &buf_brick_ops,
	.default_input_types = buf_input_types,
	.default_output_types = buf_output_types,
	.brick_construct = &buf_brick_construct,
};
EXPORT_SYMBOL_GPL(buf_brick_type);

////////////////// module init stuff /////////////////////////

static int __init init_buf(void)
{
	printk(MARS_INFO "init_buf()\n");
	return buf_register_brick_type();
}

static void __exit exit_buf(void)
{
	printk(MARS_INFO "exit_buf()\n");
	buf_unregister_brick_type();
}

MODULE_DESCRIPTION("MARS buf brick");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_buf);
module_exit(exit_buf);
