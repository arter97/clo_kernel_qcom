// SPDX-License-Identifier: GPL-2.0+
/*
 * OnSemi NB7VPQ904M Type-C nb7 driver
 *
 * Copyright (C) 2020 Dmitry Baryshkov <dmitry.baryshkov@linaro.org>
 */
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>

#define NB7_CHNA		0
#define NB7_CHNB		1
#define NB7_CHNC		2
#define NB7_CHND		3
#define NB7_IS_CHAN_AD(channel) (channel == NB7_CHNA || channel == NB7_CHND)

#define GEN_DEV_SET_REG			0x00

#define GEN_DEV_SET_CHIP_EN		BIT(0)
#define GEN_DEV_SET_CHNA_EN		BIT(4)
#define GEN_DEV_SET_CHNB_EN		BIT(5)
#define GEN_DEV_SET_CHNC_EN		BIT(6)
#define GEN_DEV_SET_CHND_EN		BIT(7)

#define GEN_DEV_SET_OP_MODE_SHIFT	1
#define GEN_DEV_SET_OP_MODE_MASK	0x0e

#define GEN_DEV_SET_OP_MODE_DP_CC2	0
#define GEN_DEV_SET_OP_MODE_DP_CC1	1
#define GEN_DEV_SET_OP_MODE_DP_4LANE	2
#define GEN_DEV_SET_OP_MODE_USB		5

#define EQ_SETTING_REG_BASE		0x01
#define EQ_SETTING_REG(n)		(EQ_SETTING_REG_BASE + (n) * 2)
#define EQ_SETTING_MASK			0x0e
#define EQ_SETTING_SHIFT		0x01

#define OUTPUT_COMPRESSION_AND_POL_REG_BASE	0x02
#define OUTPUT_COMPRESSION_AND_POL_REG(n)	(OUTPUT_COMPRESSION_AND_POL_REG_BASE + (n) * 2)
#define OUTPUT_COMPRESSION_MASK		0x06
#define OUTPUT_COMPRESSION_SHIFT	0x01

#define FLAT_GAIN_REG_BASE		0x18
#define FLAT_GAIN_REG(n)		(FLAT_GAIN_REG_BASE + (n) * 2)
#define FLAT_GAIN_MASK			0x03
#define FLAT_GAIN_SHIFT			0x00

#define LOSS_MATCH_REG_BASE		0x19
#define LOSS_MATCH_REG(n)		(LOSS_MATCH_REG_BASE + (n) * 2)
#define LOSS_MATCH_MASK			0x03
#define LOSS_MATCH_SHIFT		0x00

#define CHIP_VERSION_REG		0x17

struct nb7vpq904m {
	struct i2c_client *client;
	struct regmap *regmap;
	struct typec_switch *sw;
	struct typec_mux *mux;
};

static int nb7vpq904m_sw_set(struct typec_switch *sw,
			      enum typec_orientation orientation)
{
	struct nb7vpq904m *nb7 = typec_switch_get_drvdata(sw);
	int ret;

	dev_info(&nb7->client->dev, "SW: %d\n", orientation);

	switch (orientation) {
	case TYPEC_ORIENTATION_NONE:
		break;
	case TYPEC_ORIENTATION_NORMAL:
		break;
	case TYPEC_ORIENTATION_REVERSE:
		break;
	}

	ret = 0;
	return ret;
}

static void
nb7vpq904m_set_channel(struct nb7vpq904m *nb7, unsigned int channel, bool dp)
{
	u8 eq, out_comp, flat_gain, loss_match;

	if (dp) {
		eq = NB7_IS_CHAN_AD(channel) ? 0x6 : 0x4;
		out_comp = 0x3;
		flat_gain = NB7_IS_CHAN_AD(channel) ? 0x2 : 0x1;
		loss_match = 0x3;
	} else {
		eq = 0x4;
		out_comp = 0x3;
		flat_gain = NB7_IS_CHAN_AD(channel) ? 0x3 : 0x1;
		loss_match = NB7_IS_CHAN_AD(channel) ? 0x1 : 0x3;
	}
	regmap_update_bits(nb7->regmap, EQ_SETTING_REG(channel),
			   EQ_SETTING_MASK, eq << EQ_SETTING_SHIFT);
	regmap_update_bits(nb7->regmap, OUTPUT_COMPRESSION_AND_POL_REG(channel),
			   OUTPUT_COMPRESSION_MASK, out_comp << OUTPUT_COMPRESSION_SHIFT);
	regmap_update_bits(nb7->regmap, FLAT_GAIN_REG(channel),
			   FLAT_GAIN_MASK, flat_gain << FLAT_GAIN_SHIFT);
	regmap_update_bits(nb7->regmap, LOSS_MATCH_REG(channel),
			   LOSS_MATCH_MASK, loss_match << LOSS_MATCH_SHIFT);
}

static int
nb7vpq904m_mux_set(struct typec_mux *mux, struct typec_mux_state *state)
{
	struct nb7vpq904m *nb7 = typec_mux_get_drvdata(mux);
	bool reverse;

	dev_info(&nb7->client->dev, "MUX: %ld\n", state->mode);
	if (state->mode == TYPEC_STATE_SAFE) {
		regmap_write(nb7->regmap, GEN_DEV_SET_REG, 0x0);
		return 0;
	} else if (state->mode == TYPEC_STATE_USB) {
		regmap_write(nb7->regmap, GEN_DEV_SET_REG,
			     GEN_DEV_SET_CHIP_EN |
			     GEN_DEV_SET_CHNA_EN |
			     GEN_DEV_SET_CHNB_EN |
			     GEN_DEV_SET_CHNC_EN |
			     GEN_DEV_SET_CHND_EN |
			     (GEN_DEV_SET_OP_MODE_USB << GEN_DEV_SET_OP_MODE_SHIFT));
		nb7vpq904m_set_channel(nb7, NB7_CHNA, false);
		nb7vpq904m_set_channel(nb7, NB7_CHNB, false);
		nb7vpq904m_set_channel(nb7, NB7_CHNC, false);
		nb7vpq904m_set_channel(nb7, NB7_CHND, false);
		return 0;
	}

	dev_info(&nb7->client->dev, "MUX: %ld, orient %d, alt %x\n", state->mode, typec_altmode_get_orientation(state->alt), state->alt->svid);

	if (!state->alt || state->alt->svid != USB_TYPEC_DP_SID)
		return -EINVAL;

	reverse = (typec_altmode_get_orientation(state->alt) == TYPEC_ORIENTATION_REVERSE);

	switch (state->mode) {
	case TYPEC_DP_STATE_C:
	case TYPEC_DP_STATE_E:
		regmap_write(nb7->regmap, GEN_DEV_SET_REG,
			     GEN_DEV_SET_CHIP_EN |
			     GEN_DEV_SET_CHNA_EN |
			     GEN_DEV_SET_CHNB_EN |
			     GEN_DEV_SET_CHNC_EN |
			     GEN_DEV_SET_CHND_EN |
			     (GEN_DEV_SET_OP_MODE_DP_4LANE << GEN_DEV_SET_OP_MODE_SHIFT));
		nb7vpq904m_set_channel(nb7, NB7_CHNA, true);
		nb7vpq904m_set_channel(nb7, NB7_CHNB, true);
		nb7vpq904m_set_channel(nb7, NB7_CHNC, true);
		nb7vpq904m_set_channel(nb7, NB7_CHND, true);
		break;
	case TYPEC_DP_STATE_D:
		regmap_write(nb7->regmap, GEN_DEV_SET_REG,
			     GEN_DEV_SET_CHIP_EN |
			     GEN_DEV_SET_CHNA_EN |
			     GEN_DEV_SET_CHNB_EN |
			     GEN_DEV_SET_CHNC_EN |
			     GEN_DEV_SET_CHND_EN |
			     ((reverse ? GEN_DEV_SET_OP_MODE_DP_CC2 : GEN_DEV_SET_OP_MODE_DP_CC1)
			      << GEN_DEV_SET_OP_MODE_SHIFT));
		nb7vpq904m_set_channel(nb7, NB7_CHNA, !reverse);
		nb7vpq904m_set_channel(nb7, NB7_CHNB, !reverse);
		nb7vpq904m_set_channel(nb7, NB7_CHNC, reverse);
		nb7vpq904m_set_channel(nb7, NB7_CHND, reverse);
		break;
	default:
		return -ENOTSUPP;
	}

	return 0;
}

static const struct regmap_config nb7_regmap = {
	.max_register = 0x1f,
	.reg_bits = 8,
	.val_bits = 8,
};

static int nb7vpq904m_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_mux_desc mux_desc = { };
	struct nb7vpq904m *nb7;

	nb7 = devm_kzalloc(dev, sizeof(*nb7), GFP_KERNEL);
	if (!nb7)
		return -ENOMEM;

	nb7->client = client;

	nb7->regmap = devm_regmap_init_i2c(client, &nb7_regmap);
	if (IS_ERR(nb7->regmap)) {
		dev_err(&client->dev, "Failed to allocate register map\n");
		return PTR_ERR(nb7->regmap);
	}

	sw_desc.drvdata = nb7;
	sw_desc.fwnode = dev->fwnode;
	sw_desc.set = nb7vpq904m_sw_set;

	nb7->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(nb7->sw)) {
		dev_err(dev, "Error registering typec switch: %ld\n",
			PTR_ERR(nb7->sw));
		return PTR_ERR(nb7->sw);
	}

	mux_desc.drvdata = nb7;
	mux_desc.fwnode = dev->fwnode;
	mux_desc.set = nb7vpq904m_mux_set;

	nb7->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(nb7->mux)) {
		typec_switch_unregister(nb7->sw);
		dev_err(dev, "Error registering typec mux: %ld\n",
			PTR_ERR(nb7->mux));
		return PTR_ERR(nb7->mux);
	}

	return 0;
}

static int nb7vpq904m_remove(struct i2c_client *client)
{
	struct nb7vpq904m *nb7 = i2c_get_clientdata(client);

	typec_mux_unregister(nb7->mux);
	typec_switch_unregister(nb7->sw);

	return 0;
}

static const struct i2c_device_id nb7vpq904m_table[] = {
	{ "nb7vpq904m" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nb7vpq904m_table);

static struct i2c_driver nb7vpq904m_driver = {
	.driver = {
		.name = "nb7vpq904m",
	},
	.probe_new	= nb7vpq904m_probe,
	.remove		= nb7vpq904m_remove,
	.id_table	= nb7vpq904m_table,
};

module_i2c_driver(nb7vpq904m_driver);

MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_DESCRIPTION("Pericom PI3USB30532 Type-C mux driver");
MODULE_LICENSE("GPL");
