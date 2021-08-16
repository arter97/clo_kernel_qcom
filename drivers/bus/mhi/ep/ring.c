#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mhi_ep.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/completion.h>

#include "internal.h"

static size_t mhi_ep_ring_addr2ofst(struct mhi_ep_ring *ring, u64 p)
{
	u64 rbase;

	rbase = ring->ring_ctx->generic.rbase;

	return (p - rbase) / sizeof(union mhi_ep_ring_element_type);
}

static u32 mhi_ep_ring_num_elems(struct mhi_ep_ring *ring)
{
	return ring->ring_ctx->generic.rlen /
			sizeof(union mhi_ep_ring_element_type);
}

int mhi_ep_cache_ring(struct mhi_ep_ring *ring, size_t end)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	size_t start, copy_size;
	union mhi_ep_ring_element_type *ring_shadow;
	phys_addr_t ring_shadow_phys;
	size_t size = ring->ring_size * sizeof(union mhi_ep_ring_element_type);
	int ret;

	/* No need to cache the ring if wp is unmodified */
	if (ring->wr_offset == end) {
		dev_dbg(dev,
			"nothing to cache for ring (%d), local wr_ofst %d\n",
			ring->type, ring->wr_offset);
		dev_dbg(dev,
			"new wr_offset %d\n", end);
		return 0;
	}

	/* No need to cache event rings */
	if (ring->type == RING_TYPE_ER) {
		dev_dbg(dev,
				"not caching event ring\n");
		return 0;
	}

	start = ring->wr_offset;

	/* Map the host ring */
	ring_shadow = mhi_cntrl->alloc_addr(mhi_cntrl, &ring_shadow_phys,
					   size);
	if (!ring_shadow) {
		dev_err(dev, "failed to allocate ring_shadow\n");
		return -ENOMEM;
	}

	ret = mhi_cntrl->map_addr(mhi_cntrl, ring_shadow_phys,
				  ring->ring_ctx->generic.rbase, size);
	if (ret) {
		dev_err(dev, "failed to map ring_shadow\n\n");
		goto err_ring_free;
	}

	if (start < end) {
		copy_size = (end - start) * sizeof(union mhi_ep_ring_element_type);
		memcpy_fromio(&ring->ring_cache[start], &ring_shadow[start], copy_size);
	} else {
		copy_size = (ring->ring_size - start) * sizeof(union mhi_ep_ring_element_type);
		memcpy_fromio(&ring->ring_cache[start], &ring_shadow[start], copy_size);
		if (end)
			memcpy_fromio(&ring->ring_cache[0], &ring_shadow,
					end * sizeof(union mhi_ep_ring_element_type));
	}

	dev_dbg(dev, "Caching ring (%d) start %d end %d size %d",
		 ring->type, start, end, copy_size);


	mhi_cntrl->unmap_addr(mhi_cntrl, ring_shadow_phys);
	mhi_cntrl->free_addr(mhi_cntrl, ring_shadow_phys, ring_shadow, size);

	return 0;
err_ring_free:
	mhi_cntrl->free_addr(mhi_cntrl, ring_shadow_phys, &ring_shadow, size);

	return ret;
}

int mhi_ep_update_wr_offset(struct mhi_ep_ring *ring)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	u64 wr_offset = 0;
	size_t new_wr_offset = 0;
	int ret;

	switch (ring->type) {
	case RING_TYPE_CMD:
		mhi_ep_mmio_get_cmd_db(ring, &wr_offset);
		dev_dbg(dev,
			"ring type (%d) wr_offset from db 0x%x\n",
			ring->type, (size_t) wr_offset);
		break;
	case RING_TYPE_ER:
		mhi_ep_mmio_get_er_db(ring, &wr_offset);
		break;
	case RING_TYPE_CH:
		mhi_ep_mmio_get_ch_db(ring, &wr_offset);
		dev_dbg(dev,
			"ring %d wr_offset from db 0x%x\n",
			ring->type, (size_t) wr_offset);
		break;
	default:
		return -EINVAL;
	}

	new_wr_offset = mhi_ep_ring_addr2ofst(ring, wr_offset);

	ret = mhi_ep_cache_ring(ring, new_wr_offset);
	if (ret)
		return ret;

	ring->wr_offset = new_wr_offset;

	return 0;
}
EXPORT_SYMBOL(mhi_ep_update_wr_offset);

int mhi_ep_process_ring_element(struct mhi_ep_ring *ring, size_t offset)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	union mhi_ep_ring_element_type *el;
	int ret = -EINVAL;

	/* get the element and invoke the respective callback */
	el = &ring->ring_cache[offset];

	if (ring->ring_cb)
		ret = ring->ring_cb(ring, el);
	else
		dev_err(dev, "No callback registered for ring\n");

	return ret;
}
EXPORT_SYMBOL(mhi_ep_process_ring_element);

int mhi_ep_process_ring(struct mhi_ep_ring *ring)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	union mhi_ep_ring_element_type *el;
	int rc = 0;

	dev_dbg(dev,
			"Before wr update ring_type (%d) element (%d) with wr:%d\n",
			ring->type, ring->rd_offset, ring->wr_offset);

	rc = mhi_ep_update_wr_offset(ring);
	if (rc) {
		dev_err(dev, "Error updating write-offset for ring\n");
		return rc;
	}

	/* TODO see if can be deleted */
	/* get the element and invoke the respective callback */
	el = &ring->ring_cache[ring->wr_offset];

	if (ring->type == RING_TYPE_CH) {
		/* notify the clients that there are elements in the ring */
		dev_dbg(dev, "processing channel ring element!");
		rc = mhi_ep_process_ring_element(ring, ring->rd_offset);
		if (rc)
			pr_err("Error fetching elements\n");
		return rc;
	}

	while (ring->rd_offset != ring->wr_offset) {
		rc = mhi_ep_process_ring_element(ring, ring->rd_offset);
		if (rc) {
			dev_err(dev,
				"Error processing ring element (%d)\n",
				ring->rd_offset);
			return rc;
		}

		dev_dbg(dev, "Processing ring rd_offset:%d, wr_offset:%d\n",
			ring->rd_offset, ring->wr_offset);
		mhi_ep_ring_inc_index(ring, ring->rd_offset);
	}

	if (!(ring->rd_offset == ring->wr_offset)) {
		dev_err(dev, "Error with the rd offset/wr offset\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mhi_ep_process_ring);

/* TODO See if we can avoid passing mhi_cntrl */
int mhi_ep_ring_add_element(struct mhi_ep_cntrl *mhi_cntrl, struct mhi_ep_ring *ring,
				union mhi_ep_ring_element_type *element,
				struct event_req *ereq, int size)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	size_t old_offset = 0;
	union mhi_ep_ring_element_type *ring_shadow;
	phys_addr_t ring_shadow_phys;
	size_t ring_size = ring->ring_size * sizeof(union mhi_ep_ring_element_type);
	u32 num_elem = 1;
	u32 num_free_elem;
	int ret;

	ret = mhi_ep_update_wr_offset(ring);
	if (ret) {
		dev_err(dev, "Error updating write pointer\n");
		return ret;
	}

	if (ereq)
		num_elem = size / (sizeof(union mhi_ep_ring_element_type));

	if (ring->rd_offset < ring->wr_offset)
		num_free_elem = ring->wr_offset - ring->rd_offset - 1;
	else
		num_free_elem = ring->ring_size - ring->rd_offset +
				ring->wr_offset - 1;

	if (num_free_elem < num_elem) {
		dev_err(dev, "No space to add %d elem in ring\n",
			num_elem);
		return -EINVAL;
	}

	old_offset = ring->rd_offset;

	if (ereq) {
		ring->rd_offset += num_elem;
		if (ring->rd_offset >= ring->ring_size)
			ring->rd_offset -= ring->ring_size;
	} else
		mhi_ep_ring_inc_index(ring, ring->rd_offset);

	dev_dbg(dev,
		"Writing %d elements, ring old 0x%x, new 0x%x\n",
		num_elem, old_offset, ring->rd_offset);

	/* Update rp */
	ring->ring_ctx->generic.rp = (ring->rd_offset *
		sizeof(union mhi_ep_ring_element_type)) +
		ring->ring_ctx->generic.rbase;

	/* Map the host ring */
	ring_shadow = mhi_cntrl->alloc_addr(mhi_cntrl, &ring_shadow_phys, ring_size);
	if (!ring_shadow) {
		dev_err(dev, "failed to allocate ring_shadow\n");
		return -ENOMEM;
	}

	ret = mhi_cntrl->map_addr(mhi_cntrl, ring_shadow_phys,
				  ring->ring_ctx->generic.rbase, ring_size);
	if (ret) {
		dev_err(dev, "failed to map ring_shadow\n\n");
		goto err_ring_free;
	}

	/* Copy the element to ring */
	if (!ereq)
		memcpy_toio(&ring_shadow[old_offset], element,
			    sizeof(union mhi_ep_ring_element_type));

	mhi_cntrl->unmap_addr(mhi_cntrl, ring_shadow_phys);
	mhi_cntrl->free_addr(mhi_cntrl, ring_shadow_phys, ring_shadow, ring_size);

	/* TODO: Adding multiple ring elements */

	return 0;
err_ring_free:
	mhi_cntrl->free_addr(mhi_cntrl, ring_shadow_phys, ring_shadow, ring_size);

	return ret;
}
EXPORT_SYMBOL(mhi_ep_ring_add_element);

void mhi_ep_ring_init(struct mhi_ep_ring *ring, enum mhi_ep_ring_type type,
			u32 id)
{
	ring->state = RING_STATE_UINT;
	ring->type = type;
	if (ring->type == RING_TYPE_CMD) {
		ring->ring_cb = mhi_ep_process_cmd_ring;
		ring->db_offset_h = CRDB_HIGHER;
		ring->db_offset_l = CRDB_LOWER;
	} else if (ring->type == RING_TYPE_CH) {
		ring->ring_cb = mhi_ep_process_tre_ring;
		ring->db_offset_h = CHDB_HIGHER_n(id);
		ring->db_offset_l = CHDB_LOWER_n(id);
		ring->ch_id = id;
	} else if (ring->type == RING_TYPE_ER) {
		ring->db_offset_h = ERDB_HIGHER_n(id);
		ring->db_offset_l = ERDB_LOWER_n(id);
	}
}

/* TODO See if we can avoid passing mhi_cntrl */
int mhi_ep_ring_start(struct mhi_ep_cntrl *mhi_cntrl, struct mhi_ep_ring *ring,
			union mhi_ep_ring_ctx *ctx)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	size_t wr_offset = 0;
	int ret;

	ring->ring_ctx = ctx;
	ring->mhi_cntrl = mhi_cntrl;
	dev_dbg(dev, "rbase: %llx", ring->ring_ctx->generic.rbase);
	ring->ring_size = mhi_ep_ring_num_elems(ring);

	/* During init, both rp and wp are equal */
	ring->rd_offset = mhi_ep_ring_addr2ofst(ring,
					ring->ring_ctx->generic.rp);
	ring->wr_offset = mhi_ep_ring_addr2ofst(ring,
					ring->ring_ctx->generic.rp);
	ring->state = RING_STATE_IDLE;

	wr_offset = mhi_ep_ring_addr2ofst(ring,
					ring->ring_ctx->generic.wp);

	if (!ring->ring_cache) {
		ring->ring_cache = kcalloc(ring->ring_size,
				sizeof(union mhi_ep_ring_element_type),
				GFP_KERNEL);
		if (!ring->ring_cache) {
			dev_err(dev, "Failed to allocate ring cache\n");
			return -ENOMEM;
		}
	}

	/* TODO: Check this */
	if (ring->type != RING_TYPE_ER || ring->type != RING_TYPE_CH) {
		ret = mhi_ep_cache_ring(ring, wr_offset);
		if (ret)
			return ret;
	}

	ring->wr_offset = wr_offset;

	dev_dbg(dev, "ctx ring_base:0x%x, rp:0x%x, wp:0x%x\n",
			(size_t)ring->ring_ctx->generic.rbase,
			(size_t)ring->ring_ctx->generic.rp,
			(size_t)ring->ring_ctx->generic.wp);

	return 0;
}
EXPORT_SYMBOL(mhi_ep_ring_start);
