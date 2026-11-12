/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Virtio Video Device
 *
 * This header is BSD licensed so anyone can use the definitions
 * to implement compatible drivers/servers:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (C) 2020 OpenSynergy GmbH.
 */

#ifndef _UAPI_LINUX_VIRTIO_VIDEO_H
#define _UAPI_LINUX_VIRTIO_VIDEO_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t __u8;
typedef uint32_t __le32;
typedef uint64_t __le64;
#endif

#define UML_VIRTIO_VIDEO_VERSION 1

enum virtio_video_device_type {
	VIRTIO_VIDEO_DEVICE_ENCODER = 0x0100,
	VIRTIO_VIDEO_DEVICE_DECODER,
	VIRTIO_VIDEO_DEVICE_CAMERA,
};

/*
 * Feature bits
 */
enum {
	/* Guest pages can be used for video buffers. */
	VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES = 0,
	/* The host can process buffers even if they are non-contiguous memory
	   such as scatter-gather lists. */
	VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG = 1,
	/* Support of vendor virtqueues */
	VIRTIO_VIDEO_F_VENDOR = 2,
	/* Scatter entries may name an offset in a virtio shared-memory
	   region instead of a guest-physical address. */
	VIRTIO_VIDEO_F_RESOURCE_SHM_REGION = 3,
	/* The device speaks the V4L2 control and stream-information commands
	   (VIRTIO_VIDEO_CMD_*_V4L2_CONTROLS, VIRTIO_VIDEO_CMD_*_STREAM_INFO).
	   An encoder (device ID 30) always offers it, and a driver binds an
	   encoder only when it is negotiated. A decoder never offers it. */
	VIRTIO_VIDEO_F_V4L2_CONTROLS = 4
};


#define VIRTIO_VIDEO_MAX_PLANES 8

/*
 * Image formats
 */

enum virtio_video_format {
	/* Raw formats */
	VIRTIO_VIDEO_FORMAT_RAW_MIN = 1,
	VIRTIO_VIDEO_FORMAT_ARGB8888 = VIRTIO_VIDEO_FORMAT_RAW_MIN,
	VIRTIO_VIDEO_FORMAT_BGRA8888,
	VIRTIO_VIDEO_FORMAT_RGBA8888,
	VIRTIO_VIDEO_FORMAT_NV12, /* 12  Y/CbCr 4:2:0  */
	VIRTIO_VIDEO_FORMAT_YUV420, /* 12  YUV 4:2:0     */
	VIRTIO_VIDEO_FORMAT_YVU420, /* 12  YVU 4:2:0     */
	VIRTIO_VIDEO_FORMAT_YUV422, /* 16 YUV 4:2:2 */
	VIRTIO_VIDEO_FORMAT_P010,
	VIRTIO_VIDEO_FORMAT_NV16, /* 16  Y/CbCr 4:2:2, encoder input */
	VIRTIO_VIDEO_FORMAT_NV24, /* 24  Y/CbCr 4:4:4, encoder input */
	VIRTIO_VIDEO_FORMAT_RAW_MAX = VIRTIO_VIDEO_FORMAT_NV24,

	/* Coded formats */
	VIRTIO_VIDEO_FORMAT_CODED_MIN = 0x1000,
	VIRTIO_VIDEO_FORMAT_MPEG2 =
		VIRTIO_VIDEO_FORMAT_CODED_MIN, /* MPEG-2 Part 2 */
	VIRTIO_VIDEO_FORMAT_MPEG4, /* MPEG-4 Part 2 */
	VIRTIO_VIDEO_FORMAT_H264, /* H.264 */
	VIRTIO_VIDEO_FORMAT_HEVC, /* HEVC aka H.265*/
	VIRTIO_VIDEO_FORMAT_VP8, /* VP8 */
	VIRTIO_VIDEO_FORMAT_VP9, /* VP9 */
	VIRTIO_VIDEO_FORMAT_MJPEG,
	VIRTIO_VIDEO_FORMAT_CODED_MAX = VIRTIO_VIDEO_FORMAT_MJPEG,
};

enum virtio_video_profile {
	/* H.264 */
	VIRTIO_VIDEO_PROFILE_H264_MIN = 0x100,
	VIRTIO_VIDEO_PROFILE_H264_BASELINE = VIRTIO_VIDEO_PROFILE_H264_MIN,
	VIRTIO_VIDEO_PROFILE_H264_MAIN,
	VIRTIO_VIDEO_PROFILE_H264_EXTENDED,
	VIRTIO_VIDEO_PROFILE_H264_HIGH,
	VIRTIO_VIDEO_PROFILE_H264_HIGH10PROFILE,
	VIRTIO_VIDEO_PROFILE_H264_HIGH422PROFILE,
	VIRTIO_VIDEO_PROFILE_H264_HIGH444PREDICTIVEPROFILE,
	VIRTIO_VIDEO_PROFILE_H264_SCALABLEBASELINE,
	VIRTIO_VIDEO_PROFILE_H264_SCALABLEHIGH,
	VIRTIO_VIDEO_PROFILE_H264_STEREOHIGH,
	VIRTIO_VIDEO_PROFILE_H264_MULTIVIEWHIGH,
	VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE,
	VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_HIGH,
	VIRTIO_VIDEO_PROFILE_H264_MAX = VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_HIGH,

	/* HEVC */
	VIRTIO_VIDEO_PROFILE_HEVC_MIN = 0x200,
	VIRTIO_VIDEO_PROFILE_HEVC_MAIN = VIRTIO_VIDEO_PROFILE_HEVC_MIN,
	VIRTIO_VIDEO_PROFILE_HEVC_MAIN10,
	VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE,
	VIRTIO_VIDEO_PROFILE_HEVC_MAX =
		VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE,

	/* VP8 */
	VIRTIO_VIDEO_PROFILE_VP8_MIN = 0x300,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE0 = VIRTIO_VIDEO_PROFILE_VP8_MIN,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE1,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE2,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE3,
	VIRTIO_VIDEO_PROFILE_VP8_MAX = VIRTIO_VIDEO_PROFILE_VP8_PROFILE3,

	/* VP9 */
	VIRTIO_VIDEO_PROFILE_VP9_MIN = 0x400,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE0 = VIRTIO_VIDEO_PROFILE_VP9_MIN,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE1,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE2,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE3,
	VIRTIO_VIDEO_PROFILE_VP9_MAX = VIRTIO_VIDEO_PROFILE_VP9_PROFILE3,
};

enum virtio_video_level {
	/* H.264 */
	VIRTIO_VIDEO_LEVEL_H264_MIN = 0x100,
	VIRTIO_VIDEO_LEVEL_H264_1_0 = VIRTIO_VIDEO_LEVEL_H264_MIN,
	VIRTIO_VIDEO_LEVEL_H264_1_1,
	VIRTIO_VIDEO_LEVEL_H264_1_2,
	VIRTIO_VIDEO_LEVEL_H264_1_3,
	VIRTIO_VIDEO_LEVEL_H264_2_0,
	VIRTIO_VIDEO_LEVEL_H264_2_1,
	VIRTIO_VIDEO_LEVEL_H264_2_2,
	VIRTIO_VIDEO_LEVEL_H264_3_0,
	VIRTIO_VIDEO_LEVEL_H264_3_1,
	VIRTIO_VIDEO_LEVEL_H264_3_2,
	VIRTIO_VIDEO_LEVEL_H264_4_0,
	VIRTIO_VIDEO_LEVEL_H264_4_1,
	VIRTIO_VIDEO_LEVEL_H264_4_2,
	VIRTIO_VIDEO_LEVEL_H264_5_0,
	VIRTIO_VIDEO_LEVEL_H264_5_1,
	VIRTIO_VIDEO_LEVEL_H264_5_2,
	VIRTIO_VIDEO_LEVEL_H264_6_0,
	VIRTIO_VIDEO_LEVEL_H264_6_1,
	VIRTIO_VIDEO_LEVEL_H264_6_2,
	VIRTIO_VIDEO_LEVEL_H264_MAX = VIRTIO_VIDEO_LEVEL_H264_6_2,
    VIRTIO_VIDEO_LEVEL_HEVC_MIN = 0x200,
    VIRTIO_VIDEO_LEVEL_HEVC_1 = VIRTIO_VIDEO_LEVEL_HEVC_MIN,
    VIRTIO_VIDEO_LEVEL_HEVC_2,
    VIRTIO_VIDEO_LEVEL_HEVC_2_1,
    VIRTIO_VIDEO_LEVEL_HEVC_3,
    VIRTIO_VIDEO_LEVEL_HEVC_3_1,
    VIRTIO_VIDEO_LEVEL_HEVC_4,
    VIRTIO_VIDEO_LEVEL_HEVC_4_1,
    VIRTIO_VIDEO_LEVEL_HEVC_5,
    VIRTIO_VIDEO_LEVEL_HEVC_5_1,
    VIRTIO_VIDEO_LEVEL_HEVC_5_2,
    VIRTIO_VIDEO_LEVEL_HEVC_6,
    VIRTIO_VIDEO_LEVEL_HEVC_6_1,
    VIRTIO_VIDEO_LEVEL_HEVC_6_2,
    VIRTIO_VIDEO_LEVEL_HEVC_MAX = VIRTIO_VIDEO_LEVEL_HEVC_6_2,
};

/*
 * Config
 */

struct virtio_video_config {
	__le32 version;
	__le32 max_caps_length;
	__le32 max_resp_length;
};

/*
 * Commands
 */

enum virtio_video_cmd_type {
	/* Command */
	VIRTIO_VIDEO_CMD_QUERY_CAPABILITY = 0x0100,
	VIRTIO_VIDEO_CMD_STREAM_CREATE,
	VIRTIO_VIDEO_CMD_STREAM_DESTROY,
	VIRTIO_VIDEO_CMD_STREAM_DRAIN,
	VIRTIO_VIDEO_CMD_RESOURCE_ATTACH,
	VIRTIO_VIDEO_CMD_RESOURCE_QUEUE,
	VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES,
	VIRTIO_VIDEO_CMD_QUEUE_CLEAR,
	VIRTIO_VIDEO_CMD_GET_PARAMS,
	VIRTIO_VIDEO_CMD_SET_PARAMS,
	VIRTIO_VIDEO_CMD_QUERY_CONTROL,
	VIRTIO_VIDEO_CMD_GET_CONTROL,
	VIRTIO_VIDEO_CMD_SET_CONTROL,
	/* Encoder only; requires VIRTIO_VIDEO_F_V4L2_CONTROLS */
	VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS,
	VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS,
	VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS,
	VIRTIO_VIDEO_CMD_SET_STREAM_INFO,
	VIRTIO_VIDEO_CMD_GET_STREAM_INFO,

	/* Response */
	VIRTIO_VIDEO_RESP_OK_NODATA = 0x0200,
	VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY,
	VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE,
	VIRTIO_VIDEO_RESP_OK_GET_PARAMS,
	VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL,
	VIRTIO_VIDEO_RESP_OK_GET_CONTROL,
	VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS,
	VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS,
	VIRTIO_VIDEO_RESP_OK_STREAM_INFO,

	VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION = 0x0300,
	VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY,
	VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
	VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID,
	VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
	VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL,
	/* A control that may not change while the stream is encoding was set
	   while it was; the driver normally refuses this itself (-EBUSY). */
	VIRTIO_VIDEO_RESP_ERR_BUSY,
};

struct virtio_video_cmd_hdr {
	__le32 type; /* One of enum virtio_video_cmd_type */
	__le32 stream_id;
};

/* VIRTIO_VIDEO_CMD_QUERY_CAPABILITY */
enum virtio_video_queue_type {
	VIRTIO_VIDEO_QUEUE_TYPE_INPUT = 0x100,
	VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT,
};

struct virtio_video_query_capability {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

enum virtio_video_planes_layout_flag {
	VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER = 1 << 0,
	VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE = 1 << 1,
};

struct virtio_video_format_range {
	__le32 min;
	__le32 max;
	__le32 step;
	__u8 padding[4];
};

struct virtio_video_format_frame {
	struct virtio_video_format_range width;
	struct virtio_video_format_range height;
	__le32 num_rates;
	__u8 padding[4];
	/* Followed by struct virtio_video_format_range frame_rates[] */
};

struct virtio_video_format_desc {
	__le64 mask;
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__le32 planes_layout; /* Bitmask with VIRTIO_VIDEO_PLANES_LAYOUT_* */
	__le32 plane_align;
	__le32 num_frames;
	/* Followed by struct virtio_video_format_frame frames[] */
};

struct virtio_video_query_capability_resp {
	struct virtio_video_cmd_hdr hdr;
	__le32 num_descs;
	__u8 padding[4];
	/* Followed by struct virtio_video_format_desc descs[] */
};

/* VIRTIO_VIDEO_CMD_STREAM_CREATE */
enum virtio_video_mem_type {
	VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES,
};

struct virtio_video_stream_create {
	struct virtio_video_cmd_hdr hdr;
	__le32 in_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
	__le32 out_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
	__le32 coded_format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__u8 padding[4];
	__u8 tag[64];
};

/* VIRTIO_VIDEO_CMD_STREAM_DESTROY */
struct virtio_video_stream_destroy {
	struct virtio_video_cmd_hdr hdr;
};

/* VIRTIO_VIDEO_CMD_STREAM_DRAIN */
struct virtio_video_stream_drain {
	struct virtio_video_cmd_hdr hdr;
};

/* VIRTIO_VIDEO_CMD_RESOURCE_ATTACH */
struct virtio_video_resource_object {
	__u8 uuid [16];
};

/*
 * A scatter entry normally names guest-physical memory. An entry whose
 * VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION flag is set instead names an offset
 * in the virtio shared-memory region its flags identify, which the host
 * resolves to the memory the region's owning device published there. Such an
 * entry is only valid when VIRTIO_VIDEO_F_RESOURCE_SHM_REGION is negotiated.
 */
#define VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION (1u << 0)
#define VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_SHIFT 8
#define VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_MASK (0xffu << 8)
#define VIRTIO_VIDEO_RESOURCE_SG_FLAG_MASK \
	(VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION | \
	 VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_MASK)

struct virtio_video_resource_sg_entry {
	__le64 addr;
	__le32 length;
	__le32 flags; /* Bitmask with VIRTIO_VIDEO_RESOURCE_SG_FLAG_* */
};

struct virtio_video_resource_sg_list {
	__le32 num_entries;
	__u8 padding[4];
	struct virtio_video_resource_sg_entry entries[];
};
#define VIRTIO_VIDEO_RESOURCE_SG_SIZE(n) \
	offsetof(struct virtio_video_resource_sg_list, entries[n])

union virtio_video_resource {
	struct virtio_video_resource_sg_list sg_list;
	struct virtio_video_resource_object object;
};

struct virtio_video_resource_attach {
	__le32 cmd_type;
	__le32 stream_id;
	__le32 queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_* */
	__le32 resource_id;
	/* Followed by struct virtio_video_resource resources[] */
};

/* VIRTIO_VIDEO_CMD_RESOURCE_QUEUE */
struct virtio_video_resource_queue {
	__le32 cmd_type;
	__le32 stream_id;
	__le32 queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_* */
	__le32 resource_id;
	__le32 flags;      /* Bitmask with VIRTIO_VIDEO_ENQUEUE_FLAG_ * */
	__u8 padding[4];
	__le64 timestamp;
	__le32 data_sizes[VIRTIO_VIDEO_MAX_PLANES];
};

enum virtio_video_dequeue_flag {
	VIRTIO_VIDEO_DEQUEUE_FLAG_ERR = 1 << 0,
	VIRTIO_VIDEO_DEQUEUE_FLAG_EOS = 1 << 1,

	/* Encoder only */
	VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME = 1 << 2,
	VIRTIO_VIDEO_DEQUEUE_FLAG_PFRAME = 1 << 3,
	VIRTIO_VIDEO_DEQUEUE_FLAG_BFRAME = 1 << 4,
	/* The buffer holds only parameter sets (VPS/SPS/PPS), produced first
	   under V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE. */
	VIRTIO_VIDEO_DEQUEUE_FLAG_CODEC_CONFIG = 1 << 5,
};

/*
 * On an encoder (device ID 30):
 *  - VIRTIO_VIDEO_QUEUE_TYPE_INPUT carries raw frames (V4L2 OUTPUT) and
 *    VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT carries coded buffers (V4L2 CAPTURE).
 *  - A coded buffer's response carries the timestamp of the raw frame it
 *    encodes, data_sizes[0] = bytes written, and KEY_FRAME, PFRAME or
 *    BFRAME. H.264 and HEVC payloads are Annex B byte streams.
 *  - Coded buffers complete in decode order. The input buffer of a frame
 *    completes as soon as the host has copied it, before it is encoded.
 *  - STREAM_DRAIN completes once every queued frame is encoded; the next
 *    coded buffer then completes empty with EOS.
 *  - virtio_video_params.frame_rate is ignored; the frame interval is
 *    VIRTIO_VIDEO_CMD_SET_STREAM_INFO's.
 */

struct virtio_video_resource_queue_resp {
	struct virtio_video_cmd_hdr hdr;
	__le32 flags;
	__le64 timestamp;
	__le32 data_sizes[VIRTIO_VIDEO_MAX_PLANES];
};

/* VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES */
struct virtio_video_queue_detach_resources {
	__le32 cmd_type;
	__le32 stream_id;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

/* VIRTIO_VIDEO_CMD_QUEUE_CLEAR */
struct virtio_video_queue_clear {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

/* VIRTIO_VIDEO_CMD_GET_PARAMS */
struct virtio_video_plane_format {
	__le32 plane_size;
	__le32 stride;
};

struct virtio_video_crop {
	__le32 left;
	__le32 top;
	__le32 width;
	__le32 height;
};

struct virtio_video_params {
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__le32 frame_width;
	__le32 frame_height;
	__le32 min_buffers;
	__le32 max_buffers;
	struct virtio_video_crop crop;
	__le32 frame_rate;
	__le32 num_planes;
	struct virtio_video_plane_format plane_formats[VIRTIO_VIDEO_MAX_PLANES];
};

struct virtio_video_get_params {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

struct virtio_video_get_params_resp {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_params params;
};

/* VIRTIO_VIDEO_CMD_SET_PARAMS */
struct virtio_video_set_params {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_params params;
};

/* VIRTIO_VIDEO_CMD_QUERY_CONTROL */
enum virtio_video_control_type {
	VIRTIO_VIDEO_CONTROL_BITRATE = 1,
	VIRTIO_VIDEO_CONTROL_PROFILE,
	VIRTIO_VIDEO_CONTROL_LEVEL,
};

struct virtio_video_query_control_profile {
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
	__u8 padding[4];
};

struct virtio_video_query_control_level {
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
	__u8 padding[4];
};

struct virtio_video_query_control {
	struct virtio_video_cmd_hdr hdr;
	__le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
	__u8 padding[4];
	/*
	 * Followed by a value of struct virtio_video_query_control_*
	 * in accordance with the value of control.
	 */
};

struct virtio_video_query_control_resp_profile {
	__le32 num;
	__u8 padding[4];
	/* Followed by an array le32 profiles[] */
};

struct virtio_video_query_control_resp_level {
	__le32 num;
	__u8 padding[4];
	/* Followed by an array le32 level[] */
};

struct virtio_video_query_control_resp {
	struct virtio_video_cmd_hdr hdr;
	/* Followed by one of struct virtio_video_query_control_resp_* */
};

/* VIRTIO_VIDEO_CMD_GET_CONTROL */
struct virtio_video_get_control {
	struct virtio_video_cmd_hdr hdr;
	__le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
	__u8 padding[4];
};

struct virtio_video_control_val_bitrate {
	__le32 bitrate;
	__u8 padding[4];
};

struct virtio_video_control_val_profile {
	__le32 profile;
	__u8 padding[4];
};

struct virtio_video_control_val_level {
	__le32 level;
	__u8 padding[4];
};

struct virtio_video_get_control_resp {
	struct virtio_video_cmd_hdr hdr;
	/* Followed by one of struct virtio_video_control_val_* */
};

/* VIRTIO_VIDEO_CMD_SET_CONTROL */
struct virtio_video_set_control {
	struct virtio_video_cmd_hdr hdr;
	__le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
	__u8 padding[4];
	/* Followed by one of struct virtio_video_control_val_* */
};

struct virtio_video_set_control_resp {
	struct virtio_video_cmd_hdr hdr;
};

/*
 * V4L2 controls (encoder only, VIRTIO_VIDEO_F_V4L2_CONTROLS)
 *
 * Controls cross the wire as V4L2 control IDs and V4L2 menu indices, which
 * are stable UAPI. The host lists every control it supports with its range,
 * and the driver registers exactly that list and forwards values unchanged,
 * so a capability the host learns needs no driver change. The driver skips a
 * descriptor whose ID it does not know.
 */

/* VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS; hdr.stream_id is 0 (device scope). */
struct virtio_video_query_v4l2_controls {
	struct virtio_video_cmd_hdr hdr;
	__le32 start_index; /* first descriptor to return */
	__u8 padding[4];
};

enum virtio_video_v4l2_control_flag {
	/* May change while the stream is encoding (otherwise the driver
	   grabs it while streaming). */
	VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC = 1 << 0,
	VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY = 1 << 1,
	/* Read from the host on every get. */
	VIRTIO_VIDEO_V4L2_CTRL_F_VOLATILE = 1 << 2,
};

struct virtio_video_v4l2_control_desc {
	__le32 id; /* V4L2_CID_* */
	__le32 type; /* V4L2_CTRL_TYPE_* */
	__le64 minimum; /* two's-complement s64 */
	__le64 maximum;
	__le64 step;
	__le64 default_value;
	/* Menu types: bit n set means menu index n is unsupported, as the
	   skip mask of v4l2_ctrl_new_std_menu(). Zero otherwise. */
	__le64 menu_skip_mask;
	__le32 flags; /* Bitmask with VIRTIO_VIDEO_V4L2_CTRL_F_* */
	__le32 elem_size; /* Compound types: payload bytes. Zero otherwise. */
	/* Bit (format - VIRTIO_VIDEO_FORMAT_CODED_MIN) set for each coded
	   format the control applies to; informational. */
	__le64 coded_formats;
};

struct virtio_video_query_v4l2_controls_resp {
	struct virtio_video_cmd_hdr hdr;
	__le32 num_descs; /* descriptors in this response */
	__le32 next_index; /* start_index of the next query; 0 when complete */
	/* Followed by struct virtio_video_v4l2_control_desc descs[num_descs] */
};

/*
 * VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS / VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS
 *
 * A set applies every value or none, as VIDIOC_S_EXT_CTRLS does. A compound
 * value (elem_size != 0) is carried in the payload area that follows the
 * value array: size is its length and value its byte offset in that area,
 * a multiple of 8. A get request names the controls with size and value
 * zero; the response returns them in the same layout.
 */
struct virtio_video_v4l2_control_value {
	__le32 id; /* V4L2_CID_* */
	__le32 size; /* 0 for a scalar */
	__le64 value; /* scalar value, or payload offset */
};

struct virtio_video_v4l2_controls {
	struct virtio_video_cmd_hdr hdr;
	__le32 num_controls;
	__u8 padding[4];
	/* Followed by struct virtio_video_v4l2_control_value controls[]
	   and then the payload area */
};

struct virtio_video_v4l2_controls_resp {
	struct virtio_video_cmd_hdr hdr;
	/* num_controls when every control succeeded, else the index of the
	   first one that failed */
	__le32 error_index;
	__le32 num_controls;
	/* GET: followed by the values and payload area, as in the request */
};

/*
 * VIRTIO_VIDEO_CMD_SET_STREAM_INFO / VIRTIO_VIDEO_CMD_GET_STREAM_INFO
 *
 * The fractional frame interval (VIDIOC_S_PARM) and the raw frames'
 * colorimetry (the colorspace fields of VIDIOC_S_FMT on V4L2 OUTPUT), in
 * V4L2 enum values. The coded side reports the colorimetry it writes.
 */
struct virtio_video_stream_info {
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__le32 frame_interval_numerator;
	__le32 frame_interval_denominator;
	__le32 colorspace; /* enum v4l2_colorspace */
	__le32 xfer_func; /* enum v4l2_xfer_func */
	__le32 ycbcr_enc; /* enum v4l2_ycbcr_encoding */
	__le32 quantization; /* enum v4l2_quantization */
	__u8 padding[4];
};

struct virtio_video_set_stream_info {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_stream_info info;
};

struct virtio_video_get_stream_info {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

struct virtio_video_get_stream_info_resp {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_stream_info info;
};

/*
 * Events
 */

enum virtio_video_event_type {
	/* For all devices */
	VIRTIO_VIDEO_EVENT_ERROR = 0x0100,

	/* For decoder only */
	VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED = 0x0200,
};

struct virtio_video_event {
	__le32 event_type; /* One of VIRTIO_VIDEO_EVENT_* types */
	__le32 stream_id;
};

#endif /* _UAPI_LINUX_VIRTIO_VIDEO_H */
