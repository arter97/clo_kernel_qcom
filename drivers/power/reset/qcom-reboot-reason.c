// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/of_address.h>
#include <linux/nvmem-consumer.h>

struct qcom_reboot_reason {
	struct device *dev;
	struct notifier_block reboot_nb;
	struct nvmem_cell *nvmem_cell;
	void __iomem *imem_restart_addr;
};

struct poweroff_reason {
	const char *cmd;
	unsigned char pon_reason;
	unsigned int imem_reason;
};

static struct poweroff_reason reasons[] = {
	{ "recovery",			0x01, 0x77665502 },
	{ "bootloader",			0x02, 0x77665500 },
	{ "rtc",			0x03, 0x77665503 },
	{ "dm-verity device corrupted",	0x04, 0x77665508 },
	{ "dm-verity enforcing",	0x05, 0x77665509 },
	{ "keys clear",			0x06, 0x7766550a },
	{}
};

static int qcom_reboot_reason_reboot(struct notifier_block *this,
				     unsigned long event, void *ptr)
{
	char *cmd = ptr;
	struct qcom_reboot_reason *reboot = container_of(this,
		struct qcom_reboot_reason, reboot_nb);
	struct poweroff_reason *reason;

	if (!cmd)
		return NOTIFY_OK;
	for (reason = reasons; reason->cmd; reason++) {
		if (!strcmp(cmd, reason->cmd)) {
			if (reboot->nvmem_cell) {
				nvmem_cell_write(reboot->nvmem_cell,
						 &reason->pon_reason,
						 sizeof(reason->pon_reason));
			} else {
				if (reboot->imem_restart_addr)
					__raw_writel(reason->imem_reason,
						     reboot->imem_restart_addr);
			}

			break;
		}
	}

	return NOTIFY_OK;
}

static int qcom_reboot_reason_probe(struct platform_device *pdev)
{
	struct qcom_reboot_reason *reboot;
	struct device_node *np;

	reboot = devm_kzalloc(&pdev->dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	reboot->dev = &pdev->dev;

	reboot->nvmem_cell = nvmem_cell_get(reboot->dev, "restart_reason");

	if (IS_ERR(reboot->nvmem_cell)) {
		/* For some of old target like mdm9607 uses IMEM to save reboot reason. */
		np = of_find_compatible_node(NULL, NULL,
					     "qcom,msm-imem-restart_reason");
		if (!np) {
			dev_err(reboot->dev, "Missing qcom,msm-imem-restart_reason node\n");
			return -ENODEV;
		}
		reboot->imem_restart_addr = of_iomap(np, 0);
		if (!reboot->imem_restart_addr) {
			dev_err(reboot->dev, "Unable to map qcom,msm-imem-restart_reason offset\n");
			return -ENOMEM;
		}
		of_node_put(np);
		reboot->nvmem_cell = NULL;
	}

	reboot->reboot_nb.notifier_call = qcom_reboot_reason_reboot;
	reboot->reboot_nb.priority = 255;
	register_reboot_notifier(&reboot->reboot_nb);

	platform_set_drvdata(pdev, reboot);

	return 0;
}

static int qcom_reboot_reason_remove(struct platform_device *pdev)
{
	struct qcom_reboot_reason *reboot = platform_get_drvdata(pdev);

	unregister_reboot_notifier(&reboot->reboot_nb);
	if (reboot->nvmem_cell)
		nvmem_cell_put(reboot->nvmem_cell);
	if (reboot->imem_restart_addr)
		iounmap(reboot->imem_restart_addr);

	return 0;
}

static const struct of_device_id of_qcom_reboot_reason_match[] = {
	{ .compatible = "qcom,reboot-reason", },
	{},
};
MODULE_DEVICE_TABLE(of, of_qcom_reboot_reason_match);

static struct platform_driver qcom_reboot_reason_driver = {
	.probe = qcom_reboot_reason_probe,
	.remove = qcom_reboot_reason_remove,
	.driver = {
		.name = "qcom-reboot-reason",
		.of_match_table = of_match_ptr(of_qcom_reboot_reason_match),
	},
};

module_platform_driver(qcom_reboot_reason_driver);

MODULE_DESCRIPTION("MSM Reboot Reason Driver");
MODULE_LICENSE("GPL v2");
