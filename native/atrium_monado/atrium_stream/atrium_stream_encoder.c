// Copyright 2026, Atrium contributors.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief  Atrium streaming: H.264 on Vulkan Video. One session on the compositor's device; the
 *         source is an NV12 image the target fills; two DPB slots used in turn (each P frame
 *         references the previous frame); CBR when the driver offers it.
 * @ingroup drv_atrium_stream
 */
#include "atrium_stream_encoder.h"

#include "math/m_api.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include <stdlib.h>
#include <string.h>

#define ENC_LOG(...) U_LOG_I("Atrium encoder: " __VA_ARGS__)
#define ENC_ERR(...) U_LOG_E("Atrium encoder: " __VA_ARGS__)

#define DPB_SLOTS 2
#define BITSTREAM_SIZE (8u * 1024u * 1024u)
#define MAX_FRAME_NUM 256 // log2_max_frame_num = 8

struct atrium_encoder
{
	struct vk_bundle *vk;
	struct atrium_encoder_config cfg;
	uint32_t encode_family;

	// Video entry points (not loaded by Monado's bundle).
	PFN_vkCreateVideoSessionKHR CreateVideoSession;
	PFN_vkDestroyVideoSessionKHR DestroyVideoSession;
	PFN_vkGetVideoSessionMemoryRequirementsKHR GetVideoSessionMemoryRequirements;
	PFN_vkBindVideoSessionMemoryKHR BindVideoSessionMemory;
	PFN_vkCreateVideoSessionParametersKHR CreateVideoSessionParameters;
	PFN_vkDestroyVideoSessionParametersKHR DestroyVideoSessionParameters;
	PFN_vkGetEncodedVideoSessionParametersKHR GetEncodedVideoSessionParameters;
	PFN_vkCmdBeginVideoCodingKHR CmdBeginVideoCoding;
	PFN_vkCmdEndVideoCodingKHR CmdEndVideoCoding;
	PFN_vkCmdControlVideoCodingKHR CmdControlVideoCoding;
	PFN_vkCmdEncodeVideoKHR CmdEncodeVideo;

	// Profile (kept alive: other structs point at it).
	VkVideoEncodeH264ProfileInfoKHR h264_profile;
	VkVideoEncodeUsageInfoKHR usage;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR profile_list;

	VkVideoSessionKHR session;
	VkDeviceMemory session_mem[16];
	uint32_t session_mem_count;
	VkVideoSessionParametersKHR params;

	VkExtent2D coded; //!< extent aligned to 16 (macroblocks)
	StdVideoH264LevelIdc level; //!< the encoder's maximum

	// Source picture (NV12), filled by the target.
	VkImage src;
	VkDeviceMemory src_mem;
	VkImageView src_view;

	// Reference pictures.
	VkImage dpb[DPB_SLOTS];
	VkDeviceMemory dpb_mem[DPB_SLOTS];
	VkImageView dpb_view[DPB_SLOTS];

	// Output.
	VkBuffer bitstream;
	VkDeviceMemory bitstream_mem;
	uint8_t *bitstream_map;
	VkQueryPool query;

	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;

	// Rate control (chained into every begin after the first).
	bool rc_set;
	VkVideoEncodeRateControlModeFlagBitsKHR rc_mode;
	VkVideoEncodeH264RateControlInfoKHR rc_h264;
	VkVideoEncodeRateControlLayerInfoKHR rc_layer;
	VkVideoEncodeRateControlInfoKHR rc;

	// Codec config for the decoder (SPS + PPS).
	uint8_t config[512];
	size_t config_size;

	// Stream state.
	bool initialized; //!< DPB layouts set, session reset
	uint32_t frame_num;
	uint32_t idr_id;
	int last_slot; //!< slot holding the previous frame, -1 none
	struct
	{
		uint32_t frame_num;
		bool idr;
	} slot_info[DPB_SLOTS];

	uint8_t *out; //!< copy of the last frame
	size_t out_cap;
};

#define LOAD(e, name) ((e)->name = (PFN_vk##name##KHR)(e)->vk->vkGetDeviceProcAddr((e)->vk->device, "vk" #name "KHR"))

static uint32_t
align_up(uint32_t v, uint32_t a)
{
	return a ? (v + a - 1) / a * a : v;
}

static bool
memory_type(struct vk_bundle *vk, uint32_t bits, VkMemoryPropertyFlags props, uint32_t *out)
{
	const VkPhysicalDeviceMemoryProperties *mp = &vk->device_memory_props;
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & props) == props) {
			*out = i;
			return true;
		}
	}
	return false;
}

static bool
create_picture(struct atrium_encoder *e,
               VkImageUsageFlags usage,
               bool shared,
               VkImage *out_image,
               VkDeviceMemory *out_mem,
               VkImageView *out_view)
{
	struct vk_bundle *vk = e->vk;
	uint32_t families[2] = {e->cfg.producer_queue_family, e->encode_family};
	bool concurrent = shared && families[0] != families[1];
	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &e->profile_list,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	    .extent = {e->coded.width, e->coded.height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = usage,
	    .sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
	    .queueFamilyIndexCount = concurrent ? 2 : 0,
	    .pQueueFamilyIndices = concurrent ? families : NULL,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vk->vkCreateImage(vk->device, &ici, NULL, out_image) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, *out_image, &req);
	if (vk_alloc_and_bind_image_memory(vk, *out_image, &req, NULL, "atrium encoder picture", out_mem) != VK_SUCCESS) {
		return false;
	}
	VkImageViewCreateInfo vci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = *out_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	return vk->vkCreateImageView(vk->device, &vci, NULL, out_view) == VK_SUCCESS;
}

static bool
create_session(struct atrium_encoder *e)
{
	struct vk_bundle *vk = e->vk;

	PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR GetCaps = (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vk->vkGetInstanceProcAddr(
	    vk->instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
	if (GetCaps == NULL) {
		ENC_ERR("no Vulkan Video on this device");
		return false;
	}

	e->h264_profile = (VkVideoEncodeH264ProfileInfoKHR){
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR,
	    .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH,
	};
	e->usage = (VkVideoEncodeUsageInfoKHR){
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,
	    .pNext = &e->h264_profile,
	    .videoUsageHints = VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR,
	    .videoContentHints = VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR,
	    .tuningMode = VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR,
	};
	e->profile = (VkVideoProfileInfoKHR){
	    .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
	    .pNext = &e->usage,
	    .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR,
	    .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
	    .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
	    .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
	};
	e->profile_list = (VkVideoProfileListInfoKHR){
	    .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
	    .profileCount = 1,
	    .pProfiles = &e->profile,
	};

	VkVideoEncodeH264CapabilitiesKHR h264_caps = {.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR};
	VkVideoEncodeCapabilitiesKHR enc_caps = {.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, .pNext = &h264_caps};
	VkVideoCapabilitiesKHR caps = {.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, .pNext = &enc_caps};
	VkResult ret = GetCaps(vk->physical_device, &e->profile, &caps);
	if (ret != VK_SUCCESS) {
		ENC_ERR("H.264 High encode not supported (%d)", ret);
		return false;
	}
	e->coded.width = align_up(e->cfg.width, MAX(16u, caps.pictureAccessGranularity.width));
	e->coded.height = align_up(e->cfg.height, MAX(16u, caps.pictureAccessGranularity.height));
	if (e->coded.width > caps.maxCodedExtent.width || e->coded.height > caps.maxCodedExtent.height) {
		ENC_ERR("%ux%u is larger than the encoder's maximum %ux%u", e->coded.width, e->coded.height,
		        caps.maxCodedExtent.width, caps.maxCodedExtent.height);
		return false;
	}
	e->level = h264_caps.maxLevelIdc;
	ENC_LOG("H.264 High, %ux%u (coded %ux%u), level idc enum %d, rc modes 0x%x, %u Mbit/s @ %u fps", e->cfg.width,
	        e->cfg.height, e->coded.width, e->coded.height, (int)h264_caps.maxLevelIdc,
	        (unsigned)enc_caps.rateControlModes, (unsigned)(e->cfg.bitrate_bps / 1000000), e->cfg.fps);

	VkExtensionProperties std_header = {
	    .extensionName = VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME,
	    .specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_SPEC_VERSION,
	};
	VkVideoSessionCreateInfoKHR sci = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
	    .queueFamilyIndex = e->encode_family,
	    .pVideoProfile = &e->profile,
	    .pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	    .maxCodedExtent = e->coded,
	    .referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	    .maxDpbSlots = DPB_SLOTS,
	    .maxActiveReferencePictures = 1,
	    .pStdHeaderVersion = &std_header,
	};
	ret = e->CreateVideoSession(vk->device, &sci, NULL, &e->session);
	if (ret != VK_SUCCESS) {
		ENC_ERR("vkCreateVideoSessionKHR: %d", ret);
		return false;
	}

	uint32_t count = 0;
	e->GetVideoSessionMemoryRequirements(vk->device, e->session, &count, NULL);
	VkVideoSessionMemoryRequirementsKHR reqs[16];
	count = MIN(count, 16u);
	for (uint32_t i = 0; i < count; i++) {
		reqs[i] = (VkVideoSessionMemoryRequirementsKHR){.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR};
	}
	e->GetVideoSessionMemoryRequirements(vk->device, e->session, &count, reqs);
	VkBindVideoSessionMemoryInfoKHR binds[16];
	for (uint32_t i = 0; i < count; i++) {
		uint32_t type;
		if (!memory_type(vk, reqs[i].memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
		    !memory_type(vk, reqs[i].memoryRequirements.memoryTypeBits, 0, &type)) {
			return false;
		}
		VkMemoryAllocateInfo mai = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = reqs[i].memoryRequirements.size,
		    .memoryTypeIndex = type,
		};
		if (vk->vkAllocateMemory(vk->device, &mai, NULL, &e->session_mem[i]) != VK_SUCCESS) {
			return false;
		}
		binds[i] = (VkBindVideoSessionMemoryInfoKHR){
		    .sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
		    .memoryBindIndex = reqs[i].memoryBindIndex,
		    .memory = e->session_mem[i],
		    .memorySize = reqs[i].memoryRequirements.size,
		};
	}
	e->session_mem_count = count;
	if (count > 0 && e->BindVideoSessionMemory(vk->device, e->session, count, binds) != VK_SUCCESS) {
		ENC_ERR("binding session memory failed");
		return false;
	}

	// Rate control: CBR if offered, else VBR, else the driver's default.
	VkVideoEncodeRateControlModeFlagsKHR modes = enc_caps.rateControlModes;
	e->rc_mode = (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)   ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR
	             : (modes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR) ? VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR
	                                                                       : VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;
	uint64_t bitrate = MIN(e->cfg.bitrate_bps, enc_caps.maxBitrate ? enc_caps.maxBitrate : e->cfg.bitrate_bps);
	e->rc_layer = (VkVideoEncodeRateControlLayerInfoKHR){
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,
	    .averageBitrate = bitrate,
	    .maxBitrate = bitrate,
	    .frameRateNumerator = e->cfg.fps,
	    .frameRateDenominator = 1,
	};
	e->rc_h264 = (VkVideoEncodeH264RateControlInfoKHR){
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR,
	    .flags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REFERENCE_PATTERN_FLAT_BIT_KHR,
	    .gopFrameCount = 0, // open-ended: IDR only when needed
	    .idrPeriod = 0,
	    .consecutiveBFrameCount = 0,
	    .temporalLayerCount = 1,
	};
	// A frame or two of buffering: low latency without starving the encoder on scene changes.
	uint32_t frame_ms = 1000 / MAX(e->cfg.fps, 1u);
	e->rc = (VkVideoEncodeRateControlInfoKHR){
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
	    .pNext = &e->rc_h264,
	    .rateControlMode = e->rc_mode,
	    .layerCount = e->rc_mode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR ? 0 : 1,
	    .pLayers = &e->rc_layer,
	    .virtualBufferSizeInMs = frame_ms * 2,
	    .initialVirtualBufferSizeInMs = frame_ms,
	};
	return true;
}

static bool
create_parameters(struct atrium_encoder *e)
{
	struct vk_bundle *vk = e->vk;
	uint32_t mbs_w = e->coded.width / 16, mbs_h = e->coded.height / 16;

	// BT.709 limited range, what the target's colour conversion writes. One reference frame, no
	// reordering: the decoder can output each frame immediately.
	StdVideoH264SequenceParameterSetVui vui = {
	    .flags =
	        {
	            .video_signal_type_present_flag = 1,
	            .color_description_present_flag = 1,
	            .bitstream_restriction_flag = 1,
	        },
	    .video_format = 5,
	    .colour_primaries = 1,
	    .transfer_characteristics = 1,
	    .matrix_coefficients = 1,
	    .max_num_reorder_frames = 0,
	    .max_dec_frame_buffering = 1,
	};
	StdVideoH264SequenceParameterSet sps = {
	    .flags =
	        {
	            .direct_8x8_inference_flag = 1,
	            .frame_mbs_only_flag = 1,
	            .frame_cropping_flag = (e->coded.width != e->cfg.width || e->coded.height != e->cfg.height),
	            .vui_parameters_present_flag = 1,
	        },
	    .profile_idc = STD_VIDEO_H264_PROFILE_IDC_HIGH,
	    .level_idc = e->level,
	    .chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420,
	    .seq_parameter_set_id = 0,
	    .log2_max_frame_num_minus4 = 4,
	    .pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2,
	    .max_num_ref_frames = 1,
	    .pic_width_in_mbs_minus1 = mbs_w - 1,
	    .pic_height_in_map_units_minus1 = mbs_h - 1,
	    .frame_crop_right_offset = (e->coded.width - e->cfg.width) / 2,
	    .frame_crop_bottom_offset = (e->coded.height - e->cfg.height) / 2,
	    .pSequenceParameterSetVui = &vui,
	};
	StdVideoH264PictureParameterSet pps = {
	    .flags =
	        {
	            .transform_8x8_mode_flag = 1,
	            .deblocking_filter_control_present_flag = 1,
	            .entropy_coding_mode_flag = 1,
	        },
	    .seq_parameter_set_id = 0,
	    .pic_parameter_set_id = 0,
	    .num_ref_idx_l0_default_active_minus1 = 0,
	};
	VkVideoEncodeH264SessionParametersAddInfoKHR add = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR,
	    .stdSPSCount = 1,
	    .pStdSPSs = &sps,
	    .stdPPSCount = 1,
	    .pStdPPSs = &pps,
	};
	VkVideoEncodeH264SessionParametersCreateInfoKHR h264 = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,
	    .maxStdSPSCount = 1,
	    .maxStdPPSCount = 1,
	    .pParametersAddInfo = &add,
	};
	VkVideoSessionParametersCreateInfoKHR pci = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
	    .pNext = &h264,
	    .videoSession = e->session,
	};
	VkResult ret = e->CreateVideoSessionParameters(vk->device, &pci, NULL, &e->params);
	if (ret != VK_SUCCESS) {
		ENC_ERR("vkCreateVideoSessionParametersKHR: %d", ret);
		return false;
	}

	// The SPS/PPS as the driver will write them (it may adjust fields).
	VkVideoEncodeH264SessionParametersGetInfoKHR h264_get = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR,
	    .writeStdSPS = VK_TRUE,
	    .writeStdPPS = VK_TRUE,
	};
	VkVideoEncodeSessionParametersGetInfoKHR get = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
	    .pNext = &h264_get,
	    .videoSessionParameters = e->params,
	};
	size_t size = sizeof(e->config);
	ret = e->GetEncodedVideoSessionParameters(vk->device, &get, NULL, &size, e->config);
	if (ret != VK_SUCCESS) {
		ENC_ERR("vkGetEncodedVideoSessionParametersKHR: %d", ret);
		return false;
	}
	e->config_size = size;
	return true;
}

static bool
create_buffers(struct atrium_encoder *e)
{
	struct vk_bundle *vk = e->vk;

	if (!create_picture(e, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, &e->src,
	                    &e->src_mem, &e->src_view)) {
		ENC_ERR("source picture");
		return false;
	}
	for (int i = 0; i < DPB_SLOTS; i++) {
		if (!create_picture(e, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, false, &e->dpb[i], &e->dpb_mem[i],
		                    &e->dpb_view[i])) {
			ENC_ERR("reference picture");
			return false;
		}
	}

	VkBufferCreateInfo bci = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .pNext = &e->profile_list,
	    .size = BITSTREAM_SIZE,
	    .usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
	};
	if (vk->vkCreateBuffer(vk->device, &bci, NULL, &e->bitstream) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req;
	vk->vkGetBufferMemoryRequirements(vk->device, e->bitstream, &req);
	uint32_t type;
	if (!memory_type(vk, req.memoryTypeBits,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
	                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
	                 &type) &&
	    !memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &type)) {
		ENC_ERR("no host-visible memory for the bitstream");
		return false;
	}
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = req.size,
	    .memoryTypeIndex = type,
	};
	if (vk->vkAllocateMemory(vk->device, &mai, NULL, &e->bitstream_mem) != VK_SUCCESS ||
	    vk->vkBindBufferMemory(vk->device, e->bitstream, e->bitstream_mem, 0) != VK_SUCCESS ||
	    vk->vkMapMemory(vk->device, e->bitstream_mem, 0, VK_WHOLE_SIZE, 0, (void **)&e->bitstream_map) !=
	        VK_SUCCESS) {
		return false;
	}

	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback = {
	    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
	    .pNext = &e->profile,
	    .encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
	                           VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR,
	};
	VkQueryPoolCreateInfo qci = {
	    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
	    .pNext = &feedback,
	    .queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR,
	    .queryCount = 1,
	};
	if (vk->vkCreateQueryPool(vk->device, &qci, NULL, &e->query) != VK_SUCCESS) {
		ENC_ERR("feedback query pool");
		return false;
	}

	VkCommandPoolCreateInfo cpi = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	    .queueFamilyIndex = e->encode_family,
	};
	VkCommandBufferAllocateInfo cai = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk->vkCreateCommandPool(vk->device, &cpi, NULL, &e->pool) != VK_SUCCESS) {
		return false;
	}
	cai.commandPool = e->pool;
	return vk->vkAllocateCommandBuffers(vk->device, &cai, &e->cmd) == VK_SUCCESS &&
	       vk->vkCreateFence(vk->device, &fci, NULL, &e->fence) == VK_SUCCESS;
}

bool
atrium_encoder_create(struct vk_bundle *vk, const struct atrium_encoder_config *cfg, struct atrium_encoder **out)
{
#if defined(VK_KHR_video_encode_queue)
	if (vk->encode_queue == NULL) {
		U_LOG_E("Atrium encoder: the GPU offers no video-encode queue");
		return false;
	}
	struct atrium_encoder *e = U_TYPED_CALLOC(struct atrium_encoder);
	e->vk = vk;
	e->cfg = *cfg;
	e->encode_family = vk->encode_queue->family_index;
	e->last_slot = -1;

	LOAD(e, CreateVideoSession);
	LOAD(e, DestroyVideoSession);
	LOAD(e, GetVideoSessionMemoryRequirements);
	LOAD(e, BindVideoSessionMemory);
	LOAD(e, CreateVideoSessionParameters);
	LOAD(e, DestroyVideoSessionParameters);
	LOAD(e, GetEncodedVideoSessionParameters);
	LOAD(e, CmdBeginVideoCoding);
	LOAD(e, CmdEndVideoCoding);
	LOAD(e, CmdControlVideoCoding);
	LOAD(e, CmdEncodeVideo);
	if (!e->CreateVideoSession || !e->CmdEncodeVideo || !e->GetEncodedVideoSessionParameters) {
		U_LOG_E("Atrium encoder: Vulkan Video encode functions missing (extensions not enabled?)");
		free(e);
		return false;
	}

	if (!create_session(e) || !create_parameters(e) || !create_buffers(e)) {
		atrium_encoder_destroy(&e);
		return false;
	}
	*out = e;
	return true;
#else
	(void)vk;
	(void)cfg;
	(void)out;
	U_LOG_E("Atrium encoder: built without Vulkan Video headers");
	return false;
#endif
}

VkImage
atrium_encoder_source_image(struct atrium_encoder *e)
{
	return e->src;
}

const uint8_t *
atrium_encoder_config_nals(struct atrium_encoder *e, size_t *out_size)
{
	*out_size = e->config_size;
	return e->config;
}

static void
barrier(struct atrium_encoder *e,
        VkImage image,
        VkImageLayout from,
        VkImageLayout to,
        VkPipelineStageFlags src_stage,
        VkAccessFlags src_access,
        VkAccessFlags dst_access)
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
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	e->vk->vkCmdPipelineBarrier(e->cmd, src_stage, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

bool
atrium_encoder_encode(struct atrium_encoder *e,
                      VkSemaphore wait,
                      bool force_idr,
                      const uint8_t **out_data,
                      size_t *out_size,
                      bool *out_is_idr)
{
#if defined(VK_KHR_video_encode_queue)
	struct vk_bundle *vk = e->vk;
	bool idr = force_idr || e->last_slot < 0 || e->frame_num == 0;
	if (idr) {
		e->frame_num = 0;
	}
	int slot = e->last_slot < 0 ? 0 : (e->last_slot + 1) % DPB_SLOTS;
	int ref = idr ? -1 : e->last_slot;

	VkCommandBufferBeginInfo begin = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk->vkBeginCommandBuffer(e->cmd, &begin);

	if (!e->initialized) {
		for (int i = 0; i < DPB_SLOTS; i++) {
			barrier(e, e->dpb[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
			        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0);
		}
	}
	// The target left the source in TRANSFER_DST after copying the planes in.
	barrier(e, e->src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
	vk->vkCmdResetQueryPool(e->cmd, e->query, 0, 1);

	// Reference slot descriptions.
	StdVideoEncodeH264ReferenceInfo std_ref[DPB_SLOTS];
	VkVideoEncodeH264DpbSlotInfoKHR h264_slot[DPB_SLOTS];
	VkVideoPictureResourceInfoKHR dpb_res[DPB_SLOTS];
	for (int i = 0; i < DPB_SLOTS; i++) {
		std_ref[i] = (StdVideoEncodeH264ReferenceInfo){
		    .primary_pic_type =
		        e->slot_info[i].idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P,
		    .FrameNum = e->slot_info[i].frame_num,
		    .PicOrderCnt = (int32_t)(e->slot_info[i].frame_num * 2),
		};
		h264_slot[i] = (VkVideoEncodeH264DpbSlotInfoKHR){
		    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR,
		    .pStdReferenceInfo = &std_ref[i],
		};
		dpb_res[i] = (VkVideoPictureResourceInfoKHR){
		    .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		    .codedExtent = e->coded,
		    .imageViewBinding = e->dpb_view[i],
		};
	}
	// This frame's own reference info (written into `slot`).
	std_ref[slot] = (StdVideoEncodeH264ReferenceInfo){
	    .primary_pic_type = idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P,
	    .FrameNum = e->frame_num,
	    .PicOrderCnt = (int32_t)(e->frame_num * 2),
	};

	VkVideoReferenceSlotInfoKHR setup = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
	    .pNext = &h264_slot[slot],
	    .slotIndex = slot,
	    .pPictureResource = &dpb_res[slot],
	};
	VkVideoReferenceSlotInfoKHR begin_slots[2];
	uint32_t begin_count = 0;
	begin_slots[begin_count] = setup;
	begin_slots[begin_count++].slotIndex = -1; // being (re)activated by this frame
	VkVideoReferenceSlotInfoKHR ref_slot = {0};
	if (ref >= 0) {
		ref_slot = (VkVideoReferenceSlotInfoKHR){
		    .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
		    .pNext = &h264_slot[ref],
		    .slotIndex = ref,
		    .pPictureResource = &dpb_res[ref],
		};
		begin_slots[begin_count++] = ref_slot;
	}

	VkVideoBeginCodingInfoKHR bci = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
	    .pNext = e->rc_set && e->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR ? &e->rc : NULL,
	    .videoSession = e->session,
	    .videoSessionParameters = e->params,
	    .referenceSlotCount = begin_count,
	    .pReferenceSlots = begin_slots,
	};
	e->CmdBeginVideoCoding(e->cmd, &bci);

	if (!e->initialized) {
		VkVideoCodingControlInfoKHR reset = {
		    .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
		    .flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR,
		};
		e->CmdControlVideoCoding(e->cmd, &reset);
		if (e->rc_mode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) {
			VkVideoCodingControlInfoKHR rc = {
			    .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
			    .pNext = &e->rc,
			    .flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR,
			};
			e->CmdControlVideoCoding(e->cmd, &rc);
		}
		e->rc_set = true;
		e->initialized = true;
	}

	StdVideoEncodeH264ReferenceListsInfo lists = {
	    .num_ref_idx_l0_active_minus1 = 0,
	    .num_ref_idx_l1_active_minus1 = 0,
	};
	memset(lists.RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(lists.RefPicList0));
	memset(lists.RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(lists.RefPicList1));
	if (ref >= 0) {
		lists.RefPicList0[0] = (uint8_t)ref;
	}
	StdVideoEncodeH264SliceHeader slice = {
	    .slice_type = idr ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P,
	    .cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0,
	    // disable_deblocking_filter_idc = 0 ("DISABLED" in the enum's naming): the filter stays on.
	    .disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED,
	};
	VkVideoEncodeH264NaluSliceInfoKHR nalu = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR,
	    .constantQp = 0,
	    .pStdSliceHeader = &slice,
	};
	StdVideoEncodeH264PictureInfo pic = {
	    .flags =
	        {
	            .IdrPicFlag = idr,
	            .is_reference = 1,
	        },
	    .seq_parameter_set_id = 0,
	    .pic_parameter_set_id = 0,
	    .idr_pic_id = (uint16_t)e->idr_id,
	    .primary_pic_type = idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P,
	    .frame_num = e->frame_num,
	    .PicOrderCnt = (int32_t)(e->frame_num * 2),
	    .pRefLists = &lists,
	};
	VkVideoEncodeH264PictureInfoKHR h264_pic = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR,
	    .naluSliceEntryCount = 1,
	    .pNaluSliceEntries = &nalu,
	    .pStdPictureInfo = &pic,
	};
	VkVideoEncodeInfoKHR enc = {
	    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,
	    .pNext = &h264_pic,
	    .dstBuffer = e->bitstream,
	    .dstBufferOffset = 0,
	    .dstBufferRange = BITSTREAM_SIZE,
	    .srcPictureResource =
	        {
	            .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
	            .codedExtent = e->coded,
	            .imageViewBinding = e->src_view,
	        },
	    .pSetupReferenceSlot = &setup,
	    .referenceSlotCount = ref >= 0 ? 1 : 0,
	    .pReferenceSlots = ref >= 0 ? &ref_slot : NULL,
	};
	vk->vkCmdBeginQuery(e->cmd, e->query, 0, 0);
	e->CmdEncodeVideo(e->cmd, &enc);
	vk->vkCmdEndQuery(e->cmd, e->query, 0);

	VkVideoEndCodingInfoKHR eci = {.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
	e->CmdEndVideoCoding(e->cmd, &eci);
	// Back to TRANSFER_DST for the target's next copy.
	barrier(e, e->src, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
	vk->vkEndCommandBuffer(e->cmd);

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo si = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1 : 0,
	    .pWaitSemaphores = &wait,
	    .pWaitDstStageMask = &wait_stage,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &e->cmd,
	};
	vk_queue_lock(vk->encode_queue);
	VkResult ret = vk->vkQueueSubmit(vk->encode_queue->queue, 1, &si, e->fence);
	vk_queue_unlock(vk->encode_queue);
	if (ret != VK_SUCCESS) {
		ENC_ERR("submit: %d", ret);
		return false;
	}
	vk->vkWaitForFences(vk->device, 1, &e->fence, VK_TRUE, UINT64_MAX);
	vk->vkResetFences(vk->device, 1, &e->fence);

	// Feedback: offset, bytes written, status.
	uint32_t fb[3] = {0};
	ret = vk->vkGetQueryPoolResults(vk->device, e->query, 0, 1, sizeof(fb), fb, sizeof(fb),
	                                VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
	if (ret != VK_SUCCESS || (int32_t)fb[2] <= 0 || fb[1] == 0) {
		ENC_ERR("encode failed (result %d, status %d, %u bytes)", ret, (int32_t)fb[2], fb[1]);
		e->last_slot = -1; // restart with an IDR
		return false;
	}

	// The decoder wants SPS/PPS before each IDR.
	size_t need = fb[1] + (idr ? e->config_size : 0);
	if (need > e->out_cap) {
		e->out = realloc(e->out, need);
		e->out_cap = need;
	}
	size_t n = 0;
	if (idr) {
		memcpy(e->out, e->config, e->config_size);
		n = e->config_size;
	}
	memcpy(e->out + n, e->bitstream_map + fb[0], fb[1]);
	n += fb[1];

	e->slot_info[slot].frame_num = e->frame_num;
	e->slot_info[slot].idr = idr;
	e->last_slot = slot;
	if (idr) {
		e->idr_id = (e->idr_id + 1) % 65536;
	}
	e->frame_num = (e->frame_num + 1) % MAX_FRAME_NUM;
	if (e->frame_num == 0) {
		e->frame_num = 1; // frame_num 0 is only for IDRs here
	}

	*out_data = e->out;
	*out_size = n;
	*out_is_idr = idr;
	return true;
#else
	return false;
#endif
}

void
atrium_encoder_destroy(struct atrium_encoder **pe)
{
	struct atrium_encoder *e = *pe;
	if (e == NULL) {
		return;
	}
	struct vk_bundle *vk = e->vk;
	vk->vkDeviceWaitIdle(vk->device);
	if (e->fence) {
		vk->vkDestroyFence(vk->device, e->fence, NULL);
	}
	if (e->pool) {
		vk->vkDestroyCommandPool(vk->device, e->pool, NULL);
	}
	if (e->query) {
		vk->vkDestroyQueryPool(vk->device, e->query, NULL);
	}
	if (e->bitstream) {
		vk->vkDestroyBuffer(vk->device, e->bitstream, NULL);
	}
	if (e->bitstream_mem) {
		vk->vkFreeMemory(vk->device, e->bitstream_mem, NULL);
	}
	for (int i = 0; i < DPB_SLOTS; i++) {
		if (e->dpb_view[i]) {
			vk->vkDestroyImageView(vk->device, e->dpb_view[i], NULL);
		}
		if (e->dpb[i]) {
			vk->vkDestroyImage(vk->device, e->dpb[i], NULL);
		}
		if (e->dpb_mem[i]) {
			vk->vkFreeMemory(vk->device, e->dpb_mem[i], NULL);
		}
	}
	if (e->src_view) {
		vk->vkDestroyImageView(vk->device, e->src_view, NULL);
	}
	if (e->src) {
		vk->vkDestroyImage(vk->device, e->src, NULL);
	}
	if (e->src_mem) {
		vk->vkFreeMemory(vk->device, e->src_mem, NULL);
	}
	if (e->params && e->DestroyVideoSessionParameters) {
		e->DestroyVideoSessionParameters(vk->device, e->params, NULL);
	}
	if (e->session && e->DestroyVideoSession) {
		e->DestroyVideoSession(vk->device, e->session, NULL);
	}
	for (uint32_t i = 0; i < e->session_mem_count; i++) {
		vk->vkFreeMemory(vk->device, e->session_mem[i], NULL);
	}
	free(e->out);
	free(e);
	*pe = NULL;
}
