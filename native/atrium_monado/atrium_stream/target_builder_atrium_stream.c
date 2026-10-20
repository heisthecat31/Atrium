// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming builder: a streamed headset plus two Touch-style controllers.
 *         Chosen when ATRIUM_STREAM=1, which the Atrium app sets when it starts the service for
 *         a standalone headset (Quest, Pico).
 * @ingroup drv_atrium_stream
 */
#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_tracking.h"

#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_logging.h"

#include "target_builder_helpers.h"

#include "atrium_stream/atrium_stream_interface.h"

DEBUG_GET_ONCE_BOOL_OPTION(atrium_stream, "ATRIUM_STREAM", false)

static const char *driver_list[] = {
    "atrium_stream",
};

static xrt_result_t
atrium_stream_estimate_system(struct xrt_builder *xb,
                              cJSON *config,
                              struct xrt_prober *xp,
                              struct xrt_builder_estimate *estimate)
{
	estimate->certain.head = true;
	estimate->certain.left = true;
	estimate->certain.right = true;
	estimate->priority = 0;
	return XRT_SUCCESS;
}

static xrt_result_t
atrium_stream_open_system_impl(struct xrt_builder *xb,
                               cJSON *config,
                               struct xrt_prober *xp,
                               struct xrt_tracking_origin *origin,
                               struct xrt_system_devices *xsysd,
                               struct xrt_frame_context *xfctx,
                               struct t_builder_options *tbo)
{
	if (!atrium_stream_start()) {
		U_LOG_E("Atrium stream: the streaming core didn't start");
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct xrt_device *head = atrium_stream_hmd_create();
	if (head == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	struct xrt_device *left = atrium_stream_controller_create(0);
	struct xrt_device *right = atrium_stream_controller_create(1);

	// Everything is tracked by the headset itself, in one space.
	head->tracking_origin->type = XRT_TRACKING_TYPE_OTHER;
	left->tracking_origin = head->tracking_origin;
	right->tracking_origin = head->tracking_origin;

	xsysd->static_xdevs[xsysd->static_xdev_count++] = head;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = left;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = right;

	tbo->head = head;
	tbo->left = left;
	tbo->right = right;
	return XRT_SUCCESS;
}

static void
atrium_stream_destroy(struct xrt_builder *xb)
{
	free(xb);
}

struct xrt_builder *
atrium_stream_builder_create(void)
{
	struct t_builder *ub = U_TYPED_CALLOC(struct t_builder);

	ub->base.estimate_system = atrium_stream_estimate_system;
	ub->base.open_system = t_builder_open_system_static_roles;
	ub->base.destroy = atrium_stream_destroy;
	ub->base.identifier = "atrium_stream";
	ub->base.name = "Atrium streamed headset (Quest, Pico)";
	ub->base.driver_identifiers = driver_list;
	ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);
	ub->base.exclude_from_automatic_discovery = !debug_get_bool_option_atrium_stream();

	ub->open_system_static_roles = atrium_stream_open_system_impl;
	return &ub->base;
}
