// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/mhi_ep.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/sock.h>

#include "qrtr.h"

struct qrtr_mhi_dev {
	struct qrtr_endpoint ep;
	struct mhi_ep_device *mhi_dev;
	struct device *dev;
	struct completion out_tre;
	struct mutex out_lock;
};

/* Callback from host to notify available buffers to queue */
static void qcom_mhi_qrtr_dl_callback(struct mhi_ep_device *mhi_dev,
				      struct mhi_result *mhi_res)
{
	struct qrtr_mhi_dev *qdev = dev_get_drvdata(&mhi_dev->dev);
	struct sk_buff *skb = mhi_res->buf_addr;

	mutex_lock(&qdev->out_lock);
	complete_all(&qdev->out_tre);
	mutex_unlock(&qdev->out_lock);
}

static void qcom_mhi_qrtr_ul_callback(struct mhi_ep_device *mhi_dev,
				      struct mhi_result *mhi_res)
{
	struct qrtr_mhi_dev *qdev = dev_get_drvdata(&mhi_dev->dev);
	int rc;

	/* TODO */
	if (!qdev)
		return;

	rc = qrtr_endpoint_post(&qdev->ep, mhi_res->buf_addr,
				mhi_res->bytes_xferd);
	if (rc == -EINVAL)
		dev_err(qdev->dev, "invalid ipcrouter packet\n");
}

/* Send data over MHI */
static int qcom_mhi_qrtr_send(struct qrtr_endpoint *ep, struct sk_buff *skb)
{
	struct qrtr_mhi_dev *qdev = container_of(ep, struct qrtr_mhi_dev, ep);
	int rc;

	if (skb->sk)
		sock_hold(skb->sk);

	rc = skb_linearize(skb);
	if (rc)
		goto free_skb;

	wait_for_completion(&qdev->out_tre);

	rc = mhi_ep_queue_skb(qdev->mhi_dev, DMA_FROM_DEVICE, skb, skb->len,
			   MHI_EOT);
	if (rc)
		goto free_skb;

	reinit_completion(&qdev->out_tre);
	consume_skb(skb);

	return rc;

free_skb:
	if (skb->sk)
		sock_put(skb->sk);
	kfree_skb(skb);

	return rc;
}

static int qcom_mhi_qrtr_probe(struct mhi_ep_device *mhi_dev,
			       const struct mhi_device_id *id)
{
	struct qrtr_mhi_dev *qdev;
	int rc;

	/* start channels */
//	rc = mhi_ep_prepare_for_transfer(mhi_dev);
//	if (rc)
//		return rc;

	qdev = devm_kzalloc(&mhi_dev->dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return -ENOMEM;

	qdev->mhi_dev = mhi_dev;
	qdev->dev = &mhi_dev->dev;
	init_completion(&qdev->out_tre);
	mutex_init(&qdev->out_lock);
	qdev->ep.xmit = qcom_mhi_qrtr_send;

	dev_set_drvdata(&mhi_dev->dev, qdev);
	rc = qrtr_endpoint_register(&qdev->ep, QRTR_EP_NID_AUTO);
	if (rc)
		return rc;

	dev_dbg(qdev->dev, "Qualcomm MHI QRTR driver probed\n");

	return 0;
}

static void qcom_mhi_qrtr_remove(struct mhi_ep_device *mhi_dev)
{
	struct qrtr_mhi_dev *qdev = dev_get_drvdata(&mhi_dev->dev);

	qrtr_endpoint_unregister(&qdev->ep);
//	mhi_unprepare_from_transfer(mhi_dev);
	dev_set_drvdata(&mhi_dev->dev, NULL);
}

static const struct mhi_device_id qcom_mhi_qrtr_id_table[] = {
	{ .chan = "IPCR" },
	{}
};
MODULE_DEVICE_TABLE(mhi, qcom_mhi_qrtr_id_table);

static struct mhi_ep_driver qcom_mhi_qrtr_driver = {
	.probe = qcom_mhi_qrtr_probe,
	.remove = qcom_mhi_qrtr_remove,
	.dl_xfer_cb = qcom_mhi_qrtr_dl_callback,
	.ul_xfer_cb = qcom_mhi_qrtr_ul_callback,
	.id_table = qcom_mhi_qrtr_id_table,
	.driver = {
		.name = "qcom_mhi_qrtr",
	},
};

module_mhi_ep_driver(qcom_mhi_qrtr_driver);

MODULE_AUTHOR("Chris Lew <clew@codeaurora.org>");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_DESCRIPTION("Qualcomm IPC-Router MHI interface driver");
MODULE_LICENSE("GPL v2");
