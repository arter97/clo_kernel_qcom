/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#ifndef _UAPI_LINUX_FSPAPP_FSCRYPT_H
#define _UAPI_LINUX_FSPAPP_FSCRYPT_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FSPAPP_FSCRYPT_MAX_BLOB_SIZE          12288
#define FSPAPP_FSCRYPT_KEY_IDENTIFIER_SIZE    16

struct fspapp_fscrypt_add_key_arg {
	__s32 mount_fd;
	__u32 blob_size;
	__u8 blob[FSPAPP_FSCRYPT_MAX_BLOB_SIZE];
	__u8 identifier[FSPAPP_FSCRYPT_KEY_IDENTIFIER_SIZE];
};

#define FSPAPP_FSCRYPT_IOC_MAGIC 'F'

#define FSPAPP_FSCRYPT_ADD_KEY_FROM_BLOB \
	_IOWR(FSPAPP_FSCRYPT_IOC_MAGIC, 1, struct fspapp_fscrypt_add_key_arg)


#endif /* _UAPI_LINUX_FSPAPP_FSCRYPT_H */
