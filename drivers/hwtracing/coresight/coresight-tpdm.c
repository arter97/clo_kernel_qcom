// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/amba/bus.h>
#include <linux/bitmap.h>
#include <linux/coresight.h>
#include <linux/coresight-pmu.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include "coresight-priv.h"
#include "coresight-tpdm.h"

DEFINE_CORESIGHT_DEVLIST(tpdm_devs, "tpdm");

static bool tpdm_has_dsb_dataset(struct tpdm_drvdata *drvdata)
{
	return (drvdata->datasets & TPDM_PIDR0_DS_DSB);
}

static void tpdm_reset_datasets(struct tpdm_drvdata *drvdata)
{
	if (tpdm_has_dsb_dataset(drvdata)) {
		memset(drvdata->dsb, 0, sizeof(struct dsb_dataset));

		drvdata->dsb->trig_ts = true;
		drvdata->dsb->trig_type = false;
	}
}

static void tpdm_enable_dsb(struct tpdm_drvdata *drvdata)
{
	u32 val;

	val = readl_relaxed(drvdata->base + TPDM_DSB_TIER);
	/* Set trigger timestamp */
	if (drvdata->dsb->trig_ts)
		val |= TPDM_DSB_TIER_XTRIG_TSENAB;
	else
		val &= ~TPDM_DSB_TIER_XTRIG_TSENAB;
	writel_relaxed(val, drvdata->base + TPDM_DSB_TIER);

	val = readl_relaxed(drvdata->base + TPDM_DSB_CR);
	/* Set trigger type */
	if (drvdata->dsb->trig_type)
		val |= TPDM_DSB_CR_TRIG_TYPE;
	else
		val &= ~TPDM_DSB_CR_TRIG_TYPE;
	/* Set the enable bit of DSB control register to 1 */
	val |= TPDM_DSB_CR_ENA;
	writel_relaxed(val, drvdata->base + TPDM_DSB_CR);
}

/*
 * TPDM enable operations
 * The TPDM or Monitor serves as data collection component for various
 * dataset types. It covers Basic Counts(BC), Tenure Counts(TC),
 * Continuous Multi-Bit(CMB), Multi-lane CMB(MCMB) and Discrete Single
 * Bit(DSB). This function will initialize the configuration according
 * to the dataset type supported by the TPDM.
 */
static void __tpdm_enable(struct tpdm_drvdata *drvdata)
{
	CS_UNLOCK(drvdata->base);

	if (tpdm_has_dsb_dataset(drvdata))
		tpdm_enable_dsb(drvdata);

	CS_LOCK(drvdata->base);
}

static int tpdm_enable(struct coresight_device *csdev, struct perf_event *event,
		       enum cs_mode mode)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock(&drvdata->spinlock);
	if (drvdata->enable) {
		spin_unlock(&drvdata->spinlock);
		return -EBUSY;
	}

	__tpdm_enable(drvdata);
	drvdata->enable = true;
	spin_unlock(&drvdata->spinlock);

	dev_dbg(drvdata->dev, "TPDM tracing enabled\n");
	return 0;
}

static void tpdm_disable_dsb(struct tpdm_drvdata *drvdata)
{
	u32 val;

	/* Set the enable bit of DSB control register to 0 */
	val = readl_relaxed(drvdata->base + TPDM_DSB_CR);
	val &= ~TPDM_DSB_CR_ENA;
	writel_relaxed(val, drvdata->base + TPDM_DSB_CR);
}

/* TPDM disable operations */
static void __tpdm_disable(struct tpdm_drvdata *drvdata)
{
	CS_UNLOCK(drvdata->base);

	if (tpdm_has_dsb_dataset(drvdata))
		tpdm_disable_dsb(drvdata);

	CS_LOCK(drvdata->base);
}

static void tpdm_disable(struct coresight_device *csdev,
			 struct perf_event *event)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);

	spin_lock(&drvdata->spinlock);
	if (!drvdata->enable) {
		spin_unlock(&drvdata->spinlock);
		return;
	}

	__tpdm_disable(drvdata);
	drvdata->enable = false;
	spin_unlock(&drvdata->spinlock);

	dev_dbg(drvdata->dev, "TPDM tracing disabled\n");
}

static const struct coresight_ops_source tpdm_source_ops = {
	.enable		= tpdm_enable,
	.disable	= tpdm_disable,
};

static const struct coresight_ops tpdm_cs_ops = {
	.source_ops	= &tpdm_source_ops,
};

static int tpdm_datasets_setup(struct tpdm_drvdata *drvdata)
{
	u32 pidr;

	/*  Get the datasets present on the TPDM. */
	pidr = readl_relaxed(drvdata->base + CORESIGHT_PERIPHIDR0);
	drvdata->datasets |= pidr & GENMASK(TPDM_DATASETS - 1, 0);

	if (tpdm_has_dsb_dataset(drvdata) && (!drvdata->dsb)) {
		drvdata->dsb = devm_kzalloc(drvdata->dev,
						sizeof(*drvdata->dsb), GFP_KERNEL);
		if (!drvdata->dsb)
			return -ENOMEM;
	}
	tpdm_reset_datasets(drvdata);

	return 0;
}

static ssize_t reset_dataset_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf,
				   size_t size)
{
	int ret = 0;
	unsigned long val;
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	ret = kstrtoul(buf, 0, &val);
	if (ret || val != 1)
		return -EINVAL;

	spin_lock(&drvdata->spinlock);
	tpdm_reset_datasets(drvdata);
	spin_unlock(&drvdata->spinlock);

	return size;
}
static DEVICE_ATTR_WO(reset_dataset);

/*
 * value 1: 64 bits test data
 * value 2: 32 bits test data
 */
static ssize_t integration_test_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf,
					  size_t size)
{
	int i, ret = 0;
	unsigned long val;
	struct tpdm_drvdata *drvdata = dev_get_drvdata(dev->parent);

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	if (val != 1 && val != 2)
		return -EINVAL;

	if (!drvdata->enable)
		return -EINVAL;

	if (val == 1)
		val = ATBCNTRL_VAL_64;
	else
		val = ATBCNTRL_VAL_32;
	CS_UNLOCK(drvdata->base);
	writel_relaxed(0x1, drvdata->base + TPDM_ITCNTRL);

	for (i = 0; i < INTEGRATION_TEST_CYCLE; i++)
		writel_relaxed(val, drvdata->base + TPDM_ITATBCNTRL);

	writel_relaxed(0, drvdata->base + TPDM_ITCNTRL);
	CS_LOCK(drvdata->base);
	return size;
}
static DEVICE_ATTR_WO(integration_test);

static struct attribute *tpdm_attrs[] = {
	&dev_attr_reset_dataset.attr,
	&dev_attr_integration_test.attr,
	NULL,
};

static struct attribute_group tpdm_attr_grp = {
	.attrs = tpdm_attrs,
};

static const struct attribute_group *tpdm_attr_grps[] = {
	&tpdm_attr_grp,
	NULL,
};

static int tpdm_probe(struct amba_device *adev, const struct amba_id *id)
{
	void __iomem *base;
	struct device *dev = &adev->dev;
	struct coresight_platform_data *pdata;
	struct tpdm_drvdata *drvdata;
	struct coresight_desc desc = { 0 };
	int ret;

	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	adev->dev.platform_data = pdata;

	/* driver data*/
	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;
	drvdata->dev = &adev->dev;
	dev_set_drvdata(dev, drvdata);

	base = devm_ioremap_resource(dev, &adev->res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	drvdata->base = base;

	ret = tpdm_datasets_setup(drvdata);
	if (ret)
		return ret;

	/* Set up coresight component description */
	desc.name = coresight_alloc_device_name(&tpdm_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	desc.type = CORESIGHT_DEV_TYPE_SOURCE;
	desc.subtype.source_subtype = CORESIGHT_DEV_SUBTYPE_SOURCE_TPDM;
	desc.ops = &tpdm_cs_ops;
	desc.pdata = adev->dev.platform_data;
	desc.dev = &adev->dev;
	desc.access = CSDEV_ACCESS_IOMEM(base);
	desc.groups = tpdm_attr_grps;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev))
		return PTR_ERR(drvdata->csdev);

	spin_lock_init(&drvdata->spinlock);

	/* Decrease pm refcount when probe is done.*/
	pm_runtime_put(&adev->dev);

	return 0;
}

static void tpdm_remove(struct amba_device *adev)
{
	struct tpdm_drvdata *drvdata = dev_get_drvdata(&adev->dev);

	coresight_unregister(drvdata->csdev);
}

/*
 * Different TPDM has different periph id.
 * The difference is 0-7 bits' value. So ignore 0-7 bits.
 */
static struct amba_id tpdm_ids[] = {
	{
		.id = 0x000f0e00,
		.mask = 0x000fff00,
	},
	{ 0, 0},
};

static struct amba_driver tpdm_driver = {
	.drv = {
		.name   = "coresight-tpdm",
		.owner	= THIS_MODULE,
		.suppress_bind_attrs = true,
	},
	.probe          = tpdm_probe,
	.id_table	= tpdm_ids,
	.remove		= tpdm_remove,
};

module_amba_driver(tpdm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Trace, Profiling & Diagnostic Monitor driver");
