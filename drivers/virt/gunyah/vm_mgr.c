// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gh_vm_mgr: " fmt

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/gunyah_rsc_mgr.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include <uapi/linux/gunyah.h>

#include "vm_mgr.h"

static void gh_vm_free(struct work_struct *work);

static __must_check struct gh_vm *gh_vm_alloc(struct gh_rm *rm)
{
	struct gh_vm *ghvm;

	ghvm = kzalloc(sizeof(*ghvm), GFP_KERNEL);
	if (!ghvm)
		return ERR_PTR(-ENOMEM);

	ghvm->parent = gh_rm_get(rm);
	ghvm->rm = rm;

	mmgrab(current->mm);
	ghvm->mm = current->mm;
	mutex_init(&ghvm->mm_lock);
	INIT_LIST_HEAD(&ghvm->memory_mappings);
	INIT_WORK(&ghvm->free_work, gh_vm_free);

	return ghvm;
}

static long gh_vm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gh_vm *ghvm = filp->private_data;
	void __user *argp = (void __user *)arg;
	long r;

	switch (cmd) {
	case GH_VM_SET_USER_MEM_REGION: {
		struct gh_userspace_memory_region region;

		/* only allow owner task to add memory */
		if (ghvm->mm != current->mm)
			return -EPERM;

		if (copy_from_user(&region, argp, sizeof(region)))
			return -EFAULT;

		/* All other flag bits are reserved for future use */
		if (region.flags & ~(GH_MEM_ALLOW_READ | GH_MEM_ALLOW_WRITE | GH_MEM_ALLOW_EXEC))
			return -EINVAL;

		r = gh_vm_mem_alloc(ghvm, &region);
		break;
	}
	default:
		r = -ENOTTY;
		break;
	}

	return r;
}

static void gh_vm_free(struct work_struct *work)
{
	struct gh_vm *ghvm = container_of(work, struct gh_vm, free_work);

	gh_vm_mem_reclaim(ghvm);
	gh_rm_put(ghvm->rm);
	mmdrop(ghvm->mm);
	kfree(ghvm);
}

static int gh_vm_release(struct inode *inode, struct file *filp)
{
	struct gh_vm *ghvm = filp->private_data;

	/* VM will be reset and make RM calls which can interruptible sleep.
	 * Defer to a work so this thread can receive signal.
	 */
	schedule_work(&ghvm->free_work);
	return 0;
}

static const struct file_operations gh_vm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gh_vm_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.release = gh_vm_release,
	.llseek = noop_llseek,
};

static long gh_dev_ioctl_create_vm(struct gh_rm *rm, unsigned long arg)
{
	struct gh_vm *ghvm;
	struct file *file;
	int fd, err;

	/* arg reserved for future use. */
	if (arg)
		return -EINVAL;

	ghvm = gh_vm_alloc(rm);
	if (IS_ERR(ghvm))
		return PTR_ERR(ghvm);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_destroy_vm;
	}

	file = anon_inode_getfile("gunyah-vm", &gh_vm_fops, ghvm, O_RDWR);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_fd;
	}

	fd_install(fd, file);

	return fd;

err_put_fd:
	put_unused_fd(fd);
err_destroy_vm:
	gh_vm_free(&ghvm->free_work);
	return err;
}

long gh_dev_vm_mgr_ioctl(struct gh_rm *rm, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case GH_CREATE_VM:
		return gh_dev_ioctl_create_vm(rm, arg);
	default:
		return -ENOTTY;
	}
}
