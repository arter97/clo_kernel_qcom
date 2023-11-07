// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/videodev2.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "iris_buffer.h"
#include "iris_common.h"
#include "iris_helpers.h"
#include "iris_hfi.h"
#include "iris_instance.h"
#include "iris_vdec.h"
#include "iris_vidc.h"
#include "iris_ctrls.h"
#include "iris_vb2.h"
#include "memory.h"

#define VIDC_DRV_NAME "iris_driver"
#define VIDC_BUS_NAME "platform:iris_bus"

static int vidc_v4l2_fh_init(struct iris_inst *inst)
{
	struct iris_core *core;

	core = inst->core;

	if (inst->fh.vdev)
		return -EINVAL;

	v4l2_fh_init(&inst->fh, core->vdev_dec);
	inst->fh.ctrl_handler = &inst->ctrl_handler;
	v4l2_fh_add(&inst->fh);

	return 0;
}

static int vidc_v4l2_fh_deinit(struct iris_inst *inst)
{
	if (!inst->fh.vdev)
		return 0;

	v4l2_fh_del(&inst->fh);
	inst->fh.ctrl_handler = NULL;
	v4l2_fh_exit(&inst->fh);

	return 0;
}

static int vb2q_init(struct iris_inst *inst,
		     struct vb2_queue *q, enum v4l2_buf_type type)
{
	struct iris_core *core;

	core = inst->core;

	q->lock = &inst->ctx_q_lock;
	q->type = type;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->ops = core->vb2_ops;
	q->mem_ops = core->vb2_mem_ops;
	q->drv_priv = inst;
	q->copy_timestamp = 1;
	q->min_buffers_needed = 0;
	return vb2_queue_init(q);
}

static int vidc_vb2_queue_init(struct iris_inst *inst)
{
	int ret;

	ret = vb2q_init(inst, inst->vb2q_src, INPUT_MPLANE);
	if (ret)
		return ret;

	ret = vb2q_init(inst, inst->vb2q_dst, OUTPUT_MPLANE);
	if (ret)
		goto fail_vb2q_src_deinit;

	return ret;

fail_vb2q_src_deinit:
	vb2_queue_release(inst->vb2q_src);

	return ret;
}

static int vidc_vb2_queue_deinit(struct iris_inst *inst)
{
	vb2_queue_release(inst->vb2q_src);
	kfree(inst->vb2q_src);
	inst->vb2q_src = NULL;

	vb2_queue_release(inst->vb2q_dst);
	kfree(inst->vb2q_dst);
	inst->vb2q_dst = NULL;

	return 0;
}

static int vidc_add_session(struct iris_inst *inst)
{
	struct iris_core *core;
	struct iris_inst *i;
	u32 count = 0;
	int ret = 0;

	core = inst->core;

	mutex_lock(&core->lock);
	if (core->state != IRIS_CORE_INIT) {
		ret = -EINVAL;
		goto unlock;
	}
	list_for_each_entry(i, &core->instances, list)
		count++;

	if (count < core->cap[MAX_SESSION_COUNT].value)
		list_add_tail(&inst->list, &core->instances);
	else
		ret = -EAGAIN;
unlock:
	mutex_unlock(&core->lock);

	return ret;
}

static int vidc_remove_session(struct iris_inst *inst)
{
	struct iris_inst *i, *temp;
	struct iris_core *core;

	core = inst->core;

	mutex_lock(&core->lock);
	list_for_each_entry_safe(i, temp, &core->instances, list) {
		if (i->session_id == inst->session_id) {
			list_del_init(&i->list);
			break;
		}
	}
	mutex_unlock(&core->lock);

	return 0;
}

static struct iris_inst *get_vidc_inst(struct file *filp, void *fh)
{
	if (!filp || !filp->private_data)
		return NULL;

	return container_of(filp->private_data,
					struct iris_inst, fh);
}

int vidc_open(struct file *filp)
{
	struct iris_core *core = video_drvdata(filp);
	struct iris_inst *inst = NULL;
	int i = 0;
	int ret;

	ret = iris_core_init(core);
	if (ret)
		return ret;

	ret = iris_core_init_wait(core);
	if (ret)
		return ret;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->core = core;
	inst->session_id = hash32_ptr(inst);
	iris_inst_change_state(inst, IRIS_INST_OPEN);
	mutex_init(&inst->lock);
	mutex_init(&inst->ctx_q_lock);

	ret = vidc_add_session(inst);
	if (ret)
		goto fail_free_inst;

	ret = iris_mem_pool_init(inst);
	if (ret)
		goto fail_remove_session;

	INIT_LIST_HEAD(&inst->buffers.input.list);
	INIT_LIST_HEAD(&inst->buffers.output.list);
	INIT_LIST_HEAD(&inst->buffers.read_only.list);
	INIT_LIST_HEAD(&inst->buffers.bin.list);
	INIT_LIST_HEAD(&inst->buffers.arp.list);
	INIT_LIST_HEAD(&inst->buffers.comv.list);
	INIT_LIST_HEAD(&inst->buffers.non_comv.list);
	INIT_LIST_HEAD(&inst->buffers.line.list);
	INIT_LIST_HEAD(&inst->buffers.dpb.list);
	INIT_LIST_HEAD(&inst->buffers.persist.list);
	INIT_LIST_HEAD(&inst->buffers.vpss.list);
	INIT_LIST_HEAD(&inst->caps_list);
	for (i = 0; i < MAX_SIGNAL; i++)
		init_completion(&inst->completions[i]);

	ret = vidc_v4l2_fh_init(inst);
	if (ret)
		goto fail_mem_pool_deinit;

	ret = vdec_inst_init(inst);
	if (ret)
		goto fail_fh_deinit;

	ret = vidc_vb2_queue_init(inst);
	if (ret)
		goto fail_inst_deinit;

	ret = iris_hfi_session_open(inst);
	if (ret) {
		dev_err(core->dev, "%s: session open failed\n", __func__);
		goto fail_core_deinit;
	}
	filp->private_data = &inst->fh;

	return 0;

fail_core_deinit:
	v4l2_ctrl_handler_free(&inst->ctrl_handler);
	iris_core_deinit(core);
	vidc_vb2_queue_deinit(inst);
fail_inst_deinit:
	vdec_inst_deinit(inst);
fail_fh_deinit:
	vidc_v4l2_fh_deinit(inst);
fail_mem_pool_deinit:
	iris_mem_pool_deinit(inst);
fail_remove_session:
	vidc_remove_session(inst);
fail_free_inst:
	mutex_destroy(&inst->ctx_q_lock);
	mutex_destroy(&inst->lock);
	kfree(inst);

	return ret;
}

int vidc_close(struct file *filp)
{
	struct iris_inst *inst;

	inst = get_vidc_inst(filp, NULL);
	if (!inst)
		return -EINVAL;

	v4l2_ctrl_handler_free(&inst->ctrl_handler);
	vdec_inst_deinit(inst);
	mutex_lock(&inst->lock);
	close_session(inst);
	iris_inst_change_state(inst, IRIS_INST_CLOSE);
	vidc_vb2_queue_deinit(inst);
	vidc_v4l2_fh_deinit(inst);
	vidc_remove_session(inst);
	mutex_unlock(&inst->lock);
	mutex_destroy(&inst->ctx_q_lock);
	mutex_destroy(&inst->lock);
	kfree(inst);
	filp->private_data = NULL;

	return 0;
}

static __poll_t get_poll_flags(struct iris_inst *inst, u32 plane)
{
	struct vb2_buffer *vb = NULL;
	struct vb2_queue *q = NULL;
	unsigned long flags = 0;
	__poll_t poll = 0;

	if (plane == INPUT_MPLANE)
		q = inst->vb2q_src;
	else if (plane == OUTPUT_MPLANE)
		q = inst->vb2q_dst;

	if (!q)
		return EPOLLERR;

	spin_lock_irqsave(&q->done_lock, flags);
	if (!list_empty(&q->done_list))
		vb = list_first_entry(&q->done_list, struct vb2_buffer,
				      done_entry);
	if (vb && (vb->state == VB2_BUF_STATE_DONE ||
		   vb->state == VB2_BUF_STATE_ERROR)) {
		if (plane == OUTPUT_MPLANE)
			poll |= EPOLLIN | EPOLLRDNORM;
		else if (plane == INPUT_MPLANE)
			poll |= EPOLLOUT | EPOLLWRNORM;
	}
	spin_unlock_irqrestore(&q->done_lock, flags);

	return poll;
}

static __poll_t vidc_poll(struct file *filp, struct poll_table_struct *pt)
{
	struct iris_inst *inst;
	__poll_t poll = 0;

	inst = get_vidc_inst(filp, NULL);
	if (!inst)
		return EPOLLERR;

	if (IS_SESSION_ERROR(inst))
		return EPOLLERR;

	poll_wait(filp, &inst->fh.wait, pt);
	poll_wait(filp, &inst->vb2q_src->done_wq, pt);
	poll_wait(filp, &inst->vb2q_dst->done_wq, pt);

	if (v4l2_event_pending(&inst->fh))
		poll |= EPOLLPRI;

	poll |= get_poll_flags(inst, INPUT_MPLANE);
	poll |= get_poll_flags(inst, OUTPUT_MPLANE);

	return poll;
}

static int vidc_enum_fmt(struct file *filp, void *fh, struct v4l2_fmtdesc *f)
{
	struct iris_inst *inst;
	int ret;

	inst = get_vidc_inst(filp, fh);
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = vdec_enum_fmt(inst, f);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_try_fmt(struct file *filp, void *fh, struct v4l2_format *f)
{
	struct iris_inst *inst;
	int ret;

	inst = get_vidc_inst(filp, fh);
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (!allow_s_fmt(inst, f->type)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = vdec_try_fmt(inst, f);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_s_fmt(struct file *filp, void *fh, struct v4l2_format *f)
{
	struct iris_inst *inst;
	int ret;

	inst = get_vidc_inst(filp, fh);
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (!allow_s_fmt(inst, f->type)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = vdec_s_fmt(inst, f);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_g_fmt(struct file *filp, void *fh, struct v4l2_format *f)
{
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (f->type == INPUT_MPLANE)
		memcpy(f, inst->fmt_src, sizeof(*f));
	else if (f->type == OUTPUT_MPLANE)
		memcpy(f, inst->fmt_dst, sizeof(*f));

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_enum_framesizes(struct file *filp, void *fh,
				struct v4l2_frmsizeenum *fsize)
{
	enum colorformat_type colorfmt;
	struct iris_inst *inst;
	enum codec_type codec;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !fsize)
		return -EINVAL;

	if (fsize->index)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	codec = v4l2_codec_to_driver(inst, fsize->pixel_format);
	if (!codec) {
		colorfmt = v4l2_colorformat_to_driver(inst, fsize->pixel_format);
		if (colorfmt == FMT_NONE) {
			ret = -EINVAL;
			goto unlock;
		}
	}

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = inst->cap[FRAME_WIDTH].min;
	fsize->stepwise.max_width = inst->cap[FRAME_WIDTH].max;
	fsize->stepwise.step_width = inst->cap[FRAME_WIDTH].step_or_mask;
	fsize->stepwise.min_height = inst->cap[FRAME_HEIGHT].min;
	fsize->stepwise.max_height = inst->cap[FRAME_HEIGHT].max;
	fsize->stepwise.step_height = inst->cap[FRAME_HEIGHT].step_or_mask;

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_reqbufs(struct file *filp, void *fh, struct v4l2_requestbuffers *b)
{
	struct vb2_queue *vb2q = NULL;
	struct iris_inst *inst;
	int ret;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !b)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (!allow_reqbufs(inst, b->type)) {
		ret = -EBUSY;
		goto unlock;
	}

	vb2q = get_vb2q(inst, b->type);
	if (!vb2q) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = vb2_reqbufs(vb2q, b);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_querybuf(struct file *filp, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *vb2q = NULL;
	struct iris_inst *inst;
	int ret;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !b)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	vb2q = get_vb2q(inst, b->type);
	if (!vb2q) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = vb2_querybuf(vb2q, b);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_create_bufs(struct file *filp, void *fh, struct v4l2_create_buffers *b)
{
	struct iris_inst *inst;
	struct vb2_queue *vb2q;
	struct v4l2_format *f;
	int ret;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !b)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	f = &b->format;
	vb2q = get_vb2q(inst, f->type);
	if (!vb2q) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = vb2_create_bufs(vb2q, b);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_prepare_buf(struct file *filp, void *fh, struct v4l2_buffer *b)
{
	struct video_device *vdev;
	struct iris_inst *inst;
	struct vb2_queue *vb2q;
	int ret;

	inst = get_vidc_inst(filp, fh);
	vdev = video_devdata(filp);
	if (!inst || !vdev || !vdev->v4l2_dev->mdev)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	vb2q = get_vb2q(inst, b->type);
	if (!vb2q) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = vb2_prepare_buf(vb2q, vdev->v4l2_dev->mdev, b);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_qbuf(struct file *filp, void *fh, struct v4l2_buffer *b)
{
	struct video_device *vdev;
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	vdev = video_devdata(filp);
	if (!inst || !b)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (b->type == INPUT_MPLANE)
		ret = vb2_qbuf(inst->vb2q_src, vdev->v4l2_dev->mdev, b);
	else if (b->type == OUTPUT_MPLANE)
		ret = vb2_qbuf(inst->vb2q_dst, vdev->v4l2_dev->mdev, b);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_dqbuf(struct file *filp, void *fh, struct v4l2_buffer *b)
{
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !b)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EINVAL;
		goto unlock;
	}

	if (b->type == INPUT_MPLANE)
		ret = vb2_dqbuf(inst->vb2q_src, b, true);
	else if (b->type == OUTPUT_MPLANE)
		ret = vb2_dqbuf(inst->vb2q_dst, b, true);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_streamon(struct file *filp, void *fh, enum v4l2_buf_type type)
{
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!allow_streamon(inst, type)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (type == INPUT_MPLANE)
		ret = vb2_streamon(inst->vb2q_src, type);
	else if (type == OUTPUT_MPLANE)
		ret = vb2_streamon(inst->vb2q_dst, type);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_streamoff(struct file *filp, void *fh, enum v4l2_buf_type type)
{
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EINVAL;
		goto unlock;
	}

	if (!allow_streamoff(inst, type)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (type == INPUT_MPLANE)
		ret = vb2_streamoff(inst->vb2q_src, type);
	else if (type == OUTPUT_MPLANE)
		ret = vb2_streamoff(inst->vb2q_dst, type);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_querycap(struct file *filp, void *fh, struct v4l2_capability *cap)
{
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	strscpy(cap->driver, VIDC_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->bus_info, VIDC_BUS_NAME, sizeof(cap->bus_info));
	memset(cap->reserved, 0, sizeof(cap->reserved));
	strscpy(cap->card, "iris_decoder", sizeof(cap->card));

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_queryctrl(struct file *filp, void *fh, struct v4l2_queryctrl *q_ctrl)
{
	struct v4l2_ctrl *ctrl;
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !q_ctrl)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	ctrl = v4l2_ctrl_find(&inst->ctrl_handler, q_ctrl->id);
	if (!ctrl) {
		ret = -EINVAL;
		goto unlock;
	}

	q_ctrl->minimum = ctrl->minimum;
	q_ctrl->maximum = ctrl->maximum;
	q_ctrl->default_value = ctrl->default_value;
	q_ctrl->flags = 0;
	q_ctrl->step = ctrl->step;

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_querymenu(struct file *filp, void *fh, struct v4l2_querymenu *qmenu)
{
	struct v4l2_ctrl *ctrl;
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !qmenu)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	ctrl = v4l2_ctrl_find(&inst->ctrl_handler, qmenu->id);
	if (!ctrl) {
		ret = -EINVAL;
		goto unlock;
	}

	if (ctrl->type != V4L2_CTRL_TYPE_MENU) {
		ret = -EINVAL;
		goto unlock;
	}

	if (qmenu->index < ctrl->minimum || qmenu->index > ctrl->maximum) {
		ret = -EINVAL;
		goto unlock;
	}

	if (ctrl->menu_skip_mask & (1 << qmenu->index)) {
		ret = -EINVAL;
		goto unlock;
	}

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	struct iris_inst *inst;
	int ret;

	inst = container_of(fh, struct iris_inst, fh);

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = vdec_subscribe_event(inst, sub);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_unsubscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	struct iris_inst *inst;
	int ret;

	inst = container_of(fh, struct iris_inst, fh);

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = v4l2_event_unsubscribe(&inst->fh, sub);

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static int vidc_g_selection(struct file *filp, void *fh, struct v4l2_selection *s)
{
	struct iris_inst *inst;
	int ret = 0;

	inst = get_vidc_inst(filp, fh);
	if (!inst || !s)
		return -EINVAL;

	mutex_lock(&inst->lock);
	if (IS_SESSION_ERROR(inst)) {
		ret = -EBUSY;
		goto unlock;
	}

	if (s->type != OUTPUT_MPLANE && s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		ret = -EINVAL;
		goto unlock;
	}

	switch (s->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE:
		s->r.left = inst->crop.left;
		s->r.top = inst->crop.top;
		s->r.width = inst->crop.width;
		s->r.height = inst->crop.height;
		break;
	default:
		ret = -EINVAL;
	}

unlock:
	mutex_unlock(&inst->lock);

	return ret;
}

static const struct v4l2_file_operations v4l2_file_ops = {
	.owner                          = THIS_MODULE,
	.open                           = vidc_open,
	.release                        = vidc_close,
	.unlocked_ioctl                 = video_ioctl2,
	.poll                           = vidc_poll,
};

static const struct vb2_ops iris_vb2_ops = {
	.queue_setup                    = iris_vb2_queue_setup,
};

static struct vb2_mem_ops iris_vb2_mem_ops = {
	.alloc                          = iris_vb2_alloc,
	.put                            = iris_vb2_put,
	.mmap                           = iris_vb2_mmap,
	.attach_dmabuf                  = iris_vb2_attach_dmabuf,
	.detach_dmabuf                  = iris_vb2_detach_dmabuf,
	.map_dmabuf                     = iris_vb2_map_dmabuf,
	.unmap_dmabuf                   = iris_vb2_unmap_dmabuf,
};

static const struct v4l2_ioctl_ops v4l2_ioctl_ops = {
	.vidioc_enum_fmt_vid_cap        = vidc_enum_fmt,
	.vidioc_enum_fmt_vid_out        = vidc_enum_fmt,
	.vidioc_try_fmt_vid_cap_mplane  = vidc_try_fmt,
	.vidioc_try_fmt_vid_out_mplane  = vidc_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane    = vidc_s_fmt,
	.vidioc_s_fmt_vid_out_mplane    = vidc_s_fmt,
	.vidioc_g_fmt_vid_cap_mplane    = vidc_g_fmt,
	.vidioc_g_fmt_vid_out_mplane    = vidc_g_fmt,
	.vidioc_enum_framesizes         = vidc_enum_framesizes,
	.vidioc_reqbufs                 = vidc_reqbufs,
	.vidioc_querybuf                = vidc_querybuf,
	.vidioc_create_bufs             = vidc_create_bufs,
	.vidioc_prepare_buf             = vidc_prepare_buf,
	.vidioc_qbuf                    = vidc_qbuf,
	.vidioc_dqbuf                   = vidc_dqbuf,
	.vidioc_streamon                = vidc_streamon,
	.vidioc_streamoff               = vidc_streamoff,
	.vidioc_querycap                = vidc_querycap,
	.vidioc_queryctrl               = vidc_queryctrl,
	.vidioc_querymenu               = vidc_querymenu,
	.vidioc_subscribe_event         = vidc_subscribe_event,
	.vidioc_unsubscribe_event       = vidc_unsubscribe_event,
	.vidioc_g_selection             = vidc_g_selection,
};

int init_ops(struct iris_core *core)
{
	core->v4l2_file_ops = &v4l2_file_ops;
	core->vb2_ops = &iris_vb2_ops;
	core->vb2_mem_ops = &iris_vb2_mem_ops;
	core->v4l2_ioctl_ops = &v4l2_ioctl_ops;

	return 0;
}
