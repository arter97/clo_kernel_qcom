// SPDX-License-Identifier: GPL-2.0
/*
 * PCI EPF controller driver for MHI Endpoint device
 *
 * Copyright (C) 2021 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 */

#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci_ids.h>
#include <linux/random.h>
#include <linux/of_address.h>
#include <linux/pci_regs.h>
#include <linux/platform_device.h>

#include <linux/mhi_ep.h>
#include <linux/pci_regs.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>

#define MHI_VERSION_1_0 0x01000000

struct pci_epf_mhi_ep_info {
	const struct mhi_ep_cntrl_config *config;
	struct pci_epf_header *epf_header;
	enum pci_barno bar_num;
	u32 epf_flags;
	u32 msi_count;
};

#define MHI_EP_CHANNEL_CONFIG_UL(ch_num, ch_name)	\
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.dir = DMA_TO_DEVICE,			\
	}

#define MHI_EP_CHANNEL_CONFIG_DL(ch_num, ch_name)	\
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.dir = DMA_FROM_DEVICE,			\
	}

static const struct mhi_ep_channel_config mhi_v1_channels[] = {
	MHI_EP_CHANNEL_CONFIG_UL(0, "LOOPBACK"),
	MHI_EP_CHANNEL_CONFIG_DL(1, "LOOPBACK"),
	MHI_EP_CHANNEL_CONFIG_UL(2, "SAHARA"),
	MHI_EP_CHANNEL_CONFIG_DL(3, "SAHARA"),
	MHI_EP_CHANNEL_CONFIG_UL(4, "DIAG"),
	MHI_EP_CHANNEL_CONFIG_DL(5, "DIAG"),
	MHI_EP_CHANNEL_CONFIG_UL(6, "SSR"),
	MHI_EP_CHANNEL_CONFIG_DL(7, "SSR"),
	MHI_EP_CHANNEL_CONFIG_UL(8, "QDSS"),
	MHI_EP_CHANNEL_CONFIG_DL(9, "QDSS"),
	MHI_EP_CHANNEL_CONFIG_UL(10, "EFS"),
	MHI_EP_CHANNEL_CONFIG_DL(11, "EFS"),
	MHI_EP_CHANNEL_CONFIG_UL(12, "MBIM"),
	MHI_EP_CHANNEL_CONFIG_DL(13, "MBIM"),
	MHI_EP_CHANNEL_CONFIG_UL(14, "QMI"),
	MHI_EP_CHANNEL_CONFIG_DL(15, "QMI"),
	MHI_EP_CHANNEL_CONFIG_UL(16, "QMI"),
	MHI_EP_CHANNEL_CONFIG_DL(17, "QMI"),
	MHI_EP_CHANNEL_CONFIG_UL(18, "IP-CTRL-1"),
	MHI_EP_CHANNEL_CONFIG_DL(19, "IP-CTRL-1"),
	MHI_EP_CHANNEL_CONFIG_UL(20, "IPCR"),
	MHI_EP_CHANNEL_CONFIG_DL(21, "IPCR"),
	MHI_EP_CHANNEL_CONFIG_UL(32, "DUN"),
	MHI_EP_CHANNEL_CONFIG_DL(33, "DUN"),
	MHI_EP_CHANNEL_CONFIG_UL(36, "IP_SW0"),
	MHI_EP_CHANNEL_CONFIG_DL(37, "IP_SW0"),
};

static const struct mhi_ep_cntrl_config mhi_v1_config = {
	.max_channels = 128,
	.num_channels = ARRAY_SIZE(mhi_v1_channels),
	.ch_cfg = mhi_v1_channels,
	.mhi_version = MHI_VERSION_1_0,
};

static struct pci_epf_header sdx55_header = {
	.vendorid		= 0x17cb,
	.deviceid		= 0x0306,
	.revid			= 0x0,
	.progif_code		= 0x0,
	.subclass_code		= 0x0,
	.baseclass_code		= 0xff,
	.cache_line_size	= 0x10,
	.subsys_vendor_id	= 0x0,
	.subsys_id		= 0x0,
};

static const struct pci_epf_mhi_ep_info sdx55_info = {
	.config = &mhi_v1_config,
	.epf_header = &sdx55_header,
	.bar_num = BAR_0,
	.epf_flags = PCI_BASE_ADDRESS_MEM_TYPE_32,
	.msi_count = 4,
};

struct pci_epf_mhi {
	struct mhi_ep_cntrl mhi_cntrl;
	struct pci_epf *epf;
	const struct pci_epf_mhi_ep_info *info;
	void __iomem *mmio;
	resource_size_t mmio_phys;
	u32 mmio_size;
	int irq;
};

void __iomem *pci_epf_mhi_alloc_addr(struct mhi_ep_cntrl *mhi_cntrl,
				  phys_addr_t *phys_addr, size_t size)
{
	struct pci_epf_mhi *epf_mhi = container_of(mhi_cntrl, struct pci_epf_mhi, mhi_cntrl);
	struct pci_epc *epc = epf_mhi->epf->epc;

	return pci_epc_mem_alloc_addr(epc, phys_addr, size);
}

void pci_epf_mhi_free_addr(struct mhi_ep_cntrl *mhi_cntrl,
			  phys_addr_t phys_addr, void __iomem *virt_addr, size_t size)
{
	struct pci_epf_mhi *epf_mhi = container_of(mhi_cntrl, struct pci_epf_mhi, mhi_cntrl);
	struct pci_epc *epc = epf_mhi->epf->epc;

	pci_epc_mem_free_addr(epc, phys_addr, virt_addr, size);
}

inline int pci_epf_mhi_map_addr(struct mhi_ep_cntrl *mhi_cntrl,
			phys_addr_t phys_addr, u64 pci_addr, size_t size)
{
	struct pci_epf_mhi *epf_mhi = container_of(mhi_cntrl, struct pci_epf_mhi, mhi_cntrl);
	struct pci_epf *epf = epf_mhi->epf;
	struct pci_epc *epc = epf->epc;

	return pci_epc_map_addr(epc, epf->func_no, epf->vfunc_no, phys_addr, pci_addr, size);
}

void pci_epf_mhi_unmap_addr(struct mhi_ep_cntrl *mhi_cntrl, phys_addr_t phys_addr)
{
	struct pci_epf_mhi *epf_mhi = container_of(mhi_cntrl, struct pci_epf_mhi, mhi_cntrl);
	struct pci_epf *epf = epf_mhi->epf;
	struct pci_epc *epc = epf->epc;

	pci_epc_unmap_addr(epc, epf->func_no, epf->vfunc_no, phys_addr);
}

void pci_epf_mhi_raise_irq(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct pci_epf_mhi *epf_mhi = container_of(mhi_cntrl, struct pci_epf_mhi, mhi_cntrl);
	struct pci_epf *epf = epf_mhi->epf;
	struct pci_epc *epc = epf->epc;

	pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no, PCI_EPC_IRQ_MSI, 1);
}

static int pci_epf_mhi_notifier(struct notifier_block *nb, unsigned long val,
				 void *data)
{
	struct pci_epf *epf = container_of(nb, struct pci_epf, nb);
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	struct mhi_ep_cntrl *mhi_cntrl = &epf_mhi->mhi_cntrl;
	const struct pci_epf_mhi_ep_info *info = epf_mhi->info;
	struct pci_epf_bar *epf_bar = &epf->bar[info->bar_num];
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	int ret;
	u32 dstate;

	switch (val) {
	case CORE_INIT:
		ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, info->epf_header);
		if (ret) {
			dev_err(dev, "Configuration header write failed: %d\n", ret);
			return NOTIFY_BAD;
		}

		epf_bar->phys_addr = epf_mhi->mmio_phys;
		epf_bar->size = epf_mhi->mmio_size;
		epf_bar->barno = info->bar_num;
		epf_bar->flags = info->epf_flags;
		ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
		if (ret) {
			dev_err(dev, "Failed to set BAR0: %d\n", ret);
			return NOTIFY_BAD;
		}

		ret = pci_epc_set_msi(epc, epf->func_no, epf->vfunc_no, order_base_2(info->msi_count));
		if (ret) {
			dev_err(dev, "MSI configuration failed: %d\n", ret);
			return NOTIFY_BAD;
		}

		mhi_cntrl->mmio = epf_mhi->mmio;
		mhi_cntrl->irq = epf_mhi->irq;

		/* Assign the struct dev of PCI EP as MHI controller device */
		mhi_cntrl->cntrl_dev = epc->dev.parent;
		mhi_cntrl->raise_irq = pci_epf_mhi_raise_irq;
		mhi_cntrl->alloc_addr = pci_epf_mhi_alloc_addr;
		mhi_cntrl->free_addr = pci_epf_mhi_free_addr;
		mhi_cntrl->map_addr = pci_epf_mhi_map_addr;
		mhi_cntrl->unmap_addr = pci_epf_mhi_unmap_addr;

		ret = mhi_ep_register_controller(mhi_cntrl, info->config);
		if (ret) {
			dev_err(dev, "Failed to register MHI EP controller\n");
			return NOTIFY_BAD;
		}

		break;
	case LINK_UP:
		break;
	case BME:
		mhi_ep_power_up(mhi_cntrl);
		break;
	case D_STATE:
		dstate = (int)data;

		if (dstate == 0)
			dev_info(dev, "Received D0 event\n");
		break;
	default:
		dev_err(&epf->dev, "Invalid MHI device notifier event: %d\n", ret);
		return NOTIFY_BAD;
	}

	return NOTIFY_OK;
}

static int pci_epf_mhi_bind(struct pci_epf *epf)
{
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct platform_device *pdev = to_platform_device(epc->dev.parent);
	struct device *dev = &epf->dev;
	struct resource *res;
	int ret;

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	/* Get MMIO physical and virtual address from controller device */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mmio");
	epf_mhi->mmio_phys = res->start;
	epf_mhi->mmio_size = resource_size(res);

	epf_mhi->mmio = devm_ioremap_wc(dev, epf_mhi->mmio_phys, epf_mhi->mmio_size);
	if (IS_ERR(epf_mhi->mmio))
		return PTR_ERR(epf_mhi->mmio);

	ret = platform_get_irq_byname(pdev, "doorbell");
	if (ret < 0) {
		dev_err(dev, "Failed to get Doorbell IRQ\n");
		return ret;
	}

	epf_mhi->irq = ret;
	epf->nb.notifier_call = pci_epf_mhi_notifier;
	pci_epc_register_notifier(epc, &epf->nb);

	return 0;
}

static void pci_epf_mhi_unbind(struct pci_epf *epf)
{
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar = &epf->bar[0];

	pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no, epf_bar);
}

static int pci_epf_mhi_probe(struct pci_epf *epf)
{
	struct pci_epf_mhi_ep_info *info =
		(struct pci_epf_mhi_ep_info *) epf->driver->id_table->driver_data;
	struct pci_epf_mhi *epf_mhi;
	struct device *dev = &epf->dev;

	epf_mhi = devm_kzalloc(dev, sizeof(*epf_mhi), GFP_KERNEL);
	if (!epf_mhi)
		return -ENOMEM;

	epf_mhi->info = info;
	epf_mhi->epf = epf;
	epf_set_drvdata(epf, epf_mhi);

	return 0;
}

static void pci_epf_mhi_remove(struct pci_epf *epf)
{
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	struct mhi_ep_cntrl *mhi_cntrl = &epf_mhi->mhi_cntrl;
	
	mhi_ep_unregister_controller(mhi_cntrl);
}

static const struct pci_epf_device_id pci_epf_mhi_ids[] = {
	{
		.name = "pci_epf_mhi", .driver_data = (kernel_ulong_t) &sdx55_info,
	},
	{},
};

static struct pci_epf_ops pci_epf_mhi_ops = {
	.unbind	= pci_epf_mhi_unbind,
	.bind	= pci_epf_mhi_bind,
};

static struct pci_epf_driver pci_epf_mhi_driver = {
	.driver.name	= "pci_epf_mhi",
	.probe		= pci_epf_mhi_probe,
	.remove		= pci_epf_mhi_remove,
	.id_table	= pci_epf_mhi_ids,
	.ops		= &pci_epf_mhi_ops,
	.owner		= THIS_MODULE,
};

static int __init pci_epf_mhi_init(void)
{
	int ret;

	ret = pci_epf_register_driver(&pci_epf_mhi_driver);
	if (ret) {
		pr_err("Failed to register PCI EPF MHI driver: %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(pci_epf_mhi_init);

static void __exit pci_epf_mhi_exit(void)
{
	pci_epf_unregister_driver(&pci_epf_mhi_driver);
}
module_exit(pci_epf_mhi_exit);

MODULE_DESCRIPTION("PCI EPF Controller driver for MHI Device");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_LICENSE("GPL v2");
