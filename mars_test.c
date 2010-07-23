// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include <linux/major.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>

#define _STRATEGY
#include "mars.h"

#include "mars_if_device.h"
#include "mars_device_sio.h"
#include "mars_buf.h"

GENERIC_MAKE_CONNECT(if_device, device_sio);
GENERIC_MAKE_CONNECT(if_device, buf);
GENERIC_MAKE_CONNECT(buf, device_sio);

static struct if_device_brick *if_brick = NULL;
static struct buf_brick *buf_brick = NULL;
static struct device_sio_brick *device_brick = NULL;

static struct mars_buf_object_layout *_init_buf_object_layout(struct buf_output *output)
{
	const int layout_size = 1024;
	const int max_aspects = 16;
	struct mars_buf_object_layout *res;
	int status;
	void *data = kzalloc(layout_size, GFP_KERNEL);
	if (!data) {
		MARS_ERR("emergency, cannot allocate object_layout!\n");
		return NULL;
	}
	res = mars_buf_init_object_layout(data, layout_size, max_aspects, &mars_buf_type);
	if (unlikely(!res)) {
		MARS_ERR("emergency, cannot init object_layout!\n");
		goto err_free;
	}
	
	status = output->ops->make_object_layout(output, (struct generic_object_layout*)res);
	if (unlikely(status < 0)) {
		MARS_ERR("emergency, cannot add aspects to object_layout!\n");
		goto err_free;
	}
	MARS_INF("OK, buf_object_layout init succeeded.\n");
	return res;

err_free:
	kfree(res);
	return NULL;
}

static struct mars_buf_callback_object_layout *_init_buf_callback_object_layout(struct buf_output *output)
{
	const int layout_size = 1024;
	const int max_aspects = 16;
	struct mars_buf_callback_object_layout *res;
	int status;
	void *data = kzalloc(layout_size, GFP_KERNEL);
	if (!data) {
		MARS_ERR("emergency, cannot allocate object_layout!\n");
		return NULL;
	}
	res = mars_buf_callback_init_object_layout(data, layout_size, max_aspects, &mars_buf_callback_type);
	if (unlikely(!res)) {
		MARS_ERR("emergency, cannot init object_layout!\n");
		goto err_free;
	}
	
	status = output->ops->make_object_layout(output, (struct generic_object_layout*)res);
	if (unlikely(status < 0)) {
		MARS_ERR("emergency, cannot add aspects to object_layout!\n");
		goto err_free;
	}
	MARS_INF("OK, buf_callback_object_layout init succeeded.\n");
	return res;

err_free:
	kfree(res);
	return NULL;
}

static int test_endio(struct mars_buf_callback_object *mbuf_cb)
{
	MARS_DBG("test_endio() called! error=%d\n", mbuf_cb->cb_error);
	return 0;
}

void make_test_instance(void)
{
	static char *names[] = { "brick" };
	int size = 4096;
	int buf_size = 4096 * 8;
	int status;
	void *mem;

	mem = kzalloc(size, GFP_KERNEL);
	if (!mem) {
		MARS_ERR("cannot grab test memory\n");
		return;
	}

	MARS_DBG("starting....\n");

	status = device_sio_brick_init_full(mem, size, &device_sio_brick_type, NULL, NULL, names);
	MARS_DBG("done (status=%d)\n", status);
	if (status) {
		MARS_ERR("cannot init brick device_sio\n");
		return;
	}
	device_brick = mem;

	mem = kzalloc(size, GFP_KERNEL);
	if (!mem) {
		MARS_ERR("cannot grab test memory\n");
		return;
	}

	status = if_device_brick_init_full(mem, size, &if_device_brick_type, NULL, NULL, names);
	MARS_DBG("done (status=%d)\n", status);
	if (status) {
		MARS_ERR("cannot init brick if_device\n");
		return;
	}
	if_brick = mem;

#if 1
	mem = kzalloc(buf_size, GFP_KERNEL);
	if (!mem) {
		MARS_ERR("cannot grab test memory\n");
		return;
	}

	status = buf_brick_init_full(mem, buf_size, &buf_brick_type, NULL, NULL, names);
	MARS_DBG("done (status=%d)\n", status);
	if (status) {
		MARS_ERR("cannot init brick buf\n");
		return;
	}
	buf_brick = mem;
	buf_brick->backing_order = 0;
	buf_brick->backing_size = PAGE_SIZE << buf_brick->backing_order;
	buf_brick->max_count = 512;

	status = buf_device_sio_connect(buf_brick->inputs[0], device_brick->outputs[0]);
	MARS_DBG("connect (status=%d)\n", status);

#if 1
	status = if_device_buf_connect(if_brick->inputs[0], buf_brick->outputs[0]);
	MARS_DBG("connect (status=%d)\n", status);
#endif

	if (true) {
		struct buf_output *output = buf_brick->outputs[0];
		struct mars_buf_object_layout *buf_layout = _init_buf_object_layout(output);
		struct mars_buf_callback_object_layout *buf_callback_layout = _init_buf_callback_object_layout(output);
		struct mars_buf_object *mbuf = NULL;

		if (!buf_layout) {
			MARS_ERR("cannot init buf_layout\n");
			return;
		}
		if (!buf_callback_layout) {
			MARS_ERR("cannot init buf_callback_layout\n");
			return;
		}

		status = GENERIC_OUTPUT_CALL(output, mars_buf_get, &mbuf, buf_layout, 0, PAGE_SIZE);
		MARS_DBG("buf_get (status=%d)\n", status);

		if (mbuf) {
			if (true) {
				void *data = kzalloc(buf_callback_layout->object_size, GFP_KERNEL);
				struct mars_buf_callback_object *mbuf_cb = NULL;
				if (!data) {
					MARS_ERR("cannot alloc buf_callback\n");
					return;
				}
				mbuf_cb = mars_buf_callback_construct(data, buf_callback_layout);
				if (!mbuf_cb) {
					MARS_ERR("cannot init buf_callback\n");
					return;
				}
				mbuf_cb->cb_mbuf = mbuf;
				mbuf_cb->cb_rw = READ;
				mbuf_cb->cb_buf_endio = test_endio;


				status = GENERIC_OUTPUT_CALL(output, mars_buf_io, mbuf_cb);
				MARS_DBG("buf_io (status=%d)\n", status);
			}
			status = GENERIC_OUTPUT_CALL(output, mars_buf_put, mbuf);
			MARS_DBG("buf_put (status=%d)\n", status);
		}
	}
#else

	status = if_device_device_sio_connect(if_brick->inputs[0], device_brick->outputs[0]);
	MARS_DBG("connect (status=%d)\n", status);
#endif

}

void destroy_test_instance(void)
{
	if (if_brick) {
		if_device_device_sio_disconnect(if_brick->inputs[0]);
		if_device_brick_exit_full(if_brick);
		kfree(if_brick);
		if_brick = NULL;
	}
	if (buf_brick) {
		buf_device_sio_disconnect(buf_brick->inputs[0]);
		buf_brick_exit_full(buf_brick);
		kfree(buf_brick);
		buf_brick = NULL;
	}
	if (device_brick) {
		device_sio_brick_exit_full(device_brick);
		kfree(device_brick);
		device_brick = NULL;
	}
}

static void __exit exit_test(void)
{
	MARS_DBG("destroy_test_instance()\n");
	destroy_test_instance();
}

static int __init init_test(void)
{
	MARS_DBG("make_test_instance()\n");
	make_test_instance();
	return 0;
}

MODULE_DESCRIPTION("MARS TEST");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_test);
module_exit(exit_test);
