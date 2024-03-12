// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.

#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/refcount.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/idr.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/termios.h>
#include <linux/soc/qcom/apr.h>
#include <dt-bindings/soc/qcom,gpr.h>
#include "msm_audio_mem.h"
#include "q6apm.h"

#define APM_CMD_SHARED_MEM_MAP_REGIONS          0x0100100C
#define APM_MEMORY_MAP_BIT_MASK_IS_OFFSET_MODE  0x00000004UL

static bool audio_pkt_probed;
/* Define Logging Macros */
static int audio_pkt_debug_mask;
enum {
		AUDIO_PKT_INFO = 1U << 0,
};

#define AUDIO_PKT_INFO(x, ...)							\
do {										\
	if (audio_pkt_debug_mask & AUDIO_PKT_INFO) {				\
		pr_info_ratelimited("[%s]: "x, __func__, ##__VA_ARGS__);	\
	}									\
} while (0)

#define AUDIO_PKT_ERR(x, ...)							\
{										\
	pr_err_ratelimited("[%s]: "x, __func__, ##__VA_ARGS__);			\
}

#define MODULE_NAME "audio-pkt"
#define MINOR_NUMBER_COUNT 1
#define AUDPKT_DRIVER_NAME "aud_pasthru_adsp"
#define CHANNEL_NAME "to_apps"

/**
 * struct audio_pkt - driver context, relates rpdev to cdev
 * @adev:	gpr device node
 * @dev:	audio pkt device
 * @cdev:	cdev for the audio pkt device
 * @lock:	synchronization of @rpdev
 * @queue_lock:	synchronization of @queue operations
 * @queue:	incoming message queue
 * @readq:	wait object for incoming queue
 * @dev_name:	/dev/@dev_name for audio_pkt device
 * @ch_name:	audio channel to match to
 * @audio_pkt_major: Major number of audio pkt driver
 * @audio_pkt_class: audio pkt class pointer
 */
struct audio_pkt_device {
	gpr_device_t *adev;
	struct device *dev;
	struct cdev cdev;

	struct mutex lock;

	spinlock_t queue_lock;
	struct sk_buff_head queue;
	wait_queue_head_t readq;

	char dev_name[20];
	char ch_name[20];

	dev_t audio_pkt_major;
	struct class *audio_pkt_class;
};

struct audio_pkt_apm_cmd_shared_mem_map_regions_t {
	uint16_t mem_pool_id;
	uint16_t num_regions;
	uint32_t property_flag;

};

struct audio_pkt_apm_shared_map_region_payload_t {
	uint32_t shm_addr_lsw;
	uint32_t shm_addr_msw;
	uint32_t mem_size_bytes;
};

struct audio_pkt_apm_mem_map {
	struct audio_pkt_apm_cmd_shared_mem_map_regions_t mmap_header;
	struct audio_pkt_apm_shared_map_region_payload_t mmap_payload;
};

struct audio_gpr_pkt {
	struct gpr_hdr audpkt_hdr;
	struct audio_pkt_apm_mem_map audpkt_mem_map;
};

typedef void (*audio_pkt_clnt_cb_fn)(void *buf, int len, void *priv);

struct audio_pkt_clnt_ch {
	int client_id;
	audio_pkt_clnt_cb_fn func;
};

#define dev_to_audpkt_dev(_dev) container_of(_dev, struct audio_pkt_device, dev)
#define cdev_to_audpkt_dev(_cdev) container_of(_cdev, struct audio_pkt_device, cdev)

/**
 * audio_pkt_open() - open() syscall for the audio_pkt device
 * inode:	Pointer to the inode structure.
 * file:	Pointer to the file structure.
 *
 * This function is used to open the audio pkt device when
 * userspace client do a open() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
int audio_pkt_open(struct inode *inode, struct file *file)
{
	struct audio_pkt_device *audpkt_dev = cdev_to_audpkt_dev(inode->i_cdev);
	struct device *dev = audpkt_dev->dev;

	AUDIO_PKT_ERR("for %s\n", audpkt_dev->ch_name);

	get_device(dev);
	file->private_data = audpkt_dev;

	return 0;
}

/**
 * audio_pkt_release() - release operation on audio_pkt device
 * inode:	Pointer to the inode structure.
 * file:	Pointer to the file structure.
 *
 * This function is used to release the audio pkt device when
 * userspace client do a close() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
int audio_pkt_release(struct inode *inode, struct file *file)
{
	struct audio_pkt_device *audpkt_dev = cdev_to_audpkt_dev(inode->i_cdev);
	struct device *dev = audpkt_dev->dev;
	struct sk_buff *skb;
	unsigned long flags;

	spin_lock_irqsave(&audpkt_dev->queue_lock, flags);

	/* Discard all SKBs */
	while (!skb_queue_empty(&audpkt_dev->queue)) {
		skb = skb_dequeue(&audpkt_dev->queue);
		kfree_skb(skb);
	}
	wake_up_interruptible(&audpkt_dev->readq);
	spin_unlock_irqrestore(&audpkt_dev->queue_lock, flags);

	put_device(dev);
	file->private_data = NULL;
	q6apm_close_all();
	msm_audio_mem_crash_handler();

	return 0;
}

/**
 * audio_pkt_read() - read() syscall for the audio_pkt device
 * file:	Pointer to the file structure.
 * buf:		Pointer to the userspace buffer.
 * count:	Number bytes to read from the file.
 * ppos:	Pointer to the position into the file.
 *
 * This function is used to Read the data from audio pkt device when
 * userspace client do a read() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
ssize_t audio_pkt_read(struct file *file, char __user *buf,
		       size_t count, loff_t *ppos)
{
	struct audio_pkt_device *audpkt_dev = file->private_data;
	unsigned long flags;
	struct sk_buff *skb;
	int use;
	uint32_t *temp;

	if (!audpkt_dev) {
		AUDIO_PKT_ERR("invalid device handle\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&audpkt_dev->queue_lock, flags);
	/* Wait for data in the queue */
	if (skb_queue_empty(&audpkt_dev->queue)) {
		spin_unlock_irqrestore(&audpkt_dev->queue_lock, flags);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* Wait until we get data or the endpoint goes away */
		if (wait_event_interruptible(audpkt_dev->readq,
					!skb_queue_empty(&audpkt_dev->queue)))
			return -ERESTARTSYS;

		spin_lock_irqsave(&audpkt_dev->queue_lock, flags);
	}

	skb = skb_dequeue(&audpkt_dev->queue);
	spin_unlock_irqrestore(&audpkt_dev->queue_lock, flags);
	if (!skb)
		return -EFAULT;

	use = min_t(size_t, count, skb->len);
	if (copy_to_user(buf, skb->data, use))
		use = -EFAULT;
	temp = (uint32_t *) skb->data;
	kfree_skb(skb);

	return use;
}

/**
 * audpkt_update_physical_addr - Update physical address
 * audpkt_hdr:	Pointer to the file structure.
 */
int audpkt_chk_and_update_physical_addr(struct audio_gpr_pkt *gpr_pkt)
{
	size_t pa_len = 0;
	dma_addr_t paddr = 0;
	int ret = 0;

	if (gpr_pkt->audpkt_mem_map.mmap_header.property_flag &
				APM_MEMORY_MAP_BIT_MASK_IS_OFFSET_MODE) {

		/* TODO: move physical address mapping to use DMA-BUF heaps */
		ret = msm_audio_get_phy_addr(
				(int) gpr_pkt->audpkt_mem_map.mmap_payload.shm_addr_lsw,
				&paddr, &pa_len);
		if (ret < 0) {
			AUDIO_PKT_ERR("%s Get phy. address failed, ret %d\n",
					__func__, ret);
			return ret;
		}

		AUDIO_PKT_INFO("%s physical address %pK", __func__,
				(void *) paddr);
		gpr_pkt->audpkt_mem_map.mmap_payload.shm_addr_lsw = (uint32_t) paddr;
		gpr_pkt->audpkt_mem_map.mmap_payload.shm_addr_msw = (uint64_t) paddr >> 32;
	}
	return ret;
}

/**
 * audio_pkt_write() - write() syscall for the audio_pkt device
 * file:	Pointer to the file structure.
 * buf:		Pointer to the userspace buffer.
 * count:	Number bytes to read from the file.
 * ppos:	Pointer to the position into the file.
 *
 * This function is used to write the data to audio pkt device when
 * userspace client do a write() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
ssize_t audio_pkt_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	struct audio_pkt_device *audpkt_dev = file->private_data;
	struct gpr_hdr *audpkt_hdr = NULL;
	void *kbuf;
	int ret;

	if (!audpkt_dev)  {
		AUDIO_PKT_ERR("invalid device handle\n");
		return -EINVAL;
	}

	kbuf = memdup_user(buf, count);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	audpkt_hdr = (struct gpr_hdr *) kbuf;
	if (audpkt_hdr->opcode == APM_CMD_SHARED_MEM_MAP_REGIONS) {
		ret = audpkt_chk_and_update_physical_addr((struct audio_gpr_pkt *) audpkt_hdr);
		if (ret < 0) {
			AUDIO_PKT_ERR("Update Physical Address Failed -%d\n", ret);
			return ret;
		}
	}

	if (mutex_lock_interruptible(&audpkt_dev->lock)) {
		ret = -ERESTARTSYS;
		goto free_kbuf;
	}
	ret = gpr_send_pkt(audpkt_dev->adev, (struct gpr_pkt *) kbuf);
	if (ret < 0) {
		AUDIO_PKT_ERR("APR Send Packet Failed ret -%d\n", ret);
		return ret;
	}
	mutex_unlock(&audpkt_dev->lock);

free_kbuf:
	kfree(kbuf);
	return ret < 0 ? ret : count;
}

/**
 * audio_pkt_poll() - poll() syscall for the audio_pkt device
 * file:	Pointer to the file structure.
 * wait:	pointer to Poll table.
 *
 * This function is used to poll on the audio pkt device when
 * userspace client do a poll() system call. All input arguments are
 * validated by the virtual file system before calling this function.
 */
static unsigned int audio_pkt_poll(struct file *file, poll_table *wait)
{
	struct audio_pkt_device *audpkt_dev = file->private_data;
	unsigned int mask = 0;
	unsigned long flags;

	audpkt_dev = file->private_data;
	if (!audpkt_dev) {
		AUDIO_PKT_ERR("invalid device handle\n");
		return POLLERR;
	}

	poll_wait(file, &audpkt_dev->readq, wait);

	mutex_lock(&audpkt_dev->lock);

	spin_lock_irqsave(&audpkt_dev->queue_lock, flags);
	if (!skb_queue_empty(&audpkt_dev->queue))
		mask |= POLLIN | POLLRDNORM;

	spin_unlock_irqrestore(&audpkt_dev->queue_lock, flags);

	mutex_unlock(&audpkt_dev->lock);

	return mask;
}

static const struct file_operations audio_pkt_fops = {
	.owner = THIS_MODULE,
	.open = audio_pkt_open,
	.release = audio_pkt_release,
	.read = audio_pkt_read,
	.write = audio_pkt_write,
	.poll = audio_pkt_poll,
};

/**
 * audio_pkt_srvc_callback() - Callback from gpr driver
 * adev:	pointer to the gpr device of this audio packet device
 * data:	APR response data packet
 *
 * return:	0 for success, Standard Linux errors
 */
static int audio_pkt_srvc_callback(struct gpr_resp_pkt *data, void *priv, int op)
{
	gpr_device_t *gdev = priv;
	struct audio_pkt_device *audpkt_dev = dev_get_drvdata(&gdev->dev);
	unsigned long flags;
	struct sk_buff *skb;
	struct gpr_hdr *hdr = &data->hdr;
	uint8_t *pkt = NULL;
	uint16_t hdr_size, pkt_size;

	hdr_size = hdr->hdr_size * 4;
	pkt_size = hdr->pkt_size;

	pkt = kmalloc(pkt_size, GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	memcpy(pkt, (uint8_t *)data, hdr_size);
	memcpy(pkt + hdr_size, (uint8_t *)data->payload, pkt_size - hdr_size);

	skb = alloc_skb(pkt_size, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	skb_put_data(skb, (void *)pkt, pkt_size);

	kfree(pkt);
	spin_lock_irqsave(&audpkt_dev->queue_lock, flags);
	skb_queue_tail(&audpkt_dev->queue, skb);
	spin_unlock_irqrestore(&audpkt_dev->queue_lock, flags);

	/* wake up any blocking processes, waiting for new data */
	wake_up_interruptible(&audpkt_dev->readq);
	return 0;
}

/**
 * audio_pkt_probe() - Probe a AUDIO packet device
 *
 * adev:	Pointer to gpr device.
 *
 * return:	0 on success, standard Linux error codes on error.
 *
 * This function is called when the underlying device tree driver registers
 * a gpr device, mapped to a Audio packet device.
 */
static int audio_pkt_probe(gpr_device_t *adev)
{
	struct audio_pkt_device *audpkt_dev;
	struct device *dev = &adev->dev;
	int ret;

	if (audio_pkt_probed) {
		AUDIO_PKT_ERR("audio packet probe already done, ssr unsupported\n");
		return -EINVAL;
	}

	audpkt_dev = devm_kzalloc(dev, sizeof(*audpkt_dev), GFP_KERNEL);
	if (!audpkt_dev)
		return -ENOMEM;

	ret = alloc_chrdev_region(&audpkt_dev->audio_pkt_major, 0,
				  MINOR_NUMBER_COUNT, AUDPKT_DRIVER_NAME);
	if (ret < 0) {
		AUDIO_PKT_ERR("alloc_chrdev_region failed ret:%d\n", ret);
		goto err_chrdev;
	}

	audpkt_dev->audio_pkt_class = class_create(AUDPKT_DRIVER_NAME);
	if (IS_ERR(audpkt_dev->audio_pkt_class)) {
		ret = PTR_ERR(audpkt_dev->audio_pkt_class);
		AUDIO_PKT_ERR("class_create failed ret:%ld\n",
			      PTR_ERR(audpkt_dev->audio_pkt_class));
		goto err_class;
	}

	audpkt_dev->dev = device_create(audpkt_dev->audio_pkt_class, NULL,
					audpkt_dev->audio_pkt_major, NULL,
					AUDPKT_DRIVER_NAME);
	if (IS_ERR(audpkt_dev->dev)) {
		ret = PTR_ERR(audpkt_dev->dev);
		AUDIO_PKT_ERR("device_create failed ret:%ld\n",
			      PTR_ERR(audpkt_dev->dev));
		goto err_device;
	}
	strscpy(audpkt_dev->dev_name, CHANNEL_NAME, 20);
	strscpy(audpkt_dev->ch_name, CHANNEL_NAME, 20);
	dev_set_name(audpkt_dev->dev, audpkt_dev->dev_name);

	mutex_init(&audpkt_dev->lock);

	spin_lock_init(&audpkt_dev->queue_lock);
	skb_queue_head_init(&audpkt_dev->queue);
	init_waitqueue_head(&audpkt_dev->readq);

	audpkt_dev->adev = adev;
	dev_set_drvdata(dev, audpkt_dev);

	cdev_init(&audpkt_dev->cdev, &audio_pkt_fops);
	audpkt_dev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&audpkt_dev->cdev, audpkt_dev->audio_pkt_major,
		       MINOR_NUMBER_COUNT);
	if (ret) {
		AUDIO_PKT_ERR("cdev_add failed for %s ret:%d\n",
			      audpkt_dev->dev_name, ret);
		goto free_dev;
	}

	AUDIO_PKT_INFO("Audio Packet Port Driver Initialized\n");
	audio_pkt_probed = true;
	return of_platform_populate(dev->of_node, NULL, NULL, dev);

free_dev:
	put_device(dev);
	device_destroy(audpkt_dev->audio_pkt_class, audpkt_dev->audio_pkt_major);
err_device:
	class_destroy(audpkt_dev->audio_pkt_class);
err_class:
	unregister_chrdev_region(MAJOR(audpkt_dev->audio_pkt_major),
				 MINOR_NUMBER_COUNT);
err_chrdev:
	return ret;

}

/**
 * audio_pkt_remove() - Remove a AUDIO packet device
 *
 * adev:	Pointer to gpr device.
 *
 * return:	0 on success, standard Linux error codes on error.
 *
 * This function is called when the underlying device tree driver
 * removeds a gpr device, mapped to a Audio packet device.
 */
static void audio_pkt_remove(gpr_device_t *adev)
{
	of_platform_depopulate(&adev->dev);
	AUDIO_PKT_INFO("Audio Packet Port Driver Removed\n");
}

static const struct of_device_id audio_pkt_match_table[] = {
	{ .compatible = "qcom,audio-pkt" },
	{},
};
MODULE_DEVICE_TABLE(of, audio_pkt_match_table);

static gpr_driver_t audio_pkt_driver = {
	.probe = audio_pkt_probe,
	.remove = audio_pkt_remove,
	.gpr_callback = audio_pkt_srvc_callback,
	.driver = {
		.name = MODULE_NAME,
		.of_match_table = of_match_ptr(audio_pkt_match_table),
	 },
};

module_gpr_driver(audio_pkt_driver);
MODULE_DESCRIPTION("MSM Audio Packet Driver");
MODULE_LICENSE("GPL");
