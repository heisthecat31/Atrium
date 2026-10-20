// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming: Touch-style controllers. Poses from the headset's tracking; button
 *         values from the core's button queue, matched to Monado's Touch inputs by OpenXR path.
 * @ingroup drv_atrium_stream
 */
#include "atrium_stream_internal.h"

#include "xrt/xrt_device.h"
#include "math/m_api.h"
#include "os/os_time.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include <stdio.h>
#include <string.h>

//! One Touch input and the OpenXR path(s) ALVR sends for it ("x" is the left-hand name of "a").
struct input_def
{
	enum xrt_input_name name;
	const char *left;  //!< path under /user/hand/left/input/
	const char *right; //!< path under /user/hand/right/input/
	int axis;          //!< for thumbsticks: 0 = x, 1 = y; -1 otherwise
};

static const struct input_def g_inputs[] = {
    {XRT_INPUT_TOUCH_X_CLICK, "x/click", "a/click", -1},
    {XRT_INPUT_TOUCH_X_TOUCH, "x/touch", "a/touch", -1},
    {XRT_INPUT_TOUCH_Y_CLICK, "y/click", "b/click", -1},
    {XRT_INPUT_TOUCH_Y_TOUCH, "y/touch", "b/touch", -1},
    {XRT_INPUT_TOUCH_MENU_CLICK, "menu/click", "system/click", -1},
    {XRT_INPUT_TOUCH_SQUEEZE_VALUE, "squeeze/value", "squeeze/value", -1},
    {XRT_INPUT_TOUCH_TRIGGER_TOUCH, "trigger/touch", "trigger/touch", -1},
    {XRT_INPUT_TOUCH_TRIGGER_VALUE, "trigger/value", "trigger/value", -1},
    {XRT_INPUT_TOUCH_THUMBSTICK_CLICK, "thumbstick/click", "thumbstick/click", -1},
    {XRT_INPUT_TOUCH_THUMBSTICK_TOUCH, "thumbstick/touch", "thumbstick/touch", -1},
    {XRT_INPUT_TOUCH_THUMBSTICK, "thumbstick/x", "thumbstick/x", 0},
    {XRT_INPUT_TOUCH_THUMBSTICK, "thumbstick/y", "thumbstick/y", 1},
    {XRT_INPUT_TOUCH_THUMBREST_TOUCH, "thumbrest/touch", "thumbrest/touch", -1},
};
#define INPUT_DEF_COUNT (sizeof(g_inputs) / sizeof(g_inputs[0]))

// The right hand's A/B and system buttons map onto the same Monado inputs as the left's X/Y and
// menu (Monado's Touch profile names them per hand when binding).

struct atrium_stream_controller
{
	struct xrt_device base;
	int hand;
};

//! Per hand: the ALVR path id of each input_def, and the latest values.
static struct
{
	uint64_t id[INPUT_DEF_COUNT];
	float value[INPUT_DEF_COUNT];
	bool ready;
} g_hand[2];

static struct os_mutex g_buttons_lock;
static bool g_buttons_lock_ready;

static void
init_ids(void)
{
	if (g_hand[0].ready) {
		return;
	}
	char path[128];
	for (size_t i = 0; i < INPUT_DEF_COUNT; i++) {
		snprintf(path, sizeof(path), "/user/hand/left/input/%s", g_inputs[i].left);
		g_hand[0].id[i] = alvr_path_to_id(path);
		snprintf(path, sizeof(path), "/user/hand/right/input/%s", g_inputs[i].right);
		g_hand[1].id[i] = alvr_path_to_id(path);
	}
	g_hand[0].ready = g_hand[1].ready = true;
}

void
atrium_stream_buttons_updated(void)
{
	if (!g_buttons_lock_ready) {
		return; // no controllers yet: drop the batch
	}
	uint64_t count = alvr_get_buttons(NULL);
	if (count == 0 || count > 256) {
		alvr_get_buttons(NULL);
		return;
	}
	struct AlvrButtonEntry entries[256];
	count = alvr_get_buttons(entries);

	os_mutex_lock(&g_buttons_lock);
	for (uint64_t e = 0; e < count; e++) {
		for (int h = 0; h < 2; h++) {
			for (size_t i = 0; i < INPUT_DEF_COUNT; i++) {
				if (g_hand[h].id[i] != entries[e].id) {
					continue;
				}
				bool scalar = XRT_GET_INPUT_TYPE(g_inputs[i].name) != XRT_INPUT_TYPE_BOOLEAN;
				g_hand[h].value[i] = scalar ? entries[e].value.float_ : (entries[e].value.scalar ? 1.0f : 0.0f);
			}
		}
	}
	os_mutex_unlock(&g_buttons_lock);
}

static xrt_result_t
controller_update_inputs(struct xrt_device *xdev)
{
	struct atrium_stream_controller *c = (struct atrium_stream_controller *)xdev;
	int64_t now = os_monotonic_get_ns();

	os_mutex_lock(&g_buttons_lock);
	// inputs[0..1] are the poses; buttons follow in g_inputs order (thumbstick x/y share one).
	uint32_t slot = 2;
	for (size_t i = 0; i < INPUT_DEF_COUNT; i++) {
		struct xrt_input *in = &xdev->inputs[slot];
		in->timestamp = now;
		in->active = true;
		switch (XRT_GET_INPUT_TYPE(g_inputs[i].name)) {
		case XRT_INPUT_TYPE_BOOLEAN: in->value.boolean = g_hand[c->hand].value[i] > 0.5f; break;
		case XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE:
			if (g_inputs[i].axis == 0) {
				in->value.vec2.x = g_hand[c->hand].value[i];
			} else {
				in->value.vec2.y = g_hand[c->hand].value[i];
			}
			break;
		default: in->value.vec1.x = g_hand[c->hand].value[i]; break;
		}
		// Both thumbstick axes live in one input: only move on after the y axis.
		if (!(g_inputs[i].axis == 0)) {
			slot++;
		}
	}
	os_mutex_unlock(&g_buttons_lock);
	return XRT_SUCCESS;
}

static xrt_result_t
controller_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            int64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	struct atrium_stream_controller *c = (struct atrium_stream_controller *)xdev;
	if (name != XRT_INPUT_TOUCH_GRIP_POSE && name != XRT_INPUT_TOUCH_AIM_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	// ALVR reports one pose per hand (grip); aim uses the same pose for now.
	if (!atrium_stream_get_relation(g_atrium_stream.hand_id[c->hand], at_timestamp_ns, out_relation)) {
		U_ZERO(out_relation);
		out_relation->pose.orientation.w = 1.0f;
	}
	return XRT_SUCCESS;
}

// Vibration goes to the headset's controllers through the core.
static xrt_result_t
controller_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct atrium_stream_controller *c = (struct atrium_stream_controller *)xdev;
	if (name != XRT_OUTPUT_NAME_TOUCH_HAPTIC || value->type != XRT_OUTPUT_VALUE_TYPE_VIBRATION) {
		return XRT_SUCCESS;
	}
	const struct xrt_output_value_vibration *v = &value->vibration;
	// OpenXR's "minimum duration" is -1 and "unspecified frequency" is 0: give both a sane value.
	float seconds = v->duration_ns > 0 ? (float)v->duration_ns / 1e9f : 0.05f;
	float frequency = v->frequency > 0 ? v->frequency : 160.0f;
	alvr_send_haptics(g_atrium_stream.hand_id[c->hand], seconds, frequency, v->amplitude);
	return XRT_SUCCESS;
}

static void
controller_destroy(struct xrt_device *xdev)
{
	u_device_free(xdev);
}

struct xrt_device *
atrium_stream_controller_create(int hand)
{
	if (!g_buttons_lock_ready) {
		os_mutex_init(&g_buttons_lock);
		g_buttons_lock_ready = true;
	}
	init_ids();

	// 2 poses + one input per g_inputs entry, minus one because thumbstick x/y share an input.
	uint32_t input_count = 2 + INPUT_DEF_COUNT - 1;
	struct atrium_stream_controller *c =
	    U_DEVICE_ALLOCATE(struct atrium_stream_controller, U_DEVICE_ALLOC_TRACKING_NONE, input_count, 1);
	c->hand = hand;

	// Safe "not implemented" defaults for everything optional; then ours.
	u_device_populate_function_pointers(&c->base, controller_get_tracked_pose, controller_destroy);
	c->base.name = XRT_DEVICE_TOUCH_CONTROLLER;
	c->base.device_type = hand == 0 ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
	c->base.update_inputs = controller_update_inputs;
	c->base.set_output = controller_set_output;
	c->base.supported.orientation_tracking = true;
	c->base.supported.position_tracking = true;
	snprintf(c->base.str, XRT_DEVICE_NAME_LEN, "Atrium streamed %s controller", hand == 0 ? "left" : "right");
	snprintf(c->base.serial, XRT_DEVICE_NAME_LEN, "atrium-stream-%s", hand == 0 ? "left" : "right");

	c->base.inputs[0].name = XRT_INPUT_TOUCH_GRIP_POSE;
	c->base.inputs[1].name = XRT_INPUT_TOUCH_AIM_POSE;
	uint32_t slot = 2;
	for (size_t i = 0; i < INPUT_DEF_COUNT; i++) {
		if (g_inputs[i].axis == 1) {
			continue; // y shares the thumbstick input created for x
		}
		c->base.inputs[slot++].name = g_inputs[i].name;
	}
	c->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;
	return &c->base;
}
