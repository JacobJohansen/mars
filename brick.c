// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#define _STRATEGY
#define BRICK_OBJ_NR /*empty => leads to an open array */
#include "brick.h"

//////////////////////////////////////////////////////////////

// object stuff

//////////////////////////////////////////////////////////////

// brick stuff

static int nr_brick_types = 0;
static const struct generic_brick_type *brick_types[MAX_BRICK_TYPES] = {};

int generic_register_brick_type(const struct generic_brick_type *new_type)
{
	int i;
	int found = -1;
	BRICK_DBG("generic_register_brick_type()\n");
	for (i = 0; i < nr_brick_types; i++) {
		if (!brick_types[i]) {
			found = i;
			continue;
		}
		if (!strcmp(brick_types[i]->type_name, new_type->type_name)) {
			printk("sorry, bricktype %s is already registered.\n", new_type->type_name);
			return -EEXIST;
		}
	}
	if (found < 0) {
		if (nr_brick_types >= MAX_BRICK_TYPES) {
			printk("sorry, cannot register bricktype %s.\n", new_type->type_name);
			return -EEXIST;
		}
		found = nr_brick_types++;
	}
	brick_types[found] = new_type;
	BRICK_DBG("generic_register_brick_type() done.\n");
	return 0;
}
EXPORT_SYMBOL_GPL(generic_register_brick_type);

int generic_unregister_brick_type(const struct generic_brick_type *old_type)
{
	BRICK_DBG("generic_unregister_brick_type()\n");
	return -1; // NYI
}
EXPORT_SYMBOL_GPL(generic_unregister_brick_type);

int generic_brick_init_full(
	void *data, 
	int size, 
	const struct generic_brick_type *brick_type,
	const struct generic_input_type **input_types,
	const struct generic_output_type **output_types,
	char **names)
{
	struct generic_brick *brick = data;
	int status;
	int i;

	BRICK_DBG("generic_brick_init_full()\n");
	// first, call the generic constructors

	status = generic_brick_init(brick_type, brick, *names++);
	if (status)
		return status;
	data += brick_type->brick_size;
	size -= brick_type->brick_size;
	if (size < 0)
		return -ENOMEM;
	if (!input_types) {
		BRICK_DBG("generic_brick_init_full: switch to default input_types\n");
		input_types = brick_type->default_input_types;
		names = brick_type->default_input_names;
	}
	if (input_types) {
		BRICK_DBG("generic_brick_init_full: input_types\n");
		brick->inputs = data;
		data += sizeof(void*) * brick_type->max_inputs;
		size -= sizeof(void*) * brick_type->max_inputs;
		if (size < 0)
			return -1;
		for (i = 0; i < brick_type->max_inputs; i++) {
			struct generic_input *input = data;
			const struct generic_input_type *type = *input_types++;
			BRICK_DBG("generic_brick_init_full: calling generic_input_init()\n");
			status = generic_input_init(brick, i, type, input, names ? *names++ : type->type_name);
			if (status)
				return status;
			data += type->input_size;
			size -= type->input_size;
			if (size < 0)
				return -ENOMEM;
		}
	}
	if (!output_types) {
		BRICK_DBG("generic_brick_init_full: switch to default output_types\n");
		output_types = brick_type->default_output_types;
		names = brick_type->default_output_names;
	}
	if (output_types) {
		BRICK_DBG("generic_brick_init_full: output_types\n");
		brick->outputs = data;
		data += sizeof(void*) * brick_type->max_outputs;
		size -= sizeof(void*) * brick_type->max_outputs;
		if (size < 0)
			return -1;
		for (i = 0; i < brick_type->max_outputs; i++) {
			struct generic_output *output = data;
			const struct generic_output_type *type = *output_types++;
			BRICK_DBG("generic_brick_init_full: calling generic_output_init()\n");
			generic_output_init(brick, i, type, output, names ? *names++ : type->type_name);
			if (status)
				return status;
			data += type->output_size;
			size -= type->output_size;
			if (size < 0)
				return -ENOMEM;
		}
	}

	// call the specific constructors
	BRICK_DBG("generic_brick_init_full: call specific contructors.\n");
	if (brick_type->brick_construct) {
		BRICK_DBG("generic_brick_init_full: calling brick_construct()\n");
		status = brick_type->brick_construct(brick);
		if (status)
			return status;
	}
	for (i = 0; i < brick_type->max_inputs; i++) {
		struct generic_input *input = brick->inputs[i];
		if (!input)
			continue;
		if (!input->type) {
			BRICK_ERR("input has no associated type!\n");
			continue;
		}
		if (input->type->input_construct) {
			BRICK_DBG("generic_brick_init_full: calling input_construct()\n");
			status = input->type->input_construct(input);
			if (status)
				return status;
		}
	}
	for (i = 0; i < brick_type->max_outputs; i++) {
		struct generic_output *output = brick->outputs[i];
		if (!output)
			continue;
		if (!output->type) {
			BRICK_ERR("output has no associated type!\n");
			continue;
		}
		if (output->type->output_construct) {
			BRICK_DBG("generic_brick_init_full: calling output_construct()\n");
			status = output->type->output_construct(output);
			if (status)
				return status;
		}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(generic_brick_init_full);

int generic_brick_exit_full(struct generic_brick *brick)
{
	int i;
	int status;
	// first, check all outputs
	for (i = 0; i < brick->nr_outputs; i++) {
		struct generic_output *output = brick->outputs[i];
		if (!output)
			continue;
		if (!output->type) {
			BRICK_ERR("output has no associated type!\n");
			continue;
		}
		if (output->nr_connected) {
			BRICK_DBG("output is connected!\n");
			return -EPERM;
		}
	}
        // ok, test succeeded. start destruction...
	for (i = 0; i < brick->type->max_outputs; i++) {
		struct generic_output *output = brick->outputs[i];
		if (!output)
			continue;
		if (!output->type) {
			BRICK_ERR("output has no associated type!\n");
			continue;
		}
		if (output->type->output_destruct) {
			BRICK_DBG("generic_brick_exit_full: calling output_destruct()\n");
			status = output->type->output_destruct(output);
			if (status)
				return status;
			brick->outputs[i] = NULL; // others may remain leftover
		}
	}
	for (i = 0; i < brick->type->max_inputs; i++) {
		struct generic_input *input = brick->inputs[i];
		if (!input)
			continue;
		if (!input->type) {
			BRICK_ERR("input has no associated type!\n");
			continue;
		}
		if (input->type->input_destruct) {
			BRICK_DBG("generic_brick_exit_full: calling input_destruct()\n");
			status = input->type->input_destruct(input);
			if (status)
				return status;
			brick->inputs[i] = NULL; // others may remain leftover
			status = generic_disconnect(input);
			if (status)
				return status;
		}
	}
	if (brick->type->brick_destruct) {
		BRICK_DBG("generic_brick_exit_full: calling brick_destruct()\n");
		status = brick->type->brick_destruct(brick);
		if (status)
			return status;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(generic_brick_exit_full);

int generic_brick_exit_recursively(struct generic_brick *brick)
{
	int final_status = 0;
	LIST_HEAD(head);
	list_add(&brick->tmp_head, &head);
	while (!list_empty(&head)) {
		int i;
		int status;
		brick = container_of(head.next, struct generic_brick, tmp_head);
		for (i = 0; i < brick->nr_outputs; i++) {
			struct generic_output *output = brick->outputs[i];
			if (output->nr_connected) {
				list_del(&brick->tmp_head);
				continue;
			}
		}
		list_del(&brick->tmp_head);
		for (i = 0; i < brick->nr_inputs; i++) {
			struct generic_input *input = brick->inputs[i];
			if (input->connect) {
				struct generic_brick *other = input->connect->brick;
				list_add(&other->tmp_head, &head);
			}
		}
		status = generic_brick_exit_full(brick);
		if (status)
			final_status = status;
	}
	return final_status;
}
EXPORT_SYMBOL_GPL(generic_brick_exit_recursively);

////////////////////////////////////////////////////////////////////////

// default implementations

#if 0
int default_make_object_layout(struct generic_output *output, struct generic_object_layout *object_layout)
{
	const struct generic_object_type *object_type = object_layout->object_type;
	int status;
	int aspect_size = 0;
	struct default_brick *brick = output->brick;
	int i;

	for (i = 0; i < brick->type->max_inputs; i++) {
		struct dummy_input *input = brick->inputs[i];
		if (input && input->connect) {
			int substatus = input->connect->ops->make_object_layout(input->connect, object_layout);
			if (substatus < 0)
				return substatus;
			aspect_size += substatus;
		}
	}

	if (object_type == &mars_io_type) {
		aspect_size = sizeof(struct dummy_mars_io_aspect);
		status = generic_io_add_aspect(output, object_layout, &dummy_mars_io_aspect_type);
	} else if (object_type == &mars_buf_type) {
		aspect_size = sizeof(struct dummy_mars_buf_aspect);
		status = generic_buf_add_aspect(output, object_layout, &dummy_mars_buf_aspect_type);
	} else if (object_type == &mars_buf_callback_type) {
		aspect_size = sizeof(struct dummy_mars_buf_callback_aspect);
		status = generic_buf_callback_add_aspect(output, object_layout, &dummy_mars_buf_callback_aspect_type);
	} else {
		return 0;
	}

	if (status < 0)
		return status;

	return aspect_size;
}
#endif
