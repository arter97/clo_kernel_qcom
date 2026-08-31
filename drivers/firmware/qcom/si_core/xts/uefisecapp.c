// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/efi.h>
#include <linux/ucs2_string.h>
#include <linux/firmware/qcom/si_object.h>

#define pr_fmt(fmt) "uefisecapp: %s: " fmt, __func__

static struct si_object *uefisecapp;
static struct efivars qcom_efivars;

#define IUEFISecApp_SUCCESS 0
#define IUEFISecApp_ERROR_INVALID_PARAMETER 10
#define IUEFISecApp_ERROR_UNSUPPORTED 11
#define IUEFISecApp_ERROR_WRITE_PROTECTED 12
#define IUEFISecApp_ERROR_SECURITY_VIOLATION 13
#define IUEFISecApp_ERROR_DEVICE_ERROR 14
#define IUEFISecApp_ERROR_OUT_OF_RESOURCES 15
#define IUEFISecApp_ERROR_VOLUME_CORRUPTED 16
#define IUEFISecApp_ERROR_SIZE_OUT 17
#define IUEFISecApp_ERROR_NOT_FOUND 18
#define IUEFISecApp_ERROR_ALREADY_STARTED 19

static inline efi_status_t uefisecapp_err_to_efi_status(u32 err)
{
	switch (err) {
	case IUEFISecApp_SUCCESS:
		return EFI_SUCCESS;

	case IUEFISecApp_ERROR_INVALID_PARAMETER:
		return EFI_INVALID_PARAMETER;

	case IUEFISecApp_ERROR_UNSUPPORTED:
		return EFI_UNSUPPORTED;

	case IUEFISecApp_ERROR_WRITE_PROTECTED:
		return EFI_WRITE_PROTECTED;

	case IUEFISecApp_ERROR_SECURITY_VIOLATION:
		return EFI_SECURITY_VIOLATION;

	case IUEFISecApp_ERROR_DEVICE_ERROR:
		return EFI_DEVICE_ERROR;

	case IUEFISecApp_ERROR_OUT_OF_RESOURCES:
		return EFI_OUT_OF_RESOURCES;

	case IUEFISecApp_ERROR_SIZE_OUT:
		return EFI_BUFFER_TOO_SMALL;

	case IUEFISecApp_ERROR_NOT_FOUND:
		return EFI_NOT_FOUND;

	/* No matching on EFI_* list. */
	case IUEFISecApp_ERROR_ALREADY_STARTED:	/* EFI_ALREADY_STARTED.  */
	case IUEFISecApp_ERROR_VOLUME_CORRUPTED: /* EFI_VOLUME_CORRUPTED. */
	default:
		return EFI_DEVICE_ERROR;
	}
}

#define IUEFISecApp_OP_getVariable 0
#define IUEFISecApp_OP_setVariable 1
#define IUEFISecApp_OP_queryVariableInfo 2
#define IUEFISecApp_OP_getNextVariableName 3

/* Init instance of 'struct si_buffer'. */
#define SET_SI_BUFFER(buf, bv, t) do { \
		(buf).type = (t); \
		(buf).b = (bv);  \
	} while (0)

#define SI_BUFFER(x) ((struct si_buffer) { &(x), sizeof(x) })

/**
 * qcuefi_get_variable - Finds variable in variable store (Volatile or Non-Volatile).
 * @in_variable: name of variable to be found.
 * @guid: variable vendor GUID.
 * @in_attributes: attribute value of input.
 * @data: buffer to receive data.
 * @out_attributes: attribute value of the variable found.
 * @out_errno: return status as returned by uefisecapp.
 *
 * @data is both input and output. @data.size is buffer size on input but will be
 * updated to variable size on return.
 * @out_errno is 0 on success, otherwise an IUEFISecApp_ERROR_*.
 *
 * Returns 0 on success or a negative number. Check the comments for 'si_object_do_invoke'
 * for details of the return values.
 */
static int qcuefi_get_variable(struct si_buffer in_variable,
			       efi_guid_t *guid,
			       struct si_buffer in_attributes,
			       struct si_buffer *data,
			       u32 *out_attributes,
			       u32 *out_errno)
{
	int ret, result = 0;
	struct si_arg args[6] = { 0 };

	/* IDL uses separate variables 'in_data_size' and 'out_data_size' for input
	 * and output buffer size. It is necessary as TA always returns SUCCESS so
	 * the size in 'in_variable.size' and 'out_attributes.size' should remain
	 * sensible, i.e. either valid data in buffer or maximum size of buffer.
	 */
	struct {
		efi_guid_t guid;
		u32 in_data_size;
	} in_cong = { 0 };

	struct {
		u32 out_data_size;
		u32 attributes;
		u32 errno;
	} out_cong = { 0 };

	in_cong.guid = *guid;
	in_cong.in_data_size = data->size;

	/* 'in_attribute' is useless. If '!in_attribute.addr' then 'out_attributes' is updated. */
	/* On SUCCESS (!out_errno || out_errno == IUEFISecApp_ERROR_SIZE_OUT),
	 * 'out_attributes' is output attribute.
	 * On failure, 'in_attribute' is copied to 'out_attributes'.
	 */

	SET_SI_BUFFER(args[0], SI_BUFFER(in_cong), SI_AT_IB);
	SET_SI_BUFFER(args[1], in_variable, SI_AT_IB);
	SET_SI_BUFFER(args[2], in_attributes, SI_AT_IB);
	SET_SI_BUFFER(args[3], SI_BUFFER(out_cong), SI_AT_OB);
	SET_SI_BUFFER(args[4], *data, SI_AT_OB);
	args[5].type = SI_AT_END;

	struct si_object_invoke_ctx *oic __free(kfree) =
		kzalloc(sizeof(struct si_object_invoke_ctx), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	ret = si_object_do_invoke(oic, uefisecapp,
				  IUEFISecApp_OP_getVariable, args, &result);
	if (ret || result) {
		pr_err("IUEFISecApp_OP_getVariable invoke ret: %d, result: 0x%x\n",
		       ret, result);
		return ret ?: result;
	}

	/* TA does not touch 'in_data.size'. Update it here. */
	/* On SUCCESS (!out_errno), 'out_data_size' is size of data in 'in_data.addr'. */
	/* On failure (out_errno == IUEFISecApp_ERROR_SIZE_OUT), 'out_data_size' is
	 * actual variable size.
	 * Otherwise, is undefined.
	 */
	data->size = out_cong.out_data_size;
	*out_attributes = out_cong.attributes;
	*out_errno = out_cong.errno;

	return ret;
}

/**
 * qcuefi_set_variable - Sets variable in storage blocks (Volatile or Non-Volatile).
 * @in_variable: name of variable to be found.
 * @guid: variable vendor GUID.
 * @attributes: attribute value of the variable found.
 * @data: buffer to send data.
 * @out_errno: return status as returned by uefisecapp.
 *
 * @out_errno is 0 on success, otherwise an IUEFISecApp_ERROR_*.
 *
 * Returns 0 on success or a negative number. Check the comments for 'si_object_do_invoke'
 * for details of the return values.
 */
static int qcuefi_set_variable(struct si_buffer in_variable,
			       efi_guid_t *guid,
			       u32 attributes,
			       struct si_buffer data,
			       u32 *out_errno)
{
	int ret, result = 0;
	struct si_arg args[5] = { 0 };

	struct {
		efi_guid_t guid;
		u32 attributes;
		u32 in_data_size;
	} in_cong = { 0 };

	in_cong.guid = *guid;
	in_cong.attributes = attributes;
	in_cong.in_data_size = data.size;

	SET_SI_BUFFER(args[0], SI_BUFFER(in_cong), SI_AT_IB);
	SET_SI_BUFFER(args[1], in_variable, SI_AT_IB);
	SET_SI_BUFFER(args[2], data, SI_AT_IB);
	SET_SI_BUFFER(args[3], SI_BUFFER(*out_errno), SI_AT_OB);
	args[4].type = SI_AT_END;

	struct si_object_invoke_ctx *oic __free(kfree) =
		kzalloc(sizeof(struct si_object_invoke_ctx), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	ret = si_object_do_invoke(oic, uefisecapp,
				  IUEFISecApp_OP_setVariable, args, &result);
	if (ret || result) {
		pr_err("IUEFISecApp_OP_setVariable invoke ret: %d, result: 0x%x\n",
		       ret, result);
		return ret ?: result;
	}

	return ret;
}

/**
 * qcuefi_query_variable_info - Returns information about the EFI variables.
 * @attributes: Attributes bitmask to specify type of variables.
 * @maximum_variable_storage_size: Maximum size of storage space available for the variables.
 * @remaining_variable_storage_size: Remaining size of storage space available for variables.
 * @maximum_variable_size: Maximum size of an individual variables.
 * @out_errno: return status as returned by uefisecapp.
 *
 * @out_errno is 0 on success, otherwise an IUEFISecApp_ERROR_*.
 *
 * Returns 0 on success or a negative number. Check the comments for 'si_object_do_invoke'
 * for details of the return values.
 */
static int qcuefi_query_variable_info(u32 attributes,
				      u64 *maximum_variable_storage_size,
				      u64 *remaining_variable_storage_size,
				      u64 *maximum_variable_size,
				      u32 *out_errno)
{
	int ret, result = 0;
	struct si_arg args[3] = { 0 };

	struct {
		u64 a, b, c;
		u32 errno;
	} out_cong = { 0 };

	SET_SI_BUFFER(args[0], SI_BUFFER(attributes), SI_AT_IB);
	SET_SI_BUFFER(args[1], SI_BUFFER(out_cong), SI_AT_OB);
	args[2].type = SI_AT_END;

	struct si_object_invoke_ctx *oic __free(kfree) =
		kzalloc(sizeof(struct si_object_invoke_ctx), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	ret = si_object_do_invoke(oic, uefisecapp,
				  IUEFISecApp_OP_queryVariableInfo, args, &result);
	if (ret || result) {
		pr_err("IUEFISecApp_OP_queryVariableInfo invoke ret: %d, result: 0x%x\n",
		       ret, result);
		return ret ?: result;
	}

	*maximum_variable_storage_size = out_cong.a;
	*remaining_variable_storage_size = out_cong.b;
	*maximum_variable_size = out_cong.c;
	*out_errno = out_cong.errno;

	return ret;
}

/**
 * qcuefi_get_next_variable - Finds the next available variable.
 * @in_variable: current variable.
 * @guid: variable vendor GUID.
 * @out_variable: next variable.
 * @out_vendor_guid: variable vendor GUID.
 * @out_errno: return status as returned by uefisecapp.
 *
 * @out_errno is 0 on success, otherwise an IUEFISecApp_ERROR_*.
 *
 * Returns 0 on success or a negative number. Check the comments for 'si_object_do_invoke'
 * for details of the return values.
 */
static int qcuefi_get_next_variable(struct si_buffer in_variable,
				    efi_guid_t *guid,
				    struct si_buffer *out_variable,
				    efi_guid_t *out_vendor_guid,
				    u32 *out_errno)
{
	int ret, result = 0;
	struct si_arg args[5] = { 0 };

	struct {
		efi_guid_t guid;
		u32 in_data_size;
	} in_cong = { 0 };

	struct {
		efi_guid_t guid;
		u32 out_data_size;
		u32 errno;
	} out_cong = { 0 };

	/* Pass size of available buffer; see 'qcuefi_get_next_variable'. */
	in_cong.in_data_size = out_variable->size;
	in_cong.guid = *guid;

	SET_SI_BUFFER(args[0], SI_BUFFER(in_cong), SI_AT_IB);
	SET_SI_BUFFER(args[1], in_variable, SI_AT_IB);
	SET_SI_BUFFER(args[2], SI_BUFFER(out_cong), SI_AT_OB);
	SET_SI_BUFFER(args[3], *out_variable, SI_AT_OB);
	args[4].type = SI_AT_END;

	struct si_object_invoke_ctx *oic __free(kfree) =
		kzalloc(sizeof(struct si_object_invoke_ctx), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	ret = si_object_do_invoke(oic, uefisecapp,
				  IUEFISecApp_OP_getNextVariableName, args, &result);
	if (ret || result) {
		pr_err("IUEFISecApp_OP_getNextVariableName invoke ret: %d, result: 0x%x\n",
		       ret, result);
		return ret ?: result;
	}

	/* TA does not touch 'out_variable.size'. Update it here.
	 * On SUCCESS (!out_errno), 'out_data_size' is length of name in
	 * 'out_variable.addr'.
	 * On failure (out_errno == IUEFISecApp_ERROR_SIZE_OUT), 'out_data_size'
	 * is actual name length.
	 * Otherwise, is undefined.
	 */
	out_variable->size = out_cong.out_data_size;
	*out_vendor_guid = out_cong.guid;
	*out_errno = out_cong.errno;

	return ret;
}

/* -- Global efivar interface. ---------------------------------------------- */

static efi_status_t uefisecapp_get_variable(efi_char16_t *name,
					    efi_guid_t *guid, u32 *attr,
					    unsigned long *data_size, void *data)
{
	int ret;
	u32 in_attr, out_attributes, out_errno;
	struct si_buffer in_data = (struct si_buffer){ data, *data_size };

	if (!name || !guid)
		return EFI_INVALID_PARAMETER;

	/* 'attr' can be NULL, however an input attribute is always expected
	 * by uefisecapp TA.
	 */
	in_attr = 0;
	if (attr)
		in_attr = *attr;

	ret = qcuefi_get_variable(
		(struct si_buffer){ name, (ucs2_strlen(name) + 1) * sizeof(*name) },
		guid,
		/* 'attr' can be NULL; if NULL 'out_attributes' is not updated. */
		(struct si_buffer){ &in_attr, sizeof(u32) },
		/* On SUCCESS, 'data' has already been updated. */
		&in_data,
		&out_attributes,
		&out_errno
	);

	if (ret)
		return EFI_DEVICE_ERROR;

	if (!out_errno || out_errno == IUEFISecApp_ERROR_SIZE_OUT) {
		if (attr)
			*attr = out_attributes;

		*data_size = in_data.size;
	}

	return uefisecapp_err_to_efi_status(out_errno);
}

static efi_status_t uefisecapp_set_variable(efi_char16_t *name,
					    efi_guid_t *guid, u32 attr,
					    unsigned long data_size, void *data)
{
	int ret;
	u32 out_errno;
	struct si_buffer in_data = (struct si_buffer){ data, data_size };

	if (!name || !guid)
		return EFI_INVALID_PARAMETER;

	ret = qcuefi_set_variable(
		(struct si_buffer){ name, (ucs2_strlen(name) + 1) * sizeof(*name) },
		guid, attr, in_data, &out_errno);

	if (ret)
		return EFI_DEVICE_ERROR;

	return uefisecapp_err_to_efi_status(out_errno);
}

static efi_status_t uefisecapp_get_next_variable(unsigned long *name_size,
						 efi_char16_t *name,
						 efi_guid_t *guid)
{
	int ret;
	u32 out_errno;
	efi_guid_t out_guid;
	struct si_buffer in_variable, out_variable;

	if (!name_size || !name || !guid)
		return EFI_INVALID_PARAMETER;

	if (*name_size == 0)
		return EFI_INVALID_PARAMETER;

	/* For 'in_variable', 'name_size' is not necessarily size of 'name';
	 * could be size of buffer where 'name' has been stored. TA expects a
	 * NULL-terminated string in 'name' and ignores the size.
	 */
	in_variable = (struct si_buffer){ name, *name_size };
	/* For 'out_variable', 'name_size' is size of buffer pointed by 'name'. */
	out_variable = (struct si_buffer){ name, *name_size };

	ret = qcuefi_get_next_variable(in_variable, guid, &out_variable,
				       &out_guid, &out_errno);
	if (ret)
		return EFI_DEVICE_ERROR;

	if (!out_errno)
		*guid = out_guid;

	if (!out_errno || out_errno == IUEFISecApp_ERROR_SIZE_OUT)
		*name_size = out_variable.size;

	/* On SUCCESS, 'name' stores the next variable name. */
	return uefisecapp_err_to_efi_status(out_errno);
}

static efi_status_t uefisecapp_query_variable_info(u32 attr, u64 *storage_space,
						   u64 *remaining_space,
						   u64 *max_variable_size)
{
	int ret;
	u32 out_errno;
	u64 maximum_variable_storage_size;
	u64 remaining_variable_storage_size;
	u64 maximum_variable_size;

	if (!storage_space || !remaining_space || !max_variable_size)
		return EFI_INVALID_PARAMETER;

	ret = qcuefi_query_variable_info(attr,
					 &maximum_variable_storage_size,
					 &remaining_variable_storage_size,
					 &maximum_variable_size,
					 &out_errno);

	if (ret)
		return EFI_DEVICE_ERROR;

	if (!out_errno) {
		*storage_space = maximum_variable_storage_size;
		*remaining_space = remaining_variable_storage_size;
		*max_variable_size = maximum_variable_size;
	}

	return uefisecapp_err_to_efi_status(out_errno);
}

static const struct efivar_operations qcom_efivar_ops = {
	.get_variable = uefisecapp_get_variable,
	.set_variable = uefisecapp_set_variable,
	.query_variable_info = uefisecapp_query_variable_info,
	.get_next_variable = uefisecapp_get_next_variable,
};

/* -- Driver setup. --------------------------------------------------------- */

static int get_client_env(struct si_object **client_env)
{
	int ret, result;
	struct si_arg args[3] = { 0 };

	args[0].o = NULL_SI_OBJECT;
	args[0].type = SI_AT_IO;
	args[1].type = SI_AT_OO;
	args[2].type = SI_AT_END;

	struct si_object_invoke_ctx *oic __free(kfree) =
		kzalloc(sizeof(struct si_object_invoke_ctx), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	/* IClientEnv_OP_registerWithCredentials is 5. */
	ret = si_object_do_invoke(oic, ROOT_SI_OBJECT, 5, args, &result);
	if (ret)
		return ret;

	if (result) {
		pr_err("returned with %d\n", result);
		return -EINVAL;
	}

	*client_env = args[1].o;
	return 0;
}

static int client_env_open(struct si_object *client_env)
{
	int ret, result;
	struct si_arg args[3] = { 0 };
	/* UID of uefisec TA. */
	u32 uid = 413;

	args[0].b = SI_BUFFER(uid);
	args[0].type = SI_AT_IB;
	args[1].type = SI_AT_OO;
	args[2].type = SI_AT_END;

	struct si_object_invoke_ctx *oic __free(kfree) =
		kzalloc(sizeof(struct si_object_invoke_ctx), GFP_KERNEL);
	if (!oic)
		return -ENOMEM;

	/* IClientEnv_OP_open is 0. */
	ret = si_object_do_invoke(oic, client_env, 0, args, &result);
	if (ret)
		return ret;

	if (result) {
		pr_err("returned with %d\n", result);
		return -EINVAL;
	}

	uefisecapp = args[1].o;
	return 0;
}

static int uefisecapp_probe(struct platform_device *pdev)
{
	int ret;
	struct si_object *client_env;

	ret = get_client_env(&client_env);
	if (ret) {
		pr_err("get_client_env failed %d.\n", ret);
		return ret;
	}

	ret = client_env_open(client_env);
	if (ret) {
		pr_err("client_env_open failed %d.\n", ret);
		goto err_env_open;
	}

	ret = efivars_register(&qcom_efivars, &qcom_efivar_ops);
	if (ret) {
		pr_err("efivars_register failed %d.\n", ret);
		goto err_efivars_register;
	}

	/* RELEASE 'client_env'. */
	put_si_object(client_env);
	return 0;

err_efivars_register:
	put_si_object(uefisecapp);
err_env_open:
	put_si_object(client_env);
	return ret;
}

static int uefisecapp_remove(struct platform_device *pdev)
{
	efivars_unregister(&qcom_efivars);
	put_si_object(uefisecapp);
	return 0;
}

static const struct of_device_id app_match[] = {
	{ .compatible = "qcom,app-uefisecapp", },
	{}
};
MODULE_DEVICE_TABLE(of, app_match);

static struct platform_driver uefisecapp_plat_driver = {
	.probe = uefisecapp_probe,
	.remove = uefisecapp_remove,
	.driver = {
		.name = "uefisecapp",
		.of_match_table = app_match,
	},
};

module_platform_driver(uefisecapp_plat_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Client driver UEFI Secure Application");
