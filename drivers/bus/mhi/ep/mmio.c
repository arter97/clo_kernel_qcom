#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mhi_ep.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/platform_device.h>

#include "internal.h"

void mhi_ep_mmio_read(struct mhi_ep_cntrl *mhi_cntrl, u32 offset, u32 *regval)
{
	*regval = readl(mhi_cntrl->mmio + offset);
}
EXPORT_SYMBOL(mhi_ep_mmio_read);

void mhi_ep_mmio_write(struct mhi_ep_cntrl *mhi_cntrl, u32 offset, u32 val)
{
	writel(val, mhi_cntrl->mmio + offset);
}
EXPORT_SYMBOL(mhi_ep_mmio_write);

void mhi_ep_mmio_masked_write(struct mhi_ep_cntrl *mhi_cntrl, u32 offset, u32 mask,
			       u32 shift, u32 val)
{
	u32 regval;

	mhi_ep_mmio_read(mhi_cntrl, offset, &regval);
	regval &= ~mask;
	regval |= ((val << shift) & mask);;
	mhi_ep_mmio_write(mhi_cntrl, offset, regval);
}
EXPORT_SYMBOL(mhi_ep_mmio_masked_write);

int mhi_ep_mmio_masked_read(struct mhi_ep_cntrl *dev, u32 offset,
			     u32 mask, u32 shift, u32 *regval)
{
	mhi_ep_mmio_read(dev, offset, regval);
	*regval &= mask;
	*regval >>= shift;

	return 0;
}
EXPORT_SYMBOL(mhi_ep_mmio_masked_read);

void mhi_ep_mmio_get_mhi_state(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ep_state *state,
				bool *mhi_reset)
{
	u32 regval;

	mhi_ep_mmio_read(mhi_cntrl, MHICTRL, &regval);
	*state = FIELD_GET(MHICTRL_MHISTATE_MASK, regval);
	*mhi_reset = !!FIELD_GET(MHICTRL_RESET_MASK, regval);
}
EXPORT_SYMBOL(mhi_ep_mmio_get_mhi_state);

static void mhi_ep_mmio_mask_set_chdb_int_a7(struct mhi_ep_cntrl *mhi_cntrl,
						u32 chdb_id, bool enable)
{
	u32 chid_mask, chid_idx, chid_shft, val = 0;

	chid_shft = chdb_id % 32;
	chid_mask = BIT(chid_shft);
	chid_idx = chdb_id / 32;

	if (chid_idx >= MHI_MASK_ROWS_CH_EV_DB)
		return;

	if (enable)
		val = 1;

	mhi_ep_mmio_masked_write(mhi_cntrl, MHI_CHDB_INT_MASK_A7_n(chid_idx),
				  chid_mask, chid_shft, val);
	mhi_ep_mmio_read(mhi_cntrl, MHI_CHDB_INT_MASK_A7_n(chid_idx),
			  &mhi_cntrl->chdb[chid_idx].mask);
}

void mhi_ep_mmio_enable_chdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 chdb_id)
{
	mhi_ep_mmio_mask_set_chdb_int_a7(mhi_cntrl, chdb_id, true);
}
EXPORT_SYMBOL(mhi_ep_mmio_enable_chdb_a7);

void mhi_ep_mmio_disable_chdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 chdb_id)
{
	mhi_ep_mmio_mask_set_chdb_int_a7(mhi_cntrl, chdb_id, false);
}
EXPORT_SYMBOL(mhi_ep_mmio_disable_chdb_a7);

static void mhi_ep_mmio_set_erdb_int_a7(struct mhi_ep_cntrl *mhi_cntrl,
					u32 erdb_ch_id, bool enable)
{
	u32 erdb_id_shft, erdb_id_mask, erdb_id_idx, val = 0;

	erdb_id_shft = erdb_ch_id % 32;
	erdb_id_mask = BIT(erdb_id_shft);
	erdb_id_idx = erdb_ch_id / 32;

	if (erdb_id_idx >= MHI_MASK_ROWS_CH_EV_DB)
		return;

	if (enable)
		val = 1;

	mhi_ep_mmio_masked_write(mhi_cntrl, MHI_ERDB_INT_MASK_A7_n(erdb_id_idx),
				  erdb_id_mask, erdb_id_shft, val);
}

void mhi_ep_mmio_enable_erdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 erdb_id)
{
	mhi_ep_mmio_set_erdb_int_a7(mhi_cntrl, erdb_id, true);
}
EXPORT_SYMBOL(mhi_ep_mmio_enable_erdb_a7);

void mhi_ep_mmio_disable_erdb_a7(struct mhi_ep_cntrl *mhi_cntrl, u32 erdb_id)
{
	mhi_ep_mmio_set_erdb_int_a7(mhi_cntrl, erdb_id, false);
}
EXPORT_SYMBOL(mhi_ep_mmio_disable_erdb_a7);

static void mhi_ep_mmio_set_chdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl, bool enable)
{
	u32 val = 0, i = 0;

	if (enable)
		val = MHI_CHDB_INT_MASK_A7_n_EN_ALL;

	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++) {
		mhi_ep_mmio_write(mhi_cntrl, MHI_CHDB_INT_MASK_A7_n(i), val);
		mhi_cntrl->chdb[i].mask = val;
	}
}

void mhi_ep_mmio_enable_chdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_set_chdb_interrupts(mhi_cntrl, true);
}
EXPORT_SYMBOL(mhi_ep_mmio_enable_chdb_interrupts);

void mhi_ep_mmio_mask_chdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_set_chdb_interrupts(mhi_cntrl, false);
}
EXPORT_SYMBOL(mhi_ep_mmio_mask_chdb_interrupts);

void mhi_ep_mmio_read_chdb_status_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	u32 i;

	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++)
		mhi_ep_mmio_read(mhi_cntrl, MHI_CHDB_INT_STATUS_A7_n(i),
				  &mhi_cntrl->chdb[i].status);
}
EXPORT_SYMBOL(mhi_ep_mmio_read_chdb_status_interrupts);

static void mhi_ep_mmio_set_erdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl, bool enable)
{
	u32 val = 0, i;

	if (enable)
		val = MHI_ERDB_INT_MASK_A7_n_EN_ALL;

	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++)
		mhi_ep_mmio_write(mhi_cntrl, MHI_ERDB_INT_MASK_A7_n(i), val);
}

void mhi_ep_mmio_enable_erdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_set_erdb_interrupts(mhi_cntrl, true);
}
EXPORT_SYMBOL(mhi_ep_mmio_enable_erdb_interrupts);

void mhi_ep_mmio_mask_erdb_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_set_erdb_interrupts(mhi_cntrl, false);
}
EXPORT_SYMBOL(mhi_ep_mmio_mask_erdb_interrupts);

void mhi_ep_mmio_read_erdb_status_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	u32 i;

	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++)
		mhi_ep_mmio_read(mhi_cntrl, MHI_ERDB_INT_STATUS_A7_n(i),
				  &mhi_cntrl->evdb[i].status);
}
EXPORT_SYMBOL(mhi_ep_mmio_read_erdb_status_interrupts);

void mhi_ep_mmio_enable_ctrl_interrupt(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_masked_write(mhi_cntrl, MHI_CTRL_INT_MASK_A7,
				  MHI_CTRL_MHICTRL_MASK,
				  MHI_CTRL_MHICTRL_SHFT, 1);
}
EXPORT_SYMBOL(mhi_ep_mmio_enable_ctrl_interrupt);

void mhi_ep_mmio_disable_ctrl_interrupt(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_masked_write(mhi_cntrl, MHI_CTRL_INT_MASK_A7,
				  MHI_CTRL_MHICTRL_MASK,
				  MHI_CTRL_MHICTRL_SHFT, 0);
}
EXPORT_SYMBOL(mhi_ep_mmio_disable_ctrl_interrupt);

void mhi_ep_mmio_enable_cmdb_interrupt(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_masked_write(mhi_cntrl, MHI_CTRL_INT_MASK_A7,
				  MHI_CTRL_CRDB_MASK,
				  MHI_CTRL_CRDB_SHFT, 1);
}
EXPORT_SYMBOL(mhi_ep_mmio_enable_cmdb_interrupt);

void mhi_ep_mmio_disable_cmdb_interrupt(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_masked_write(mhi_cntrl, MHI_CTRL_INT_MASK_A7,
				  MHI_CTRL_CRDB_MASK,
				  MHI_CTRL_CRDB_SHFT, 0);
}
EXPORT_SYMBOL(mhi_ep_mmio_disable_cmdb_interrupt);

void mhi_ep_mmio_mask_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_disable_ctrl_interrupt(mhi_cntrl);
	mhi_ep_mmio_disable_cmdb_interrupt(mhi_cntrl);
	mhi_ep_mmio_mask_chdb_interrupts(mhi_cntrl);
	mhi_ep_mmio_mask_erdb_interrupts(mhi_cntrl);
}
EXPORT_SYMBOL(mhi_ep_mmio_mask_interrupts);

void mhi_ep_mmio_clear_interrupts(struct mhi_ep_cntrl *mhi_cntrl)
{
	u32 i = 0;

	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++)
		mhi_ep_mmio_write(mhi_cntrl, MHI_CHDB_INT_CLEAR_A7_n(i),
				   MHI_CHDB_INT_CLEAR_A7_n_CLEAR_ALL);

	for (i = 0; i < MHI_MASK_ROWS_CH_EV_DB; i++)
		mhi_ep_mmio_write(mhi_cntrl, MHI_ERDB_INT_CLEAR_A7_n(i),
				   MHI_ERDB_INT_CLEAR_A7_n_CLEAR_ALL);

	mhi_ep_mmio_write(mhi_cntrl, MHI_CTRL_INT_CLEAR_A7,
			   MHI_CTRL_INT_MMIO_WR_CLEAR |
			   MHI_CTRL_INT_CRDB_CLEAR |
			   MHI_CTRL_INT_CRDB_MHICTRL_CLEAR);
}
EXPORT_SYMBOL(mhi_ep_mmio_clear_interrupts);

void mhi_ep_mmio_get_chc_base(struct mhi_ep_cntrl *mhi_cntrl)
{
	u32 ccabap_value = 0;

	mhi_ep_mmio_read(mhi_cntrl, CCABAP_HIGHER, &ccabap_value);
	mhi_cntrl->ch_ctx_host_pa = ccabap_value;
	mhi_cntrl->ch_ctx_host_pa <<= 32;

	mhi_ep_mmio_read(mhi_cntrl, CCABAP_LOWER, &ccabap_value);
	mhi_cntrl->ch_ctx_host_pa |= ccabap_value;
}
EXPORT_SYMBOL(mhi_ep_mmio_get_chc_base);

void mhi_ep_mmio_get_erc_base(struct mhi_ep_cntrl *mhi_cntrl)
{
	u32 ecabap_value = 0;

	mhi_ep_mmio_read(mhi_cntrl, ECABAP_HIGHER, &ecabap_value);
	mhi_cntrl->ev_ctx_host_pa = ecabap_value;
	mhi_cntrl->ev_ctx_host_pa <<= 32;

	mhi_ep_mmio_read(mhi_cntrl, ECABAP_LOWER, &ecabap_value);
	mhi_cntrl->ev_ctx_host_pa |= ecabap_value;
}
EXPORT_SYMBOL(mhi_ep_mmio_get_erc_base);

void mhi_ep_mmio_get_crc_base(struct mhi_ep_cntrl *mhi_cntrl)
{
	u32 crcbap_value = 0;

	mhi_ep_mmio_read(mhi_cntrl, CRCBAP_HIGHER, &crcbap_value);
	mhi_cntrl->cmd_ctx_host_pa = crcbap_value;
	mhi_cntrl->cmd_ctx_host_pa <<= 32;

	mhi_ep_mmio_read(mhi_cntrl, CRCBAP_LOWER, &crcbap_value);
	mhi_cntrl->cmd_ctx_host_pa |= crcbap_value;
}
EXPORT_SYMBOL(mhi_ep_mmio_get_crc_base);

void mhi_ep_mmio_get_ch_db(struct mhi_ep_ring *ring, u64 *wr_offset)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	u32 value = 0;

	mhi_ep_mmio_read(mhi_cntrl, ring->db_offset_h, &value);
	*wr_offset = value;
	*wr_offset <<= 32;

	mhi_ep_mmio_read(mhi_cntrl, ring->db_offset_l, &value);

	*wr_offset |= value;
}
EXPORT_SYMBOL(mhi_ep_mmio_get_ch_db);

void mhi_ep_mmio_get_er_db(struct mhi_ep_ring *ring, u64 *wr_offset)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	u32 value = 0;

	mhi_ep_mmio_read(mhi_cntrl, ring->db_offset_h, &value);
	*wr_offset = value;
	*wr_offset <<= 32;

	mhi_ep_mmio_read(mhi_cntrl, ring->db_offset_l, &value);

	*wr_offset |= value;
}
EXPORT_SYMBOL(mhi_ep_mmio_get_er_db);

void mhi_ep_mmio_get_cmd_db(struct mhi_ep_ring *ring, u64 *wr_offset)
{
	struct mhi_ep_cntrl *mhi_cntrl = ring->mhi_cntrl;
	u32 value = 0;

	mhi_ep_mmio_read(mhi_cntrl, ring->db_offset_h, &value);
	*wr_offset = value;
	*wr_offset <<= 32;

	mhi_ep_mmio_read(mhi_cntrl, ring->db_offset_l, &value);
	*wr_offset |= value;
}
EXPORT_SYMBOL(mhi_ep_mmio_get_cmd_db);

void mhi_ep_mmio_set_env(struct mhi_ep_cntrl *mhi_cntrl, u32 value)
{
	mhi_ep_mmio_write(mhi_cntrl, BHI_EXECENV, value);
}
EXPORT_SYMBOL(mhi_ep_mmio_set_env);

void mhi_ep_mmio_clear_reset(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_masked_write(mhi_cntrl, MHICTRL, MHICTRL_RESET_MASK,
				  MHICTRL_RESET_SHIFT, 0);
}
EXPORT_SYMBOL(mhi_ep_mmio_clear_reset);

void mhi_ep_mmio_reset(struct mhi_ep_cntrl *mhi_cntrl)
{
	mhi_ep_mmio_write(mhi_cntrl, MHICTRL, 0);
	mhi_ep_mmio_write(mhi_cntrl, MHISTATUS, 0);
	mhi_ep_mmio_clear_interrupts(mhi_cntrl);
}
EXPORT_SYMBOL(mhi_ep_mmio_reset);

void mhi_ep_mmio_init(struct mhi_ep_cntrl *mhi_cntrl)
{
	int mhi_cfg = 0;

	mhi_ep_mmio_read(mhi_cntrl, MHIREGLEN, &mhi_cntrl->reg_len);
	mhi_ep_mmio_read(mhi_cntrl, CHDBOFF, &mhi_cntrl->chdb_offset);
	mhi_ep_mmio_read(mhi_cntrl, ERDBOFF, &mhi_cntrl->erdb_offset);

	mhi_ep_mmio_read(mhi_cntrl, MHICFG, &mhi_cfg);
	mhi_cntrl->event_rings = FIELD_GET(MHICFG_NER_MASK, mhi_cfg);
	mhi_cntrl->hw_event_rings = FIELD_GET(MHICFG_NHWER_MASK, mhi_cfg);

	mhi_ep_mmio_reset(mhi_cntrl);
}
EXPORT_SYMBOL(mhi_ep_mmio_init);

void mhi_ep_mmio_update_ner(struct mhi_ep_cntrl *mhi_cntrl)
{
	int mhi_cfg = 0;

	mhi_ep_mmio_read(mhi_cntrl, MHICFG, &mhi_cfg);
	mhi_cntrl->event_rings = FIELD_GET(MHICFG_NER_MASK, mhi_cfg);
	mhi_cntrl->hw_event_rings = FIELD_GET(MHICFG_NHWER_MASK, mhi_cfg);
}
EXPORT_SYMBOL(mhi_ep_mmio_update_ner);
