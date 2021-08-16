// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Endpoint Function Driver for MHI device
 *
 * Copyright (C) 2021 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-direction.h>
#include <linux/io.h>
#include <linux/mhi_ep.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci_ids.h>
#include <linux/random.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>

#include "internal.h"

#define MHI_EP_BAR_NUM			0
#define MHI_EP_MSI_COUNT		4
#define MHI_EP_VERSION			0x1000000

/* Wait time on the device for Host to set M0 state */
#define MHI_EP_M0_MAX_CNT		30
/* Wait time before suspend/resume is complete */
#define MHI_SUSPEND_MIN			100
#define MHI_SUSPEND_TIMEOUT		600
/* Wait time on the device for Host to set BHI_INTVEC */
#define MHI_BHI_INTVEC_MAX_CNT			200
#define MHI_BHI_INTVEC_WAIT_MS		50
#define MHI_MASK_CH_EV_LEN		32
#define MHI_RING_CMD_ID			0

#define MHI_MMIO_CTRL_INT_STATUS_A7_MSK	0x1
#define MHI_MMIO_CTRL_CRDB_STATUS_MSK	0x2

#define HOST_ADDR(lsb, msb)		((lsb) | ((u64)(msb) << 32))

int mhi_create_device(struct mhi_ep_cntrl *mhi_cntrl, u32 ch_id);

int mhi_ep_send_event(struct mhi_ep_cntrl *mhi_cntrl, u32 evnt_ring,
					union mhi_ep_ring_element_type *el)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_ring *ring = &mhi_cntrl->mhi_event[evnt_ring].ring;
	union mhi_ep_ring_ctx *ctx;
	int ret = 0;
	unsigned long flags;

	mutex_lock(&mhi_cntrl->event_lock);

	ctx = (union mhi_ep_ring_ctx *)&mhi_cntrl->ev_ctx_cache[evnt_ring];
	if (ring->state == RING_STATE_UINT) {
		dev_dbg(dev, "ring (%d) init!!!", ring->type);
		ret = mhi_ep_ring_start(mhi_cntrl, ring, ctx);
		if (ret) {
			dev_err(dev,
				"error starting event ring %d\n", evnt_ring);
			spin_unlock_irqrestore(&mhi_cntrl->mhi_event[0].lock, flags);
			return ret;
		}
	}

	/* add the ring element */
	ret = mhi_ep_ring_add_element(mhi_cntrl, ring, el, NULL, 0);
	if (ret) {
		dev_err(dev, "Error adding ring element\n");
		mutex_unlock(&mhi_cntrl->event_lock);
		return ret;
	}

	/*
	 * rp update in host memory should be flushed
	 * before sending a MSI to the host
	 */
	wmb();

	mutex_unlock(&mhi_cntrl->event_lock);

	dev_dbg(dev, "evnt ptr : 0x%llx\n", el->evt_tr_comp.ptr);
	dev_dbg(dev, "evnt len : 0x%x\n", el->evt_tr_comp.len);
	dev_dbg(dev, "evnt code :0x%x\n", el->evt_tr_comp.code);
	dev_dbg(dev, "evnt type :0x%x\n", el->evt_tr_comp.type);
	dev_dbg(dev, "evnt chid :0x%x\n", el->evt_tr_comp.chid);

	mhi_cntrl->raise_irq(mhi_cntrl);

	return 0;
}

static int mhi_ep_send_completion_event(struct mhi_ep_cntrl *mhi_cntrl,
			struct mhi_ep_ring *ring, uint32_t len,
			enum mhi_ep_cmd_completion_code code)
{
	union mhi_ep_ring_element_type event = {};
	u32 er_index;

	er_index = mhi_cntrl->ch_ctx_cache[ring->ch_id].err_indx;
	event.evt_tr_comp.chid = ring->ch_id;
	event.evt_tr_comp.type =
				MHI_EP_RING_EL_TRANSFER_COMPLETION_EVENT;
	event.evt_tr_comp.len = len;
	event.evt_tr_comp.code = code;
	event.evt_tr_comp.ptr = ring->ring_ctx->generic.rbase +
			ring->rd_offset * sizeof(struct mhi_ep_transfer_ring_element);

	return mhi_ep_send_event(mhi_cntrl, er_index, &event);
}

int mhi_ep_send_state_change_event(struct mhi_ep_cntrl *mhi_cntrl,
						enum mhi_ep_state state)
{
	union mhi_ep_ring_element_type event = {};

	event.evt_state_change.type = MHI_EP_RING_EL_MHI_STATE_CHG;
	event.evt_state_change.mhistate = state;

	return mhi_ep_send_event(mhi_cntrl, 0, &event);
}
EXPORT_SYMBOL(mhi_ep_send_state_change_event);

int mhi_ep_send_ee_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ep_execenv exec_env)
{
	union mhi_ep_ring_element_type event = {};

	event.evt_ee_state.type = MHI_EP_RING_EL_EE_STATE_CHANGE_NOTIFY;
	event.evt_ee_state.execenv = exec_env;

	return mhi_ep_send_event(mhi_cntrl, 0, &event);
}
EXPORT_SYMBOL(mhi_ep_send_ee_event);

static int mhi_ep_send_cmd_comp_event(struct mhi_ep_cntrl *mhi_cntrl,
				enum mhi_ep_cmd_completion_code code)
{
	union mhi_ep_ring_element_type event = {};
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int ret;

	if (code > MHI_CMD_COMPL_CODE_RES) {
		dev_err(dev,
			"Invalid cmd compl code: %d\n", code);
		return -EINVAL;
	}

	/* send the command completion event to the host */
	event.evt_cmd_comp.ptr = mhi_cntrl->cmd_ctx_cache->rbase
			+ (mhi_cntrl->mhi_cmd->ring.rd_offset *
			(sizeof(union mhi_ep_ring_element_type)));
	dev_dbg(dev, "evt cmd comp ptr :0x%x\n",
			(size_t) event.evt_cmd_comp.ptr);
	event.evt_cmd_comp.type = MHI_EP_RING_EL_CMD_COMPLETION_EVT;
	event.evt_cmd_comp.code = code;
	return mhi_ep_send_event(mhi_cntrl, 0, &event);
}

int mhi_ep_process_cmd_ring(struct mhi_ep_ring *ring, union mhi_ep_ring_element_type *el)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_ring *ch_ring, *evt_ring;
	struct mhi_ep_chan *chan;
	union mhi_ep_ring_ctx *evt_ctx;
	u32 ch_id = 0;
	u32 evnt_ring_idx;
	int ret;

	ch_id = el->generic.chid;

	switch (el->generic.type) {
	case MHI_EP_RING_EL_START:
		dev_dbg(dev, "recived start cmd for channel %d\n",ch_id);
		ch_ring = &mhi_cntrl->mhi_chan[ch_id].ring;
		/* Initialize and configure the corresponding channel ring */
		ret = mhi_ep_ring_start(mhi_cntrl, ch_ring,
			(union mhi_ep_ring_ctx *)&mhi_cntrl->ch_ctx_cache[ch_id]);
		if (ret) {
			dev_err(dev,
				"start ring failed for ch %d\n", ch_id);
			ret = mhi_ep_send_cmd_comp_event(mhi_cntrl,
						MHI_CMD_COMPL_CODE_UNDEFINED);
			if (ret)
				dev_err(dev,
					"Error with compl event\n");
			return ret;
		}

		chan = &mhi_cntrl->mhi_chan[ch_id];
		chan->state = MHI_EP_CH_STATE_ENABLED;

		/* enable DB for event ring */
		mhi_ep_mmio_enable_chdb_a7(mhi_cntrl, ch_id);

		evnt_ring_idx = mhi_cntrl->ch_ctx_cache[ch_id].err_indx;
		evt_ring = &mhi_cntrl->mhi_event[evnt_ring_idx].ring;
		evt_ctx = (union mhi_ep_ring_ctx *)&mhi_cntrl->ev_ctx_cache[evnt_ring_idx];
		if (evt_ring->state == RING_STATE_UINT) {
			ret = mhi_ep_ring_start(mhi_cntrl, evt_ring, evt_ctx);
			if (ret) {
				dev_err(dev,
				"error starting event ring %d\n",
				mhi_cntrl->ch_ctx_cache[ch_id].err_indx);
				return ret;
			}
		}
	//		mhi_ep_alloc_evt_buf_evt_req(mhi_cntrl, &mhi_cntrl->ch[ch_id],
	//				evt_ring);

		mhi_cntrl->ch_ctx_cache[ch_id].ch_state = MHI_EP_CH_STATE_RUNNING;

		ret = mhi_ep_send_cmd_comp_event(mhi_cntrl,
						MHI_CMD_COMPL_CODE_SUCCESS);
		if (ret) {
			pr_err("Error sending command completion event\n");
			return ret;
		}

		/* Create MHI device for the UL channel */
		if (!(ch_id % 2)) {
			ret = mhi_create_device(mhi_cntrl, ch_id);
			if (ret) {
				pr_err("Error creating device\n");
				return ret;
			}
		}
		break;
	default:
		break;
	}

	return 0;
}

static int mhi_ep_check_tre_bytes_left(struct mhi_ep_cntrl *mhi_cntrl,
				       struct mhi_ep_ring *ring,
				       union mhi_ep_ring_element_type *el)
{
	struct mhi_ep_chan *mhi_chan = &mhi_cntrl->mhi_chan[ring->ch_id];
	bool td_done = 0;

	/*
	 * A full TRE worth of data was consumed.
	 * Check if we are at a TD boundary.
	 */
	if (mhi_chan->tre_bytes_left == 0) {
		if (el->tre.chain) {
			if (el->tre.ieob)
				mhi_ep_send_completion_event(mhi_cntrl,
				ring, el->tre.len, MHI_CMD_COMPL_CODE_EOB);
		} else {
			if (el->tre.ieot)
				mhi_ep_send_completion_event(mhi_cntrl,
				ring, el->tre.len, MHI_CMD_COMPL_CODE_EOT);
			td_done = 1;
		}
		mhi_ep_ring_inc_index(ring, ring->rd_offset);
		mhi_chan->tre_bytes_left = 0;
		mhi_chan->tre_loc = 0;
	}

	return td_done;
}

static int mhi_ep_read_channel(struct mhi_ep_cntrl *mhi_cntrl,
			       struct mhi_ep_ring *ring,
			       struct mhi_result *result,
			       u32 len)
{
	struct mhi_ep_chan *mhi_chan = &mhi_cntrl->mhi_chan[ring->ch_id];
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	union mhi_ep_ring_element_type *el;
	size_t bytes_to_read, addr_offset;
	uint64_t read_from_loc;
	ssize_t bytes_read = 0;
	size_t write_to_loc;
	uint32_t usr_buf_remaining;
	bool td_done = 0;
	int ret;

	usr_buf_remaining = len;	

	mutex_lock(&mhi_chan->lock);
	do {
		el = &ring->ring_cache[ring->rd_offset];

		if (mhi_chan->tre_loc) {
			bytes_to_read = min(usr_buf_remaining,
						mhi_chan->tre_bytes_left);
			dev_dbg(dev, "remaining buffered data size %d", mhi_chan->tre_bytes_left);
		} else {
			if (ring->rd_offset == ring->wr_offset) {
				dev_dbg(dev, "nothing to read, returning\n");
				ret = 0;
				goto exit;
			}

			mhi_chan->tre_loc = el->tre.data_buf_ptr;
			mhi_chan->tre_size = el->tre.len;
			mhi_chan->tre_bytes_left = mhi_chan->tre_size;

			/* TODO change to min */
			bytes_to_read = min(usr_buf_remaining, mhi_chan->tre_size);
		}

		bytes_read += bytes_to_read;
		addr_offset = mhi_chan->tre_size - mhi_chan->tre_bytes_left;
		read_from_loc = mhi_chan->tre_loc + addr_offset;
		write_to_loc = (size_t) result->buf_addr + (len - usr_buf_remaining);
		mhi_chan->tre_bytes_left -= bytes_to_read;

		if (!mhi_chan->tre_buf) {
			mhi_chan->tre_buf = mhi_cntrl->alloc_addr(mhi_cntrl, &mhi_chan->tre_phys, bytes_to_read);
			if (!mhi_chan->tre_buf) {
				dev_err(dev, "Failed to allocate TRE buffer\n");
				return -ENOMEM;
			}
		}

		ret = mhi_cntrl->map_addr(mhi_cntrl, mhi_chan->tre_phys, read_from_loc, bytes_to_read);
		if (ret) {
			dev_err(dev, "Failed to map TRE buffer\n");
			goto err_tre_free;
		}

		dev_dbg(dev, "Reading %d bytes from channel: %d", bytes_to_read, ring->ch_id);
		memcpy_fromio((void *)write_to_loc, mhi_chan->tre_buf, bytes_to_read);

		mhi_cntrl->unmap_addr(mhi_cntrl, mhi_chan->tre_phys);

		usr_buf_remaining -= bytes_to_read;
		td_done = mhi_ep_check_tre_bytes_left(mhi_cntrl, ring, el);
	} while(usr_buf_remaining && !td_done);

	result->bytes_xferd = bytes_read;

	mutex_unlock(&mhi_chan->lock);

	return 0;

err_tre_free:
	mhi_cntrl->free_addr(mhi_cntrl, mhi_chan->tre_phys, mhi_chan->tre_buf, bytes_to_read);
exit:
	mutex_unlock(&mhi_chan->lock);

	return ret;
}

int mhi_ep_process_tre_ring(struct mhi_ep_ring *ring, union mhi_ep_ring_element_type *el)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_chan *mhi_chan;
	struct mhi_result result = {};
	u32 len = MHI_NET_DEFAULT_MTU;
	int ret;

	if (ring->ch_id > mhi_cntrl->max_chan) {
		dev_err(dev, "Invalid channel ring id: %d\n", ring->ch_id);
		return -EINVAL;
	}

	dev_dbg(dev, "Processing TRE ring for channel: %d\n", ring->ch_id);

	mhi_chan = &mhi_cntrl->mhi_chan[ring->ch_id];

	if (ring->ch_id % 2) {
		/* DL channel */
		result.dir = mhi_chan->dir;
		mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
	} else {
		/* UL channel */
		result.buf_addr = kzalloc(len, GFP_KERNEL);
		if (!result.buf_addr)
			return -EINVAL;

		ret = mhi_ep_read_channel(mhi_cntrl, ring, &result, len);
		if (ret) {
			dev_err(dev, "Failed to read channel: %d\n", ring->ch_id);
			return -EINVAL;
		}	

		result.dir = mhi_chan->dir;
		mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
		kfree(result.buf_addr);
	}

	return 0;
}

static int mhi_ep_cache_host_cfg(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	phys_addr_t ch_ctx_cache_phys, ev_ctx_cache_phys, cmd_ctx_cache_phys;
	size_t ch_ctx_host_size, ev_ctx_host_size, cmd_ctx_host_size;
	int ret = 0;

	/* Update the number of event rings (NER) programmed by the host */
	mhi_ep_mmio_update_ner(mhi_cntrl);

	ch_ctx_host_size = sizeof(struct mhi_ep_ch_ctx) *
					mhi_cntrl->max_chan;
	ev_ctx_host_size = sizeof(struct mhi_ep_ev_ctx) *
					mhi_cntrl->event_rings;
	cmd_ctx_host_size = sizeof(struct mhi_ep_cmd_ctx);
	dev_dbg(dev, "Number of Event rings: %d, HW Event rings: %d\n",
			mhi_cntrl->event_rings, mhi_cntrl->hw_event_rings);

	/* Get the channel context base pointer from host */
	mhi_ep_mmio_get_chc_base(mhi_cntrl);

	mhi_cntrl->ch_ctx_cache = mhi_cntrl->alloc_addr(mhi_cntrl, &ch_ctx_cache_phys,
						   ch_ctx_host_size);
	if (!mhi_cntrl->ch_ctx_cache) {
		dev_err(dev, "Failed to allocate ch_ctx_cache address\n");
		return -ENOMEM;
	}

	ret = mhi_cntrl->map_addr(mhi_cntrl, ch_ctx_cache_phys,
			       mhi_cntrl->ch_ctx_host_pa, ch_ctx_host_size);
	if (ret) {
		dev_err(dev, "Failed to map ch_ctx_cache address\n");
		goto err_ch_ctx;
	}

	/* Get the event context base pointer from host */
	mhi_ep_mmio_get_erc_base(mhi_cntrl);

	mhi_cntrl->ev_ctx_cache = mhi_cntrl->alloc_addr(mhi_cntrl, &ev_ctx_cache_phys,
						   ev_ctx_host_size);
	if (!mhi_cntrl->ev_ctx_cache) {
		dev_err(dev, "Failed to allocate ev_ctx_cache address\n");
		ret = -ENOMEM;
		goto err_ch_ctx_map;
	}

	ret = mhi_cntrl->map_addr(mhi_cntrl, ev_ctx_cache_phys,
			       mhi_cntrl->ev_ctx_host_pa, ev_ctx_host_size);
	if (ret) {
		dev_err(dev, "Failed to map ev_ctx_cache address\n");
		goto err_ev_ctx;
	}

	/* Get the command context base pointer from host */
	mhi_ep_mmio_get_crc_base(mhi_cntrl);

	mhi_cntrl->cmd_ctx_cache = mhi_cntrl->alloc_addr(mhi_cntrl, &cmd_ctx_cache_phys,
						    cmd_ctx_host_size);
	if (!mhi_cntrl->cmd_ctx_cache) {
		dev_err(dev, "Failed to allocate cmd_ctx_cache address\n");
		ret = -ENOMEM;
		goto err_ev_ctx_map;
	}

	ret = mhi_cntrl->map_addr(mhi_cntrl, cmd_ctx_cache_phys,
			       mhi_cntrl->cmd_ctx_host_pa, cmd_ctx_host_size);
	if (ret) {
		dev_err(dev, "Failed to map address\n");
		goto err_cmd_ctx;
	}

	dev_dbg(dev, 
			"cmd ring_base:0x%llx, rp:0x%llx, wp:0x%llx\n",
					mhi_cntrl->cmd_ctx_cache->rbase,
					mhi_cntrl->cmd_ctx_cache->rp,
					mhi_cntrl->cmd_ctx_cache->wp);
	dev_dbg(dev, 
			"ev ring_base:0x%llx, rp:0x%llx, wp:0x%llx\n",
					mhi_cntrl->ev_ctx_cache->rbase,
					mhi_cntrl->ev_ctx_cache->rp,
					mhi_cntrl->ev_ctx_cache->wp);

	/* Initialize command ring */
	ret = mhi_ep_ring_start(mhi_cntrl, &mhi_cntrl->mhi_cmd->ring,
			(union mhi_ep_ring_ctx *)mhi_cntrl->cmd_ctx_cache);
	if (ret) {
		dev_err(dev, "Failed to start the MHI ring\n");
		goto err_cmd_ctx_map;
	}

	return ret;

err_cmd_ctx_map:
	mhi_cntrl->unmap_addr(mhi_cntrl, cmd_ctx_cache_phys);

err_cmd_ctx:
	mhi_cntrl->free_addr(mhi_cntrl, cmd_ctx_cache_phys, mhi_cntrl->cmd_ctx_cache,
			      cmd_ctx_host_size);

err_ev_ctx_map:
	mhi_cntrl->unmap_addr(mhi_cntrl, ev_ctx_cache_phys);

err_ev_ctx:
	mhi_cntrl->free_addr(mhi_cntrl, ev_ctx_cache_phys, mhi_cntrl->ev_ctx_cache,
			      ev_ctx_host_size);

err_ch_ctx_map:
	mhi_cntrl->unmap_addr(mhi_cntrl, ch_ctx_cache_phys);

err_ch_ctx:
	mhi_cntrl->free_addr(mhi_cntrl, ch_ctx_cache_phys, mhi_cntrl->ch_ctx_cache,
			      ch_ctx_host_size);

	return ret;
}

static void mhi_ep_enable_int(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_enable_chdb_interrupts(mhi_cntrl);
	mhi_ep_mmio_enable_ctrl_interrupt(mhi_cntrl);
	mhi_ep_mmio_enable_cmdb_interrupt(mhi_cntrl);

	enable_irq(mhi_cntrl->irq);
}

static void mhi_ep_enable(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_ep_state state;
	u32 max_cnt = 0;
	int ret, i;
	bool mhi_reset;

	/* Initialize command, channel and event rings */ 
	mhi_ep_ring_init(&mhi_cntrl->mhi_cmd->ring, RING_TYPE_CMD, 0);
	for (i = 0; i < mhi_cntrl->max_chan; i++)
		mhi_ep_ring_init(&mhi_cntrl->mhi_chan[i].ring, RING_TYPE_CH, i);
	for (i = 0; i < mhi_cntrl->event_rings; i++) {
		mhi_ep_ring_init(&mhi_cntrl->mhi_event[i].ring, RING_TYPE_ER, i);
	}

	/* Check if host has set M0 state */
	mhi_ep_mmio_get_mhi_state(mhi_cntrl, &state, &mhi_reset);
	if (mhi_reset) {
		mhi_ep_mmio_clear_reset(mhi_cntrl);
		dev_dbg(dev, "Cleared reset before waiting for M0\n");
	}

	/* Wait for Host to set the M0 state if not done */
	while (state != MHI_EP_M0_STATE && max_cnt < MHI_SUSPEND_TIMEOUT) {
		msleep(MHI_SUSPEND_MIN);
		mhi_ep_mmio_get_mhi_state(mhi_cntrl, &state, &mhi_reset);
		if (mhi_reset) {
			mhi_ep_mmio_clear_reset(mhi_cntrl);
			dev_dbg(dev, "Host initiated reset while waiting for M0\n");
		}
		max_cnt++;
	}

	if (state == MHI_EP_M0_STATE) {
		ret = mhi_ep_cache_host_cfg(mhi_cntrl);
		if (ret) {
			dev_err(dev, "Failed to cache the host config\n");
			return;
		}

		/* TODO: Check if this is necessary */
		mhi_ep_mmio_set_env(mhi_cntrl, MHI_EP_AMSS_EE);
	} else {
		dev_err(dev, "MHI device failed to enter M0\n");
		return;
	}

	mhi_ep_enable_int(mhi_cntrl);
}

static void mhi_ep_process_ring_pending(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work,
				struct mhi_ep_cntrl, ring_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_ring *ring;
	struct list_head *cp, *q;
	int rc = 0;

	mutex_lock(&mhi_cntrl->lock);
	rc = mhi_ep_process_ring(&mhi_cntrl->mhi_cmd->ring);
	if (rc) {
		dev_err(dev, "error processing command ring\n");
		goto exit;
	}

	list_for_each_safe(cp, q, &mhi_cntrl->process_ring_list) {
		ring = list_entry(cp, struct mhi_ep_ring, list);
		list_del(cp);
		rc = mhi_ep_process_ring(ring);
		if (rc) {
			dev_err(dev,
				"error processing channel ring: %d\n", ring->ch_id);
			goto exit;
		}

		/* Enable channel interrupt */
		mhi_ep_mmio_enable_chdb_a7(mhi_cntrl, ring->ch_id);
	}

exit:
	mutex_unlock(&mhi_cntrl->lock);
	return;
}

static int mhi_ep_get_event(enum mhi_ep_state state, enum mhi_ep_event_type *event)
{
	switch (state) {
	case MHI_EP_M0_STATE:
		*event = MHI_EP_EVENT_M0_STATE;
		break;
	case MHI_EP_M1_STATE:
		*event = MHI_EP_EVENT_M1_STATE;
		break;
	case MHI_EP_M2_STATE:
		*event = MHI_EP_EVENT_M2_STATE;
		break;
	case MHI_EP_M3_STATE:
		*event = MHI_EP_EVENT_M3_STATE;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void mhi_ep_queue_channel_db(struct mhi_ep_cntrl *mhi_cntrl,
					u32 chintr_value, uint32_t ch_num)
{
	struct mhi_ep_ring *ring;

	for (; chintr_value; ch_num++, chintr_value >>= 1) {
		if (chintr_value & 1) {
			ring = &mhi_cntrl->mhi_chan[ch_num].ring;
			ring->state = RING_STATE_PENDING;
			list_add(&ring->list, &mhi_cntrl->process_ring_list);
			/*
			 * Disable the channel interrupt here and enable it once
			 * the current interrupt got serviced
			 */
			mhi_ep_mmio_disable_chdb_a7(mhi_cntrl, ch_num);
			queue_work(mhi_cntrl->ring_wq, &mhi_cntrl->ring_work);
		}
	}
}

static void mhi_ep_check_channel_interrupt(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	u32 chintr_value = 0, ch_num = 0;
	int i;

	mhi_ep_mmio_read_chdb_status_interrupts(mhi_cntrl);

	dev_dbg(dev, "Checking for channel db");
	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++) {
		ch_num = i * MHI_MASK_CH_EV_LEN;
		/* Process channel status whose mask is enabled */
		chintr_value = (mhi_cntrl->chdb[i].status & mhi_cntrl->chdb[i].mask);
		if (chintr_value) {
			dev_dbg(dev,
				"processing id: %d, ch interrupt 0x%x\n",
							i, chintr_value);
			mhi_ep_queue_channel_db(mhi_cntrl, chintr_value, ch_num);
			mhi_ep_mmio_write(mhi_cntrl, MHI_CHDB_INT_CLEAR_A7_n(i),
							mhi_cntrl->chdb[i].status);
		}
	}
}

/*
 * Interrupt handler that services interrupts raised by the host writing to
 * MHICTRL and Command ring doorbell (CRDB) registers
 */
static void mhi_ep_chdb_ctrl_handler(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work,
				struct mhi_ep_cntrl, chdb_ctrl_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_ep_state state;
	enum mhi_ep_event_type event = 0;
	u32 int_value = 0;
	int ret = 0;
	bool mhi_reset;

	mutex_lock(&mhi_cntrl->lock);

	/* Acknowledge the interrupts */
	mhi_ep_mmio_read(mhi_cntrl, MHI_CTRL_INT_STATUS_A7, &int_value);
	mhi_ep_mmio_write(mhi_cntrl, MHI_CTRL_INT_CLEAR_A7, int_value);

	/* Check for cntrl interrupts */
	if (int_value & MHI_MMIO_CTRL_INT_STATUS_A7_MSK) {
		dev_dbg(dev, "Processing ctrl interrupt with : %d\n", int_value);

		mhi_ep_mmio_get_mhi_state(mhi_cntrl, &state, &mhi_reset);

		/* TODO: Check for MHI host reset */

		ret = mhi_ep_get_event(state, &event);
		if (ret) {
			dev_err(dev, "Unsupported state :%d\n", state);
			goto fail;
		}

		ret = mhi_ep_notify_sm_event(mhi_cntrl, event);
		if (ret) {
			dev_err(dev, "error sending SM event\n");
			goto fail;
		}
	}

	/* Check for cmd db interrupts */
	if (int_value & MHI_MMIO_CTRL_CRDB_STATUS_MSK) {
		dev_dbg(dev,
			"processing cmd db interrupt with %d\n", int_value);
		/* TODO Mark pending ring */
		queue_work(mhi_cntrl->ring_wq, &mhi_cntrl->ring_work);
	}

	/* Check for channel interrupts */
	mhi_ep_check_channel_interrupt(mhi_cntrl);

fail:
	mutex_unlock(&mhi_cntrl->lock);
	enable_irq(mhi_cntrl->irq);
}

static irqreturn_t mhi_ep_irq(int irq, void *data)
{
	struct mhi_ep_cntrl *mhi_cntrl = data;

	disable_irq_nosync(irq);
	schedule_work(&mhi_cntrl->chdb_ctrl_work);

	return IRQ_HANDLED;
}

void mhi_ep_hw_init(struct work_struct *work)
{
	struct mhi_ep_cntrl *mhi_cntrl = container_of(work, struct mhi_ep_cntrl, init_work);
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int ret;

	/* Mask all interrupts until the state machine is ready */
	mhi_ep_mmio_mask_interrupts(mhi_cntrl);

	mhi_ep_mmio_init(mhi_cntrl);
	dev_dbg(dev, "Number of Event rings: %d, HW Event rings: %d\n",
			mhi_cntrl->event_rings, mhi_cntrl->hw_event_rings);

	mhi_cntrl->mhi_event = kzalloc(mhi_cntrl->event_rings * (sizeof(*mhi_cntrl->mhi_event)),
					GFP_KERNEL);
	if (!mhi_cntrl->mhi_event)
		return;

	/* TODO: Initialize lock for all event rings */
	spin_lock_init(&mhi_cntrl->mhi_event[0].lock);

	/* Init state machine */
	ret = mhi_ep_sm_init(mhi_cntrl);
	if (ret)
		kfree(mhi_cntrl->mhi_event);

	/* All set, notify the host */
	ret = mhi_ep_sm_set_ready(mhi_cntrl);
	if (ret)
		kfree(mhi_cntrl->mhi_event);

	irq_set_status_flags(mhi_cntrl->irq, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, mhi_cntrl->irq, mhi_ep_irq,
			       IRQF_TRIGGER_HIGH, "doorbell_irq", mhi_cntrl);
	if (ret) {
		dev_err(dev, "Failed to request Doorbell IRQ\n");
		kfree(mhi_cntrl->mhi_event);
	}

	mhi_ep_enable(mhi_cntrl);

	dev_dbg(dev, "Power on setup success\n");
}

static void skip_to_next_td(struct mhi_ep_chan *mhi_chan, struct mhi_ep_ring *ring)
{
	union mhi_ep_ring_element_type *el;
	uint32_t td_boundary_reached = 0;

	mhi_chan->skip_td = 1;
	el = &ring->ring_cache[ring->rd_offset];
	while (ring->rd_offset != ring->wr_offset) {
		if (td_boundary_reached) {
			mhi_chan->skip_td = 0;
			break;
		}
		if (!el->tre.chain)
			td_boundary_reached = 1;
		mhi_ep_ring_inc_index(ring, ring->rd_offset);
		el = &ring->ring_cache[ring->rd_offset];
	}
}

int mhi_ep_queue_skb(struct mhi_ep_device *mhi_dev, enum dma_data_direction dir,
		  struct sk_buff *skb, size_t len, enum mhi_flags mflags)
{
	struct mhi_ep_chan *mhi_chan = (dir == DMA_FROM_DEVICE) ? mhi_dev->dl_chan :
							       mhi_dev->ul_chan;
	struct mhi_ep_cntrl *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_ring *ring;
	union mhi_ep_ring_element_type *el;
	enum mhi_ep_cmd_completion_code code = MHI_CMD_COMPL_CODE_INVALID;
	int ret;
	u32 tre_len;
	u64 write_to_loc, skip_tres = 0;
	size_t read_from_loc;
	uint32_t usr_buf_remaining;
	size_t usr_buf_offset = 0;
	size_t bytes_to_write = 0;
	size_t bytes_written = 0;

	if (dir == DMA_TO_DEVICE)
		return -EINVAL;
	
	usr_buf_remaining = len;
	ring = &mhi_cntrl->mhi_chan[mhi_chan->chan].ring;

	if (mhi_chan->skip_td)
		skip_to_next_td(mhi_chan, ring);

	do {
		if (ring->rd_offset == ring->wr_offset) {
			dev_err(dev, "TRE not available!\n");
			return -EINVAL;
		}

		el = &ring->ring_cache[ring->rd_offset];
		tre_len = el->tre.len;
		if (skb->len > tre_len) {
			dev_err(dev, "Buffer size is too big to queue!\n");
			return -ENOMEM;
		}

		bytes_to_write = min(usr_buf_remaining, tre_len);
		usr_buf_offset = skb->len - bytes_to_write;
		read_from_loc = (size_t) skb->data;
		write_to_loc = el->tre.data_buf_ptr;

		if (!mhi_chan->tre_buf) {
			mhi_chan->tre_buf = mhi_cntrl->alloc_addr(mhi_cntrl, &mhi_chan->tre_phys, bytes_to_write);
			if (!mhi_chan->tre_buf) {
				dev_err(dev, "Failed to allocate TRE buffer\n");
				return -ENOMEM;
			}
		}

		ret = mhi_cntrl->map_addr(mhi_cntrl, mhi_chan->tre_phys, write_to_loc, bytes_to_write);
		if (ret) {
			dev_err(dev, "Failed to map TRE buffer\n");
			goto err_tre_free;
		}

		dev_dbg(dev, "Writing to: %llx", el->tre.data_buf_ptr);
		dev_dbg(dev, "Writing %d bytes to chan: %d", bytes_to_write, ring->ch_id);
		memcpy_toio(mhi_chan->tre_buf, (void *)read_from_loc, bytes_to_write);

		/* TODO: See if we can return bytes_written */
		bytes_written += bytes_to_write;
		usr_buf_remaining -= bytes_to_write;

		if (usr_buf_remaining) {
			if (!el->tre.chain)
				code = MHI_CMD_COMPL_CODE_OVERFLOW;
			else if (el->tre.ieob)
				code = MHI_CMD_COMPL_CODE_EOB;
		} else {
			if (el->tre.chain)
				skip_tres = 1;
			code = MHI_CMD_COMPL_CODE_EOT;
		}

		dev_dbg(dev, "Sending completion code: %d", code);
		/* TODO: Handle the completion code properly */
		ret = mhi_ep_send_completion_event(mhi_cntrl, ring,
						   bytes_to_write, code);
		if (ret) {
			dev_err(dev, "Err in snding cmpl evt ch: %d\n", ring->ch_id);
			goto err_tre_unmap;
		}

		mhi_ep_ring_inc_index(ring, ring->rd_offset);	

		mhi_cntrl->unmap_addr(mhi_cntrl, mhi_chan->tre_phys);
	} while (!skip_tres && usr_buf_remaining);

	if (skip_tres)
		skip_to_next_td(mhi_chan, ring);

	return 0;

err_tre_unmap:
	mhi_cntrl->unmap_addr(mhi_cntrl, mhi_chan->tre_phys);
err_tre_free:
	mhi_cntrl->free_addr(mhi_cntrl, mhi_chan->tre_phys, mhi_chan->tre_buf, tre_len);

	return ret;		
}
EXPORT_SYMBOL_GPL(mhi_ep_queue_skb);

void mhi_ep_power_up(struct mhi_ep_cntrl *mhi_cntrl)
{
	queue_work(mhi_cntrl->init_wq, &mhi_cntrl->init_work);
}

static void mhi_ep_release_device(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);

	kfree(mhi_dev);
}

struct mhi_ep_device *mhi_ep_alloc_device(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_device *mhi_dev;
	struct device *dev;

	mhi_dev = kzalloc(sizeof(*mhi_dev), GFP_KERNEL);
	if (!mhi_dev)
		return ERR_PTR(-ENOMEM);

	dev = &mhi_dev->dev;
	device_initialize(dev);
	dev->bus = &mhi_ep_bus_type;
	dev->release = mhi_ep_release_device;

	if (mhi_cntrl->mhi_dev) {
		/* for MHI client devices, parent is the MHI controller device */
		dev->parent = &mhi_cntrl->mhi_dev->dev;
	} else {
		/* for MHI controller device, parent is the bus device (e.g. pci device) */
		dev->parent = mhi_cntrl->cntrl_dev;
	}

	mhi_dev->mhi_cntrl = mhi_cntrl;

	return mhi_dev;
}

int mhi_create_device(struct mhi_ep_cntrl *mhi_cntrl, u32 ch_id)
{
	struct mhi_ep_device *mhi_dev;
	struct mhi_ep_chan *mhi_chan = &mhi_cntrl->mhi_chan[ch_id];
	int ret;

	mhi_dev = mhi_ep_alloc_device(mhi_cntrl);
	if (IS_ERR(mhi_dev))
		return PTR_ERR(mhi_dev);

	mhi_dev->dev_type = MHI_DEVICE_XFER;

	/* Configure primary channel */
	if (mhi_chan->dir == DMA_TO_DEVICE) {
		mhi_dev->ul_chan = mhi_chan;
		mhi_dev->ul_chan_id = mhi_chan->chan;
	} else {
		mhi_dev->dl_chan = mhi_chan;
		mhi_dev->dl_chan_id = mhi_chan->chan;
	}

	get_device(&mhi_dev->dev);
	mhi_chan->mhi_dev = mhi_dev;

	/* Configure secondary channel as well */
	mhi_chan++;
	if (mhi_chan->dir == DMA_TO_DEVICE) {
		mhi_dev->ul_chan = mhi_chan;
		mhi_dev->ul_chan_id = mhi_chan->chan;
	} else {
		mhi_dev->dl_chan = mhi_chan;
		mhi_dev->dl_chan_id = mhi_chan->chan;
	}

	get_device(&mhi_dev->dev);
	mhi_chan->mhi_dev = mhi_dev;

	/* Channel name is same for both UL and DL */
	mhi_dev->name = mhi_chan->name;
	dev_set_name(&mhi_dev->dev, "%s_%s",
		     dev_name(&mhi_cntrl->mhi_dev->dev),
		     mhi_dev->name);

	ret = device_add(&mhi_dev->dev);
	if (ret)
		put_device(&mhi_dev->dev);

	return ret;
}

static int parse_ch_cfg(struct mhi_ep_cntrl *mhi_cntrl,
			const struct mhi_ep_cntrl_config *config)
{
	const struct mhi_ep_channel_config *ch_cfg;
	struct device *dev = mhi_cntrl->cntrl_dev;
	u32 chan, i;

	mhi_cntrl->max_chan = config->max_channels;

	/*
	 * The allocation of MHI channels can exceed 32KB in some scenarios,
	 * so to avoid any memory possible allocation failures, vzalloc is
	 * used here
	 */
	mhi_cntrl->mhi_chan = kzalloc(mhi_cntrl->max_chan *
				      sizeof(*mhi_cntrl->mhi_chan), GFP_KERNEL);
	if (!mhi_cntrl->mhi_chan)
		return -ENOMEM;

	/* We allocate max_channels and then only populate the defined channels */
	for (i = 0; i < config->num_channels; i++) {
		struct mhi_ep_chan *mhi_chan;

		ch_cfg = &config->ch_cfg[i];

		chan = ch_cfg->num;
		if (chan >= mhi_cntrl->max_chan) {
			dev_err(dev, "Channel %d not available\n", chan);
			goto error_chan_cfg;
		}

		mhi_chan = &mhi_cntrl->mhi_chan[chan];
		mhi_chan->name = ch_cfg->name;
		mhi_chan->chan = chan;
		mhi_chan->dir = ch_cfg->dir;
		mutex_init(&mhi_chan->lock);

		/*
		 * Bi-directional and direction less channels are not supported
		 */
		if (mhi_chan->dir == DMA_BIDIRECTIONAL || mhi_chan->dir == DMA_NONE) {
			dev_err(dev, "Invalid channel configuration\n");
			goto error_chan_cfg;
		}

		mhi_chan->configured = true;
	}

	return 0;

error_chan_cfg:
	kfree(mhi_cntrl->mhi_chan);

	return -EINVAL;
}

static int parse_config(struct mhi_ep_cntrl *mhi_cntrl,
			const struct mhi_ep_cntrl_config *config)
{
	int ret;

	ret = parse_ch_cfg(mhi_cntrl, config);
	if (ret)
		return ret;

	return 0;
}

/*
 * Allocate channel and command rings here. The event rings will be allocated
 * in mhi_ep_prepare_for_power_up() as it is set by the host.
 */
int mhi_ep_register_controller(struct mhi_ep_cntrl *mhi_cntrl,
				const struct mhi_ep_cntrl_config *config)
{
	struct mhi_ep_device *mhi_dev;
	int ret;

	if (!mhi_cntrl || !mhi_cntrl->cntrl_dev || !mhi_cntrl->mmio || !mhi_cntrl->irq)
		return -EINVAL;

	ret = parse_config(mhi_cntrl, config);
	if (ret)
		return ret;

	mhi_cntrl->mhi_cmd = kzalloc(NR_OF_CMD_RINGS *
				     sizeof(*mhi_cntrl->mhi_cmd), GFP_KERNEL);
	if (!mhi_cntrl->mhi_cmd) {
		ret = -ENOMEM;
		goto err_free_ch;
	}

	INIT_WORK(&mhi_cntrl->ring_work, mhi_ep_process_ring_pending);
	INIT_WORK(&mhi_cntrl->chdb_ctrl_work, mhi_ep_chdb_ctrl_handler);
	INIT_WORK(&mhi_cntrl->init_work, mhi_ep_hw_init);

	mhi_cntrl->ring_wq = alloc_ordered_workqueue("mhi_ep_ring_wq",
							WQ_HIGHPRI);
	if (!mhi_cntrl->ring_wq) {
		ret = -ENOMEM;
		goto err_free_cmd;
	}

	mhi_cntrl->init_wq = alloc_ordered_workqueue("mhi_ep_init_wq", WQ_HIGHPRI);
	if (!mhi_cntrl->init_wq) {
		ret = -ENOMEM;
		goto err_destroy_ring_wq;
	}

	INIT_LIST_HEAD(&mhi_cntrl->process_ring_list);
	mutex_init(&mhi_cntrl->lock);
	mutex_init(&mhi_cntrl->event_lock);

	/* Set MHI version and AMSS EE before link up */
	mhi_ep_mmio_write(mhi_cntrl, MHIVER, config->mhi_version);
	mhi_ep_mmio_set_env(mhi_cntrl, MHI_EP_AMSS_EE);
	
	/* Register controller with MHI bus */
	mhi_dev = mhi_ep_alloc_device(mhi_cntrl);
	if (IS_ERR(mhi_dev)) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to allocate MHI device\n");
		ret = PTR_ERR(mhi_dev);
		goto err_destroy_init_wq;
	}

	mhi_dev->dev_type = MHI_DEVICE_CONTROLLER;
	dev_set_name(&mhi_dev->dev, "sdx55");
	mhi_dev->name = dev_name(&mhi_dev->dev);

	ret = device_add(&mhi_dev->dev);
	if (ret)
		goto err_release_dev;

	mhi_cntrl->mhi_dev = mhi_dev;

	dev_dbg(&mhi_dev->dev, "MHI EP Controller registered\n");
	
	return 0;

err_release_dev:
	put_device(&mhi_dev->dev);
err_destroy_init_wq:
	destroy_workqueue(mhi_cntrl->init_wq);
err_destroy_ring_wq:
	destroy_workqueue(mhi_cntrl->ring_wq);
err_free_cmd:
	kfree(mhi_cntrl->mhi_cmd);
err_free_ch:
	vfree(mhi_cntrl->mhi_chan);

	return ret;
}

static int mhi_ep_driver_probe(struct device *dev)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct device_driver *drv = dev->driver;
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(drv);
	struct mhi_ep_chan *ul_chan = mhi_dev->ul_chan;
	struct mhi_ep_chan *dl_chan = mhi_dev->dl_chan;

	if (ul_chan)
		ul_chan->xfer_cb = mhi_drv->ul_xfer_cb;
	
	if (dl_chan)
		dl_chan->xfer_cb = mhi_drv->dl_xfer_cb;

	return mhi_drv->probe(mhi_dev, mhi_dev->id);
}

static int mhi_ep_driver_remove(struct device *dev)
{
	return 0;
}

int __mhi_ep_driver_register(struct mhi_ep_driver *mhi_drv, struct module *owner)
{
	struct device_driver *driver = &mhi_drv->driver;

	if (!mhi_drv->probe || !mhi_drv->remove)
		return -EINVAL;

	driver->bus = &mhi_ep_bus_type;
	driver->owner = owner;
	driver->probe = mhi_ep_driver_probe;
	driver->remove = mhi_ep_driver_remove;

	return driver_register(driver);
}
EXPORT_SYMBOL_GPL(__mhi_ep_driver_register);

void mhi_ep_driver_unregister(struct mhi_ep_driver *mhi_drv)
{
	driver_unregister(&mhi_drv->driver);
}
EXPORT_SYMBOL_GPL(mhi_ep_driver_unregister);

static int mhi_ep_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);

	return add_uevent_var(env, "MODALIAS=" MHI_DEVICE_MODALIAS_FMT,
					mhi_dev->name);
}

static int mhi_ep_match(struct device *dev, struct device_driver *drv)
{
	struct mhi_ep_device *mhi_dev = to_mhi_ep_device(dev);
	struct mhi_ep_driver *mhi_drv = to_mhi_ep_driver(drv);
	const struct mhi_device_id *id;

	/*
	 * If the device is a controller type then there is no client driver
	 * associated with it
	 */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	for (id = mhi_drv->id_table; id->chan[0]; id++)
		if (!strcmp(mhi_dev->name, id->chan)) {
			mhi_dev->id = id;
			return 1;
		}

	return 0;
};

struct bus_type mhi_ep_bus_type = {
	.name = "mhi_ep",
	.dev_name = "mhi_ep",
	.match = mhi_ep_match,
	.uevent = mhi_ep_uevent,
};

static int __init mhi_ep_init(void)
{
	return bus_register(&mhi_ep_bus_type);
}

static void __exit mhi_ep_exit(void)
{
	bus_unregister(&mhi_ep_bus_type);
}

postcore_initcall(mhi_ep_init);
module_exit(mhi_ep_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI Device Implementation");
