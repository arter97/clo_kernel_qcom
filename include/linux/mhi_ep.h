/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021, Linaro Ltd.
 *
 */
#ifndef _MHI_EP_H_
#define _MHI_EP_H_

#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/mhi.h>

struct mhi_ep_chan;
struct mhi_ep_cmd;
struct mhi_ep_event;
struct mhi_ep_cmd_ctx;
struct mhi_ep_ev_ctx;
struct mhi_ep_ch_ctx;

struct mhi_ep_channel_config {
	char *name;
	u32 num;
	u32 num_elements;
	enum dma_data_direction dir;
};

struct mhi_ep_cntrl_config {
	u32 max_channels;
	u32 num_channels;
	const struct mhi_ep_channel_config *ch_cfg;
	u32 mhi_version;
};

struct mhi_ep_interrupt_state {
	u32	mask;
	u32	status;
};

struct mhi_ep_cntrl {
	struct device *cntrl_dev;
        struct mhi_ep_device *mhi_dev;
	void __iomem *mmio;
	int irq;

	u32 max_chan;
	struct mhi_ep_chan *mhi_chan;
	struct mhi_ep_cmd *mhi_cmd;
	struct mhi_ep_event *mhi_event;
	struct mhi_ep_sm *sm;

	/* Host control base information */
	struct mhi_ep_ch_ctx *ch_ctx_cache;
	struct mhi_ep_ev_ctx *ev_ctx_cache;
	struct mhi_ep_cmd_ctx *cmd_ctx_cache;

	u64 ch_ctx_host_pa;
	u64 ev_ctx_host_pa;
	u64 cmd_ctx_host_pa;

	struct workqueue_struct *init_wq;
	struct workqueue_struct	*ring_wq;
	struct work_struct init_work;
	struct work_struct chdb_ctrl_work;
	struct work_struct ring_work;

	struct list_head process_ring_list;

	struct mutex lock;
	struct mutex event_lock;

        /* CHDB and EVDB device interrupt state */
        struct mhi_ep_interrupt_state chdb[4];
        struct mhi_ep_interrupt_state evdb[4];

	u32	reg_len;
	u32	version;
	u32	event_rings;
	u32	hw_event_rings;
	u32	channels;
	u32	chdb_offset;
	u32	erdb_offset;

	void (*raise_irq)(struct mhi_ep_cntrl *mhi_cntrl);
	void __iomem *(*alloc_addr)(struct mhi_ep_cntrl *mhi_cntrl,
				  phys_addr_t *phys_addr, size_t size);
	void (*free_addr)(struct mhi_ep_cntrl *mhi_cntrl,
			  phys_addr_t phys_addr, void __iomem *virt_addr, size_t size);
	int (*map_addr)(struct mhi_ep_cntrl *mhi_cntrl,
			phys_addr_t phys_addr, u64 pci_addr, size_t size);
	void (*unmap_addr)(struct mhi_ep_cntrl *mhi_cntrl,
			   phys_addr_t phys_addr);
};

struct mhi_ep_device {
	struct mhi_ep_cntrl *mhi_cntrl;
	const struct mhi_device_id *id;
	const char *name;
	struct device dev;
	struct mhi_ep_chan *ul_chan;
	struct mhi_ep_chan *dl_chan;
	enum mhi_device_type dev_type;
	int ul_chan_id;
	int dl_chan_id;
};

struct mhi_ep_driver {
	const struct mhi_device_id *id_table;
	struct device_driver driver;
	int (*probe)(struct mhi_ep_device *mhi_ep,
		     const struct mhi_device_id *id);
	void (*remove)(struct mhi_ep_device *mhi_ep);
	void (*ul_xfer_cb)(struct mhi_ep_device *mhi_dev,
			   struct mhi_result *result);
	void (*dl_xfer_cb)(struct mhi_ep_device *mhi_dev,
			   struct mhi_result *result);
};

#define to_mhi_ep_device(dev) container_of(dev, struct mhi_ep_device, dev)
#define to_mhi_ep_driver(drv) container_of(drv, struct mhi_ep_driver, driver)

/*
 * module_mhi_ep_driver() - Helper macro for drivers that don't do
 * anything special other than using default mhi_ep_driver_register() and
 * mhi_ep_driver_unregister().  This eliminates a lot of boilerplate.
 * Each module may only use this macro once.
 */
#define module_mhi_ep_driver(mhi_drv) \
	module_driver(mhi_drv, mhi_ep_driver_register, \
		      mhi_ep_driver_unregister)

/*
 * Macro to avoid include chaining to get THIS_MODULE
 */
#define mhi_ep_driver_register(mhi_drv) \
	__mhi_ep_driver_register(mhi_drv, THIS_MODULE)

int __mhi_ep_driver_register(struct mhi_ep_driver *mhi_drv, struct module *owner);
void mhi_ep_driver_unregister(struct mhi_ep_driver *mhi_drv);

int mhi_ep_register_controller(struct mhi_ep_cntrl *mhi_cntrl,
				const struct mhi_ep_cntrl_config *config);
void mhi_ep_power_up(struct mhi_ep_cntrl *mhi_cntrl);

int mhi_ep_queue_skb(struct mhi_ep_device *mhi_dev, enum dma_data_direction dir,
		  struct sk_buff *skb, size_t len, enum mhi_flags mflags);

#endif
