// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming: state shared by the core, the devices and the compositor target.
 * @ingroup drv_atrium_stream
 */
#pragma once

#include "xrt/xrt_defines.h"
#include "os/os_threading.h"

#include "atrium_stream_interface.h"
#include "alvr_server_core.h"

#ifdef __cplusplus
extern "C" {
#endif

struct atrium_stream_state
{
	struct os_mutex lock;

	//! A headset is connected and streaming.
	bool connected;
	//! Latest tracking sample (ALVR time), 0 until the first one.
	uint64_t last_sample_ns;
	//! The headset has asked for a full (IDR) frame.
	bool idr_requested;

	//! What the headset reported for its views (defaults until it does).
	bool have_views;
	struct xrt_fov fov[2];
	struct xrt_pose eye[2];

	//! From the core's settings: per-eye render and stream resolution.
	uint32_t render_width, render_height;
	uint32_t stream_width, stream_height;

	//! ALVR device ids.
	uint64_t head_id;
	uint64_t hand_id[2];
};

extern struct atrium_stream_state g_atrium_stream;

/*!
 * The device's relation at a Monado monotonic time, predicted from the latest sample.
 * False until the headset has sent tracking.
 */
bool
atrium_stream_get_relation(uint64_t device_id, int64_t at_timestamp_ns, struct xrt_space_relation *out);

//! New button values are queued in the core; the controllers pick them up.
void
atrium_stream_buttons_updated(void);

#ifdef __cplusplus
}
#endif
