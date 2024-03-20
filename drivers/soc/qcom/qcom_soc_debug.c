// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/panic_notifier.h>
#include <soc/qcom/wdt_core.h>

#include <asm/virt.h>

#define KASLR_IMEM_ADDR_NAME	"qcom,msm-imem-kaslr_offset"
#define KASLR_IMEM_MAGIC	0xdead4ead
#define KASLR_OFFSET_MASK	0x00000000FFFFFFFF

struct notifier_block panic_notifier;

static int panic_handler(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	pr_info("Triggering bite\n");
	qcom_wdt_trigger_bite();
	return NOTIFY_DONE;
}

static void __iomem *get_iomap_addr(const char *prop_name)
{
	struct device_node *node;
	void __iomem *ioaddr;

	node = of_find_compatible_node(NULL, NULL, prop_name);
	if (!node) {
		pr_err("DT property - read error: %s\n", prop_name);
		return NULL;
	}

	ioaddr = of_iomap(node, 0);
	if (!ioaddr)
		pr_err("DT property - map fail: %s\n", prop_name);

	return ioaddr;
}

static void store_kaslr_offset(void)
{
	void __iomem *imem_kaslr_addr = get_iomap_addr(KASLR_IMEM_ADDR_NAME);

	if (!imem_kaslr_addr)
		return;

	__raw_writel(KASLR_IMEM_MAGIC, imem_kaslr_addr);
	__raw_writel((kimage_vaddr - KIMAGE_VADDR) & KASLR_OFFSET_MASK,
		     imem_kaslr_addr + 4);
	__raw_writel(((kimage_vaddr - KIMAGE_VADDR) >> 32) & KASLR_OFFSET_MASK,
		     imem_kaslr_addr + 8);

	iounmap(imem_kaslr_addr);
}

static void disable_soc_wdt_if_required(void)
{
	struct device_node *soc_wdt;
	struct property *newprop;
	int ret;

	if (is_hyp_mode_available())
		return;

	/*
	 * Linux is not booted in hyp mode, assume that Gunyah is active
	 * and removed SoC watchdog I/O access to Linux. Disable SoC
	 * watchdog device node.
	 */
	soc_wdt = of_find_compatible_node(NULL, NULL, "qcom,kpss-wdt");
	if (!soc_wdt)
		return;

	/* Disable the node by flipping the status to disabled */
	newprop = kzalloc(sizeof(*newprop), GFP_KERNEL);
	if (!newprop)
		goto put_node;

	newprop->name = kstrdup("status", GFP_KERNEL);
	if (!newprop->name)
		goto free_prop;

	newprop->value = kstrdup("disabled", GFP_KERNEL);
	if (!newprop->value)
		goto free_prop;

	newprop->length = sizeof("disabled");
	ret = of_update_property(soc_wdt, newprop);
	if (ret) {
		pr_err("Failed to disable SoC watchdog with err %d\n", ret);
		goto free_prop;
	}

	/* newprop has references in dt structures, so don't free it */
	of_node_put(soc_wdt);
	return;
free_prop:
	kfree(newprop->value);
	kfree(newprop->name);
	kfree(newprop);
put_node:
	of_node_put(soc_wdt);
}

static int __init qcom_soc_debug_init(void)
{
	store_kaslr_offset();
	disable_soc_wdt_if_required();

	panic_notifier.priority = INT_MAX - 1;
	panic_notifier.notifier_call = panic_handler;
	atomic_notifier_chain_register(&panic_notifier_list,
				       &panic_notifier);
	return 0;
}

#if IS_MODULE(CONFIG_QCOM_SOC_DEBUG)
module_init(qcom_soc_debug_init);
#else
pure_initcall(qcom_soc_debug_init);
#endif

MODULE_DESCRIPTION("QCOM SOC Debug Driver");
MODULE_LICENSE("GPL");
