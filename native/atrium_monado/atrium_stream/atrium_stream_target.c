// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming: the compositor's output for a streamed headset. The compositor renders
 *         each finished frame (both eyes side by side) into our image; we convert it to NV12 with
 *         a compute shader, encode it on the GPU's video-encode queue and hand it to ALVR's core,
 *         which sends it to the headset. Chosen when ATRIUM_STREAM=1.
 * @ingroup drv_atrium_stream
 */
#include "atrium_stream_internal.h"
#include "atrium_stream_encoder.h"
#include "rgb_to_nv12_spv.h"

#include "main/comp_compositor.h"
#include "main/comp_target.h"
#include "util/comp_scratch.h"

#include "math/m_api.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "util/u_time.h"

#include <stdlib.h>

DEBUG_GET_ONCE_BOOL_OPTION(atrium_stream_target, "ATRIUM_STREAM", false)
DEBUG_GET_ONCE_NUM_OPTION(atrium_stream_mbps, "ATRIUM_STREAM_MBPS", 40)

struct atrium_stream_target
{
	struct comp_target base;

	//! Render images: the compositor writes through the sRGB view, we read the raw (UNORM) one.
	struct comp_scratch_single_images target;
	struct comp_target_image images[COMP_SCRATCH_NUM_IMAGES];
	int64_t index;
	struct u_pacing_compositor *upc;
	bool has_init_vulkan;

	//! The compositor signals this (timeline, value = frame id) when a frame is rendered.
	VkSemaphore render_complete;

	// RGB -> NV12.
	VkImage plane_y, plane_uv;
	VkDeviceMemory plane_y_mem, plane_uv_mem;
	VkImageView plane_y_view, plane_uv_view;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkDescriptorPool desc_pool;
	VkDescriptorSet sets[COMP_SCRATCH_NUM_IMAGES];
	VkCommandPool cmd_pool;
	VkCommandBuffer cmd;
	VkSemaphore converted;
	bool planes_ready;

	struct atrium_encoder *encoder;
	bool config_sent;
	uint64_t frames;
};

static inline struct atrium_stream_target *
ast(struct comp_target *ct)
{
	return (struct atrium_stream_target *)ct;
}

/*
 * Conversion resources.
 */

static bool
create_plane(struct vk_bundle *vk, VkExtent2D extent, VkFormat format, VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
	VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (vk_create_image_simple(vk, extent, format, usage, mem, img) != VK_SUCCESS) {
		return false;
	}
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return vk_create_view(vk, *img, VK_IMAGE_VIEW_TYPE_2D, format, range, view) == VK_SUCCESS;
}

static bool
create_conversion(struct atrium_stream_target *t, VkExtent2D coded)
{
	struct vk_bundle *vk = &t->base.c->base.vk;

	if (!create_plane(vk, coded, VK_FORMAT_R8_UNORM, &t->plane_y, &t->plane_y_mem, &t->plane_y_view) ||
	    !create_plane(vk, (VkExtent2D){coded.width / 2, coded.height / 2}, VK_FORMAT_R8G8_UNORM, &t->plane_uv,
	                  &t->plane_uv_mem, &t->plane_uv_view)) {
		return false;
	}

	VkDescriptorSetLayoutBinding bindings[3];
	for (uint32_t i = 0; i < 3; i++) {
		bindings[i] = (VkDescriptorSetLayoutBinding){
		    .binding = i,
		    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		    .descriptorCount = 1,
		    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		};
	}
	VkDescriptorSetLayoutCreateInfo dslci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 3,
	    .pBindings = bindings,
	};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &dslci, NULL, &t->set_layout) != VK_SUCCESS ||
	    vk_create_pipeline_layout(vk, t->set_layout, &t->pipeline_layout) != VK_SUCCESS) {
		return false;
	}

	VkShaderModuleCreateInfo smci = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(atrium_rgb_to_nv12_spv),
	    .pCode = atrium_rgb_to_nv12_spv,
	};
	VkShaderModule shader;
	if (vk->vkCreateShaderModule(vk->device, &smci, NULL, &shader) != VK_SUCCESS) {
		return false;
	}
	VkResult ret = vk_create_compute_pipeline(vk, VK_NULL_HANDLE, shader, t->pipeline_layout, NULL, &t->pipeline);
	vk->vkDestroyShaderModule(vk->device, shader, NULL);
	if (ret != VK_SUCCESS) {
		return false;
	}

	VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * COMP_SCRATCH_NUM_IMAGES};
	VkDescriptorPoolCreateInfo dpci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = COMP_SCRATCH_NUM_IMAGES,
	    .poolSizeCount = 1,
	    .pPoolSizes = &size,
	};
	if (vk->vkCreateDescriptorPool(vk->device, &dpci, NULL, &t->desc_pool) != VK_SUCCESS) {
		return false;
	}
	for (uint32_t i = 0; i < COMP_SCRATCH_NUM_IMAGES; i++) {
		VkDescriptorSetAllocateInfo dsai = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = t->desc_pool,
		    .descriptorSetCount = 1,
		    .pSetLayouts = &t->set_layout,
		};
		if (vk->vkAllocateDescriptorSets(vk->device, &dsai, &t->sets[i]) != VK_SUCCESS) {
			return false;
		}
		VkDescriptorImageInfo infos[3] = {
		    {VK_NULL_HANDLE, t->target.images[i].unorm_view, VK_IMAGE_LAYOUT_GENERAL},
		    {VK_NULL_HANDLE, t->plane_y_view, VK_IMAGE_LAYOUT_GENERAL},
		    {VK_NULL_HANDLE, t->plane_uv_view, VK_IMAGE_LAYOUT_GENERAL},
		};
		VkWriteDescriptorSet writes[3];
		for (uint32_t b = 0; b < 3; b++) {
			writes[b] = (VkWriteDescriptorSet){
			    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			    .dstSet = t->sets[i],
			    .dstBinding = b,
			    .descriptorCount = 1,
			    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			    .pImageInfo = &infos[b],
			};
		}
		vk->vkUpdateDescriptorSets(vk->device, 3, writes, 0, NULL);
	}

	VkCommandPoolCreateInfo cpci = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	    .queueFamilyIndex = vk->main_queue->family_index,
	};
	VkCommandBufferAllocateInfo cbai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	if (vk->vkCreateCommandPool(vk->device, &cpci, NULL, &t->cmd_pool) != VK_SUCCESS) {
		return false;
	}
	cbai.commandPool = t->cmd_pool;
	return vk->vkAllocateCommandBuffers(vk->device, &cbai, &t->cmd) == VK_SUCCESS &&
	       vk->vkCreateSemaphore(vk->device, &sci, NULL, &t->converted) == VK_SUCCESS;
}

static void
image_barrier(struct vk_bundle *vk,
              VkCommandBuffer cmd,
              VkImage image,
              VkImageAspectFlags aspect,
              VkImageLayout from,
              VkImageLayout to,
              VkAccessFlags src_access,
              VkAccessFlags dst_access,
              VkPipelineStageFlags src_stage,
              VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = src_access,
	    .dstAccessMask = dst_access,
	    .oldLayout = from,
	    .newLayout = to,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = image,
	    .subresourceRange = {aspect, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/*!
 * Record: render image -> Y / CbCr planes (compute) -> the encoder's NV12 source (copies).
 */
static void
record_conversion(struct atrium_stream_target *t, uint32_t index)
{
	struct vk_bundle *vk = &t->base.c->base.vk;
	VkCommandBuffer cmd = t->cmd;
	VkImage nv12 = atrium_encoder_source_image(t->encoder);
	uint32_t w = t->base.width, h = t->base.height;
	uint32_t cw = (w + 15) / 16 * 16, ch = (h + 15) / 16 * 16;

	VkCommandBufferBeginInfo begin = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(cmd, &begin);

	VkImageLayout planes_from = t->planes_ready ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	image_barrier(vk, cmd, t->plane_y, VK_IMAGE_ASPECT_COLOR_BIT, planes_from, VK_IMAGE_LAYOUT_GENERAL,
	              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	image_barrier(vk, cmd, t->plane_uv, VK_IMAGE_ASPECT_COLOR_BIT, planes_from, VK_IMAGE_LAYOUT_GENERAL,
	              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	// The render image: written by the compositor's render pass, left in GENERAL (final_layout).
	image_barrier(vk, cmd, t->target.images[index].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
	              VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	              VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t->pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, t->pipeline_layout, 0, 1, &t->sets[index], 0,
	                            NULL);
	vk->vkCmdDispatch(cmd, (cw / 2 + 7) / 8, (ch / 2 + 7) / 8, 1);

	image_barrier(vk, cmd, t->plane_y, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
	              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	image_barrier(vk, cmd, t->plane_uv, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
	              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	// The encoder leaves its source in TRANSFER_DST after each frame; the first time it's new.
	image_barrier(vk, cmd, nv12, VK_IMAGE_ASPECT_COLOR_BIT,
	              t->planes_ready ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
	              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageCopy copies[2] = {
	    {
	        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	        .dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1},
	        .extent = {cw, ch, 1},
	    },
	    {
	        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	        .dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1},
	        .extent = {cw / 2, ch / 2, 1},
	    },
	};
	vk->vkCmdCopyImage(cmd, t->plane_y, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, nv12, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   1, &copies[0]);
	vk->vkCmdCopyImage(cmd, t->plane_uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, nv12,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[1]);
	vk->vkEndCommandBuffer(cmd);
	t->planes_ready = true;
}

/*
 * comp_target functions.
 */

static bool
target_init_pre_vulkan(struct comp_target *ct)
{
	return true;
}

static bool
target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	struct atrium_stream_target *t = ast(ct);
	struct vk_bundle *vk = &ct->c->base.vk;
	t->has_init_vulkan = true;

	// Timeline semaphore the compositor signals with the frame id when a frame is rendered.
	VkSemaphoreTypeCreateInfo type = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
	if (vk->vkCreateSemaphore(vk->device, &sci, NULL, &t->render_complete) != VK_SUCCESS) {
		return false;
	}
	ct->semaphores.render_complete = t->render_complete;
	ct->semaphores.render_complete_is_timeline = true;
	return true;
}

static bool
target_check_ready(struct comp_target *ct)
{
	return true;
}

static void
target_create_images(struct comp_target *ct,
                     const struct comp_target_create_images_info *create_info,
                     struct vk_bundle_queue *present_queue)
{
	struct atrium_stream_target *t = ast(ct);
	struct vk_bundle *vk = &ct->c->base.vk;
	(void)present_queue;

	bool srgb = false;
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		srgb = srgb || create_info->formats[i] == VK_FORMAT_R8G8B8A8_SRGB ||
		       create_info->formats[i] == VK_FORMAT_B8G8R8A8_SRGB;
	}

	VkExtent2D extent = create_info->extent;
	comp_scratch_single_images_ensure_mutable(&t->target, vk, extent);
	for (uint32_t i = 0; i < COMP_SCRATCH_NUM_IMAGES; i++) {
		t->images[i].handle = t->target.images[i].image;
		t->images[i].view = srgb ? t->target.images[i].srgb_view : t->target.images[i].unorm_view;
	}
	ct->image_count = COMP_SCRATCH_NUM_IMAGES;
	ct->images = &t->images[0];
	ct->width = extent.width;
	ct->height = extent.height;
	ct->format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	// We read the result with a compute shader right after the render pass.
	ct->final_layout = VK_IMAGE_LAYOUT_GENERAL;

	if (t->encoder != NULL) {
		return; // Same size every time for a streamed headset.
	}
	int64_t interval = ct->c->settings.nominal_frame_interval_ns;
	struct atrium_encoder_config cfg = {
	    .width = extent.width,
	    .height = extent.height,
	    .fps = interval > 0 ? (uint32_t)((U_TIME_1S_IN_NS + interval / 2) / interval) : 72,
	    .bitrate_bps = (uint64_t)debug_get_num_option_atrium_stream_mbps() * 1000000u,
	    .producer_queue_family = vk->main_queue->family_index,
	};
	if (!atrium_encoder_create(vk, &cfg, &t->encoder)) {
		U_LOG_E("Atrium stream: no encoder, the headset will stay black");
		return;
	}
	VkExtent2D coded = {(extent.width + 15) / 16 * 16, (extent.height + 15) / 16 * 16};
	if (!create_conversion(t, coded)) {
		U_LOG_E("Atrium stream: colour conversion setup failed");
		atrium_encoder_destroy(&t->encoder);
	}
}

static bool
target_has_images(struct comp_target *ct)
{
	return ast(ct)->base.images != NULL;
}

static VkResult
target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct atrium_stream_target *t = ast(ct);
	uint32_t index = 0;
	comp_scratch_single_images_get(&t->target, &index);
	t->index = index;
	*out_index = index;
	return VK_SUCCESS;
}

static VkResult
target_present(struct comp_target *ct,
               struct vk_bundle_queue *present_queue,
               uint32_t index,
               uint64_t timeline_semaphore_value,
               int64_t desired_present_time_ns,
               int64_t present_slop_ns)
{
	struct atrium_stream_target *t = ast(ct);
	struct vk_bundle *vk = &ct->c->base.vk;

	os_mutex_lock(&g_atrium_stream.lock);
	bool connected = g_atrium_stream.connected;
	uint64_t sample_ns = g_atrium_stream.last_sample_ns;
	bool idr = g_atrium_stream.idr_requested;
	g_atrium_stream.idr_requested = false;
	os_mutex_unlock(&g_atrium_stream.lock);

	if (t->encoder == NULL || !connected || sample_ns == 0) {
		// Nobody to send to: let the frame go (the timeline semaphore needs no waiter).
		if (!connected) {
			t->config_sent = false;
		}
		comp_scratch_single_images_done(&t->target);
		t->index = -1;
		return VK_SUCCESS;
	}

	// Convert on the compositor's queue once rendering is done.
	record_conversion(t, index);
	VkTimelineSemaphoreSubmitInfo timeline = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
	    .waitSemaphoreValueCount = 1,
	    .pWaitSemaphoreValues = &timeline_semaphore_value,
	};
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .pNext = &timeline,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &t->render_complete,
	    .pWaitDstStageMask = &wait_stage,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &t->cmd,
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores = &t->converted,
	};
	vk_queue_lock(present_queue);
	VkResult ret = vk->vkQueueSubmit(present_queue->queue, 1, &si, VK_NULL_HANDLE);
	vk_queue_unlock(present_queue);
	if (ret != VK_SUCCESS) {
		U_LOG_E("Atrium stream: conversion submit failed (%d)", ret);
		comp_scratch_single_images_done(&t->target);
		t->index = -1;
		return VK_SUCCESS;
	}

	// The decoder needs the codec config before the first frame.
	if (!t->config_sent) {
		size_t n = 0;
		const uint8_t *cfg = atrium_encoder_config_nals(t->encoder, &n);
		alvr_set_video_config_nals(ALVR_CODEC_TYPE_H264, cfg, (int32_t)n);
		t->config_sent = true;
		idr = true;
	}

	const uint8_t *data = NULL;
	size_t size = 0;
	bool is_idr = false;
	if (atrium_encoder_encode(t->encoder, t->converted, idr, &data, &size, &is_idr)) {
		// The timestamp names the tracking sample the frame was rendered for; the headset
		// reprojects from that pose.
		alvr_send_video_nal(sample_ns, (uint8_t *)data, (int32_t)size, is_idr);
		alvr_report_composed(sample_ns, 0);
		t->frames++;
		if (t->frames == 1 || t->frames % 600 == 0) {
			U_LOG_I("Atrium stream: %llu frames sent (last %zu bytes%s)", (unsigned long long)t->frames, size,
			        is_idr ? ", IDR" : "");
		}
	}

	comp_scratch_single_images_done(&t->target);
	t->index = -1;
	return VK_SUCCESS;
}

static VkResult
target_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void
target_flush(struct comp_target *ct)
{
}

static void
target_calc_frame_pacing(struct comp_target *ct,
                         int64_t *out_frame_id,
                         int64_t *out_wake_up_time_ns,
                         int64_t *out_desired_present_time_ns,
                         int64_t *out_present_slop_ns,
                         int64_t *out_predicted_display_time_ns)
{
	struct atrium_stream_target *t = ast(ct);
	int64_t predicted_display_period_ns = 0;
	int64_t min_display_period_ns = 0;
	u_pc_predict(t->upc, os_monotonic_get_ns(), out_frame_id, out_wake_up_time_ns, out_desired_present_time_ns,
	             out_present_slop_ns, out_predicted_display_time_ns, &predicted_display_period_ns,
	             &min_display_period_ns);
}

static void
target_mark_timing_point(struct comp_target *ct, enum comp_target_timing_point point, int64_t frame_id, int64_t when_ns)
{
	struct atrium_stream_target *t = ast(ct);
	switch (point) {
	case COMP_TARGET_TIMING_POINT_WAKE_UP: u_pc_mark_point(t->upc, U_TIMING_POINT_WAKE_UP, frame_id, when_ns); break;
	case COMP_TARGET_TIMING_POINT_BEGIN: u_pc_mark_point(t->upc, U_TIMING_POINT_BEGIN, frame_id, when_ns); break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN:
		u_pc_mark_point(t->upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_END:
		u_pc_mark_point(t->upc, U_TIMING_POINT_SUBMIT_END, frame_id, when_ns);
		break;
	default: break;
	}
}

static VkResult
target_update_timings(struct comp_target *ct)
{
	return VK_SUCCESS;
}

static void
target_info_gpu(struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{
	u_pc_info_gpu(ast(ct)->upc, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
}

static VkResult
target_queue_supports_present(struct comp_target *ct, struct vk_bundle_queue *queue, VkBool32 *out_supported)
{
	*out_supported = VK_TRUE;
	return VK_SUCCESS;
}

static void
target_set_title(struct comp_target *ct, const char *title)
{
}

static bool
target_is_shared_presentable_image(struct comp_target *ct)
{
	return false;
}

static void
target_destroy(struct comp_target *ct)
{
	struct atrium_stream_target *t = ast(ct);
	struct vk_bundle *vk = &ct->c->base.vk;
	if (t->has_init_vulkan) {
		vk->vkDeviceWaitIdle(vk->device);
		atrium_encoder_destroy(&t->encoder);
		if (t->cmd_pool) {
			vk->vkDestroyCommandPool(vk->device, t->cmd_pool, NULL);
		}
		if (t->converted) {
			vk->vkDestroySemaphore(vk->device, t->converted, NULL);
		}
		if (t->desc_pool) {
			vk->vkDestroyDescriptorPool(vk->device, t->desc_pool, NULL);
		}
		if (t->pipeline) {
			vk->vkDestroyPipeline(vk->device, t->pipeline, NULL);
		}
		if (t->pipeline_layout) {
			vk->vkDestroyPipelineLayout(vk->device, t->pipeline_layout, NULL);
		}
		if (t->set_layout) {
			vk->vkDestroyDescriptorSetLayout(vk->device, t->set_layout, NULL);
		}
		VkImageView views[2] = {t->plane_y_view, t->plane_uv_view};
		VkImage imgs[2] = {t->plane_y, t->plane_uv};
		VkDeviceMemory mems[2] = {t->plane_y_mem, t->plane_uv_mem};
		for (int i = 0; i < 2; i++) {
			if (views[i]) {
				vk->vkDestroyImageView(vk->device, views[i], NULL);
			}
			if (imgs[i]) {
				vk->vkDestroyImage(vk->device, imgs[i], NULL);
			}
			if (mems[i]) {
				vk->vkFreeMemory(vk->device, mems[i], NULL);
			}
		}
		if (t->render_complete) {
			vk->vkDestroySemaphore(vk->device, t->render_complete, NULL);
		}
		comp_scratch_single_images_free(&t->target, vk);
	}
	comp_scratch_single_images_destroy(&t->target);
	u_pc_destroy(&t->upc);
	free(t);
}

static struct comp_target *
target_create(struct comp_compositor *c)
{
	struct atrium_stream_target *t = U_TYPED_CALLOC(struct atrium_stream_target);
	t->base.name = "atrium_stream";
	t->base.init_pre_vulkan = target_init_pre_vulkan;
	t->base.init_post_vulkan = target_init_post_vulkan;
	t->base.check_ready = target_check_ready;
	t->base.create_images = target_create_images;
	t->base.has_images = target_has_images;
	t->base.acquire = target_acquire;
	t->base.present = target_present;
	t->base.wait_for_present = target_wait_for_present;
	t->base.flush = target_flush;
	t->base.calc_frame_pacing = target_calc_frame_pacing;
	t->base.mark_timing_point = target_mark_timing_point;
	t->base.update_timings = target_update_timings;
	t->base.info_gpu = target_info_gpu;
	t->base.set_title = target_set_title;
	t->base.queue_supports_present = target_queue_supports_present;
	t->base.is_shared_presentable_image = target_is_shared_presentable_image;
	t->base.destroy = target_destroy;
	t->base.c = c;
	t->base.wait_for_present_supported = false;
	t->index = -1;
	u_pc_fake_create(c->settings.nominal_frame_interval_ns, os_monotonic_get_ns(), &t->upc);
	comp_scratch_single_images_init(&t->target);
	return &t->base;
}

/*
 * Factory.
 */

static const char *atrium_stream_device_extensions[] = {
#ifdef VK_KHR_video_queue
    VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
#endif
};

static bool
factory_detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	return debug_get_bool_option_atrium_stream_target();
}

static bool
factory_create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_target *ct = target_create(c);
	if (ct == NULL) {
		return false;
	}
	*out_ct = ct;
	return true;
}

const struct comp_target_factory comp_target_factory_atrium_stream = {
    .name = "Atrium streaming (Quest, Pico)",
    .identifier = "atrium_stream",
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .required_instance_version = 0,
    .required_instance_extensions = NULL,
    .required_instance_extension_count = 0,
    .optional_device_extensions = atrium_stream_device_extensions,
    .optional_device_extension_count = ARRAY_SIZE(atrium_stream_device_extensions),
    .detect = factory_detect,
    .create_target = factory_create_target,
};
