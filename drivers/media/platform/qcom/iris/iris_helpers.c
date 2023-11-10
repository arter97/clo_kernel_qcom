// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/pm_runtime.h>

#include "hfi_defines.h"
#include "iris_core.h"
#include "iris_helpers.h"
#include "iris_hfi.h"
#include "iris_hfi_packet.h"
#include "iris_instance.h"
#include "iris_power.h"
#include "iris_vidc.h"
#include "memory.h"

int check_core_lock(struct iris_core *core)
{
	bool fatal = !mutex_is_locked(&core->lock);

	WARN_ON(fatal);

	return fatal ? -EINVAL : 0;
}

bool res_is_less_than(u32 width, u32 height,
		      u32 ref_width, u32 ref_height)
{
	u32 num_mbs = NUM_MBS_PER_FRAME(height, width);
	u32 max_side = max(ref_width, ref_height);

	if (num_mbs < NUM_MBS_PER_FRAME(ref_height, ref_width) &&
	    width < max_side &&
	    height < max_side)
		return true;

	return false;
}

u32 get_port_info(struct iris_inst *inst,
		  enum plat_inst_cap_type cap_id)
{
	if (inst->cap[cap_id].flags & CAP_FLAG_INPUT_PORT &&
	    inst->cap[cap_id].flags & CAP_FLAG_OUTPUT_PORT) {
		if (inst->vb2q_dst->streaming)
			return get_hfi_port(INPUT_MPLANE);
		else
			return get_hfi_port(OUTPUT_MPLANE);
	}

	if (inst->cap[cap_id].flags & CAP_FLAG_INPUT_PORT)
		return get_hfi_port(INPUT_MPLANE);
	else if (inst->cap[cap_id].flags & CAP_FLAG_OUTPUT_PORT)
		return get_hfi_port(OUTPUT_MPLANE);
	else
		return HFI_PORT_NONE;
}

enum iris_buffer_type v4l2_type_to_driver(u32 type)
{
	switch (type) {
	case INPUT_MPLANE:
		return BUF_INPUT;
	case OUTPUT_MPLANE:
		return BUF_OUTPUT;
	default:
		return 0;
	}
}

u32 v4l2_type_from_driver(enum iris_buffer_type buffer_type)
{
	switch (buffer_type) {
	case BUF_INPUT:
		return INPUT_MPLANE;
	case BUF_OUTPUT:
		return OUTPUT_MPLANE;
	default:
		return 0;
	}
}

int v4l2_to_hfi_enum(struct iris_inst *inst,
		     enum plat_inst_cap_type cap_id, u32 *value)
{
	switch (cap_id) {
	case ROTATION:
		switch (inst->cap[cap_id].value) {
		case 0:
			*value = HFI_ROTATION_NONE;
			break;
		case 90:
			*value = HFI_ROTATION_90;
			break;
		case 180:
			*value = HFI_ROTATION_180;
			break;
		case 270:
			*value = HFI_ROTATION_270;
			break;
		default:
			*value = HFI_ROTATION_NONE;
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

int get_mbpf(struct iris_inst *inst)
{
	int height = 0, width = 0;
	struct v4l2_format *inp_f;

	inp_f = inst->fmt_src;
	width = max(inp_f->fmt.pix_mp.width, inst->crop.width);
	height = max(inp_f->fmt.pix_mp.height, inst->crop.height);

	return NUM_MBS_PER_FRAME(height, width);
}

inline bool is_linear_colorformat(u32 colorformat)
{
	return colorformat == V4L2_PIX_FMT_NV12 || colorformat == V4L2_PIX_FMT_NV21;
}

bool is_split_mode_enabled(struct iris_inst *inst)
{
	if (is_linear_colorformat(inst->fmt_dst->fmt.pix_mp.pixelformat))
		return true;

	return false;
}

inline bool is_10bit_colorformat(enum colorformat_type colorformat)
{
	return colorformat == FMT_TP10C;
}

inline bool is_8bit_colorformat(enum colorformat_type colorformat)
{
	return colorformat == FMT_NV12 ||
		colorformat == FMT_NV12C ||
		colorformat == FMT_NV21;
}

u32 v4l2_codec_from_driver(struct iris_inst *inst, enum codec_type codec)
{
	const struct codec_info *codec_info;
	struct iris_core *core;
	u32 v4l2_codec = 0;
	u32 i, size;

	core = inst->core;
	codec_info = core->platform_data->format_data->codec_info;
	size = core->platform_data->format_data->codec_info_size;

	for (i = 0; i < size; i++) {
		if (codec_info[i].codec == codec)
			return codec_info[i].v4l2_codec;
	}

	return v4l2_codec;
}

enum codec_type v4l2_codec_to_driver(struct iris_inst *inst, u32 v4l2_codec)
{
	const struct codec_info *codec_info;
	enum codec_type codec = 0;
	struct iris_core *core;
	u32 i, size;

	core = inst->core;
	codec_info = core->platform_data->format_data->codec_info;
	size = core->platform_data->format_data->codec_info_size;

	for (i = 0; i < size; i++) {
		if (codec_info[i].v4l2_codec == v4l2_codec)
			return codec_info[i].codec;
	}

	return codec;
}

u32 v4l2_colorformat_from_driver(struct iris_inst *inst, enum colorformat_type colorformat)
{
	const struct color_format_info *color_format_info;
	u32 v4l2_colorformat = 0;
	struct iris_core *core;
	u32 i, size;

	core = inst->core;
	color_format_info = core->platform_data->format_data->color_format_info;
	size = core->platform_data->format_data->color_format_info_size;

	for (i = 0; i < size; i++) {
		if (color_format_info[i].color_format == colorformat)
			return color_format_info[i].v4l2_color_format;
	}

	return v4l2_colorformat;
}

enum colorformat_type v4l2_colorformat_to_driver(struct iris_inst *inst, u32 v4l2_colorformat)
{
	const struct color_format_info *color_format_info;
	enum colorformat_type colorformat = 0;
	struct iris_core *core;
	u32 i, size;

	core = inst->core;
	color_format_info = core->platform_data->format_data->color_format_info;
	size = core->platform_data->format_data->color_format_info_size;

	for (i = 0; i < size; i++) {
		if (color_format_info[i].v4l2_color_format == v4l2_colorformat)
			return color_format_info[i].color_format;
	}

	return colorformat;
}

struct vb2_queue *get_vb2q(struct iris_inst *inst, u32 type)
{
	struct vb2_queue *vb2q = NULL;

	switch (type) {
	case INPUT_MPLANE:
		vb2q = inst->vb2q_src;
		break;
	case OUTPUT_MPLANE:
		vb2q = inst->vb2q_dst;
		break;
	default:
		return NULL;
	}

	return vb2q;
}

static int process_inst_timeout(struct iris_inst *inst)
{
	struct iris_inst *instance;
	struct iris_core *core;
	bool found = false;
	int ret = 0;

	core = inst->core;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		if (instance == inst) {
			found = true;
			break;
		}
	}
	if (!found) {
		ret = -EINVAL;
		goto unlock;
	}

	iris_change_core_state(core, IRIS_CORE_ERROR);

	iris_core_deinit_locked(core);

unlock:
	mutex_unlock(&core->lock);

	return ret;
}

int close_session(struct iris_inst *inst)
{
	u32 hw_response_timeout_val;
	bool wait_for_response;
	struct iris_core *core;
	int ret;

	core = inst->core;
	hw_response_timeout_val = core->cap[HW_RESPONSE_TIMEOUT].value;
	wait_for_response = true;
	ret = iris_hfi_session_close(inst);
	if (ret)
		wait_for_response = false;

	kfree(inst->packet);
	inst->packet = NULL;

	if (wait_for_response) {
		mutex_unlock(&inst->lock);
		ret = wait_for_completion_timeout(&inst->completions[SIGNAL_CMD_CLOSE],
						  msecs_to_jiffies(hw_response_timeout_val));
		if (!ret) {
			ret = -ETIMEDOUT;
			process_inst_timeout(inst);
		}
		mutex_lock(&inst->lock);
	}

	return ret;
}

static int check_core_mbps_mbpf(struct iris_inst *inst)
{
	u32 mbpf = 0, mbps = 0, total_mbpf = 0, total_mbps = 0;
	struct iris_core *core;
	struct iris_inst *instance;
	u32 fps;

	core = inst->core;

	mutex_lock(&core->lock);
	list_for_each_entry(instance, &core->instances, list) {
		fps = inst->cap[QUEUED_RATE].value >> 16;
		mbpf = get_mbpf(inst);
		mbps = mbpf * fps;
		total_mbpf += mbpf;
		total_mbps += mbps;
	}
	mutex_unlock(&core->lock);

	if (total_mbps > core->cap[MAX_MBPS].value ||
	    total_mbpf > core->cap[MAX_MBPF].value)
		return -ENOMEM;

	return 0;
}

static int check_inst_mbpf(struct iris_inst *inst)
{
	u32 mbpf = 0, max_mbpf = 0;

	max_mbpf = inst->cap[MBPF].max;
	mbpf = get_mbpf(inst);
	if (mbpf > max_mbpf)
		return -ENOMEM;

	return 0;
}

static int check_resolution_supported(struct iris_inst *inst)
{
	u32 width = 0, height = 0, min_width, min_height,
		max_width, max_height;

	width = inst->fmt_src->fmt.pix_mp.width;
	height = inst->fmt_src->fmt.pix_mp.height;

	min_width = inst->cap[FRAME_WIDTH].min;
	max_width = inst->cap[FRAME_WIDTH].max;
	min_height = inst->cap[FRAME_HEIGHT].min;
	max_height = inst->cap[FRAME_HEIGHT].max;

	if (!(min_width <= width && width <= max_width) ||
	    !(min_height <= height && height <= max_height))
		return -EINVAL;

	return 0;
}

static int check_max_sessions(struct iris_inst *inst)
{
	struct iris_core *core;
	u32 num_sessions = 0;
	struct iris_inst *i;

	core = inst->core;
	mutex_lock(&core->lock);
	list_for_each_entry(i, &core->instances, list) {
		num_sessions++;
	}
	mutex_unlock(&core->lock);

	if (num_sessions > core->cap[MAX_SESSION_COUNT].value)
		return -ENOMEM;

	return 0;
}

int check_session_supported(struct iris_inst *inst)
{
	int ret;

	ret = check_core_mbps_mbpf(inst);
	if (ret)
		goto exit;

	ret = check_inst_mbpf(inst);
	if (ret)
		goto exit;

	ret = check_resolution_supported(inst);
	if (ret)
		goto exit;

	ret = check_max_sessions(inst);
	if (ret)
		goto exit;

	return ret;
exit:
	dev_err(inst->core->dev, "current session not supported(%d)\n", ret);

	return ret;
}

int signal_session_msg_receipt(struct iris_inst *inst,
			       enum signal_session_response cmd)
{
	if (cmd < MAX_SIGNAL)
		complete(&inst->completions[cmd]);

	return 0;
}

struct iris_inst *to_instance(struct iris_core *core, u32 session_id)
{
	struct iris_inst *inst = NULL;

	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {
		if (inst->session_id == session_id) {
			mutex_unlock(&core->lock);
			return inst;
		}
	}
	mutex_unlock(&core->lock);

	return NULL;
}

static int kill_session(struct iris_inst *inst)
{
	if (!inst->session_id)
		return 0;

	close_session(inst);
	iris_inst_change_state(inst, IRIS_INST_ERROR);

	return 0;
}

struct iris_buffer *get_driver_buf(struct iris_inst *inst, u32 plane, u32 index)
{
	struct iris_buffer *iter = NULL;
	struct iris_buffer *buf = NULL;
	enum iris_buffer_type buf_type;
	struct iris_buffers *buffers;

	bool found = false;

	buf_type = v4l2_type_to_driver(plane);
	if (!buf_type)
		return NULL;

	buffers = iris_get_buffer_list(inst, buf_type);
	if (!buffers)
		return NULL;

	list_for_each_entry(iter, &buffers->list, list) {
		if (iter->index == index) {
			found = true;
			buf = iter;
			break;
		}
	}

	if (!found)
		return NULL;

	return buf;
}

static void process_requeued_readonly_buffers(struct iris_inst *inst,
					      struct iris_buffer *buf)
{
	struct iris_buffer *ro_buf, *dummy;

	list_for_each_entry_safe(ro_buf, dummy, &inst->buffers.read_only.list, list) {
		if (ro_buf->device_addr != buf->device_addr)
			continue;
		if (ro_buf->attr & BUF_ATTR_READ_ONLY &&
		    !(ro_buf->attr & BUF_ATTR_PENDING_RELEASE)) {
			buf->attr |= BUF_ATTR_READ_ONLY;

			list_del_init(&ro_buf->list);
			iris_return_buffer_to_pool(inst, ro_buf);
			break;
		}
	}
}

int queue_buffer(struct iris_inst *inst, struct iris_buffer *buf)
{
	int ret;

	if (buf->type == BUF_OUTPUT)
		process_requeued_readonly_buffers(inst, buf);

	ret = iris_hfi_queue_buffer(inst, buf);
	if (ret)
		return ret;

	buf->attr &= ~BUF_ATTR_DEFERRED;
	buf->attr |= BUF_ATTR_QUEUED;

	return ret;
}

int queue_deferred_buffers(struct iris_inst *inst, enum iris_buffer_type buf_type)
{
	struct iris_buffers *buffers;
	struct iris_buffer *buf;
	int ret = 0;

	buffers = iris_get_buffer_list(inst, buf_type);
	if (!buffers)
		return -EINVAL;

	iris_scale_power(inst);

	list_for_each_entry(buf, &buffers->list, list) {
		if (!(buf->attr & BUF_ATTR_DEFERRED))
			continue;
		ret = queue_buffer(inst, buf);
		if (ret)
			return ret;
	}

	return ret;
}

int iris_release_nonref_buffers(struct iris_inst *inst)
{
	u32 fw_ro_count = 0, nonref_ro_count = 0;
	struct iris_buffer *ro_buf;
	bool found = false;
	int ret = 0;
	int i = 0;

	list_for_each_entry(ro_buf, &inst->buffers.read_only.list, list) {
		if (!(ro_buf->attr & BUF_ATTR_READ_ONLY))
			continue;
		if (ro_buf->attr & BUF_ATTR_PENDING_RELEASE)
			continue;
		fw_ro_count++;
	}

	if (fw_ro_count <= MAX_DPB_COUNT)
		return 0;

	/*
	 * Mark the read only buffers present in read_only list as
	 * non-reference if it's not part of dpb_list_payload.
	 * dpb_list_payload details:
	 * payload[0-1]           : 64 bits base_address of DPB-1
	 * payload[2]             : 32 bits addr_offset  of DPB-1
	 * payload[3]             : 32 bits data_offset  of DPB-1
	 */
	list_for_each_entry(ro_buf, &inst->buffers.read_only.list, list) {
		found = false;
		if (!(ro_buf->attr & BUF_ATTR_READ_ONLY))
			continue;
		if (ro_buf->attr & BUF_ATTR_PENDING_RELEASE)
			continue;
		for (i = 0; (i + 3) < MAX_DPB_LIST_ARRAY_SIZE; i = i + 4) {
			if (ro_buf->device_addr == inst->dpb_list_payload[i] &&
			    ro_buf->data_offset == inst->dpb_list_payload[i + 3]) {
				found = true;
				break;
			}
		}
		if (!found)
			nonref_ro_count++;
	}

	if (nonref_ro_count <= inst->buffers.output.min_count)
		return 0;

	list_for_each_entry(ro_buf, &inst->buffers.read_only.list, list) {
		found = false;
		if (!(ro_buf->attr & BUF_ATTR_READ_ONLY))
			continue;
		if (ro_buf->attr & BUF_ATTR_PENDING_RELEASE)
			continue;
		for (i = 0; (i + 3) < MAX_DPB_LIST_ARRAY_SIZE; i = i + 4) {
			if (ro_buf->device_addr == inst->dpb_list_payload[i] &&
			    ro_buf->data_offset == inst->dpb_list_payload[i + 3]) {
				found = true;
				break;
			}
		}
		if (!found) {
			ro_buf->attr |= BUF_ATTR_PENDING_RELEASE;
			ret = iris_hfi_release_buffer(inst, ro_buf);
			if (ret)
				return ret;
		}
	}

	return ret;
}

int iris_vb2_buffer_done(struct iris_inst *inst,
			 struct iris_buffer *buf)
{
	struct vb2_v4l2_buffer *vbuf;
	struct vb2_queue *q = NULL;
	struct vb2_buffer *iter;
	struct vb2_buffer *vb2;
	int type, state;
	bool found;

	type = v4l2_type_from_driver(buf->type);
	if (!type)
		return -EINVAL;

	if (type == INPUT_MPLANE)
		q = inst->vb2q_src;
	else if (type == OUTPUT_MPLANE)
		q = inst->vb2q_dst;
	if (!q || !q->streaming)
		return -EINVAL;

	found = false;
	list_for_each_entry(iter, &q->queued_list, queued_entry) {
		if (iter->state != VB2_BUF_STATE_ACTIVE)
			continue;
		if (iter->index == buf->index) {
			found = true;
			vb2 = iter;
			break;
		}
	}
	if (!found)
		return -EINVAL;

	if (buf->flags & BUF_FLAG_ERROR)
		state = VB2_BUF_STATE_ERROR;
	else
		state = VB2_BUF_STATE_DONE;

	vbuf = to_vb2_v4l2_buffer(vb2);
	vbuf->flags = buf->flags;
	vb2->timestamp = buf->timestamp;
	vb2->planes[0].bytesused = buf->data_size + vb2->planes[0].data_offset;
	vb2_buffer_done(vb2, state);

	return 0;
}

static int iris_flush_deferred_buffers(struct iris_inst *inst,
				       enum iris_buffer_type type)
{
	struct iris_buffer *buf, *dummy;
	struct iris_buffers *buffers;

	buffers = iris_get_buffer_list(inst, type);
	if (!buffers)
		return -EINVAL;

	list_for_each_entry_safe(buf, dummy, &buffers->list, list) {
		if (buf->attr & BUF_ATTR_DEFERRED) {
			if (!(buf->attr & BUF_ATTR_BUFFER_DONE)) {
				buf->attr |= BUF_ATTR_BUFFER_DONE;
				buf->data_size = 0;
				iris_vb2_buffer_done(inst, buf);
			}
		}
	}

	return 0;
}

static int iris_flush_read_only_buffers(struct iris_inst *inst,
					enum iris_buffer_type type)
{
	struct iris_buffer *ro_buf, *dummy;

	if (type != BUF_OUTPUT)
		return 0;

	list_for_each_entry_safe(ro_buf, dummy, &inst->buffers.read_only.list, list) {
		if (ro_buf->attr & BUF_ATTR_READ_ONLY)
			continue;
		if (ro_buf->attach && ro_buf->sg_table)
			dma_buf_unmap_attachment(ro_buf->attach, ro_buf->sg_table,
						 DMA_BIDIRECTIONAL);
		if (ro_buf->attach && ro_buf->dmabuf)
			dma_buf_detach(ro_buf->dmabuf, ro_buf->attach);
		ro_buf->attach = NULL;
		ro_buf->sg_table = NULL;
		ro_buf->dmabuf = NULL;
		ro_buf->device_addr = 0x0;
		list_del_init(&ro_buf->list);
		iris_return_buffer_to_pool(inst, ro_buf);
	}

	return 0;
}

void iris_destroy_buffers(struct iris_inst *inst)
{
	struct iris_buffer *buf, *dummy;
	struct iris_buffers *buffers;

	static const enum iris_buffer_type ext_buf_types[] = {
		BUF_INPUT,
		BUF_OUTPUT,
	};
	static const enum iris_buffer_type internal_buf_types[] = {
		BUF_BIN,
		BUF_COMV,
		BUF_NON_COMV,
		BUF_LINE,
		BUF_DPB,
		BUF_PERSIST,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(internal_buf_types); i++) {
		buffers = iris_get_buffer_list(inst, internal_buf_types[i]);
		if (!buffers)
			continue;
		list_for_each_entry_safe(buf, dummy, &buffers->list, list)
			iris_destroy_internal_buffer(inst, buf);
	}

	list_for_each_entry_safe(buf, dummy, &inst->buffers.read_only.list, list) {
		if (buf->attach && buf->sg_table)
			dma_buf_unmap_attachment(buf->attach, buf->sg_table, DMA_BIDIRECTIONAL);
		if (buf->attach && buf->dmabuf)
			dma_buf_detach(buf->dmabuf, buf->attach);
		list_del_init(&buf->list);
		iris_return_buffer_to_pool(inst, buf);
	}

	for (i = 0; i < ARRAY_SIZE(ext_buf_types); i++) {
		buffers = iris_get_buffer_list(inst, ext_buf_types[i]);
		if (!buffers)
			continue;
		list_for_each_entry_safe(buf, dummy, &buffers->list, list) {
			if (buf->attach && buf->sg_table)
				dma_buf_unmap_attachment(buf->attach, buf->sg_table,
							 DMA_BIDIRECTIONAL);
			if (buf->attach && buf->dmabuf)
				dma_buf_detach(buf->dmabuf, buf->attach);
			list_del_init(&buf->list);
			iris_return_buffer_to_pool(inst, buf);
		}
	}

	iris_mem_pool_deinit(inst);
}

static int get_num_queued_buffers(struct iris_inst *inst,
				  enum iris_buffer_type type)
{
	struct iris_buffers *buffers;
	struct iris_buffer *vbuf;
	int count = 0;

	if (type == BUF_INPUT)
		buffers = &inst->buffers.input;
	else if (type == BUF_OUTPUT)
		buffers = &inst->buffers.output;
	else
		return count;

	list_for_each_entry(vbuf, &buffers->list, list) {
		if (vbuf->type != type)
			continue;
		if (!(vbuf->attr & BUF_ATTR_QUEUED))
			continue;
		count++;
	}

	return count;
}

int session_streamoff(struct iris_inst *inst, u32 plane)
{
	enum signal_session_response signal_type;
	enum iris_buffer_type buffer_type;
	u32 hw_response_timeout_val;
	struct iris_core *core;
	int count = 0;
	int ret;

	if (plane == INPUT_MPLANE) {
		signal_type = SIGNAL_CMD_STOP_INPUT;
		buffer_type = BUF_INPUT;
	} else if (plane == OUTPUT_MPLANE) {
		signal_type = SIGNAL_CMD_STOP_OUTPUT;
		buffer_type = BUF_OUTPUT;
	} else {
		return -EINVAL;
	}

	ret = iris_hfi_stop(inst, plane);
	if (ret)
		goto error;

	core = inst->core;
	hw_response_timeout_val = core->cap[HW_RESPONSE_TIMEOUT].value;
	mutex_unlock(&inst->lock);
	ret = wait_for_completion_timeout(&inst->completions[signal_type],
					  msecs_to_jiffies(hw_response_timeout_val));
	if (!ret) {
		ret = -ETIMEDOUT;
		process_inst_timeout(inst);
	} else {
		ret = 0;
	}
	mutex_lock(&inst->lock);

	if (ret)
		goto error;

	if (plane == INPUT_MPLANE)
		iris_flush_input_timer(inst);

	/* no more queued buffers after streamoff */
	count = get_num_queued_buffers(inst, buffer_type);
	if (count) {
		ret = -EINVAL;
		goto error;
	}

	ret = iris_inst_state_change_streamoff(inst, plane);
	if (ret)
		goto error;

	iris_flush_deferred_buffers(inst, buffer_type);
	iris_flush_read_only_buffers(inst, buffer_type);
	return 0;

error:
	kill_session(inst);
	iris_flush_deferred_buffers(inst, buffer_type);
	iris_flush_read_only_buffers(inst, buffer_type);

	return ret;
}

int iris_pm_get(struct iris_core *core)
{
	int ret;

	mutex_lock(&core->pm_lock);
	ret = pm_runtime_resume_and_get(core->dev);
	mutex_unlock(&core->pm_lock);

	return ret;
}

int iris_pm_put(struct iris_core *core, bool autosuspend)
{
	int ret;

	mutex_lock(&core->pm_lock);

	if (autosuspend)
		ret = pm_runtime_put_autosuspend(core->dev);
	else
		ret = pm_runtime_put_sync(core->dev);
	if (ret > 0)
		ret = 0;

	mutex_unlock(&core->pm_lock);

	return ret;
}

int iris_pm_get_put(struct iris_core *core)
{
	int ret = 0;

	mutex_lock(&core->pm_lock);

	if (pm_runtime_suspended(core->dev)) {
		ret = pm_runtime_resume_and_get(core->dev);
		if (ret < 0)
			goto error;

		ret = pm_runtime_put_autosuspend(core->dev);
	}
	if (ret > 0)
		ret = 0;

error:
	mutex_unlock(&core->pm_lock);

	return ret;
}

void iris_pm_touch(struct iris_core *core)
{
	mutex_lock(&core->pm_lock);
	pm_runtime_mark_last_busy(core->dev);
	mutex_unlock(&core->pm_lock);
}
