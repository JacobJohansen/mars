// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_COPY_H
#define MARS_COPY_H

#include <linux/wait.h>
#include <linux/semaphore.h>

#define INPUT_A_IO   0
#define INPUT_A_COPY 1
#define INPUT_B_IO   2
#define INPUT_B_COPY 3

//#define COPY_CHUNK      (64 * 1024)
#define COPY_CHUNK      (PAGE_SIZE)
#define MAX_COPY_PARA   (32 * 1024 * 1024 / COPY_CHUNK)

enum {
	COPY_STATE_START    = 0,
	COPY_STATE_READ1,
	COPY_STATE_READ2,
	COPY_STATE_READ3,
	COPY_STATE_WRITE,
	COPY_STATE_WRITTEN,
	COPY_STATE_CLEANUP,
	COPY_STATE_FINISHED,
};

struct copy_state {
	struct mref_object *table[2];
	bool active[2];
	char state;
	short prev;
	short len;
	short error;
};

struct copy_mref_aspect {
	GENERIC_ASPECT(mref);
	struct copy_brick *brick;
	int queue;
};

struct copy_brick {
	MARS_BRICK(copy);
	// parameters
	loff_t copy_start;
	loff_t copy_end; // stop working if == 0
	int io_prio;
	int append_mode; // 1 = passively, 2 = actively
	bool verify_mode;
	bool repair_mode; // whether to repair in case of verify errors
	bool utilize_mode; // utilize already copied data
	bool abort_mode; // abort on IO error (default is retry forever)
	// readonly from outside
	loff_t copy_last; // current working position
	int copy_error;
	int verify_ok_count;
	int verify_error_count;
	bool low_dirty;
	bool is_aborting;
	// internal
	bool trigger;
	unsigned long clash;
	atomic_t io_flight;
	atomic_t copy_flight;
	long long last_jiffies;
	wait_queue_head_t event;
	struct semaphore mutex;
	struct task_struct *thread;
	struct copy_state st[MAX_COPY_PARA];
};

struct copy_input {
	MARS_INPUT(copy);
};

struct copy_output {
	MARS_OUTPUT(copy);
};

MARS_TYPES(copy);

#endif
