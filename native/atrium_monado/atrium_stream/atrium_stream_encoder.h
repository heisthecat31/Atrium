// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming: H.264 encoder on Vulkan Video (VK_KHR_video_encode_h264), on the
 *         compositor's own device and its video-encode queue. Low-latency stream: I then P
 *         frames, one reference, no B frames; IDR when asked.
 * @ingroup drv_atrium_stream
 */
#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

struct atrium_encoder;

struct atrium_encoder_config
{
	uint32_t width, height; //!< picture size (both eyes side by side)
	uint32_t fps;
	uint64_t bitrate_bps;
	//! Queue family that writes the source image (the compositor's), shared with encode.
	uint32_t producer_queue_family;
};

/*!
 * Set up the session. False when the device can't encode H.264 (then streaming can't work).
 */
bool
atrium_encoder_create(struct vk_bundle *vk, const struct atrium_encoder_config *cfg, struct atrium_encoder **out);

//! The NV12 image to fill before each encode (TRANSFER_DST, then handed to the encoder).
VkImage
atrium_encoder_source_image(struct atrium_encoder *e);

/*!
 * Codec config (SPS + PPS, Annex B) for the decoder; valid after create.
 */
const uint8_t *
atrium_encoder_config_nals(struct atrium_encoder *e, size_t *out_size);

/*!
 * Encode the source image once `wait` (binary semaphore, may be VK_NULL_HANDLE) is signalled.
 * Blocks until the frame is encoded; *out_data/*out_size stay valid until the next call.
 * @param force_idr  start a new IDR (first frame always is one).
 */
bool
atrium_encoder_encode(struct atrium_encoder *e,
                      VkSemaphore wait,
                      bool force_idr,
                      const uint8_t **out_data,
                      size_t *out_size,
                      bool *out_is_idr);

void
atrium_encoder_destroy(struct atrium_encoder **e);

#ifdef __cplusplus
}
#endif
