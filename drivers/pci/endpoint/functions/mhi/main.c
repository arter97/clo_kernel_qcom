#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci_ids.h>
#include <linux/random.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>

struct pci_epf_mhi {
	struct pci_epf *epf;
	void __iomem *mmio;
	resource_size_t mmio_phys;
	u32 mmio_size;
};

static struct pci_epf_header mhi_header = {
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

static int pci_epf_mhi_notifier(struct notifier_block *nb, unsigned long val,
				 void *data)
{
	struct pci_epf *epf = container_of(nb, struct pci_epf, nb);
	struct pci_epf_mhi *epf_mhi = epf_get_drvdata(epf);
	struct pci_epf_bar *epf_bar = &epf->bar[0];
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	int ret;

	switch (val) {
	case CORE_INIT:
		ret = pci_epc_write_header(epc, 0, &mhi_header);
		if (ret) {
			dev_err(dev, "Configuration header write failed\n");
			return NOTIFY_BAD;
		}

		epf_bar->phys_addr = epf_mhi->mmio_phys;
		epf_bar->size = epf_mhi->mmio_size;
		epf_bar->barno = BAR_0;
		epf_bar->flags = PCI_BASE_ADDRESS_MEM_TYPE_32;
		ret = pci_epc_set_bar(epc, 0, epf_bar);
		if (ret) {
			dev_err(dev, "Failed to set BAR0\n");
			return NOTIFY_BAD;
		}

		ret = pci_epc_set_msi(epc, 0, order_base_2(16));
		if (ret) {
			dev_err(dev, "MSI configuration failed\n");
			return NOTIFY_BAD;
		}
		break;
	case LINK_UP:
		break;
	default:
		dev_err(&epf->dev, "Invalid EPF mhi notifier event\n");
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

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	/* Get MMIO physical and virtual address from controller device */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mmio");
	epf_mhi->mmio_phys = res->start;
	epf_mhi->mmio_size = resource_size(res);

	epf_mhi->mmio = devm_ioremap_wc(dev, epf_mhi->mmio_phys, epf_mhi->mmio_size);
	if (IS_ERR(epf_mhi->mmio))
		return PTR_ERR(epf_mhi->mmio);

	epf->nb.notifier_call = pci_epf_mhi_notifier;
	pci_epc_register_notifier(epc, &epf->nb);

	return 0;
}

static void pci_epf_mhi_unbind(struct pci_epf *epf)
{
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar = &epf->bar[0];

	pci_epc_clear_bar(epc, epf->func_no, epf_bar);
}

static int pci_epf_mhi_probe(struct pci_epf *epf)
{
	struct pci_epf_mhi *epf_mhi;
	struct device *dev = &epf->dev;

	epf_mhi = devm_kzalloc(dev, sizeof(*epf_mhi), GFP_KERNEL);
	if (!epf_mhi)
		return -ENOMEM;

	epf_set_drvdata(epf, epf_mhi);

	return 0;
}

static const struct pci_epf_device_id pci_epf_mhi_ids[] = {
	{
		.name = "pci_epf_mhi",
	},
	{},
};

static struct pci_epf_ops ops = {
	.unbind	= pci_epf_mhi_unbind,
	.bind	= pci_epf_mhi_bind,
};

static struct pci_epf_driver mhi_driver = {
	.driver.name	= "pci_epf_mhi",
	.probe		= pci_epf_mhi_probe,
	.id_table	= pci_epf_mhi_ids,
	.ops		= &ops,
	.owner		= THIS_MODULE,
};

static int __init pci_epf_mhi_init(void)
{
	int ret;

	ret = pci_epf_register_driver(&mhi_driver);
	if (ret) {
		pr_err("Failed to register pci epf mhi driver --> %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(pci_epf_mhi_init);

static void __exit pci_epf_mhi_exit(void)
{
	pci_epf_unregister_driver(&mhi_driver);
}
module_exit(pci_epf_mhi_exit);
