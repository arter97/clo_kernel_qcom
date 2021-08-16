#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/mhi_ep.h>
#include <linux/errno.h>
#include <linux/interrupt.h>

#include "internal.h"

static const char *mhi_sm_dev_event_str(enum mhi_ep_event_type state)
{
	const char *str;

	switch (state) {
	case MHI_EP_EVENT_CTRL_TRIG:
		str = "MHI_EP_EVENT_CTRL_TRIG";
		break;
	case MHI_EP_EVENT_M0_STATE:
		str = "MHI_EP_EVENT_M0_STATE";
		break;
	case MHI_EP_EVENT_M1_STATE:
		str = "MHI_EP_EVENT_M1_STATE";
		break;
	case MHI_EP_EVENT_M2_STATE:
		str = "MHI_EP_EVENT_M2_STATE";
		break;
	case MHI_EP_EVENT_M3_STATE:
		str = "MHI_EP_EVENT_M3_STATE";
		break;
	case MHI_EP_EVENT_HW_ACC_WAKEUP:
		str = "MHI_EP_EVENT_HW_ACC_WAKEUP";
		break;
	case MHI_EP_EVENT_CORE_WAKEUP:
		str = "MHI_EP_EVENT_CORE_WAKEUP";
		break;
	default:
		str = "INVALID MHI_EP_EVENT";
	}

	return str;
}

static const char *mhi_sm_mstate_str(enum mhi_ep_state state)
{
	const char *str;

	switch (state) {
	case MHI_EP_RESET_STATE:
		str = "RESET";
		break;
	case MHI_EP_READY_STATE:
		str = "READY";
		break;
	case MHI_EP_M0_STATE:
		str = "M0";
		break;
	case MHI_EP_M1_STATE:
		str = "M1";
		break;
	case MHI_EP_M2_STATE:
		str = "M2";
		break;
	case MHI_EP_M3_STATE:
		str = "M3";
		break;
	case MHI_EP_SYSERR_STATE:
		str = "SYSTEM ERROR";
		break;
	default:
		str = "INVALID";
		break;
	}

	return str;
}

static const char *mhi_sm_dstate_str(enum mhi_ep_pcie_state state)
{
	const char *str;

	switch (state) {
	case MHI_EP_PCIE_LINK_DISABLE:
		str = "EP_PCIE_LINK_DISABLE";
		break;
	case MHI_EP_PCIE_D0_STATE:
		str = "D0_STATE";
		break;
	case MHI_EP_PCIE_D3_HOT_STATE:
		str = "D3_HOT_STATE";
		break;
	case MHI_EP_PCIE_D3_COLD_STATE:
		str = "D3_COLD_STATE";
		break;
	default:
		str = "INVALID D-STATE";
		break;
	}

	return str;
}

static inline const char *mhi_sm_pcie_event_str(enum mhi_ep_pcie_event event)
{
	const char *str;

	switch (event) {
	case MHI_EP_PCIE_EVENT_LINKDOWN:
		str = "EP_PCIE_LINKDOWN_EVENT";
		break;
	case MHI_EP_PCIE_EVENT_LINKUP:
		str = "EP_PCIE_LINKUP_EVENT";
		break;
	case MHI_EP_PCIE_EVENT_PM_D3_HOT:
		str = "EP_PCIE_PM_D3_HOT_EVENT";
		break;
	case MHI_EP_PCIE_EVENT_PM_D3_COLD:
		str = "EP_PCIE_PM_D3_COLD_EVENT";
		break;
	case MHI_EP_PCIE_EVENT_PM_RST_DEAST:
		str = "EP_PCIE_PM_RST_DEAST_EVENT";
		break;
	case MHI_EP_PCIE_EVENT_PM_D0:
		str = "EP_PCIE_PM_D0_EVENT";
		break;
	case MHI_EP_PCIE_EVENT_MHI_A7:
		str = "EP_PCIE_MHI_A7";
		break;
	default:
		str = "INVALID_PCIE_EVENT";
		break;
	}

	return str;
}

static void mhi_ep_sm_mmio_set_status(struct mhi_ep_cntrl *mhi_cntrl,
				       enum mhi_ep_state state)
{
	struct mhi_ep_sm *sm = mhi_cntrl->sm;

	switch (state) {
	case MHI_EP_READY_STATE:
		dev_dbg(&mhi_cntrl->mhi_dev->dev, "set MHISTATUS to READY mode\n");
		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_READY_MASK,
				MHISTATUS_READY_SHIFT, 1);

		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_MHISTATE_MASK,
				MHISTATUS_MHISTATE_SHIFT, state);
		break;
	case MHI_EP_SYSERR_STATE:
		dev_dbg(&mhi_cntrl->mhi_dev->dev, "set MHISTATUS to SYSTEM ERROR mode\n");
		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_SYSERR_MASK,
				MHISTATUS_SYSERR_SHIFT, 1);

		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
				MHISTATUS_MHISTATE_MASK,
				MHISTATUS_MHISTATE_SHIFT, state);
		break;
	case MHI_EP_M1_STATE:
	case MHI_EP_M2_STATE:
		dev_err(&mhi_cntrl->mhi_dev->dev, "Not supported state, can't set MHISTATUS to %s\n",
			mhi_sm_mstate_str(state));
		return;
	case MHI_EP_M0_STATE:
	case MHI_EP_M3_STATE:
		dev_dbg(&mhi_cntrl->mhi_dev->dev, "set MHISTATUS.MHISTATE to %s state\n",
			mhi_sm_mstate_str(state));
		mhi_ep_mmio_masked_write(mhi_cntrl, MHISTATUS,
					  MHISTATUS_MHISTATE_MASK,
					  MHISTATUS_MHISTATE_SHIFT, state);
		break;
	default:
		dev_err(&mhi_cntrl->mhi_dev->dev, "Invalid mhi state: 0x%x state", state);
		return;
	}

	sm->state = state;
}

/**
 * mhi_sm_is_legal_event_on_state() - Determine if MHI state transition is valid
 * @curr_state: current MHI state
 * @event: MHI state change event
 *
 * Determine according to MHI state management if the state change event
 * is valid on the current mhi state.
 * Note: The decision doesn't take into account M1 and M2 states.
 *
 * Return:	true: transition is valid
 *		false: transition is not valid
 */
static bool mhi_sm_is_legal_event_on_state(struct mhi_ep_sm *sm,
					   enum mhi_ep_state curr_state,
					   enum mhi_ep_event_type event)
{
	struct mhi_ep_cntrl *mhi_cntrl = sm->mhi_cntrl;
	bool res;

	switch (event) {
	case MHI_EP_EVENT_M0_STATE:
		res = (sm->d_state == MHI_EP_PCIE_D0_STATE &&
			curr_state != MHI_EP_RESET_STATE);
		break;
	case MHI_EP_EVENT_M3_STATE:
	case MHI_EP_EVENT_HW_ACC_WAKEUP:
	case MHI_EP_EVENT_CORE_WAKEUP:
		res = (curr_state == MHI_EP_M3_STATE ||
			curr_state == MHI_EP_M0_STATE);
		break;
	default:
		dev_err(&mhi_cntrl->mhi_dev->dev, "Received invalid event: %s\n",
			mhi_sm_dev_event_str(event));
		res = false;
		break;
	}

	return res;
}

/**
 * mhi_sm_change_to_M0() - switch to M0 state.
 *
 * Switch MHI-device state to M0, if possible according to MHI state machine.
 * Notify the MHI-host on the transition. If MHI is suspended, resume MHI.
 *
 * Return:	0: success
 *		negative: failure
 */
static int mhi_sm_change_to_M0(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_sm *sm = mhi_cntrl->sm;
	enum mhi_ep_state old_state;
	int ret;

	old_state = sm->state;

	switch (old_state) {
	case MHI_EP_M0_STATE:
		dev_dbg(&mhi_cntrl->mhi_dev->dev, "Nothing to do, already in M0 state\n");
		return 0;
	case MHI_EP_M3_STATE:
	case MHI_EP_READY_STATE:
		break;
	default:
		dev_err(&mhi_cntrl->mhi_dev->dev, "unexpected old_state: %s\n",
			mhi_sm_mstate_str(old_state));
		return -EINVAL;
	}

	mhi_ep_sm_mmio_set_status(mhi_cntrl, MHI_EP_M0_STATE);

	/* Tell the host, device move to M0 */
	if (old_state == MHI_EP_M3_STATE) {
		/* TODO: Resume MHI */
#if 0
		res = mhi_ep_resume(sm);
		if (res) {
			dev_err(&mhi_cntrl->mhi_dev->dev, "Failed resuming mhi core, returned %d",
				res);
			goto exit;
		}
#endif
	}

	ret = mhi_ep_send_state_change_event(mhi_cntrl, MHI_EP_M0_STATE);
	if (ret) {
		dev_err(&mhi_cntrl->mhi_dev->dev, "Failed sending M0 state change event to host: %d\n", ret);
		return ret;
	}

	if (old_state == MHI_EP_READY_STATE) {
		/* Allow the host to process state change event */
		mdelay(1);

		/* Tell the host the EE */
		ret = mhi_ep_send_ee_event(mhi_cntrl, 2);
		if (ret) {
			dev_err(&mhi_cntrl->mhi_dev->dev, "Failed sending EE event to host: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static void mhi_ep_sm_handle_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ep_event_type event)
{
	struct mhi_ep_sm *sm = mhi_cntrl->sm;
	int ret;

	mutex_lock(&sm->lock);
	dev_dbg(&mhi_cntrl->mhi_dev->dev, "Start handling %s event, current states: %s & %s\n",
		mhi_sm_dev_event_str(event),
		mhi_sm_mstate_str(sm->state),
		mhi_sm_dstate_str(sm->d_state));

	/* TODO: Check for syserr before handling the event */

	if (!mhi_sm_is_legal_event_on_state(sm, sm->state, event)) {
		dev_err(&mhi_cntrl->mhi_dev->dev, "Event %s illegal in current MHI states: %s and %s\n",
			mhi_sm_dev_event_str(event),
			mhi_sm_mstate_str(sm->state),
			mhi_sm_dstate_str(sm->d_state));
		/* TODO: Transition to syserr */
		goto unlock_and_exit;
	}

	switch (event) {
	case MHI_EP_EVENT_M0_STATE:
		ret = mhi_sm_change_to_M0(mhi_cntrl);
		if (ret)
			dev_err(&mhi_cntrl->mhi_dev->dev, "Failed switching to M0 state\n");
		break;
	case MHI_EP_EVENT_M3_STATE:
//		ret = mhi_sm_change_to_M3();
		if (ret)
			dev_err(&mhi_cntrl->mhi_dev->dev, "Failed switching to M3 state\n");
//		mhi_ep_pm_relax();
		break;
	case MHI_EP_EVENT_HW_ACC_WAKEUP:
	case MHI_EP_EVENT_CORE_WAKEUP:
//		ret = mhi_sm_wakeup_host(event);
		if (ret)
			dev_err(&mhi_cntrl->mhi_dev->dev, "Failed to wakeup MHI host\n");
		break;
	case MHI_EP_EVENT_CTRL_TRIG:
	case MHI_EP_EVENT_M1_STATE:
	case MHI_EP_EVENT_M2_STATE:
		dev_err(&mhi_cntrl->mhi_dev->dev, "Error: %s event is not supported\n",
			mhi_sm_dev_event_str(event));
		break;
	default:
		dev_err(&mhi_cntrl->mhi_dev->dev, "Error: Invalid event, 0x%x", event);
		break;
	}

unlock_and_exit:
	mutex_unlock(&sm->lock);
}

int mhi_ep_notify_sm_event(struct mhi_ep_cntrl *mhi_cntrl, enum mhi_ep_event_type event)
{
	int ret;

	switch (event) {
	case MHI_EP_EVENT_M0_STATE:
//		sm->stats.m0_event_cnt++;
		break;
	case MHI_EP_EVENT_M3_STATE:
//		sm->stats.m3_event_cnt++;
		break;
	case MHI_EP_EVENT_HW_ACC_WAKEUP:
//		sm->stats.hw_acc_wakeup_event_cnt++;
		break;
	case MHI_EP_EVENT_CORE_WAKEUP:
//		sm->stats.mhi_core_wakeup_event_cnt++;
		break;
	case MHI_EP_EVENT_CTRL_TRIG:
	case MHI_EP_EVENT_M1_STATE:
	case MHI_EP_EVENT_M2_STATE:
		dev_err(&mhi_cntrl->mhi_dev->dev, "Received unsupported event: %s\n",
			mhi_sm_dev_event_str(event));
		return -ENOTSUPP;
		goto exit;
	default:
		dev_err(&mhi_cntrl->mhi_dev->dev, "Received invalid event: %d\n", event);
		ret =  -EINVAL;
		goto exit;
	}

	/* Change to wq is possible */
	mhi_ep_sm_handle_event(mhi_cntrl, event);

	return 0;

exit:
	return ret;
}
EXPORT_SYMBOL(mhi_ep_notify_sm_event);

int mhi_ep_sm_set_ready(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct mhi_ep_sm *sm = mhi_cntrl->sm;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_ep_state state;
	int is_ready;

	mutex_lock(&sm->lock);

	/* Ensure that the MHISTATUS is set to RESET by host */
	mhi_ep_mmio_masked_read(mhi_cntrl, MHISTATUS, MHISTATUS_MHISTATE_MASK,
				 MHISTATUS_MHISTATE_SHIFT, &state);
	mhi_ep_mmio_masked_read(mhi_cntrl, MHISTATUS, MHISTATUS_READY_MASK,
				 MHISTATUS_READY_SHIFT, &is_ready);

	if (state != MHI_EP_RESET_STATE || is_ready) {
		dev_err(dev, "READY transition failed. MHI host not in RESET state\n");
		mutex_unlock(&sm->lock);
		return -EFAULT;
	}

	mhi_ep_sm_mmio_set_status(mhi_cntrl, MHI_EP_READY_STATE);

	mutex_unlock(&sm->lock);
	return 0;
}
EXPORT_SYMBOL(mhi_ep_sm_set_ready);

int mhi_ep_sm_init(struct mhi_ep_cntrl *mhi_cntrl)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ep_sm *sm;

	sm = devm_kzalloc(dev, sizeof(*mhi_cntrl->sm), GFP_KERNEL);
	if (!sm)
		return -ENOMEM;

	sm->sm_wq = alloc_workqueue("mhi_ep_sm_wq", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!sm->sm_wq) {
		dev_err(dev, "Failed to create SM workqueue\n");
		return -ENOMEM;
	}

	sm->mhi_cntrl = mhi_cntrl;
	sm->state = MHI_EP_RESET_STATE;
	sm->d_state = MHI_EP_PCIE_D0_STATE;
	mutex_init(&sm->lock);
	mhi_cntrl->sm = sm;

	return 0;
}
EXPORT_SYMBOL(mhi_ep_sm_init);
