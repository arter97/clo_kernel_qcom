/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _UAPI_LINUX_SMCINVOKE_H_
#define _UAPI_LINUX_SMCINVOKE_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define SMCINVOKE_USERSPACE_OBJ_NULL	-1
#define DEFAULT_CB_OBJ_THREAD_CNT	4
#define SMCINVOKE_TZ_MIN_BUF_SIZE	4096

struct smcinvoke_buf {
	__u64 addr;
	__u64 size;
};

struct smcinvoke_obj {
	__s64 fd;
	__s64 cb_server_fd;
	__s64 reserved;
};

union smcinvoke_arg {
	struct smcinvoke_buf b;
	struct smcinvoke_obj o;
};

/*
 * struct smcinvoke_cmd_req: This structure is transparently sent to TEE
 * @op - Operation to be performed
 * @counts - number of aruments passed
 * @result - result of invoke operation
 * @argsize - size of each of arguments
 * @args - args is pointer to buffer having all arguments
 * @reserved: IN/OUT: Usage is not defined but should be set to 0
 */
struct smcinvoke_cmd_req {
	__u32 op;
	__u32 counts;
	__s32 result;
	__u32 argsize;
	__u64 args;
	__s64 reserved;
};

/*
 * struct smcinvoke_accept: structure to process CB req from TEE
 * @has_resp: IN: Whether IOCTL is carrying response data
 * @result: IN: Outcome of operation op
 * @op: OUT: Operation to be performed on target object
 * @counts: OUT: Number of arguments, embedded in buffer pointed by
 *               buf_addr, to complete operation
 * @reserved: IN/OUT: Usage is not defined but should be set to 0.
 * @argsize: IN: Size of any argument, all of equal size, embedded
 *               in buffer pointed by buf_addr
 * @txn_id: OUT: An id that should be passed as it is for response
 * @cbobj_id: OUT: Callback object which is target of operation op
 * @buf_len: IN: Len of buffer pointed by buf_addr
 * @buf_addr: IN: Buffer containing all arguments which are needed
 *                to complete operation op
 */
struct smcinvoke_accept {
	__u32 has_resp;
	__s32 result;
	__u32 op;
	__u32 counts;
	__s32 reserved;
	__u32 argsize;
	__u64 txn_id;
	__s64 cbobj_id;
	__u64 buf_len;
	__u64 buf_addr;
};

/*
 * @cb_buf_size: IN: Max buffer size for any callback obj implemented by client
 * @reserved: IN/OUT: Usage is not defined but should be set to 0
 */
struct smcinvoke_server {
	__u64 cb_buf_size;
	__s64 reserved;
};

#define SMCINVOKE_IOC_MAGIC    0x98

#define SMCINVOKE_IOCTL_INVOKE_REQ \
	_IOWR(SMCINVOKE_IOC_MAGIC, 1, struct smcinvoke_cmd_req)

#define SMCINVOKE_IOCTL_ACCEPT_REQ \
	_IOWR(SMCINVOKE_IOC_MAGIC, 2, struct smcinvoke_accept)

#define SMCINVOKE_IOCTL_SERVER_REQ \
	_IOWR(SMCINVOKE_IOC_MAGIC, 3, struct smcinvoke_server)

#define SMCINVOKE_IOCTL_ACK_LOCAL_OBJ \
	_IOWR(SMCINVOKE_IOC_MAGIC, 4, __s64)

/*
 * smcinvoke logging buffer is for communicating with the smcinvoke driver
 * additional info for debugging to be included in driver's log (if any)
 */
#define SMCINVOKE_LOG_BUF_SIZE 100
#define SMCINVOKE_IOCTL_LOG \
	_IOC(_IOC_READ|_IOC_WRITE, SMCINVOKE_IOC_MAGIC, 255, SMCINVOKE_LOG_BUF_SIZE)

#endif /* _UAPI_LINUX_SMCINVOKE_H_ */
