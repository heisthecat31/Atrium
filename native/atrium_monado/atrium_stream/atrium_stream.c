// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming core: runs ALVR's server_core inside the Atrium service, keeps the
 *         latest state the headset reports (connection, views, tracking timestamp) and turns
 *         its tracking into Monado space relations.
 * @ingroup drv_atrium_stream
 */
#include "atrium_stream_internal.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "math/m_api.h"
#include "math/m_predict.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct atrium_stream_state g_atrium_stream;

static struct os_thread_helper g_events_thread;
static bool g_started;

uint64_t
atrium_stream_alvr_time(int64_t monotonic_ns)
{
	// Both clocks are monotonic; sample their offset now (cheap, no drift to track).
	int64_t offset = (int64_t)alvr_get_time_ns() - os_monotonic_get_ns();
	return (uint64_t)(monotonic_ns + offset);
}

static int64_t
monotonic_from_alvr(uint64_t alvr_ns)
{
	int64_t offset = (int64_t)alvr_get_time_ns() - os_monotonic_get_ns();
	return (int64_t)alvr_ns - offset;
}

static struct xrt_pose
pose_from_alvr(const struct AlvrPose *p)
{
	struct xrt_pose out = {
	    .orientation = {p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w},
	    .position = {p->position[0], p->position[1], p->position[2]},
	};
	return out;
}

bool
atrium_stream_get_relation(uint64_t device_id, int64_t at_timestamp_ns, struct xrt_space_relation *out)
{
	U_ZERO(out);
	out->pose.orientation.w = 1.0f;

	os_mutex_lock(&g_atrium_stream.lock);
	uint64_t sample_ns = g_atrium_stream.last_sample_ns;
	os_mutex_unlock(&g_atrium_stream.lock);
	if (sample_ns == 0) {
		return false;
	}

	struct AlvrDeviceMotion motion;
	if (!alvr_get_device_motion(device_id, sample_ns, &motion)) {
		return false;
	}

	struct xrt_space_relation rel = {
	    .relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                      XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	                      XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT,
	    .pose = pose_from_alvr(&motion.pose),
	    .linear_velocity = {motion.linear_velocity[0], motion.linear_velocity[1], motion.linear_velocity[2]},
	    .angular_velocity = {motion.angular_velocity[0], motion.angular_velocity[1], motion.angular_velocity[2]},
	};

	// The sample was taken at sample_ns (ALVR time); predict to the time Monado asks for.
	double delta_s = time_ns_to_s(at_timestamp_ns - monotonic_from_alvr(sample_ns));
	if (delta_s < -0.1 || delta_s > 0.1) {
		delta_s = delta_s < 0 ? -0.1 : 0.1; // don't fly off on stale samples
	}
	m_predict_relation(&rel, delta_s, out);
	return true;
}

static void
handle_event(const union AlvrEvent *e)
{
	switch (e->tag) {
	case ALVR_EVENT_CLIENT_CONNECTED:
		U_LOG_I("Atrium stream: headset connected");
		os_mutex_lock(&g_atrium_stream.lock);
		g_atrium_stream.connected = true;
		os_mutex_unlock(&g_atrium_stream.lock);
		break;
	case ALVR_EVENT_CLIENT_DISCONNECTED:
		U_LOG_I("Atrium stream: headset disconnected");
		os_mutex_lock(&g_atrium_stream.lock);
		g_atrium_stream.connected = false;
		g_atrium_stream.last_sample_ns = 0;
		os_mutex_unlock(&g_atrium_stream.lock);
		break;
	case ALVR_EVENT_VIEWS_CONFIG: {
		os_mutex_lock(&g_atrium_stream.lock);
		for (int i = 0; i < 2; i++) {
			const struct AlvrFov *f = &e->VIEWS_CONFIG.fov[i];
			g_atrium_stream.fov[i] = (struct xrt_fov){f->left, f->right, f->up, f->down};
			g_atrium_stream.eye[i] = pose_from_alvr(&e->VIEWS_CONFIG.local_view_transform[i]);
		}
		g_atrium_stream.have_views = true;
		os_mutex_unlock(&g_atrium_stream.lock);
		U_LOG_I("Atrium stream: views fov L %.3f %.3f %.3f %.3f, eyes %.4f / %.4f m", e->VIEWS_CONFIG.fov[0].left,
		        e->VIEWS_CONFIG.fov[0].right, e->VIEWS_CONFIG.fov[0].up, e->VIEWS_CONFIG.fov[0].down,
		        e->VIEWS_CONFIG.local_view_transform[0].position[0],
		        e->VIEWS_CONFIG.local_view_transform[1].position[0]);
		break;
	}
	case ALVR_EVENT_TRACKING_UPDATED:
		os_mutex_lock(&g_atrium_stream.lock);
		g_atrium_stream.last_sample_ns = e->TRACKING_UPDATED.sample_timestamp_ns;
		os_mutex_unlock(&g_atrium_stream.lock);
		break;
	case ALVR_EVENT_BUTTONS_UPDATED: atrium_stream_buttons_updated(); break;
	case ALVR_EVENT_REQUEST_IDR:
		os_mutex_lock(&g_atrium_stream.lock);
		g_atrium_stream.idr_requested = true;
		os_mutex_unlock(&g_atrium_stream.lock);
		break;
	case ALVR_EVENT_SHUTDOWN_PENDING:
	case ALVR_EVENT_RESTART_PENDING: U_LOG_I("Atrium stream: server core asks for restart/shutdown"); break;
	default: break;
	}
}

static void *
events_thread(void *ptr)
{
	(void)ptr;
	os_thread_helper_lock(&g_events_thread);
	while (os_thread_helper_is_running_locked(&g_events_thread)) {
		os_thread_helper_unlock(&g_events_thread);
		union AlvrEvent e;
		if (alvr_poll_event(&e, 50 * U_TIME_1MS_IN_NS)) {
			handle_event(&e);
		}
		os_thread_helper_lock(&g_events_thread);
	}
	os_thread_helper_unlock(&g_events_thread);
	return NULL;
}

// Atrium's folder for the streaming core's settings and logs.
static void
stream_dir(char *out, size_t size)
{
#ifdef XRT_OS_WINDOWS
	const char *appdata = getenv("APPDATA");
	snprintf(out, size, "%s\\Atrium\\stream", appdata ? appdata : ".");
#else
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0]) {
		snprintf(out, size, "%s/atrium/stream", xdg);
	} else {
		snprintf(out, size, "%s/.config/atrium/stream", home ? home : ".");
	}
#endif
}

bool
atrium_stream_start(void)
{
	if (g_started) {
		return true;
	}
	os_mutex_init(&g_atrium_stream.lock);

	char dir[512];
	stream_dir(dir, sizeof(dir));
	char log[600];
#ifdef XRT_OS_WINDOWS
	snprintf(log, sizeof(log), "%s\\stream.log", dir);
#else
	snprintf(log, sizeof(log), "%s/stream.log", dir);
#endif
	alvr_initialize_environment(dir, dir);
	alvr_initialize_logging(log, NULL);

	struct AlvrTargetConfig target = alvr_initialize();
	g_atrium_stream.render_width = target.game_render_width;
	g_atrium_stream.render_height = target.game_render_height;
	g_atrium_stream.stream_width = target.stream_width;
	g_atrium_stream.stream_height = target.stream_height;
	U_LOG_I("Atrium stream: core up, eye render %ux%u, stream %ux%u (settings in %s)", target.game_render_width,
	        target.game_render_height, target.stream_width, target.stream_height, dir);

	g_atrium_stream.head_id = alvr_path_to_id("/user/head");
	g_atrium_stream.hand_id[0] = alvr_path_to_id("/user/hand/left");
	g_atrium_stream.hand_id[1] = alvr_path_to_id("/user/hand/right");

	// Defaults until the headset reports its views: ~Quest 2 per-eye FOV and 63 mm IPD.
	for (int i = 0; i < 2; i++) {
		float s = i == 0 ? -1.0f : 1.0f;
		g_atrium_stream.fov[i] = (struct xrt_fov){-0.87f, 0.87f, 0.87f, -0.87f};
		g_atrium_stream.eye[i] = (struct xrt_pose){{0, 0, 0, 1}, {s * 0.0315f, 0, 0}};
	}

	alvr_start_connection();

	os_thread_helper_init(&g_events_thread);
	os_thread_helper_start(&g_events_thread, events_thread, NULL);
	g_started = true;
	return true;
}
