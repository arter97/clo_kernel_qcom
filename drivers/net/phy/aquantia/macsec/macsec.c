// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Marvell International Ltd.
 */

#include "macsec.h"
#include <linux/rtnetlink.h>
#include <asm/unaligned.h>

#include "../aqr_macsec/aqr_macsec.h"
#include "../aquantia.h"
#define AQR_MACSEC_KEY_LEN_128_BIT 16
#define AQR_MACSEC_KEY_LEN_192_BIT 24
#define AQR_MACSEC_KEY_LEN_256_BIT 32

#define AQR_SA_EXPIRED_STATUS_REGISTER_ADDR 0x5060
#define AQR_SA_THRESHOLD_EXPIRED_STATUS_REGISTER_ADDR 0x5062

#define AQR_NUMROWS_EGRESSCTLFRECORD 24
#define AQR_NUMROWS_INGRESSPRECTLFRECORD 24

enum aqr_clear_type {
	/* update HW configuration */
	AQR_CLEAR_HW = BIT(0),
	/* update SW configuration (busy bits, pointers) */
	AQR_CLEAR_SW = BIT(1),
	/* update both HW and SW configuration */
	AQR_CLEAR_ALL = AQR_CLEAR_HW | AQR_CLEAR_SW,
};

static int aqr_clear_txsc(struct phy_device *phydev, const int txsc_idx,
			 enum aqr_clear_type clear_type);
static int aqr_clear_txsa(struct phy_device *phydev, struct aqr_macsec_txsc *aq_txsc,
			 const int sa_num, enum aqr_clear_type clear_type);
static int aqr_clear_rxsc(struct phy_device *phydev, const int rxsc_idx,
			 enum aqr_clear_type clear_type);
static int aqr_clear_rxsa(struct phy_device *phydev, struct aqr_macsec_rxsc *aq_rxsc,
			 const int sa_num, enum aqr_clear_type clear_type);
static int aqr_clear_secy(struct phy_device *phydev, const struct macsec_secy *secy,
			 enum aqr_clear_type clear_type);
static int aqr_apply_macsec_cfg(struct aqr107_priv *priv);
static int aqr_apply_secy_cfg(struct aqr107_priv *priv,
			     const struct macsec_secy *secy);

static void aqr_ether_addr_to_mac(u32 mac[2], unsigned char *emac)
{
	u32 tmp[2] = { 0 };

	memcpy(((u8 *)tmp) + 2, emac, ETH_ALEN);

	mac[0] = swab32(tmp[1]);
	mac[1] = swab32(tmp[0]);
}

/* There's a 1:1 mapping between SecY and TX SC */
static int aqr_get_txsc_idx_from_secy(struct aqr_macsec_cfg *macsec_cfg,
				     const struct macsec_secy *secy)
{
	int i;

	if (unlikely(!secy))
		return -1;

	for (i = 0; i < AQR_MACSEC_MAX_SC; i++) {
		if (macsec_cfg->aq_txsc[i].sw_secy == secy)
			return i;
	}
	return -1;
}

static int aqr_get_rxsc_idx_from_rxsc(struct aqr_macsec_cfg *macsec_cfg,
				     const struct macsec_rx_sc *rxsc)
{
	int i;

	if (unlikely(!rxsc))
		return -1;

	for (i = 0; i < AQR_MACSEC_MAX_SC; i++) {
		if (macsec_cfg->aq_rxsc[i].sw_rxsc == rxsc)
			return i;
	}

	return -1;
}

static int aqr_get_txsc_idx_from_sc_idx(const enum aqr_macsec_sc_sa sc_sa,
				       const int sc_idx)
{
	switch (sc_sa) {
	case aqr_macsec_sa_sc_4sa_8sc:
		return sc_idx >> 2;
	case aqr_macsec_sa_sc_2sa_16sc:
		return sc_idx >> 1;
	case aqr_macsec_sa_sc_1sa_32sc:
		return sc_idx;
	default:
		WARN_ONCE(true, "Invalid sc_sa");
	}
	return -1;
}

/* Rotate keys u32[8] */
static void aqr_rotate_keys(u32 (*key)[8], const int key_len)
{
	u32 tmp[8] = { 0 };

	memcpy(&tmp, key, sizeof(tmp));
	memset(*key, 0, sizeof(*key));

	if (key_len == AQR_MACSEC_KEY_LEN_128_BIT) {
		(*key)[0] = swab32(tmp[3]);
		(*key)[1] = swab32(tmp[2]);
		(*key)[2] = swab32(tmp[1]);
		(*key)[3] = swab32(tmp[0]);
	} else if (key_len == AQR_MACSEC_KEY_LEN_192_BIT) {
		(*key)[0] = swab32(tmp[5]);
		(*key)[1] = swab32(tmp[4]);
		(*key)[2] = swab32(tmp[3]);
		(*key)[3] = swab32(tmp[2]);
		(*key)[4] = swab32(tmp[1]);
		(*key)[5] = swab32(tmp[0]);
	} else if (key_len == AQR_MACSEC_KEY_LEN_256_BIT) {
		(*key)[0] = swab32(tmp[7]);
		(*key)[1] = swab32(tmp[6]);
		(*key)[2] = swab32(tmp[5]);
		(*key)[3] = swab32(tmp[4]);
		(*key)[4] = swab32(tmp[3]);
		(*key)[5] = swab32(tmp[2]);
		(*key)[6] = swab32(tmp[1]);
		(*key)[7] = swab32(tmp[0]);
	} else {
		pr_warn("Rotate_keys: invalid key_len\n");
	}
}

#define STATS_2x32_TO_64(stat_field)                                           \
	(((u64)stat_field[1] << 32) | stat_field[0])

static int aqr_get_macsec_common_stats(struct aqr_port *port,
				      struct aqr_macsec_common_stats *stats)
{
	struct aqr_mss_ingress_common_counters ingress_counters;
	struct aqr_mss_egress_common_counters egress_counters;
	int ret;

	/* MACSEC counters */
	ret = aqr_mss_get_ingress_common_counters(port, &ingress_counters);
	if (unlikely(ret))
		return ret;

	stats->in.ctl_pkts = STATS_2x32_TO_64(ingress_counters.ctl_pkts);
	stats->in.tagged_miss_pkts =
		STATS_2x32_TO_64(ingress_counters.tagged_miss_pkts);
	stats->in.untagged_miss_pkts =
		STATS_2x32_TO_64(ingress_counters.untagged_miss_pkts);
	stats->in.notag_pkts = STATS_2x32_TO_64(ingress_counters.notag_pkts);
	stats->in.untagged_pkts =
		STATS_2x32_TO_64(ingress_counters.untagged_pkts);
	stats->in.bad_tag_pkts =
		STATS_2x32_TO_64(ingress_counters.bad_tag_pkts);
	stats->in.no_sci_pkts = STATS_2x32_TO_64(ingress_counters.no_sci_pkts);
	stats->in.unknown_sci_pkts =
		STATS_2x32_TO_64(ingress_counters.unknown_sci_pkts);
	stats->in.ctrl_prt_pass_pkts =
		STATS_2x32_TO_64(ingress_counters.ctrl_prt_pass_pkts);
	stats->in.unctrl_prt_pass_pkts =
		STATS_2x32_TO_64(ingress_counters.unctrl_prt_pass_pkts);
	stats->in.ctrl_prt_fail_pkts =
		STATS_2x32_TO_64(ingress_counters.ctrl_prt_fail_pkts);
	stats->in.unctrl_prt_fail_pkts =
		STATS_2x32_TO_64(ingress_counters.unctrl_prt_fail_pkts);
	stats->in.too_long_pkts =
		STATS_2x32_TO_64(ingress_counters.too_long_pkts);
	stats->in.igpoc_ctl_pkts =
		STATS_2x32_TO_64(ingress_counters.igpoc_ctl_pkts);
	stats->in.ecc_error_pkts =
		STATS_2x32_TO_64(ingress_counters.ecc_error_pkts);
	stats->in.unctrl_hit_drop_redir =
		STATS_2x32_TO_64(ingress_counters.unctrl_hit_drop_redir);

	ret = aqr_mss_get_egress_common_counters(port, &egress_counters);
	if (unlikely(ret))
		return ret;
	stats->out.ctl_pkts = STATS_2x32_TO_64(egress_counters.ctl_pkt);
	stats->out.unknown_sa_pkts =
		STATS_2x32_TO_64(egress_counters.unknown_sa_pkts);
	stats->out.untagged_pkts =
		STATS_2x32_TO_64(egress_counters.untagged_pkts);
	stats->out.too_long = STATS_2x32_TO_64(egress_counters.too_long);
	stats->out.ecc_error_pkts =
		STATS_2x32_TO_64(egress_counters.ecc_error_pkts);
	stats->out.unctrl_hit_drop_redir =
		STATS_2x32_TO_64(egress_counters.unctrl_hit_drop_redir);

	return 0;
}

static int aqr_get_rxsa_stats(struct aqr_port *port, const int sa_idx,
			     struct aqr_macsec_rx_sa_stats *stats)
{
	struct aqr_mss_ingress_sa_counters i_sa_counters;
	int ret;

	ret = aqr_mss_get_ingress_sa_counters(port, &i_sa_counters, sa_idx);
	if (unlikely(ret))
		return ret;

	stats->untagged_hit_pkts =
		STATS_2x32_TO_64(i_sa_counters.untagged_hit_pkts);
	stats->ctrl_hit_drop_redir_pkts =
		STATS_2x32_TO_64(i_sa_counters.ctrl_hit_drop_redir_pkts);
	stats->not_using_sa = STATS_2x32_TO_64(i_sa_counters.not_using_sa);
	stats->unused_sa = STATS_2x32_TO_64(i_sa_counters.unused_sa);
	stats->not_valid_pkts = STATS_2x32_TO_64(i_sa_counters.not_valid_pkts);
	stats->invalid_pkts = STATS_2x32_TO_64(i_sa_counters.invalid_pkts);
	stats->ok_pkts = STATS_2x32_TO_64(i_sa_counters.ok_pkts);
	stats->late_pkts = STATS_2x32_TO_64(i_sa_counters.late_pkts);
	stats->delayed_pkts = STATS_2x32_TO_64(i_sa_counters.delayed_pkts);
	stats->unchecked_pkts = STATS_2x32_TO_64(i_sa_counters.unchecked_pkts);
	stats->validated_octets =
		STATS_2x32_TO_64(i_sa_counters.validated_octets);
	stats->decrypted_octets =
		STATS_2x32_TO_64(i_sa_counters.decrypted_octets);

	return 0;
}

static int aqr_get_txsa_stats(struct aqr_port *port, const int sa_idx,
			     struct aqr_macsec_tx_sa_stats *stats)
{
	struct aqr_mss_egress_sa_counters e_sa_counters;
	int ret;

	ret = aqr_mss_get_egress_sa_counters(port, &e_sa_counters, sa_idx);
	if (unlikely(ret))
		return ret;

	stats->sa_hit_drop_redirect =
		STATS_2x32_TO_64(e_sa_counters.sa_hit_drop_redirect);
	stats->sa_protected2_pkts =
		STATS_2x32_TO_64(e_sa_counters.sa_protected2_pkts);
	stats->sa_protected_pkts =
		STATS_2x32_TO_64(e_sa_counters.sa_protected_pkts);
	stats->sa_encrypted_pkts =
		STATS_2x32_TO_64(e_sa_counters.sa_encrypted_pkts);

	return 0;
}

static int aqr_get_txsa_next_pn(struct aqr_port *port, const int sa_idx, u32 *pn)
{
	struct aqr_mss_egress_sa_record sa_rec;
	int ret;

	ret = aqr_mss_get_egress_sa_record(port, &sa_rec, sa_idx);
	if (likely(!ret))
		*pn = sa_rec.next_pn;

	return ret;
}

static int aqr_get_rxsa_next_pn(struct aqr_port *port, const int sa_idx, u32 *pn)
{
	struct aqr_mss_ingress_sa_record sa_rec;
	int ret;

	ret = aqr_mss_get_ingress_sa_record(port, &sa_rec, sa_idx);
	if (likely(!ret))
		*pn = (!sa_rec.sat_nextpn) ? sa_rec.next_pn : 0;

	return ret;
}

static int aqr_get_txsc_stats(struct aqr_port *port, const int sc_idx,
			     struct aqr_macsec_tx_sc_stats *stats)
{
	struct aqr_mss_egress_sc_counters e_sc_counters;
	int ret;

	ret = aqr_mss_get_egress_sc_counters(port, &e_sc_counters, sc_idx);
	if (unlikely(ret))
		return ret;

	stats->sc_protected_pkts =
		STATS_2x32_TO_64(e_sc_counters.sc_protected_pkts);
	stats->sc_encrypted_pkts =
		STATS_2x32_TO_64(e_sc_counters.sc_encrypted_pkts);
	stats->sc_protected_octets =
		STATS_2x32_TO_64(e_sc_counters.sc_protected_octets);
	stats->sc_encrypted_octets =
		STATS_2x32_TO_64(e_sc_counters.sc_encrypted_octets);

	return 0;
}

static int aqr_mdo_dev_open(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct phy_device *phydev = ctx->phydev;
	int ret = 0;

	if (ctx->prepare)
		return 0;

	/*This function was never called before*/
	/*Found during debug*/
	aqr_macsec_enable(phydev);
	printk(KERN_ERR "%s %d", "aquantia PHYDEV link", phydev->link);
	if (phydev->link)
		ret = aqr_apply_secy_cfg(priv, ctx->secy);

	return ret;
}

static int aqr_mdo_dev_stop(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	int i;

	if (ctx->prepare)
		return 0;

	for (i = 0; i < AQR_MACSEC_MAX_SC; i++) {
		if (priv->macsec_cfg.txsc_idx_busy & BIT(i))
			aqr_clear_secy(ctx->phydev, priv->macsec_cfg.aq_txsc[i].sw_secy,
				      AQR_CLEAR_HW);
	}

	return 0;
}

static int aqr_set_txsc(struct aqr107_priv *priv, const int txsc_idx)
{
	struct aqr_macsec_txsc *aq_txsc = &priv->macsec_cfg.aq_txsc[txsc_idx];
	struct aqr_mss_egress_class_record tx_class_rec = { 0 };
	const struct macsec_secy *secy = aq_txsc->sw_secy;
	struct aqr_mss_egress_sc_record sc_rec = { 0 };
	unsigned int sc_idx = aq_txsc->hw_sc_idx;
	struct aqr_port *port = &priv->port;
	int ret = 0;

	aqr_ether_addr_to_mac(tx_class_rec.mac_sa, secy->netdev->dev_addr);

	put_unaligned_be64((__force u64)secy->sci, tx_class_rec.sci);
	tx_class_rec.sci_mask = 0;

	tx_class_rec.sa_mask = 0x3f;

	tx_class_rec.action = 0; /* forward to SA/SC table */
	tx_class_rec.valid = 1;

	tx_class_rec.sc_idx = sc_idx;

	tx_class_rec.sc_sa = priv->macsec_cfg.sc_sa;

	ret = aqr_mss_set_egress_class_record(port, &tx_class_rec, txsc_idx);
	if (ret)
		return ret;

	sc_rec.protect = secy->protect_frames;
	if (secy->tx_sc.encrypt)
		sc_rec.tci |= BIT(1);
	if (secy->tx_sc.scb)
		sc_rec.tci |= BIT(2);
	if (secy->tx_sc.send_sci)
		sc_rec.tci |= BIT(3);
	if (secy->tx_sc.end_station)
		sc_rec.tci |= BIT(4);
	/* The C bit is clear if and only if the Secure Data is
	 * exactly the same as the User Data and the ICV is 16 octets long.
	 */
	if (!(secy->icv_len == 16 && !secy->tx_sc.encrypt))
		sc_rec.tci |= BIT(0);

	sc_rec.an_roll = 0;

	switch (secy->key_len) {
	case AQR_MACSEC_KEY_LEN_128_BIT:
		sc_rec.sak_len = 0;
		break;
	case AQR_MACSEC_KEY_LEN_192_BIT:
		sc_rec.sak_len = 1;
		break;
	case AQR_MACSEC_KEY_LEN_256_BIT:
		sc_rec.sak_len = 2;
		break;
	default:
		WARN_ONCE(true, "Invalid sc_sa");
		return -EINVAL;
	}

	sc_rec.curr_an = secy->tx_sc.encoding_sa;
	sc_rec.valid = 1;
	sc_rec.fresh = 1;

	return aqr_mss_set_egress_sc_record(port, &sc_rec, sc_idx);
}

static u32 aqr_sc_idx_max(const enum aqr_macsec_sc_sa sc_sa)
{
	u32 result = 0;

	switch (sc_sa) {
	case aqr_macsec_sa_sc_4sa_8sc:
		result = 8;
		break;
	case aqr_macsec_sa_sc_2sa_16sc:
		result = 16;
		break;
	case aqr_macsec_sa_sc_1sa_32sc:
		result = 32;
		break;
	default:
		break;
	}

	return result;
}

static u32 aqr_to_hw_sc_idx(const u32 sc_idx, const enum aqr_macsec_sc_sa sc_sa)
{
	switch (sc_sa) {
	case aqr_macsec_sa_sc_4sa_8sc:
		return sc_idx << 2;
	case aqr_macsec_sa_sc_2sa_16sc:
		return sc_idx << 1;
	case aqr_macsec_sa_sc_1sa_32sc:
		return sc_idx;
	default:
		WARN_ONCE(true, "Invalid sc_sa");
	}

	return sc_idx;
}

static enum aqr_macsec_sc_sa sc_sa_from_num_an(const int num_an)
{
	enum aqr_macsec_sc_sa sc_sa = aqr_macsec_sa_sc_not_used;

	switch (num_an) {
	case 4:
		sc_sa = aqr_macsec_sa_sc_4sa_8sc;
		break;
	case 2:
		sc_sa = aqr_macsec_sa_sc_2sa_16sc;
		break;
	case 1:
		sc_sa = aqr_macsec_sa_sc_1sa_32sc;
		break;
	default:
		break;
	}

	return sc_sa;
}

static int aqr_mdo_add_secy(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct phy_device *phydev = ctx->phydev;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	const struct macsec_secy *secy = ctx->secy;
	enum aqr_macsec_sc_sa sc_sa;
	u32 txsc_idx;
	int ret = 0;
	printk(KERN_ERR "%s %d", "aqr_mdo_add_secy entry \n", ret);

	if (secy->xpn) {
		printk(KERN_ERR "%s %d", "aqr_mdo_add_secy EOPNOTSUPP \n", ret);
		return -EOPNOTSUPP;
	}
	sc_sa = sc_sa_from_num_an(MACSEC_NUM_AN);
	if (sc_sa == aqr_macsec_sa_sc_not_used) {
		printk(KERN_ERR "%s %d", "aqr_mdo_add_secy EINVAL \n", ret);
		return -EINVAL;
	}
	if (hweight32(cfg->txsc_idx_busy) >= aqr_sc_idx_max(sc_sa)) {
		printk(KERN_ERR "%s %d", "aqr_mdo_add_secy hweight32 ENOSPC \n", ret);
		return -ENOSPC;
	}
	txsc_idx = ffz(cfg->txsc_idx_busy);
	if (txsc_idx == AQR_MACSEC_MAX_SC) {
		printk(KERN_ERR "%s %d", "aqr_mdo_add_secy AQR_MACSEC_MAX_SC ENOSPC \n", ret);
		return -ENOSPC;
	}
	if (ctx->prepare) {
		printk(KERN_ERR "%s %d", "aqr_mdo_add_secy ctx->prepare \n", ret);
		return 0;
	}

	cfg->sc_sa = sc_sa;
	cfg->aq_txsc[txsc_idx].hw_sc_idx = aqr_to_hw_sc_idx(txsc_idx, sc_sa);
	cfg->aq_txsc[txsc_idx].sw_secy = secy;

	if (phydev->link && netif_running(secy->netdev))
		ret = aqr_set_txsc(priv, txsc_idx);

	set_bit(txsc_idx, &cfg->txsc_idx_busy);

	return ret;
}

static int aqr_mdo_upd_secy(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	const struct macsec_secy *secy = ctx->secy;
	struct phy_device *phydev = ctx->phydev;
	int txsc_idx;
	int ret = 0;

	txsc_idx = aqr_get_txsc_idx_from_secy(&priv->macsec_cfg, secy);
	if (txsc_idx < 0)
		return -ENOENT;

	if (ctx->prepare)
		return 0;

	if (phydev->link && netif_running(secy->netdev))
		ret = aqr_set_txsc(priv, txsc_idx);

	return ret;
}

static int aqr_clear_txsc(struct phy_device *phydev, const int txsc_idx,
			 enum aqr_clear_type clear_type)
{
	struct aqr107_priv *priv = phydev->priv;
	struct aqr_macsec_txsc *tx_sc = &priv->macsec_cfg.aq_txsc[txsc_idx];
	struct aqr_mss_egress_class_record tx_class_rec = { 0 };
	struct aqr_mss_egress_sc_record sc_rec = { 0 };
	struct aqr_port *port = &priv->port;
	int ret = 0;
	int sa_num;

	for_each_set_bit(sa_num, &tx_sc->tx_sa_idx_busy, AQR_MACSEC_MAX_SA) {
		ret = aqr_clear_txsa(phydev, tx_sc, sa_num, clear_type);
		if (ret)
			return ret;
	}

	if (clear_type & AQR_CLEAR_HW) {
		ret = aqr_mss_set_egress_class_record(port, &tx_class_rec,
						     txsc_idx);
		if (ret)
			return ret;

		sc_rec.fresh = 1;
		ret = aqr_mss_set_egress_sc_record(port, &sc_rec,
						  tx_sc->hw_sc_idx);
		if (ret)
			return ret;
	}

	if (clear_type & AQR_CLEAR_SW) {
		clear_bit(txsc_idx, &priv->macsec_cfg.txsc_idx_busy);
		priv->macsec_cfg.aq_txsc[txsc_idx].sw_secy = NULL;
	}

	return ret;
}

static int aqr_mdo_del_secy(struct macsec_context *ctx)
{
	int ret = 0;

	if (ctx->prepare)
		return 0;

	ret = aqr_clear_secy(ctx->phydev, ctx->secy, AQR_CLEAR_ALL);

	return ret;
}

static int aqr_update_txsa(struct aqr107_priv *priv, const unsigned int sc_idx,
			  const struct macsec_secy *secy,
			  const struct macsec_tx_sa *tx_sa,
			  const unsigned char *key, const unsigned char an)
{
	const u32 next_pn = tx_sa->next_pn_halves.lower;
	struct aqr_mss_egress_sakey_record key_rec;
	const unsigned int sa_idx = sc_idx | an;
	struct aqr_mss_egress_sa_record sa_rec;
	struct aqr_port *port = &priv->port;
	int ret = 0;

	memset(&sa_rec, 0, sizeof(sa_rec));
	sa_rec.valid = tx_sa->active;
	sa_rec.fresh = 1;
	sa_rec.next_pn = next_pn;

	ret = aqr_mss_set_egress_sa_record(port, &sa_rec, sa_idx);
	if (ret)
		return ret;

	if (!key)
		return ret;

	memset(&key_rec, 0, sizeof(key_rec));
	memcpy(&key_rec.key, key, secy->key_len);

	aqr_rotate_keys(&key_rec.key, secy->key_len);

	ret = aqr_mss_set_egress_sakey_record(port, &key_rec, sa_idx);

	return ret;
}

static int aqr_mdo_add_txsa(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	struct phy_device *phydev = ctx->phydev;
	const struct macsec_secy *secy = ctx->secy;
	struct aqr_macsec_txsc *aq_txsc;
	int txsc_idx;
	int ret = 0;

	txsc_idx = aqr_get_txsc_idx_from_secy(cfg, secy);
	if (txsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	aq_txsc = &cfg->aq_txsc[txsc_idx];
	set_bit(ctx->sa.assoc_num, &aq_txsc->tx_sa_idx_busy);

	memcpy(aq_txsc->tx_sa_key[ctx->sa.assoc_num], ctx->sa.key,
	       secy->key_len);

	if (phydev->link && netif_running(secy->netdev))
		ret = aqr_update_txsa(priv, aq_txsc->hw_sc_idx, secy,
				     ctx->sa.tx_sa, ctx->sa.key,
				     ctx->sa.assoc_num);

	return ret;
}

static int aqr_mdo_upd_txsa(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	const struct macsec_secy *secy = ctx->secy;
	struct phy_device *phydev = ctx->phydev;
	struct aqr_macsec_txsc *aq_txsc;
	int txsc_idx;
	int ret = 0;

	txsc_idx = aqr_get_txsc_idx_from_secy(cfg, secy);
	if (txsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	aq_txsc = &cfg->aq_txsc[txsc_idx];

	if (phydev->link && netif_running(secy->netdev))
		ret = aqr_update_txsa(priv, aq_txsc->hw_sc_idx, secy,
				     ctx->sa.tx_sa, NULL, ctx->sa.assoc_num);

	return ret;
}

static int aqr_clear_txsa(struct phy_device *phydev, struct aqr_macsec_txsc *aq_txsc,
			 const int sa_num, enum aqr_clear_type clear_type)
{
	const int sa_idx = aq_txsc->hw_sc_idx | sa_num;
	struct aqr107_priv *priv = phydev->priv;
	struct aqr_port *port = &priv->port;
	int ret = 0;

	if (clear_type & AQR_CLEAR_SW)
		clear_bit(sa_num, &aq_txsc->tx_sa_idx_busy);

	if ((clear_type & AQR_CLEAR_HW) && phydev->link) {
		struct aqr_mss_egress_sakey_record key_rec;
		struct aqr_mss_egress_sa_record sa_rec;

		memset(&sa_rec, 0, sizeof(sa_rec));
		sa_rec.fresh = 1;

		ret = aqr_mss_set_egress_sa_record(port, &sa_rec, sa_idx);
		if (ret)
			return ret;

		memset(&key_rec, 0, sizeof(key_rec));
		return aqr_mss_set_egress_sakey_record(port, &key_rec, sa_idx);
	}

	return 0;
}

static int aqr_mdo_del_txsa(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	int txsc_idx;
	int ret = 0;

	txsc_idx = aqr_get_txsc_idx_from_secy(cfg, ctx->secy);
	if (txsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	ret = aqr_clear_txsa(ctx->phydev, &cfg->aq_txsc[txsc_idx], ctx->sa.assoc_num,
			    AQR_CLEAR_ALL);

	return ret;
}

static int aqr_rxsc_validate_frames(const enum macsec_validation_type validate)
{
	switch (validate) {
	case MACSEC_VALIDATE_DISABLED:
		return 2;
	case MACSEC_VALIDATE_CHECK:
		return 1;
	case MACSEC_VALIDATE_STRICT:
		return 0;
	default:
		WARN_ONCE(true, "Invalid validation type");
	}

	return 0;
}

static int aqr_set_rxsc(struct aqr107_priv *priv, const u32 rxsc_idx)
{
	const struct aqr_macsec_rxsc *aq_rxsc =
		&priv->macsec_cfg.aq_rxsc[rxsc_idx];
	struct aqr_mss_ingress_preclass_record pre_class_record;
	const struct macsec_rx_sc *rx_sc = aq_rxsc->sw_rxsc;
	const struct macsec_secy *secy = aq_rxsc->sw_secy;
	const u32 hw_sc_idx = aq_rxsc->hw_sc_idx;
	struct aqr_mss_ingress_sc_record sc_record;
	struct aqr_port *port = &priv->port;
	int ret = 0;

	memset(&pre_class_record, 0, sizeof(pre_class_record));
	put_unaligned_be64((__force u64)rx_sc->sci, pre_class_record.sci);
	pre_class_record.sci_mask = 0xff;
	/* match all MACSEC ethertype packets */
	pre_class_record.eth_type = ETH_P_MACSEC;
	pre_class_record.eth_type_mask = 0x3;

	aqr_ether_addr_to_mac(pre_class_record.mac_sa, (char *)&rx_sc->sci);
	pre_class_record.sa_mask = 0x3f;

	pre_class_record.an_mask = priv->macsec_cfg.sc_sa;
	pre_class_record.sc_idx = hw_sc_idx;
	/* strip SecTAG & forward for decryption */
	pre_class_record.action = 0x0;
	pre_class_record.valid = 1;

	ret = aqr_mss_set_ingress_preclass_record(port, &pre_class_record,
						 2 * rxsc_idx + 1);
	if (ret)
		return ret;

	/* If SCI is absent, then match by SA alone */
	pre_class_record.sci_mask = 0;
	pre_class_record.sci_from_table = 1;

	ret = aqr_mss_set_ingress_preclass_record(port, &pre_class_record,
						 2 * rxsc_idx);
	if (ret)
		return ret;

	memset(&sc_record, 0, sizeof(sc_record));
	sc_record.validate_frames =
		aqr_rxsc_validate_frames(secy->validate_frames);
	if (secy->replay_protect) {
		sc_record.replay_protect = 1;
		sc_record.anti_replay_window = secy->replay_window;
	}
	sc_record.valid = 1;
	sc_record.fresh = 1;

	ret = aqr_mss_get_ingress_sc_record(port, &sc_record, hw_sc_idx);
	if (ret)
		return ret;

	return ret;
}

static int aqr_mdo_add_rxsc(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	const u32 rxsc_idx_max = aqr_sc_idx_max(cfg->sc_sa);
	u32 rxsc_idx;
	int ret = 0;

	if (hweight32(cfg->rxsc_idx_busy) >= rxsc_idx_max)
		return -ENOSPC;

	rxsc_idx = ffz(cfg->rxsc_idx_busy);
	if (rxsc_idx >= rxsc_idx_max)
		return -ENOSPC;

	if (ctx->prepare)
		return 0;

	cfg->aq_rxsc[rxsc_idx].hw_sc_idx = aqr_to_hw_sc_idx(rxsc_idx,
							   cfg->sc_sa);
	cfg->aq_rxsc[rxsc_idx].sw_secy = ctx->secy;
	cfg->aq_rxsc[rxsc_idx].sw_rxsc = ctx->rx_sc;

	if (ctx->phydev->link && netif_running(ctx->secy->netdev))
		ret = aqr_set_rxsc(priv, rxsc_idx);

	if (ret < 0)
		return ret;

	set_bit(rxsc_idx, &cfg->rxsc_idx_busy);

	return 0;
}

static int aqr_mdo_upd_rxsc(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	int rxsc_idx;
	int ret = 0;

	rxsc_idx = aqr_get_rxsc_idx_from_rxsc(&priv->macsec_cfg, ctx->rx_sc);
	if (rxsc_idx < 0)
		return -ENOENT;

	if (ctx->prepare)
		return 0;

	if (ctx->phydev->link && netif_running(ctx->secy->netdev))
		ret = aqr_set_rxsc(priv, rxsc_idx);

	return ret;
}

static int aqr_clear_rxsc(struct phy_device *phydev, const int rxsc_idx,
			 enum aqr_clear_type clear_type)
{
	struct aqr107_priv *priv = phydev->priv;
	struct aqr_macsec_rxsc *rx_sc = &priv->macsec_cfg.aq_rxsc[rxsc_idx];
	struct aqr_port *port = &priv->port;
	int ret = 0;
	int sa_num;

	for_each_set_bit (sa_num, &rx_sc->rx_sa_idx_busy, AQR_MACSEC_MAX_SA) {
		ret = aqr_clear_rxsa(phydev, rx_sc, sa_num, clear_type);
		if (ret)
			return ret;
	}

	if (clear_type & AQR_CLEAR_HW) {
		struct aqr_mss_ingress_preclass_record pre_class_record;
		struct aqr_mss_ingress_sc_record sc_record;

		memset(&pre_class_record, 0, sizeof(pre_class_record));
		memset(&sc_record, 0, sizeof(sc_record));

		ret = aqr_mss_set_ingress_preclass_record(port, &pre_class_record,
							 2 * rxsc_idx);
		if (ret)
			return ret;

		ret = aqr_mss_set_ingress_preclass_record(port, &pre_class_record,
							 2 * rxsc_idx + 1);
		if (ret)
			return ret;

		sc_record.fresh = 1;
		ret = aqr_mss_set_ingress_sc_record(port, &sc_record,
						   rx_sc->hw_sc_idx);
		if (ret)
			return ret;
	}

	if (clear_type & AQR_CLEAR_SW) {
		clear_bit(rxsc_idx, &priv->macsec_cfg.rxsc_idx_busy);
		rx_sc->sw_secy = NULL;
		rx_sc->sw_rxsc = NULL;
	}

	return ret;
}

static int aqr_mdo_del_rxsc(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	enum aqr_clear_type clear_type = AQR_CLEAR_SW;
	int rxsc_idx;
	int ret = 0;

	rxsc_idx = aqr_get_rxsc_idx_from_rxsc(&priv->macsec_cfg, ctx->rx_sc);
	if (rxsc_idx < 0)
		return -ENOENT;

	if (ctx->prepare)
		return 0;

	if (ctx->phydev->link)
		clear_type = AQR_CLEAR_ALL;

	ret = aqr_clear_rxsc(ctx->phydev, rxsc_idx, clear_type);

	return ret;
}

static int aqr_update_rxsa(struct aqr107_priv *priv, const unsigned int sc_idx,
			  const struct macsec_secy *secy,
			  const struct macsec_rx_sa *rx_sa,
			  const unsigned char *key, const unsigned char an)
{
	struct aqr_mss_ingress_sakey_record sa_key_record;
	const u32 next_pn = rx_sa->next_pn_halves.lower;
	struct aqr_mss_ingress_sa_record sa_record;
	struct aqr_port *port = &priv->port;
	const int sa_idx = sc_idx | an;
	int ret = 0;

	memset(&sa_record, 0, sizeof(sa_record));
	sa_record.valid = rx_sa->active;
	sa_record.fresh = 1;
	sa_record.next_pn = next_pn;

	ret = aqr_mss_set_ingress_sa_record(port, &sa_record, sa_idx);
	if (ret)
		return ret;

	if (!key)
		return ret;

	memset(&sa_key_record, 0, sizeof(sa_key_record));
	memcpy(&sa_key_record.key, key, secy->key_len);

	switch (secy->key_len) {
	case AQR_MACSEC_KEY_LEN_128_BIT:
		sa_key_record.key_len = 0;
		break;
	case AQR_MACSEC_KEY_LEN_192_BIT:
		sa_key_record.key_len = 1;
		break;
	case AQR_MACSEC_KEY_LEN_256_BIT:
		sa_key_record.key_len = 2;
		break;
	default:
		return -1;
	}

	aqr_rotate_keys(&sa_key_record.key, secy->key_len);

	ret = aqr_mss_set_ingress_sakey_record(port, &sa_key_record, sa_idx);

	return ret;
}

static int aqr_mdo_add_rxsa(struct macsec_context *ctx)
{
	const struct macsec_rx_sc *rx_sc = ctx->sa.rx_sa->sc;
	struct aqr107_priv *priv = ctx->phydev->priv;
	const struct macsec_secy *secy = ctx->secy;
	struct aqr_macsec_rxsc *aq_rxsc;
	int rxsc_idx;
	int ret = 0;

	rxsc_idx = aqr_get_rxsc_idx_from_rxsc(&priv->macsec_cfg, rx_sc);
	if (rxsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	aq_rxsc = &priv->macsec_cfg.aq_rxsc[rxsc_idx];
	set_bit(ctx->sa.assoc_num, &aq_rxsc->rx_sa_idx_busy);

	memcpy(aq_rxsc->rx_sa_key[ctx->sa.assoc_num], ctx->sa.key,
	       secy->key_len);

	if (ctx->phydev->link && netif_running(secy->netdev))
		ret = aqr_update_rxsa(priv, aq_rxsc->hw_sc_idx, secy,
				     ctx->sa.rx_sa, ctx->sa.key,
				     ctx->sa.assoc_num);

	return ret;
}

static int aqr_mdo_upd_rxsa(struct macsec_context *ctx)
{
	const struct macsec_rx_sc *rx_sc = ctx->sa.rx_sa->sc;
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	const struct macsec_secy *secy = ctx->secy;
	int rxsc_idx;
	int ret = 0;

	rxsc_idx = aqr_get_rxsc_idx_from_rxsc(cfg, rx_sc);
	if (rxsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	if (ctx->phydev->link && netif_running(secy->netdev))
		ret = aqr_update_rxsa(priv, cfg->aq_rxsc[rxsc_idx].hw_sc_idx,
				     secy, ctx->sa.rx_sa, NULL,
				     ctx->sa.assoc_num);

	return ret;
}

static int aqr_clear_rxsa(struct phy_device *phydev, struct aqr_macsec_rxsc *aq_rxsc,
			 const int sa_num, enum aqr_clear_type clear_type)
{
	struct aqr107_priv *priv = phydev->priv;
	int sa_idx = aq_rxsc->hw_sc_idx | sa_num;
	struct aqr_port *port = &priv->port;
	int ret = 0;

	if (clear_type & AQR_CLEAR_SW)
		clear_bit(sa_num, &aq_rxsc->rx_sa_idx_busy);

	if ((clear_type & AQR_CLEAR_HW) && phydev->link) {
		struct aqr_mss_ingress_sakey_record sa_key_record;
		struct aqr_mss_ingress_sa_record sa_record;

		memset(&sa_key_record, 0, sizeof(sa_key_record));
		memset(&sa_record, 0, sizeof(sa_record));
		sa_record.fresh = 1;
		ret = aqr_mss_set_ingress_sa_record(port, &sa_record, sa_idx);
		if (ret)
			return ret;

		return aqr_mss_set_ingress_sakey_record(port, &sa_key_record,
						       sa_idx);
	}

	return ret;
}

static int aqr_mdo_del_rxsa(struct macsec_context *ctx)
{
	const struct macsec_rx_sc *rx_sc = ctx->sa.rx_sa->sc;
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	int rxsc_idx;
	int ret = 0;

	rxsc_idx = aqr_get_rxsc_idx_from_rxsc(cfg, rx_sc);
	if (rxsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	ret = aqr_clear_rxsa(ctx->phydev, &cfg->aq_rxsc[rxsc_idx], ctx->sa.assoc_num,
			    AQR_CLEAR_ALL);

	return ret;
}

static int aqr_mdo_get_dev_stats(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_common_stats *stats = &priv->macsec_cfg.stats;
	struct aqr_port *port = &priv->port;

	if (ctx->prepare)
		return 0;

	aqr_get_macsec_common_stats(port, stats);

	ctx->stats.dev_stats->OutPktsUntagged = stats->out.untagged_pkts;
	ctx->stats.dev_stats->InPktsUntagged = stats->in.untagged_pkts;
	ctx->stats.dev_stats->OutPktsTooLong = stats->out.too_long;
	ctx->stats.dev_stats->InPktsNoTag = stats->in.notag_pkts;
	ctx->stats.dev_stats->InPktsBadTag = stats->in.bad_tag_pkts;
	ctx->stats.dev_stats->InPktsUnknownSCI = stats->in.unknown_sci_pkts;
	ctx->stats.dev_stats->InPktsNoSCI = stats->in.no_sci_pkts;
	ctx->stats.dev_stats->InPktsOverrun = 0;

	return 0;
}

static int aqr_mdo_get_tx_sc_stats(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_tx_sc_stats *stats;
	struct aqr_port *port = &priv->port;
	struct aqr_macsec_txsc *aq_txsc;
	int txsc_idx;

	txsc_idx = aqr_get_txsc_idx_from_secy(&priv->macsec_cfg, ctx->secy);
	if (txsc_idx < 0)
		return -ENOENT;

	if (ctx->prepare)
		return 0;

	aq_txsc = &priv->macsec_cfg.aq_txsc[txsc_idx];
	stats = &aq_txsc->stats;
	aqr_get_txsc_stats(port, aq_txsc->hw_sc_idx, stats);

	ctx->stats.tx_sc_stats->OutPktsProtected = stats->sc_protected_pkts;
	ctx->stats.tx_sc_stats->OutPktsEncrypted = stats->sc_encrypted_pkts;
	ctx->stats.tx_sc_stats->OutOctetsProtected = stats->sc_protected_octets;
	ctx->stats.tx_sc_stats->OutOctetsEncrypted = stats->sc_encrypted_octets;

	return 0;
}

static int aqr_mdo_get_tx_sa_stats(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	struct aqr_macsec_tx_sa_stats *stats;
	struct aqr_port *port = &priv->port;
	const struct macsec_secy *secy;
	struct aqr_macsec_txsc *aq_txsc;
	struct macsec_tx_sa *tx_sa;
	unsigned int sa_idx;
	int txsc_idx;
	u32 next_pn;
	int ret;

	txsc_idx = aqr_get_txsc_idx_from_secy(cfg, ctx->secy);
	if (txsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	aq_txsc = &cfg->aq_txsc[txsc_idx];
	sa_idx = aq_txsc->hw_sc_idx | ctx->sa.assoc_num;
	stats = &aq_txsc->tx_sa_stats[ctx->sa.assoc_num];
	ret = aqr_get_txsa_stats(port, sa_idx, stats);
	if (ret)
		return ret;

	ctx->stats.tx_sa_stats->OutPktsProtected = stats->sa_protected_pkts;
	ctx->stats.tx_sa_stats->OutPktsEncrypted = stats->sa_encrypted_pkts;

	secy = aq_txsc->sw_secy;
	tx_sa = rcu_dereference_bh(secy->tx_sc.sa[ctx->sa.assoc_num]);
	ret = aqr_get_txsa_next_pn(port, sa_idx, &next_pn);
	if (ret == 0) {
		spin_lock_bh(&tx_sa->lock);
		tx_sa->next_pn = next_pn;
		spin_unlock_bh(&tx_sa->lock);
	}

	return ret;
}

static int aqr_mdo_get_rx_sc_stats(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	struct aqr_macsec_rx_sa_stats *stats;
	struct aqr_port *port = &priv->port;
	struct aqr_macsec_rxsc *aq_rxsc;
	unsigned int sa_idx;
	int rxsc_idx;
	int ret = 0;
	int i;

	rxsc_idx = aqr_get_rxsc_idx_from_rxsc(cfg, ctx->rx_sc);
	if (rxsc_idx < 0)
		return -ENOENT;

	if (ctx->prepare)
		return 0;

	aq_rxsc = &cfg->aq_rxsc[rxsc_idx];
	for (i = 0; i < MACSEC_NUM_AN; i++) {
		if (!test_bit(i, &aq_rxsc->rx_sa_idx_busy))
			continue;

		stats = &aq_rxsc->rx_sa_stats[i];
		sa_idx = aq_rxsc->hw_sc_idx | i;
		ret = aqr_get_rxsa_stats(port, sa_idx, stats);
		if (ret)
			break;

		ctx->stats.rx_sc_stats->InOctetsValidated +=
			stats->validated_octets;
		ctx->stats.rx_sc_stats->InOctetsDecrypted +=
			stats->decrypted_octets;
		ctx->stats.rx_sc_stats->InPktsUnchecked +=
			stats->unchecked_pkts;
		ctx->stats.rx_sc_stats->InPktsDelayed += stats->delayed_pkts;
		ctx->stats.rx_sc_stats->InPktsOK += stats->ok_pkts;
		ctx->stats.rx_sc_stats->InPktsInvalid += stats->invalid_pkts;
		ctx->stats.rx_sc_stats->InPktsLate += stats->late_pkts;
		ctx->stats.rx_sc_stats->InPktsNotValid += stats->not_valid_pkts;
		ctx->stats.rx_sc_stats->InPktsNotUsingSA += stats->not_using_sa;
		ctx->stats.rx_sc_stats->InPktsUnusedSA += stats->unused_sa;
	}

	return ret;
}

static int aqr_mdo_get_rx_sa_stats(struct macsec_context *ctx)
{
	struct aqr107_priv *priv = ctx->phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	struct aqr_macsec_rx_sa_stats *stats;
	struct aqr_port *port = &priv->port;
	struct aqr_macsec_rxsc *aq_rxsc;
	struct macsec_rx_sa *rx_sa;
	unsigned int sa_idx;
	int rxsc_idx;
	u32 next_pn;
	int ret;

	rxsc_idx = aqr_get_rxsc_idx_from_rxsc(cfg, ctx->rx_sc);
	if (rxsc_idx < 0)
		return -EINVAL;

	if (ctx->prepare)
		return 0;

	aq_rxsc = &cfg->aq_rxsc[rxsc_idx];
	stats = &aq_rxsc->rx_sa_stats[ctx->sa.assoc_num];
	sa_idx = aq_rxsc->hw_sc_idx | ctx->sa.assoc_num;
	ret = aqr_get_rxsa_stats(port, sa_idx, stats);
	if (ret)
		return ret;

	ctx->stats.rx_sa_stats->InPktsOK = stats->ok_pkts;
	ctx->stats.rx_sa_stats->InPktsInvalid = stats->invalid_pkts;
	ctx->stats.rx_sa_stats->InPktsNotValid = stats->not_valid_pkts;
	ctx->stats.rx_sa_stats->InPktsNotUsingSA = stats->not_using_sa;
	ctx->stats.rx_sa_stats->InPktsUnusedSA = stats->unused_sa;

	rx_sa = rcu_dereference_bh(aq_rxsc->sw_rxsc->sa[ctx->sa.assoc_num]);
	ret = aqr_get_rxsa_next_pn(port, sa_idx, &next_pn);
	if (ret == 0) {
		spin_lock_bh(&rx_sa->lock);
		rx_sa->next_pn = next_pn;
		spin_unlock_bh(&rx_sa->lock);
	}

	return ret;
}

static int apply_txsc_cfg(struct aqr107_priv *priv, const int txsc_idx)
{
	struct aqr_macsec_txsc *aq_txsc = &priv->macsec_cfg.aq_txsc[txsc_idx];
	const struct macsec_secy *secy = aq_txsc->sw_secy;
	struct macsec_tx_sa *tx_sa;
	int ret = 0;
	int i;

	if (!netif_running(secy->netdev))
		return ret;

	ret = aqr_set_txsc(priv, txsc_idx);
	if (ret)
		return ret;

	for (i = 0; i < MACSEC_NUM_AN; i++) {
		tx_sa = rcu_dereference_bh(secy->tx_sc.sa[i]);
		if (tx_sa) {
			ret = aqr_update_txsa(priv, aq_txsc->hw_sc_idx, secy,
					     tx_sa, aq_txsc->tx_sa_key[i], i);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static int apply_rxsc_cfg(struct aqr107_priv *priv, const int rxsc_idx)
{
	struct aqr_macsec_rxsc *aq_rxsc = &priv->macsec_cfg.aq_rxsc[rxsc_idx];
	const struct macsec_secy *secy = aq_rxsc->sw_secy;
	struct macsec_rx_sa *rx_sa;
	int ret = 0;
	int i;

	if (!netif_running(secy->netdev))
		return ret;

	ret = aqr_set_rxsc(priv, rxsc_idx);
	if (ret)
		return ret;

	for (i = 0; i < MACSEC_NUM_AN; i++) {
		rx_sa = rcu_dereference_bh(aq_rxsc->sw_rxsc->sa[i]);
		if (rx_sa) {
			ret = aqr_update_rxsa(priv, aq_rxsc->hw_sc_idx, secy,
					     rx_sa, aq_rxsc->rx_sa_key[i], i);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static int aqr_clear_secy(struct phy_device *phydev, const struct macsec_secy *secy,
			 enum aqr_clear_type clear_type)
{
	struct aqr107_priv *priv = phydev->priv;
	struct macsec_rx_sc *rx_sc;
	int txsc_idx;
	int rxsc_idx;
	int ret = 0;

	txsc_idx = aqr_get_txsc_idx_from_secy(&priv->macsec_cfg, secy);
	if (txsc_idx >= 0) {
		ret = aqr_clear_txsc(phydev, txsc_idx, clear_type);
		if (ret)
			return ret;
	}

	for (rx_sc = rcu_dereference_bh(secy->rx_sc); rx_sc;
	     rx_sc = rcu_dereference_bh(rx_sc->next)) {
		rxsc_idx = aqr_get_rxsc_idx_from_rxsc(&priv->macsec_cfg, rx_sc);
		if (rxsc_idx < 0)
			continue;

		ret = aqr_clear_rxsc(phydev, rxsc_idx, clear_type);
		if (ret)
			return ret;
	}

	return ret;
}

static int aqr_apply_secy_cfg(struct aqr107_priv *priv,
			     const struct macsec_secy *secy)
{
	struct macsec_rx_sc *rx_sc;
	int txsc_idx;
	int rxsc_idx;
	int ret = 0;

	txsc_idx = aqr_get_txsc_idx_from_secy(&priv->macsec_cfg, secy);
	if (txsc_idx >= 0)
		apply_txsc_cfg(priv, txsc_idx);

	for (rx_sc = rcu_dereference_bh(secy->rx_sc); rx_sc && rx_sc->active;
	     rx_sc = rcu_dereference_bh(rx_sc->next)) {
		rxsc_idx = aqr_get_rxsc_idx_from_rxsc(&priv->macsec_cfg, rx_sc);
		if (unlikely(rxsc_idx < 0))
			continue;

		ret = apply_rxsc_cfg(priv, rxsc_idx);
		if (ret)
			return ret;
	}

	return ret;
}

static int aqr_apply_macsec_cfg(struct aqr107_priv *priv)
{
	int ret = 0;
	int i;

	for (i = 0; i < AQR_MACSEC_MAX_SC; i++) {
		if (priv->macsec_cfg.txsc_idx_busy & BIT(i)) {
			ret = apply_txsc_cfg(priv, i);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < AQR_MACSEC_MAX_SC; i++) {
		if (priv->macsec_cfg.rxsc_idx_busy & BIT(i)) {
			ret = apply_rxsc_cfg(priv, i);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static int aqr_sa_from_sa_idx(const enum aqr_macsec_sc_sa sc_sa, const int sa_idx)
{
	switch (sc_sa) {
	case aqr_macsec_sa_sc_4sa_8sc:
		return sa_idx & 3;
	case aqr_macsec_sa_sc_2sa_16sc:
		return sa_idx & 1;
	case aqr_macsec_sa_sc_1sa_32sc:
		return 0;
	default:
		WARN_ONCE(true, "Invalid sc_sa");
	}
	return -EINVAL;
}

static int aqr_sc_idx_from_sa_idx(const enum aqr_macsec_sc_sa sc_sa,
				 const int sa_idx)
{
	switch (sc_sa) {
	case aqr_macsec_sa_sc_4sa_8sc:
		return sa_idx & ~3;
	case aqr_macsec_sa_sc_2sa_16sc:
		return sa_idx & ~1;
	case aqr_macsec_sa_sc_1sa_32sc:
		return sa_idx;
	default:
		WARN_ONCE(true, "Invalid sc_sa");
	}
	return -EINVAL;
}

static int aqr_get_egress_sa_expired(struct phy_device *phydev, int *expired)
{
	int val, ret;

	val = phy_read_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_EXPIRED_STATUS_REGISTER_ADDR);
	if (val < 0)
		return val;

	ret = val;

	val = phy_read_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_EXPIRED_STATUS_REGISTER_ADDR + 1);
	if (val < 0)
		return val;

	ret |= val << 16;
	
	*expired = ret;

	return 0;
}

static int aqr_get_egress_sa_threshold_expired(struct phy_device *phydev, int *threshold_expired)
{
	int val, ret;

	val = phy_read_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_THRESHOLD_EXPIRED_STATUS_REGISTER_ADDR);
	if (val < 0)
		return val;

	ret = val;

	val = phy_read_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_THRESHOLD_EXPIRED_STATUS_REGISTER_ADDR + 1);
	if (val < 0)
		return val;

	ret |= val << 16;
	
	*threshold_expired = ret;

	return 0;
}

static int aqr_set_egress_sa_expired(struct phy_device *phydev, int expired)
{
	int err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_EXPIRED_STATUS_REGISTER_ADDR, expired & 0xffff);
	if (err < 0)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_EXPIRED_STATUS_REGISTER_ADDR + 1, expired >> 16);

	return err;
}

static int aqr_set_egress_sa_threshold_expired(struct phy_device *phydev, int threshold_expired)
{
	int err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_THRESHOLD_EXPIRED_STATUS_REGISTER_ADDR, threshold_expired & 0xffff);
	if (err < 0)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, AQR_SA_THRESHOLD_EXPIRED_STATUS_REGISTER_ADDR + 1, threshold_expired >> 16);

	return err;
}

void aqr_check_txsa_expiration(struct phy_device *phydev)
{
	u32 egress_sa_expired = 0;
	u32 egress_sa_threshold_expired = 0;
	struct aqr107_priv *priv = phydev->priv;
	struct aqr_macsec_cfg *cfg = &priv->macsec_cfg;
	struct aqr_macsec_txsc *aq_txsc;
	const struct macsec_secy *secy;
	int sc_idx = 0, txsc_idx = 0;
	enum aqr_macsec_sc_sa sc_sa;
	struct macsec_tx_sa *tx_sa;
	unsigned char an = 0;
	int ret;
	int i;

	sc_sa = cfg->sc_sa;

	ret = aqr_get_egress_sa_expired(phydev, &egress_sa_expired);
	if (unlikely(ret))
		return;

	ret = aqr_get_egress_sa_threshold_expired(phydev,
		&egress_sa_threshold_expired);

	for (i = 0; i < AQR_MACSEC_MAX_SA; i++) {
		if (egress_sa_expired & BIT(i)) {
			an = aqr_sa_from_sa_idx(sc_sa, i);
			sc_idx = aqr_sc_idx_from_sa_idx(sc_sa, i);
			txsc_idx = aqr_get_txsc_idx_from_sc_idx(sc_sa, sc_idx);
			if (txsc_idx < 0)
				continue;

			aq_txsc = &cfg->aq_txsc[txsc_idx];
			if (!(cfg->txsc_idx_busy & BIT(txsc_idx))) {
				phydev_warn(phydev, "PN threshold expired on invalid TX SC");
				continue;
			}

			secy = aq_txsc->sw_secy;

			if (!netif_running(secy->netdev)) {
				phydev_warn(phydev, "PN threshold expired on down TX SC");
				continue;
			}

			if (unlikely(!(aq_txsc->tx_sa_idx_busy & BIT(an)))) {
				phydev_warn(phydev, "PN threshold expired on invalid TX SA");
				continue;
			}

			tx_sa = rcu_dereference_bh(secy->tx_sc.sa[an]);
			macsec_pn_wrapped((struct macsec_secy *)secy, tx_sa);
		}
	}

	aqr_set_egress_sa_expired(phydev, egress_sa_expired);
	if (likely(!ret))
		aqr_set_egress_sa_threshold_expired(phydev,
			egress_sa_threshold_expired);
}

const struct macsec_ops aqr_macsec_ops = {
	.mdo_dev_open = aqr_mdo_dev_open,
	.mdo_dev_stop = aqr_mdo_dev_stop,
	.mdo_add_secy = aqr_mdo_add_secy,
	.mdo_upd_secy = aqr_mdo_upd_secy,
	.mdo_del_secy = aqr_mdo_del_secy,
	.mdo_add_rxsc = aqr_mdo_add_rxsc,
	.mdo_upd_rxsc = aqr_mdo_upd_rxsc,
	.mdo_del_rxsc = aqr_mdo_del_rxsc,
	.mdo_add_rxsa = aqr_mdo_add_rxsa,
	.mdo_upd_rxsa = aqr_mdo_upd_rxsa,
	.mdo_del_rxsa = aqr_mdo_del_rxsa,
	.mdo_add_txsa = aqr_mdo_add_txsa,
	.mdo_upd_txsa = aqr_mdo_upd_txsa,
	.mdo_del_txsa = aqr_mdo_del_txsa,
	.mdo_get_dev_stats = aqr_mdo_get_dev_stats,
	.mdo_get_tx_sc_stats = aqr_mdo_get_tx_sc_stats,
	.mdo_get_tx_sa_stats = aqr_mdo_get_tx_sa_stats,
	.mdo_get_rx_sc_stats = aqr_mdo_get_rx_sc_stats,
	.mdo_get_rx_sa_stats = aqr_mdo_get_rx_sa_stats,
};

int aqr_macsec_enable(struct phy_device *phydev)
{
	struct aqr107_priv *priv = phydev->priv;
	u32 ctl_ether_types[] = { ETH_P_PAE , 0x1234};
	struct aqr_port *port = &priv->port;
	int index = 0, tbl_idx;

	/* Init Ethertype bypass filters */
	for (index = 0; index < AQR_NUMROWS_EGRESSCTLFRECORD; index++) {
		struct aqr_mss_egress_ctlf_record tx_ctlf_rec;
		memset(&tx_ctlf_rec, 0, sizeof(tx_ctlf_rec));

		if (index < ARRAY_SIZE(ctl_ether_types)) {
			tx_ctlf_rec.eth_type = ctl_ether_types[index];
			tx_ctlf_rec.match_type = 4; /* Match eth_type only */
			tx_ctlf_rec.match_mask = 0xf; /* match for eth_type */
			tx_ctlf_rec.action = 0; /* Bypass MACSEC modules */
		}

		tbl_idx = AQR_NUMROWS_EGRESSCTLFRECORD - index - 1;
		aqr_mss_set_egress_ctlf_record(port, &tx_ctlf_rec, tbl_idx);
	}

	for (index = 0; index < AQR_NUMROWS_INGRESSPRECTLFRECORD; index++) {
		struct aqr_mss_ingress_prectlf_record rx_prectlf_rec;
		memset(&rx_prectlf_rec, 0, sizeof(rx_prectlf_rec));

		if (index < ARRAY_SIZE(ctl_ether_types)) {
			rx_prectlf_rec.eth_type = ctl_ether_types[index];
			rx_prectlf_rec.match_type = 4; /* Match eth_type only */
			rx_prectlf_rec.match_mask = 0xf; /* match for eth_type */
			rx_prectlf_rec.action = 0; /* Bypass MACSEC modules */
		}

		tbl_idx = AQR_NUMROWS_INGRESSPRECTLFRECORD - index - 1;
		aqr_mss_set_ingress_prectlf_record(port, &rx_prectlf_rec, tbl_idx);
	}

	/* Adding Ingress Post Class Record */
	{
		struct aqr_mss_ingress_postclass_record rx_posttlf_rec;
		memset(&rx_posttlf_rec, 0, sizeof(rx_posttlf_rec));
		rx_posttlf_rec.valid = 1;

		aqr_mss_set_ingress_postclass_record(port, &rx_posttlf_rec, 0);
	}
	return 0;
}

