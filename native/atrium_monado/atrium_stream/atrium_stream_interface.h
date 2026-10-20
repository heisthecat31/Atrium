// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming: standalone headsets (Quest, Pico) running the Atrium headset app
 *         (Atrium's build of ALVR's client) become a Monado system. Poses, buttons and the
 *         headset's views come in through ALVR's server_core (MIT, C API); frames go out as
 *         encoded video through the same core.
 * @ingroup drv_atrium_stream
 */
#pragma once

#include "xrt/xrt_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_builder;
struct xrt_device;

/*!
 * The target builder: claims the system when ATRIUM_STREAM=1 (set by the Atrium app when it
 * starts the service for a standalone headset).
 */
struct xrt_builder *
atrium_stream_builder_create(void);

/*!
 * Starts ALVR's server_core (once per process) and its event thread. Safe to call again.
 * Returns false when the core couldn't start.
 */
bool
atrium_stream_start(void);

/*!
 * The headset device. Its views follow what the headset reports; it has no lens distortion on
 * the PC side (the headset app does that).
 */
struct xrt_device *
atrium_stream_hmd_create(void);

/*!
 * A Touch-style controller for one hand (0 = left, 1 = right).
 */
struct xrt_device *
atrium_stream_controller_create(int hand);

/*!
 * ALVR server time for a Monado monotonic timestamp.
 */
uint64_t
atrium_stream_alvr_time(int64_t monotonic_ns);

#ifdef __cplusplus
}
#endif
