// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm QCM2290 Network-on-Chip (NoC) QoS driver
 *
 * Copyright (c) 2021, Linaro Ltd.
 *
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interconnect-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "icc-rpm.h"
#include "smd-rpm.h"

#define APPS_MAS_ID	0
#define EBI_SLV_ID	0

extern int qcom_icc_rpm_set(u64 m, u64 s, u64 bw);
static int ddrbrr_probe(struct platform_device *pdev) {
	int ret;

	/* wait for the RPM proxy */
	if (!qcom_icc_rpm_smd_available())
		return -EPROBE_DEFER;

	ret = qcom_icc_rpm_set(APPS_MAS_ID, EBI_SLV_ID, ULONG_MAX);
	if (!ret)
		pr_err("ddr went brr successfully\n");

	return ret;
};

static const struct of_device_id ddrbrr_of_match[] = {
	{ .compatible = "qcom,ddr-brr" },
	{ }
};
MODULE_DEVICE_TABLE(of, ddrbrr_of_match);

static struct platform_driver ddrbrr = {
	.probe = ddrbrr_probe,
	.driver = {
		.name = "ddr-brr",
		.of_match_table = ddrbrr_of_match,
	},
};
module_platform_driver(ddrbrr);
MODULE_LICENSE("GPL");
