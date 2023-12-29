/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023, Linaro Limited
 */

#ifndef __QCOM_ICE_H__
#define __QCOM_ICE_H__

#include <linux/types.h>
#include <linux/blk-crypto.h>

struct qcom_ice;

enum qcom_ice_crypto_key_size {
	QCOM_ICE_CRYPTO_KEY_SIZE_INVALID	= 0x0,
	QCOM_ICE_CRYPTO_KEY_SIZE_128		= 0x1,
	QCOM_ICE_CRYPTO_KEY_SIZE_192		= 0x2,
	QCOM_ICE_CRYPTO_KEY_SIZE_256		= 0x3,
	QCOM_ICE_CRYPTO_KEY_SIZE_512		= 0x4,
	QCOM_ICE_CRYPTO_KEY_SIZE_WRAPPED	= 0x5,
};

enum qcom_ice_crypto_alg {
	QCOM_ICE_CRYPTO_ALG_AES_XTS		= 0x0,
	QCOM_ICE_CRYPTO_ALG_BITLOCKER_AES_CBC	= 0x1,
	QCOM_ICE_CRYPTO_ALG_AES_ECB		= 0x2,
	QCOM_ICE_CRYPTO_ALG_ESSIV_AES_CBC	= 0x3,
};

int qcom_ice_enable(struct qcom_ice *ice);
int qcom_ice_resume(struct qcom_ice *ice);
int qcom_ice_suspend(struct qcom_ice *ice);
int qcom_ice_program_key(struct qcom_ice *ice,
			 u8 algorithm_id, u8 key_size,
			 const struct blk_crypto_key *bkey,
			 u8 data_unit_size, int slot);
int qcom_ice_evict_key(struct qcom_ice *ice, int slot);
bool qcom_ice_hwkm_supported(struct qcom_ice *ice);
int qcom_ice_derive_sw_secret(struct qcom_ice *ice, const u8 wkey[],
			      unsigned int wkey_size,
			      u8 sw_secret[BLK_CRYPTO_SW_SECRET_SIZE]);
int qcom_ice_generate_key(struct qcom_ice *ice,
			  u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
int qcom_ice_prepare_key(struct qcom_ice *ice,
			 const u8 *lt_key, size_t lt_key_size,
			 u8 eph_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
int qcom_ice_import_key(struct qcom_ice *ice,
			const u8 *imp_key, size_t imp_key_size,
			u8 lt_key[BLK_CRYPTO_MAX_HW_WRAPPED_KEY_SIZE]);
struct qcom_ice *of_qcom_ice_get(struct device *dev);
#endif /* __QCOM_ICE_H__ */
