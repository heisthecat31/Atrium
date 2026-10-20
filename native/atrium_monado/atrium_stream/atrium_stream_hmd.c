// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming: the headset device. Poses from the headset's tracking, views from
 *         what it reports (FOV and eye offsets); no lens distortion on the PC side.
 * @ingroup drv_atrium_stream
 */
#include "atrium_stream_internal.h"

#include "xrt/xrt_device.h"
#include "math/m_api.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_visibility_mask.h"

#include <stdio.h>

struct atrium_stream_hmd
{
	struct xrt_device base;
};

static xrt_result_t
hmd_get_tracked_pose(struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp_ns,
                     struct xrt_space_relation *out_relation)
{
	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (!atrium_stream_get_relation(g_atrium_stream.head_id, at_timestamp_ns, out_relation)) {
		// Not connected yet: a standing head at 1.6 m, so apps have something to render.
		U_ZERO(out_relation);
		out_relation->pose = (struct xrt_pose){{0, 0, 0, 1}, {0, 1.6f, 0}};
		out_relation->relation_flags =
		    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
hmd_get_view_poses(struct xrt_device *xdev,
                   const struct xrt_vec3 *default_eye_relation,
                   int64_t at_timestamp_ns,
                   enum xrt_view_type view_type,
                   uint32_t view_count,
                   struct xrt_space_relation *out_head_relation,
                   struct xrt_fov *out_fovs,
                   struct xrt_pose *out_poses)
{
	(void)default_eye_relation;
	(void)view_type;
	xrt_result_t xret = hmd_get_tracked_pose(xdev, XRT_INPUT_GENERIC_HEAD_POSE, at_timestamp_ns, out_head_relation);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	os_mutex_lock(&g_atrium_stream.lock);
	for (uint32_t i = 0; i < view_count && i < 2; i++) {
		out_fovs[i] = g_atrium_stream.fov[i];
		out_poses[i] = g_atrium_stream.eye[i];
		xdev->hmd->distortion.fov[i] = g_atrium_stream.fov[i];
	}
	os_mutex_unlock(&g_atrium_stream.lock);
	return XRT_SUCCESS;
}

static xrt_result_t
hmd_get_visibility_mask(struct xrt_device *xdev,
                        enum xrt_visibility_mask_type type,
                        uint32_t view_index,
                        struct xrt_visibility_mask **out_mask)
{
	os_mutex_lock(&g_atrium_stream.lock);
	struct xrt_fov fov = g_atrium_stream.fov[view_index < 2 ? view_index : 0];
	os_mutex_unlock(&g_atrium_stream.lock);
	u_visibility_mask_get_default(type, &fov, out_mask);
	return XRT_SUCCESS;
}

static void
hmd_destroy(struct xrt_device *xdev)
{
	u_device_free(xdev);
}

struct xrt_device *
atrium_stream_hmd_create(void)
{
	enum u_device_alloc_flags flags = U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE;
	struct atrium_stream_hmd *hmd = U_DEVICE_ALLOCATE(struct atrium_stream_hmd, flags, 1, 0);

	// Safe "not implemented" defaults for everything optional; then ours.
	u_device_populate_function_pointers(&hmd->base, hmd_get_tracked_pose, hmd_destroy);
	hmd->base.get_view_poses = hmd_get_view_poses;
	hmd->base.get_visibility_mask = hmd_get_visibility_mask;
	hmd->base.name = XRT_DEVICE_GENERIC_HMD;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.supported.orientation_tracking = true;
	hmd->base.supported.position_tracking = true;
	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "Atrium streamed headset");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "atrium-stream");

	// Per-eye panels side by side at the stream resolution (what each eye's image is encoded at).
	uint32_t w = g_atrium_stream.stream_width ? g_atrium_stream.stream_width : 1832;
	uint32_t h = g_atrium_stream.stream_height ? g_atrium_stream.stream_height : 1920;
	struct u_device_simple_info info = {0};
	info.display.w_pixels = w * 2;
	info.display.h_pixels = h;
	info.display.w_meters = 0.1f;
	info.display.h_meters = 0.1f * h / w;
	info.lens_horizontal_separation_meters = 0.063f;
	info.lens_vertical_position_meters = info.display.h_meters / 2.0f;
	info.fov[0] = 1.75f; // radians; replaced per view below
	info.fov[1] = 1.75f;
	hmd->base.hmd->view_count = 2;
	if (!u_device_setup_split_side_by_side(&hmd->base, &info)) {
		U_LOG_E("Atrium stream: failed to set up the headset's views");
		hmd_destroy(&hmd->base);
		return NULL;
	}
	for (int i = 0; i < 2; i++) {
		hmd->base.hmd->distortion.fov[i] = g_atrium_stream.fov[i];
	}
	hmd->base.hmd->screens[0].nominal_frame_interval_ns = U_TIME_1S_IN_NS / 72;

	// The headset app undistorts; the PC sends the plain eye images.
	u_distortion_mesh_set_none(&hmd->base);

	return &hmd->base;
}
