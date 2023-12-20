// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm ICE (Inline Crypto Engine) support.
 *
 * Copyright (c) 2013-2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2019, Google LLC
 * Copyright (c) 2023, Linaro Limited
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <linux/firmware/qcom/qcom_scm.h>

#include <soc/qcom/ice.h>

#define AES_256_XTS_KEY_SIZE			64

/*
 * Wrapped key sizes from HWKm is different for different versions of
 * HW. It is not expected to change again in the future.
 */
#define QCOM_ICE_HWKM_WRAPPED_KEY_SIZE(v)	\
	((v) == 1 ? 68 : 100)

/* QCOM ICE registers */
#define QCOM_ICE_REG_VERSION			0x0008
#define QCOM_ICE_REG_FUSE_SETTING		0x0010
#define QCOM_ICE_REG_BIST_STATUS		0x0070
#define QCOM_ICE_REG_ADVANCED_CONTROL		0x1000
#define QCOM_ICE_REG_CONTROL			0x0
#define QCOM_ICE_LUT_KEYS_CRYPTOCFG_R16		0x4040

/* QCOM ICE HWKM registers */
#define QCOM_ICE_REG_HWKM_TZ_KM_CTL		0x1000
#define QCOM_ICE_REG_HWKM_TZ_KM_STATUS		0x1004
#define QCOM_ICE_REG_HWKM_BANK0_BBAC_0		0x5000
#define QCOM_ICE_REG_HWKM_BANK0_BBAC_1		0x5004
#define QCOM_ICE_REG_HWKM_BANK0_BBAC_2		0x5008
#define QCOM_ICE_REG_HWKM_BANK0_BBAC_3		0x500C
#define QCOM_ICE_REG_HWKM_BANK0_BBAC_4		0x5010

/* QCOM ICE HWKM BIST vals */
#define QCOM_ICE_HWKM_BIST_DONE_V1_VAL		0x14007
#define QCOM_ICE_HWKM_BIST_DONE_V2_VAL		0x287

/* BIST ("built-in self-test") status flags */
#define QCOM_ICE_BIST_STATUS_MASK		GENMASK(31, 28)

#define QCOM_ICE_FUSE_SETTING_MASK		0x1
#define QCOM_ICE_FORCE_HW_KEY0_SETTING_MASK	0x2
#define QCOM_ICE_FORCE_HW_KEY1_SETTING_MASK	0x4

#define QCOM_ICE_LUT_KEYS_CRYPTOCFG_OFFSET	0x80

#define QCOM_ICE_HWKM_REG_OFFSET	0x8000
#define HWKM_OFFSET(reg)		(reg + QCOM_ICE_HWKM_REG_OFFSET)

#define qcom_ice_writel(engine, val, reg)	\
	writel((val), (engine)->base + (reg))

#define qcom_ice_readl(engine, reg)	\
	readl((engine)->base + (reg))

struct qcom_ice {
	struct device *dev;
	void __iomem *base;
	struct device_link *link;

	struct clk *core_clk;
	u8 hwkm_version;
	bool hwkm_init_complete;
};

union crypto_cfg {
	__le32 regval;
	struct {
		u8 dusize;
		u8 capidx;
		u8 reserved;
		u8 cfge;
	};
};

static bool qcom_ice_check_supported(struct qcom_ice *ice)
{
	u32 regval = qcom_ice_readl(ice, QCOM_ICE_REG_VERSION);
	struct device *dev = ice->dev;
	int major = FIELD_GET(GENMASK(31, 24), regval);
	int minor = FIELD_GET(GENMASK(23, 16), regval);
	int step = FIELD_GET(GENMASK(15, 0), regval);

	/* For now this driver only supports ICE version 3 and 4. */
	if (major != 3 && major != 4) {
		dev_warn(dev, "Unsupported ICE version: v%d.%d.%d\n",
			 major, minor, step);
		return false;
	}

	if ((major >= 4) || ((major == 3) && (minor == 2) && (step >= 1)))
		ice->hwkm_version = 2;
	else if ((major == 3) && (minor == 2))
		ice->hwkm_version = 1;
	else
		ice->hwkm_version = 0;

	dev_info(dev, "Found QC Inline Crypto Engine (ICE) v%d.%d.%d\n",
		 major, minor, step);
	if (!ice->hwkm_version)
		dev_info(dev, "QC ICE HWKM (Hardware Key Manager) not supported");
	else
		dev_info(dev, "QC ICE HWKM (Hardware Key Manager) version = %d",
			 ice->hwkm_version);

	/* If fuses are blown, ICE might not work in the standard way. */
	regval = qcom_ice_readl(ice, QCOM_ICE_REG_FUSE_SETTING);
	if (regval & (QCOM_ICE_FUSE_SETTING_MASK |
		      QCOM_ICE_FORCE_HW_KEY0_SETTING_MASK |
		      QCOM_ICE_FORCE_HW_KEY1_SETTING_MASK)) {
		dev_warn(dev, "Fuses are blown; ICE is unusable!\n");
		return false;
	}

	return true;
}

static void qcom_ice_low_power_mode_enable(struct qcom_ice *ice)
{
	u32 regval;

	regval = qcom_ice_readl(ice, QCOM_ICE_REG_ADVANCED_CONTROL);

	/* Enable low power mode sequence */
	regval |= 0x7000;
	qcom_ice_writel(ice, regval, QCOM_ICE_REG_ADVANCED_CONTROL);
}

static void qcom_ice_optimization_enable(struct qcom_ice *ice)
{
	u32 regval;

	/* ICE Optimizations Enable Sequence */
	regval = qcom_ice_readl(ice, QCOM_ICE_REG_ADVANCED_CONTROL);
	regval |= 0xd807100;
	/* ICE HPG requires delay before writing */
	udelay(5);
	qcom_ice_writel(ice, regval, QCOM_ICE_REG_ADVANCED_CONTROL);
	udelay(5);
}

/*
 * Wait until the ICE BIST (built-in self-test) has completed.
 *
 * This may be necessary before ICE can be used.
 * Note that we don't really care whether the BIST passed or failed;
 * we really just want to make sure that it isn't still running. This is
 * because (a) the BIST is a FIPS compliance thing that never fails in
 * practice, (b) ICE is documented to reject crypto requests if the BIST
 * fails, so we needn't do it in software too, and (c) properly testing
 * storage encryption requires testing the full storage stack anyway,
 * and not relying on hardware-level self-tests.
 *
 * However, we still care about if HWKM BIST failed (when supported) as
 * important functionality would fail later, so disable hwkm on failure.
 */
static int qcom_ice_wait_bist_status(struct qcom_ice *ice)
{
	u32 regval;
	u32 bist_done_val;
	int err;

	err = readl_poll_timeout(ice->base + QCOM_ICE_REG_BIST_STATUS,
				 regval, !(regval & QCOM_ICE_BIST_STATUS_MASK),
				 50, 5000);
	if (err)
		dev_err(ice->dev, "Timed out waiting for ICE self-test to complete\n");

	if (ice->hwkm_version) {
		bist_done_val = (ice->hwkm_version == 1) ?
				 QCOM_ICE_HWKM_BIST_DONE_V1_VAL :
				 QCOM_ICE_HWKM_BIST_DONE_V2_VAL;
		if (qcom_ice_readl(ice,
				   HWKM_OFFSET(QCOM_ICE_REG_HWKM_TZ_KM_STATUS)) !=
				   bist_done_val) {
			dev_warn(ice->dev, "HWKM BIST error\n");
			ice->hwkm_version = 0;
		}
	}
	return err;
}

static void qcom_ice_enable_standard_mode(struct qcom_ice *ice)
{
	u32 val = 0;

	if (!ice->hwkm_version)
		return;

	/*
	 * When ICE is in standard (hwkm) mode, it supports HW wrapped
	 * keys, and when it is in legacy mode, it only supports standard
	 * (non HW wrapped) keys.
	 *
	 * Put ICE in standard mode, ICE defaults to legacy mode.
	 * Legacy mode - ICE HWKM slave not supported.
	 * Standard mode - ICE HWKM slave supported.
	 *
	 * Depending on the version of HWKM, it is controlled by different
	 * registers in ICE.
	 */
	if (ice->hwkm_version >= 2) {
		val = qcom_ice_readl(ice, QCOM_ICE_REG_CONTROL);
		val = val & 0xFFFFFFFE;
		qcom_ice_writel(ice, val, QCOM_ICE_REG_CONTROL);
	} else {
		qcom_ice_writel(ice, 0x7,
				HWKM_OFFSET(QCOM_ICE_REG_HWKM_TZ_KM_CTL));
	}
}

static void qcom_ice_hwkm_init(struct qcom_ice *ice)
{
	if (!ice->hwkm_version)
		return;

	/*
	 * Give register bank of the HWKM slave access to read and modify
	 * the keyslots in ICE HWKM slave. Without this, trustzone will not
	 * be able to program keys into ICE.
	 */
	qcom_ice_writel(ice, 0xFFFFFFFF,
			HWKM_OFFSET(QCOM_ICE_REG_HWKM_BANK0_BBAC_0));
	qcom_ice_writel(ice, 0xFFFFFFFF,
			HWKM_OFFSET(QCOM_ICE_REG_HWKM_BANK0_BBAC_1));
	qcom_ice_writel(ice, 0xFFFFFFFF,
			HWKM_OFFSET(QCOM_ICE_REG_HWKM_BANK0_BBAC_2));
	qcom_ice_writel(ice, 0xFFFFFFFF,
			HWKM_OFFSET(QCOM_ICE_REG_HWKM_BANK0_BBAC_3));
	qcom_ice_writel(ice, 0xFFFFFFFF,
			HWKM_OFFSET(QCOM_ICE_REG_HWKM_BANK0_BBAC_4));

	ice->hwkm_init_complete = true;
}

int qcom_ice_enable(struct qcom_ice *ice)
{
	int err;

	qcom_ice_low_power_mode_enable(ice);
	qcom_ice_optimization_enable(ice);

	qcom_ice_enable_standard_mode(ice);

	err = qcom_ice_wait_bist_status(ice);
	if (err)
		return err;

	qcom_ice_hwkm_init(ice);

	return err;
}
EXPORT_SYMBOL_GPL(qcom_ice_enable);

int qcom_ice_resume(struct qcom_ice *ice)
{
	struct device *dev = ice->dev;
	int err;

	err = clk_prepare_enable(ice->core_clk);
	if (err) {
		dev_err(dev, "failed to enable core clock (%d)\n",
			err);
		return err;
	}

	return qcom_ice_wait_bist_status(ice);
}
EXPORT_SYMBOL_GPL(qcom_ice_resume);

int qcom_ice_suspend(struct qcom_ice *ice)
{
	clk_disable_unprepare(ice->core_clk);

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_ice_suspend);

/*
 * HW dictates the internal mapping between the ICE and HWKM slots,
 * which are different for different versions, make the translation
 * here.
 */
static int translate_hwkm_slot(struct qcom_ice *ice, int slot)
{
	return (ice->hwkm_version == 1) ? slot : (slot * 2);
}

static int qcom_ice_program_wrapped_key(struct qcom_ice *ice,
					const struct blk_crypto_key *key,
					u8 data_unit_size, int slot)
{
	int hwkm_slot;
	int err;
	union crypto_cfg cfg;

	hwkm_slot = translate_hwkm_slot(ice, slot);

	memset(&cfg, 0, sizeof(cfg));
	cfg.dusize = data_unit_size;
	cfg.capidx = QCOM_SCM_ICE_CIPHER_AES_256_XTS;
	cfg.cfge = 0x80;

	/* Clear CFGE */
	qcom_ice_writel(ice, 0x0, QCOM_ICE_LUT_KEYS_CRYPTOCFG_R16 +
				  QCOM_ICE_LUT_KEYS_CRYPTOCFG_OFFSET * slot);

	/* Call trustzone to program the wrapped key using hwkm */
	err = qcom_scm_ice_set_key(hwkm_slot, key->raw, key->size,
				   QCOM_SCM_ICE_CIPHER_AES_256_XTS, data_unit_size);
	if (err) {
		pr_err("%s:SCM call Error: 0x%x slot %d\n", __func__, err,
		       slot);
		return err;
	}

	/* Enable CFGE after programming key */
	qcom_ice_writel(ice, cfg.regval, QCOM_ICE_LUT_KEYS_CRYPTOCFG_R16 +
					 QCOM_ICE_LUT_KEYS_CRYPTOCFG_OFFSET * slot);

	return err;
}

int qcom_ice_program_key(struct qcom_ice *ice,
			 u8 algorithm_id, u8 key_size,
			 const struct blk_crypto_key *bkey,
			 u8 data_unit_size, int slot)
{
	struct device *dev = ice->dev;
	union {
		u8 bytes[AES_256_XTS_KEY_SIZE];
		u32 words[AES_256_XTS_KEY_SIZE / sizeof(u32)];
	} key;
	int i;
	int err;

	/* Only AES-256-XTS has been tested so far. */
	if (algorithm_id != QCOM_ICE_CRYPTO_ALG_AES_XTS ||
	    (key_size != QCOM_ICE_CRYPTO_KEY_SIZE_256 &&
	    key_size != QCOM_ICE_CRYPTO_KEY_SIZE_WRAPPED)) {
		dev_err_ratelimited(dev,
				    "Unhandled crypto capability; algorithm_id=%d, key_size=%d\n",
				    algorithm_id, key_size);
		return -EINVAL;
	}

	if (bkey->crypto_cfg.key_type == BLK_CRYPTO_KEY_TYPE_HW_WRAPPED) {
		if (!ice->hwkm_version)
			return -EINVAL;
		err = qcom_ice_program_wrapped_key(ice, bkey, data_unit_size,
				slot);
	} else {
		if (bkey->size != QCOM_ICE_CRYPTO_KEY_SIZE_256)
			dev_err_ratelimited(dev,
				    "Incorrect key size; bkey->size=%d\n",
				    algorithm_id);
		return -EINVAL;
		memcpy(key.bytes, bkey->raw, AES_256_XTS_KEY_SIZE);

		/* The SCM call requires that the key words are encoded in big endian */
		for (i = 0; i < ARRAY_SIZE(key.words); i++)
			__cpu_to_be32s(&key.words[i]);

		err = qcom_scm_ice_set_key(slot, key.bytes, AES_256_XTS_KEY_SIZE,
					   QCOM_SCM_ICE_CIPHER_AES_256_XTS,
					   data_unit_size);
		memzero_explicit(&key, sizeof(key));
	}

	return err;
}
EXPORT_SYMBOL_GPL(qcom_ice_program_key);

int qcom_ice_evict_key(struct qcom_ice *ice, int slot)
{
	int hwkm_slot = slot;

	if (ice->hwkm_version) {
		hwkm_slot = translate_hwkm_slot(ice, slot);
	/*
	 * Ignore calls to evict key when HWKM is supported and hwkm init
	 * is not yet done. This is to avoid the clearing all slots call
	 * during a storage reset when ICE is still in legacy mode. HWKM slave
	 * in ICE takes care of zeroing out the keytable on reset.
	 */
		if (!ice->hwkm_init_complete)
			return 0;
	}

	return qcom_scm_ice_invalidate_key(hwkm_slot);
}
EXPORT_SYMBOL_GPL(qcom_ice_evict_key);

bool qcom_ice_hwkm_supported(struct qcom_ice *ice)
{
	return (ice->hwkm_version > 0);
}
EXPORT_SYMBOL_GPL(qcom_ice_hwkm_supported);

int qcom_ice_derive_sw_secret(struct qcom_ice *ice, const u8 wrapped_key[],
			      unsigned int wrapped_key_size,
			      u8 sw_secret[BLK_CRYPTO_SW_SECRET_SIZE])
{
	return qcom_scm_derive_sw_secret(wrapped_key, wrapped_key_size,
					 sw_secret, BLK_CRYPTO_SW_SECRET_SIZE);
}
EXPORT_SYMBOL_GPL(qcom_ice_derive_sw_secret);

/**
 * qcom_ice_generate_key() - Generate a wrapped key for inline encryption
 * @lt_key: longterm wrapped key that is generated, which is
 *          BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE in size.
 *
 * Make a scm call into trustzone to generate a wrapped key for storage
 * encryption using hwkm.
 *
 * Return: Keysize on success; err on failure.
 */
int qcom_ice_generate_key(struct qcom_ice *ice,
			  u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret;

	ret = qcom_scm_generate_ice_key(lt_key,
					 QCOM_ICE_HWKM_WRAPPED_KEY_SIZE(ice->hwkm_version));
	if (!ret)
		return QCOM_ICE_HWKM_WRAPPED_KEY_SIZE(ice->hwkm_version);
	else
		return ret;
}
EXPORT_SYMBOL_GPL(qcom_ice_generate_key);

/**
 * qcom_ice_prepare_key() - Prepare a longterm wrapped key for inline encryption
 * @lt_key: longterm wrapped key that is generated or imported.
 * @lt_key_size: size of the longterm wrapped_key
 * @eph_key: wrapped key returned which has been wrapped with a per-boot ephemeral key,
 *           size of which is BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE in size.
 *
 * Make a scm call into trustzone to prepare a wrapped key for storage
 * encryption by rewrapping the longterm wrapped key with a per boot ephemeral
 * key using hwkm.
 *
 * Return: Keysize on success; err on failure.
 */
int qcom_ice_prepare_key(struct qcom_ice *ice, const u8 *lt_key, size_t lt_key_size,
			 u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret;

	ret = qcom_scm_prepare_ice_key(lt_key, lt_key_size, eph_key,
					QCOM_ICE_HWKM_WRAPPED_KEY_SIZE(ice->hwkm_version));
	if (!ret)
		return QCOM_ICE_HWKM_WRAPPED_KEY_SIZE(ice->hwkm_version);
	else
		return ret;
}
EXPORT_SYMBOL_GPL(qcom_ice_prepare_key);

/**
 * qcom_ice_import_key() - Import a raw key for inline encryption
 * @imp_key: raw key that has to be imported
 * @imp_key_size: size of the imported key
 * @lt_key: longterm wrapped key that is imported, which is
 *          BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE in size.
 *
 * Make a scm call into trustzone to import a raw key for storage encryption
 * and generate a longterm wrapped key using hwkm.
 *
 * Return: Keysize on success; err on failure.
 */
int qcom_ice_import_key(struct qcom_ice *ice, const u8 *imp_key, size_t imp_key_size,
			u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE])
{
	int ret;

	ret = qcom_scm_import_ice_key(imp_key, imp_key_size, lt_key,
				       QCOM_ICE_HWKM_WRAPPED_KEY_SIZE(ice->hwkm_version));
	if (!ret)
		return QCOM_ICE_HWKM_WRAPPED_KEY_SIZE(ice->hwkm_version);
	else
		return ret;
}
EXPORT_SYMBOL_GPL(qcom_ice_import_key);

static struct qcom_ice *qcom_ice_create(struct device *dev,
					void __iomem *base)
{
	struct qcom_ice *engine;

	if (!qcom_scm_is_available())
		return ERR_PTR(-EPROBE_DEFER);

	if (!qcom_scm_ice_available()) {
		dev_warn(dev, "ICE SCM interface not found\n");
		return NULL;
	}

	engine = devm_kzalloc(dev, sizeof(*engine), GFP_KERNEL);
	if (!engine)
		return ERR_PTR(-ENOMEM);

	engine->dev = dev;
	engine->base = base;

	/*
	 * Legacy DT binding uses different clk names for each consumer,
	 * so lets try those first. If none of those are a match, it means
	 * the we only have one clock and it is part of the dedicated DT node.
	 * Also, enable the clock before we check what HW version the driver
	 * supports.
	 */
	engine->core_clk = devm_clk_get_optional_enabled(dev, "ice_core_clk");
	if (!engine->core_clk)
		engine->core_clk = devm_clk_get_optional_enabled(dev, "ice");
	if (!engine->core_clk)
		engine->core_clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(engine->core_clk))
		return ERR_CAST(engine->core_clk);

	if (!qcom_ice_check_supported(engine))
		return ERR_PTR(-EOPNOTSUPP);

	dev_dbg(dev, "Registered Qualcomm Inline Crypto Engine\n");

	return engine;
}

/**
 * of_qcom_ice_get() - get an ICE instance from a DT node
 * @dev: device pointer for the consumer device
 *
 * This function will provide an ICE instance either by creating one for the
 * consumer device if its DT node provides the 'ice' reg range and the 'ice'
 * clock (for legacy DT style). On the other hand, if consumer provides a
 * phandle via 'qcom,ice' property to an ICE DT, the ICE instance will already
 * be created and so this function will return that instead.
 *
 * Return: ICE pointer on success, NULL if there is no ICE data provided by the
 * consumer or ERR_PTR() on error.
 */
struct qcom_ice *of_qcom_ice_get(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct qcom_ice *ice;
	struct device_node *node;
	struct resource *res;
	void __iomem *base;

	if (!dev || !dev->of_node)
		return ERR_PTR(-ENODEV);

	/*
	 * In order to support legacy style devicetree bindings, we need
	 * to create the ICE instance using the consumer device and the reg
	 * range called 'ice' it provides.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ice");
	if (res) {
		base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(base))
			return ERR_CAST(base);

		/* create ICE instance using consumer dev */
		return qcom_ice_create(&pdev->dev, base);
	}

	/*
	 * If the consumer node does not provider an 'ice' reg range
	 * (legacy DT binding), then it must at least provide a phandle
	 * to the ICE devicetree node, otherwise ICE is not supported.
	 */
	node = of_parse_phandle(dev->of_node, "qcom,ice", 0);
	if (!node)
		return NULL;

	pdev = of_find_device_by_node(node);
	if (!pdev) {
		dev_err(dev, "Cannot find device node %s\n", node->name);
		ice = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}

	ice = platform_get_drvdata(pdev);
	if (!ice) {
		dev_err(dev, "Cannot get ice instance from %s\n",
			dev_name(&pdev->dev));
		platform_device_put(pdev);
		ice = ERR_PTR(-EPROBE_DEFER);
		goto out;
	}

	ice->link = device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_SUPPLIER);
	if (!ice->link) {
		dev_err(&pdev->dev,
			"Failed to create device link to consumer %s\n",
			dev_name(dev));
		platform_device_put(pdev);
		ice = ERR_PTR(-EINVAL);
	}

out:
	of_node_put(node);

	return ice;
}
EXPORT_SYMBOL_GPL(of_qcom_ice_get);

static int qcom_ice_probe(struct platform_device *pdev)
{
	struct qcom_ice *engine;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		dev_warn(&pdev->dev, "ICE registers not found\n");
		return PTR_ERR(base);
	}

	engine = qcom_ice_create(&pdev->dev, base);
	if (IS_ERR(engine))
		return PTR_ERR(engine);

	platform_set_drvdata(pdev, engine);

	return 0;
}

static const struct of_device_id qcom_ice_of_match_table[] = {
	{ .compatible = "qcom,inline-crypto-engine" },
	{ },
};
MODULE_DEVICE_TABLE(of, qcom_ice_of_match_table);

static struct platform_driver qcom_ice_driver = {
	.probe	= qcom_ice_probe,
	.driver = {
		.name = "qcom-ice",
		.of_match_table = qcom_ice_of_match_table,
	},
};

module_platform_driver(qcom_ice_driver);

MODULE_DESCRIPTION("Qualcomm Inline Crypto Engine driver");
MODULE_LICENSE("GPL");
