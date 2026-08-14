// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
/*
 * fspapp_fscrypt.c - FSPAPP backed fscrypt v2 key loader
 *
 * Userspace passes a wrapped blob via ioctl; driver unwraps it through
 * FSPAPP/QTEE, adds the raw key to the fscrypt keyring, and returns the
 * fscrypt v2 master_key_identifier. The raw key is never exposed to userspace.
 */
#define pr_fmt(fmt) "fspapp_fscrypt: %s: " fmt, __func__
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fscrypt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/qtee_shmbridge.h>
#include <linux/firmware/qcom/si_object.h>
#include <linux/firmware/qcom/si_core_xts.h>
#include <uapi/linux/fspapp_fscrypt.h>

/* IFSPApp definitions */

#define IFSPApp_FSP_WRAP_KEY_FEATURE_ID  7
#define IFSPApp_OP_unwrap_wrapped_key    3
#define FSPAPP_SERVICE_UID               0x000001c1
#define FSPAPP_FSCRYPT_RAW_KEY_SIZE      64

/**
 * struct ifspapp_wrap_key_blob - FSPAPP unwrap request buffer
 * @feature_id:   must be IFSPApp_FSP_WRAP_KEY_FEATURE_ID
 * @is_encrypted: set to 1 for wrapped blobs
 * @wrapkey:      wrapped key blob bytes
 * @wrapkey_size: number of valid bytes in @wrapkey
 */
struct ifspapp_wrap_key_blob {
	u32 feature_id;
	u32 is_encrypted;
	u8  wrapkey[FSPAPP_FSCRYPT_MAX_BLOB_SIZE];
	u64 wrapkey_size;
};

/**
 * struct fspapp_dev - per-instance driver state
 * @service: SI object handle for the FSPAPP secure service
 * @oic:     SI object invoke context for QTEE calls
 * @lock:    serialises service open/close and QTEE invocations
 * @devt:    allocated character device number
 * @cdev:    kernel character device
 * @class:   sysfs/udev class
 * @device:  sysfs/udev device node
 */
struct fspapp_dev {
	struct si_object            *service;
	struct si_object_invoke_ctx  oic;
	struct mutex                 lock;
	dev_t           devt;
	struct cdev     cdev;
	struct class   *class;
	struct device  *device;
};

/**
 * fspapp_fscrypt_open_service() - open the FSPAPP secure service
 * @fdev: per-device driver state
 * @dev:  device used for error logging
 *
 * Return: 0 on success, negative errno on failure
 */
static int fspapp_fscrypt_open_service(struct fspapp_dev *fdev,
				       struct device *dev)
{
	struct si_object *client_env = NULL_SI_OBJECT;
	int ret;

	mutex_lock(&fdev->lock);
	if (fdev->service) {
		mutex_unlock(&fdev->lock);
		return 0;
	}
	ret = si_core_get_client_env(&fdev->oic, &client_env);
	if (ret) {
		dev_err(dev, "si_core_get_client_env failed rc=%d\n", ret);
		goto out_unlock;
	}
	if (!client_env || client_env == NULL_SI_OBJECT) {
		dev_err(dev, "si_core_get_client_env returned invalid env\n");
		ret = -ENODEV;
		goto out_unlock;
	}
	ret = si_core_client_env_open(&fdev->oic, client_env,
				      FSPAPP_SERVICE_UID, &fdev->service);
	if (ret) {
		dev_err(dev, "si_core_client_env_open failed uid=0x%x rc=%d\n",
			FSPAPP_SERVICE_UID, ret);
		fdev->service = NULL;
	}
out_unlock:
	mutex_unlock(&fdev->lock);
	return ret;
}

/**
 * fspapp_fscrypt_close_service() - release the FSPAPP secure service handle
 * @fdev: per-device driver state
 */
static void fspapp_fscrypt_close_service(struct fspapp_dev *fdev)
{
	mutex_lock(&fdev->lock);
	if (fdev->service) {
		put_si_object(fdev->service);
		fdev->service = NULL;
	}
	mutex_unlock(&fdev->lock);
}

/**
 * fspapp_fscrypt_prepare_blob() - populate an ifspapp_wrap_key_blob
 * @wrapped:   destination buffer (already allocated, will be zeroed)
 * @blob:      raw wrapped blob bytes from userspace
 * @blob_size: byte count of @blob
 *
 * Return: 0 on success, negative errno on failure
 */
static int fspapp_fscrypt_prepare_blob(struct ifspapp_wrap_key_blob *wrapped,
				       const u8 *blob, u32 blob_size)
{
	if (!wrapped || !blob) {
		pr_err("invalid input: wrapped=%p blob=%p\n", wrapped, blob);
		return -EINVAL;
	}
	if (!blob_size || blob_size > sizeof(wrapped->wrapkey)) {
		pr_err("invalid blob size=%u max=%zu\n",
		       blob_size, sizeof(wrapped->wrapkey));
		return -EINVAL;
	}
	memset(wrapped, 0, sizeof(*wrapped));
	wrapped->feature_id   = IFSPApp_FSP_WRAP_KEY_FEATURE_ID;
	wrapped->is_encrypted = 1;
	memcpy(wrapped->wrapkey, blob, blob_size);
	wrapped->wrapkey_size = blob_size;
	return 0;
}

/**
 * fspapp_fscrypt_unwrap_wrapped_key() - invoke FSPAPP to unwrap a key blob
 * @fdev:            per-device driver state
 * @wrapped_key:     prepared input blob
 * @raw_key:         output buffer for the unwrapped key
 * @raw_key_size:    capacity of @raw_key in bytes
 * @raw_key_len_out: set to the actual unwrapped key length on success
 *
 * Return: 0 on success, negative errno on failure
 */
static int fspapp_fscrypt_unwrap_wrapped_key(
	struct fspapp_dev *fdev,
	const struct ifspapp_wrap_key_blob *wrapped_key,
	void *raw_key, size_t raw_key_size, size_t *raw_key_len_out)
{
	struct si_arg args[3] = { };
	int ret, result;

	if (!wrapped_key || !raw_key || !raw_key_size) {
		pr_err("invalid input: wrapped=%p raw_key=%p size=%zu\n",
		       wrapped_key, raw_key, raw_key_size);
		return -EINVAL;
	}
	if (!wrapped_key->wrapkey_size ||
	    wrapped_key->wrapkey_size > sizeof(wrapped_key->wrapkey)) {
		pr_err("invalid wrapped key size=%llu max=%zu\n",
		       (unsigned long long)wrapped_key->wrapkey_size,
		       sizeof(wrapped_key->wrapkey));
		return -EINVAL;
	}
	mutex_lock(&fdev->lock);
	if (!fdev->service) {
		mutex_unlock(&fdev->lock);
		pr_err("FSPAPP service not open\n");
		return -ENODEV;
	}
	args[0] = (struct si_arg){ .type = SI_AT_IB,
		.b = { .addr = (void *)wrapped_key,
		       .size = sizeof(*wrapped_key) } };
	args[1] = (struct si_arg){ .type = SI_AT_OB,
		.b = { .addr = raw_key, .size = raw_key_size } };
	args[2] = (struct si_arg){ .type = SI_AT_END };

	ret = si_object_do_invoke(&fdev->oic, fdev->service,
				  IFSPApp_OP_unwrap_wrapped_key, args, &result);
	mutex_unlock(&fdev->lock);
	if (ret) {
		pr_err("FSPAPP transport invoke failed rc=%d\n", ret);
		return ret;
	}
	if (result) {
		pr_err("FSPAPP service error=%d\n", result);
		return -EINVAL;
	}
	if (args[1].b.size > raw_key_size) {
		pr_err("FSPAPP output overflow actual=%zu max=%zu\n",
		       args[1].b.size, raw_key_size);
		return -EOVERFLOW;
	}
	if (raw_key_len_out)
		*raw_key_len_out = args[1].b.size;
	return 0;
}

static long fspapp_fscrypt_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct fspapp_dev *fdev = container_of(file->private_data,
					       struct fspapp_dev, cdev);
	struct fspapp_fscrypt_add_key_arg *req = NULL;
	struct ifspapp_wrap_key_blob *wrapped = NULL;
	struct fd mount_fd;
	struct inode *inode;
	struct super_block *sb;
	u8 raw_key[FSPAPP_FSCRYPT_RAW_KEY_SIZE] = {};
	size_t raw_key_len = 0;
	int ret;

	if (cmd != FSPAPP_FSCRYPT_ADD_KEY_FROM_BLOB) {
		pr_err("unsupported ioctl cmd=0x%x\n", cmd);
		return -ENOTTY;
	}
	req = memdup_user((const void __user *)arg, sizeof(*req));
	if (IS_ERR(req))
		return PTR_ERR(req);
	if (!req->blob_size || req->blob_size > FSPAPP_FSCRYPT_MAX_BLOB_SIZE) {
		pr_err("invalid blob_size=%u max=%u\n",
		       req->blob_size, FSPAPP_FSCRYPT_MAX_BLOB_SIZE);
		ret = -EINVAL;
		goto out_free_req;
	}
	mount_fd = fdget(req->mount_fd);
	if (!mount_fd.file) {
		pr_err("fdget failed for mount_fd=%d\n", req->mount_fd);
		ret = -EBADF;
		goto out_free_req;
	}
	inode = file_inode(mount_fd.file);
	if (!inode) {
		pr_err("failed to get inode from mount fd\n");
		ret = -EINVAL;
		goto out_fdput;
	}
	if (!S_ISDIR(inode->i_mode)) {
		pr_err("mount_fd not a directory inode mode=0%o\n",
		       inode->i_mode);
		ret = -ENOTDIR;
		goto out_fdput;
	}
	sb = inode->i_sb;
	if (!sb) {
		pr_err("superblock is NULL\n");
		ret = -EINVAL;
		goto out_fdput;
	}
	wrapped = kzalloc(sizeof(*wrapped), GFP_KERNEL);
	if (!wrapped) {
		ret = -ENOMEM;
		goto out_fdput;
	}
	ret = fspapp_fscrypt_prepare_blob(wrapped, req->blob, req->blob_size);
	if (ret) {
		pr_err("prepare blob failed rc=%d\n", ret);
		goto out_free_wrapped;
	}
	ret = fspapp_fscrypt_unwrap_wrapped_key(fdev, wrapped, raw_key,
						sizeof(raw_key), &raw_key_len);
	if (ret) {
		pr_err("unwrap failed rc=%d\n", ret);
		goto out_clear;
	}
	if (raw_key_len != FSPAPP_FSCRYPT_RAW_KEY_SIZE) {
		pr_err("unexpected raw key size %zu expected=%u\n",
		       raw_key_len, FSPAPP_FSCRYPT_RAW_KEY_SIZE);
		ret = -EINVAL;
		goto out_clear;
	}
	ret = fscrypt_add_key_from_kernel(sb, raw_key, raw_key_len,
					  req->identifier);
	if (ret) {
		pr_err("fscrypt_add_key_from_kernel failed rc=%d\n", ret);
		goto out_clear;
	}
	if (copy_to_user(
		&((struct fspapp_fscrypt_add_key_arg __user *)arg)->identifier,
		req->identifier, FSCRYPT_KEY_IDENTIFIER_SIZE)) {
		pr_err("copy_to_user failed for identifier\n");
		ret = -EFAULT;
	}
out_clear:
	memzero_explicit(raw_key, sizeof(raw_key));
out_free_wrapped:
	kfree_sensitive(wrapped);
out_fdput:
	fdput(mount_fd);
out_free_req:
	if (req) {
		memzero_explicit(req->blob, sizeof(req->blob));
		kfree_sensitive(req);
	}
	return ret;
}

static int fspapp_fscrypt_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_cdev;
	return 0;
}

static const struct file_operations fspapp_fscrypt_fops = {
	.owner          = THIS_MODULE,
	.open           = fspapp_fscrypt_open,
	.unlocked_ioctl = fspapp_fscrypt_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = fspapp_fscrypt_ioctl,
#endif
};

/**
 * fspapp_fscrypt_chrdev_create() - allocate and register the character device
 * @fdev: per-device driver state
 *
 * Return: 0 on success, negative errno on failure
 */
static int fspapp_fscrypt_chrdev_create(struct fspapp_dev *fdev)
{
	int ret;

	ret = alloc_chrdev_region(&fdev->devt, 0, 1, "fspapp_fscrypt");
	if (ret)
		return ret;
	cdev_init(&fdev->cdev, &fspapp_fscrypt_fops);
	fdev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&fdev->cdev, fdev->devt, 1);
	if (ret)
		goto err_unregister;
	fdev->class = class_create("fspapp_fscrypt");
	if (IS_ERR(fdev->class)) {
		ret = PTR_ERR(fdev->class);
		goto err_cdev;
	}
	fdev->device = device_create(fdev->class, NULL, fdev->devt,
				     NULL, "fspapp_fscrypt");
	if (IS_ERR(fdev->device)) {
		ret = PTR_ERR(fdev->device);
		goto err_class;
	}
	return 0;
err_class:
	class_destroy(fdev->class);
err_cdev:
	cdev_del(&fdev->cdev);
err_unregister:
	unregister_chrdev_region(fdev->devt, 1);
	return ret;
}

/**
 * fspapp_fscrypt_chrdev_destroy() - tear down the character device
 * @fdev: per-device driver state
 */
static void fspapp_fscrypt_chrdev_destroy(struct fspapp_dev *fdev)
{
	if (fdev->device)
		device_destroy(fdev->class, fdev->devt);
	if (fdev->class)
		class_destroy(fdev->class);
	cdev_del(&fdev->cdev);
	unregister_chrdev_region(fdev->devt, 1);
}

static int fspapp_fscrypt_probe(struct platform_device *pdev)
{
	struct fspapp_dev *fdev;
	int ret;

	fdev = devm_kzalloc(&pdev->dev, sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return -ENOMEM;
	mutex_init(&fdev->lock);
	platform_set_drvdata(pdev, fdev);

	ret = fspapp_fscrypt_open_service(fdev, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "open FSPAPP service failed rc=%d\n", ret);
		return ret;
	}
	ret = fspapp_fscrypt_chrdev_create(fdev);
	if (ret) {
		dev_err(&pdev->dev, "char device create failed rc=%d\n", ret);
		fspapp_fscrypt_close_service(fdev);
	}
	return ret;
}

static int fspapp_fscrypt_remove(struct platform_device *pdev)
{
	struct fspapp_dev *fdev = platform_get_drvdata(pdev);

	fspapp_fscrypt_chrdev_destroy(fdev);
	fspapp_fscrypt_close_service(fdev);
	return 0;
}

static const struct of_device_id fspapp_fscrypt_of_match[] = {
	{ .compatible = "qcom,fspapp-fscrypt" },
	{ }
};
MODULE_DEVICE_TABLE(of, fspapp_fscrypt_of_match);

static struct platform_driver fspapp_fscrypt_driver = {
	.probe  = fspapp_fscrypt_probe,
	.remove = fspapp_fscrypt_remove,
	.driver = {
		.name           = "fspapp-fscrypt",
		.of_match_table = fspapp_fscrypt_of_match,
	},
};
module_platform_driver(fspapp_fscrypt_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QTI FSPAPP backed fscrypt v2 key loader");
MODULE_SOFTDEP("pre: si_core_module");
MODULE_IMPORT_NS(DMA_BUF);
