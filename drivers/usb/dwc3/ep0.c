/**
 * ep0.c - DesignWare USB3 DRD Controller Endpoint 0 Handling
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>

#include "core.h"
#include "debug.h"
#include "gadget.h"
#include "io.h"
#include "debug.h"


static bool enable_dwc3_u1u2;
module_param(enable_dwc3_u1u2, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(enable_dwc3_u1u2, "Enable support for U1U2 low power modes");

static void __dwc3_ep0_do_control_status(struct dwc3 *dwc, struct dwc3_ep *dep);
static void __dwc3_ep0_do_control_data(struct dwc3 *dwc,
		struct dwc3_ep *dep, struct dwc3_request *req);

static const char *dwc3_ep0_state_string(enum dwc3_ep0_state state)
{
	switch (state) {
	case EP0_UNCONNECTED:
		return "Unconnected";
	case EP0_SETUP_PHASE:
		return "Setup Phase";
	case EP0_DATA_PHASE:
		return "Data Phase";
	case EP0_STATUS_PHASE:
		return "Status Phase";
	default:
		return "UNKNOWN";
	}
}

static int dwc3_ep0_start_trans(struct dwc3 *dwc, u8 epnum, dma_addr_t buf_dma,
		u32 len, u32 type)
{
	struct dwc3_gadget_ep_cmd_params params;
	struct dwc3_trb			*trb;
	struct dwc3_ep			*dep;

	int				ret;

	dep = dwc->eps[epnum];
	if (dep->flags & DWC3_EP_BUSY) {
		dwc3_trace(trace_dwc3_ep0, "%s still busy", dep->name);
		return 0;
	}

	trb = dwc->ep0_trb;

	trb->bpl = lower_32_bits(buf_dma);
	trb->bph = upper_32_bits(buf_dma);
	trb->size = len;
	trb->ctrl = type;

	trb->ctrl |= (DWC3_TRB_CTRL_HWO
			| DWC3_TRB_CTRL_LST
			| DWC3_TRB_CTRL_IOC
			| DWC3_TRB_CTRL_ISP_IMI);

	memset(&params, 0, sizeof(params));
	params.param0 = upper_32_bits(dwc->ep0_trb_addr);
	params.param1 = lower_32_bits(dwc->ep0_trb_addr);

	ret = dwc3_send_gadget_ep_cmd(dwc, dep->number,
			DWC3_DEPCMD_STARTTRANSFER, &params);
	if (ret < 0) {
		dwc3_trace(trace_dwc3_ep0, "%s STARTTRANSFER failed",
				dep->name);
		return ret;
	}

	dep->flags |= DWC3_EP_BUSY;
	dep->resource_index = dwc3_gadget_ep_get_transfer_index(dwc,
			dep->number);

	dwc->ep0_next_event = DWC3_EP0_COMPLETE;

	return 0;
}

static int __dwc3_gadget_ep0_queue(struct dwc3_ep *dep,
		struct dwc3_request *req)
{
	struct dwc3		*dwc = dep->dwc;

	req->request.actual	= 0;
	req->request.status	= -EINPROGRESS;
	req->epnum		= dep->number;

	list_add_tail(&req->list, &dep->request_list);

	/*
	 * Gadget driver might not be quick enough to queue a request
	 * before we get a Transfer Not Ready event on this endpoint.
	 *
	 * In that case, we will set DWC3_EP_PENDING_REQUEST. When that
	 * flag is set, it's telling us that as soon as Gadget queues the
	 * required request, we should kick the transfer here because the
	 * IRQ we were waiting for is long gone.
	 */
	if (dep->flags & DWC3_EP_PENDING_REQUEST) {
		unsigned	direction;

		direction = !!(dep->flags & DWC3_EP0_DIR_IN);

		if (dwc->ep0state != EP0_DATA_PHASE) {
			dev_WARN(dwc->dev, "Unexpected pending request\n");
			return 0;
		}

		__dwc3_ep0_do_control_data(dwc, dwc->eps[direction], req);

		dep->flags &= ~(DWC3_EP_PENDING_REQUEST |
				DWC3_EP0_DIR_IN);

		return 0;
	}

	/*
	 * In case gadget driver asked us to delay the STATUS phase,
	 * handle it here.
	 */
	if (dwc->delayed_status) {
		unsigned	direction;

		direction = !dwc->ep0_expect_in;
		dwc->delayed_status = false;
		usb_gadget_set_state(&dwc->gadget, USB_STATE_CONFIGURED);

		if (dwc->ep0state == EP0_STATUS_PHASE)
			__dwc3_ep0_do_control_status(dwc, dwc->eps[direction]);
		else
			dwc3_trace(trace_dwc3_ep0,
					"too early for delayed status");

		return 0;
	}

	/*
	 * Unfortunately we have uncovered a limitation wrt the Data Phase.
	 *
	 * Section 9.4 says we can wait for the XferNotReady(DATA) event to
	 * come before issueing Start Transfer command, but if we do, we will
	 * miss situations where the host starts another SETUP phase instead of
	 * the DATA phase.  Such cases happen at least on TD.7.6 of the Link
	 * Layer Compliance Suite.
	 *
	 * The problem surfaces due to the fact that in case of back-to-back
	 * SETUP packets there will be no XferNotReady(DATA) generated and we
	 * will be stuck waiting for XferNotReady(DATA) forever.
	 *
	 * By looking at tables 9-13 and 9-14 of the Databook, we can see that
	 * it tells us to start Data Phase right away. It also mentions that if
	 * we receive a SETUP phase instead of the DATA phase, core will issue
	 * XferComplete for the DATA phase, before actually initiating it in
	 * the wire, with the TRB's status set to "SETUP_PENDING". Such status
	 * can only be used to print some debugging logs, as the core expects
	 * us to go through to the STATUS phase and start a CONTROL_STATUS TRB,
	 * just so it completes right away, without transferring anything and,
	 * only then, we can go back to the SETUP phase.
	 *
	 * Because of this scenario, SNPS decided to change the programming
	 * model of control transfers and support on-demand transfers only for
	 * the STATUS phase. To fix the issue we have now, we will always wait
	 * for gadget driver to queue the DATA phase's struct usb_request, then
	 * start it right away.
	 *
	 * If we're actually in a 2-stage transfer, we will wait for
	 * XferNotReady(STATUS).
	 */
	if (dwc->three_stage_setup) {
		unsigned        direction;

		direction = dwc->ep0_expect_in;
		dwc->ep0state = EP0_DATA_PHASE;

		__dwc3_ep0_do_control_data(dwc, dwc->eps[direction], req);

		dep->flags &= ~DWC3_EP0_DIR_IN;
	}

	return 0;
}

int dwc3_gadget_ep0_queue(struct usb_ep *ep, struct usb_request *request,
		gfp_t gfp_flags)
{
	struct dwc3_request		*req = to_dwc3_request(request);
	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3			*dwc = dep->dwc;

	unsigned long			flags;

	int				ret;
	enum dwc3_link_state		link_state;
	u32				reg;

	spin_lock_irqsave(&dwc->lock, flags);
	if (!dep->endpoint.desc) {
		dwc3_trace(trace_dwc3_ep0,
				"trying to queue request %pK to disabled %s",
				request, dep->name);
		ret = -ESHUTDOWN;
		goto out;
	}

	/* we share one TRB for ep0/1 */
	if (!list_empty(&dep->request_list)) {
		ret = -EBUSY;
		goto out;
	}

	/* if link stats is in L1 initiate  remote wakeup before queuing req */
	if (dwc->speed != DWC3_DSTS_SUPERSPEED) {
		link_state = dwc3_get_link_state(dwc);
		/* in HS this link state is same as L1 */
		if (link_state == DWC3_LINK_STATE_U2) {
			dwc->l1_remote_wakeup_cnt++;
			reg = dwc3_readl(dwc->regs, DWC3_DCTL);
			reg |= DWC3_DCTL_ULSTCHNG_RECOVERY;
			dwc3_writel(dwc->regs, DWC3_DCTL, reg);
		}
	}

	dwc3_trace(trace_dwc3_ep0,
			"queueing request %pK to %s length %d state '%s'",
			request, dep->name, request->length,
			dwc3_ep0_state_string(dwc->ep0state));

	ret = __dwc3_gadget_ep0_queue(dep, req);

out:
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

void dwc3_ep0_stall_and_restart(struct dwc3 *dwc)
{
	struct dwc3_ep		*dep;

	/* reinitialize physical ep1 */
	dep = dwc->eps[1];
	dep->flags = DWC3_EP_ENABLED;

	/* stall is always issued on EP0 */
	dep = dwc->eps[0];
	__dwc3_gadget_ep_set_halt(dep, 1, false);
	dep->flags = DWC3_EP_ENABLED;
	dwc->delayed_status = false;

	if (!list_empty(&dep->request_list)) {
		struct dwc3_request	*req;

		req = next_request(&dep->request_list);
		dwc3_gadget_giveback(dep, req, -ECONNRESET);
	}

	dwc->ep0state = EP0_SETUP_PHASE;
	dwc3_ep0_out_start(dwc);
}

int __dwc3_gadget_ep0_set_halt(struct usb_ep *ep, int value)
{
	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3			*dwc = dep->dwc;

	dbg_event(dep->number, "EP0STAL", value);
	dwc3_ep0_stall_and_restart(dwc);

	return 0;
}

int dwc3_gadget_ep0_set_halt(struct usb_ep *ep, int value)
{
	struct dwc3_ep			*dep = to_dwc3_ep(ep);
	struct dwc3			*dwc = dep->dwc;
	unsigned long			flags;
	int				ret;

	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_gadget_ep0_set_halt(ep, value);
	spin_unlock_irqrestore(&dwc->lock, flags);

	return ret;
}

void dwc3_ep0_out_start(struct dwc3 *dwc)
{
	int				ret;

	ret = dwc3_ep0_start_trans(dwc, 0, dwc->ctrl_req_addr, 8,
			DWC3_TRBCTL_CONTROL_SETUP);

	if (WARN_ON_ONCE(ret < 0))
		dbg_event(dwc->eps[0]->number, "EOUTSTART", ret);
}

static struct dwc3_ep *dwc3_wIndex_to_dep(struct dwc3 *dwc, __le16 wIndex_le)
{
	struct dwc3_ep		*dep;
	u32			windex = le16_to_cpu(wIndex_le);
	u32			epnum;

	epnum = (windex & USB_ENDPOINT_NUMBER_MASK) << 1;
	if ((windex & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN)
		epnum |= 1;

	dep = dwc->eps[epnum];
	if (dep == NULL)
		return NULL;

	if (dep->flags & DWC3_EP_ENABLED)
		return dep;

	return NULL;
}

static void dwc3_ep0_status_cmpl(struct usb_ep *ep, struct usb_request *req)
{
}

static int dwc3_ep0_delegate_req(struct dwc3 *dwc, struct usb_ctrlrequest *ctrl)
{
	int ret;

	spin_unlock(&dwc->lock);
	ret = dwc->gadget_driver->setup(&dwc->gadget, ctrl);
	spin_lock(&dwc->lock);
	return ret;
}

/*
 * ch 9.4.5
 */
static int dwc3_ep0_handle_status(struct dwc3 *dwc,
		struct usb_ctrlrequest *ctrl)
{
	int ret;
	struct dwc3_ep		*dep;
	u32			recip;
	u32			reg;
	u16			usb_status = 0;
	__le16			*response_pkt;

	recip = ctrl->bRequestType & USB_RECIP_MASK;
	switch (recip) {
	case USB_RECIP_DEVICE:
		/*
		 * LTM will be set once we know how to set this in HW.
		 */
		usb_status |= dwc->is_selfpowered << USB_DEVICE_SELF_POWERED;

		if (dwc->speed == DWC3_DSTS_SUPERSPEED) {
			reg = dwc3_readl(dwc->regs, DWC3_DCTL);
			if (reg & DWC3_DCTL_INITU1ENA)
				usb_status |= 1 << USB_DEV_STAT_U1_ENABLED;
			if (reg & DWC3_DCTL_INITU2ENA)
				usb_status |= 1 << USB_DEV_STAT_U2_ENABLED;
		} else {
			usb_status |= dwc->gadget.remote_wakeup <<
				USB_DEVICE_REMOTE_WAKEUP;
		}

		break;

	case USB_RECIP_INTERFACE:
		/*
		 * Function Remote Wake Capable	D0
		 * Function Remote Wakeup	D1
		 */

		ret = dwc3_ep0_delegate_req(dwc, ctrl);
		return ret;

	case USB_RECIP_ENDPOINT:
		dep = dwc3_wIndex_to_dep(dwc, ctrl->wIndex);
		if (!dep)
			return -EINVAL;

		if (dep->flags & DWC3_EP_STALL)
			usb_status = 1 << USB_ENDPOINT_HALT;
		break;
	default:
		return -EINVAL;
	}

	response_pkt = (__le16 *) dwc->setup_buf;
	*response_pkt = cpu_to_le16(usb_status);

	dep = dwc->eps[0];
	dwc->ep0_usb_req.dep = dep;
	dwc->ep0_usb_req.request.length = sizeof(*response_pkt);
	dwc->ep0_usb_req.request.buf = dwc->setup_buf;
	dwc->ep0_usb_req.request.complete = dwc3_ep0_status_cmpl;

	return __dwc3_gadget_ep0_queue(dep, &dwc->ep0_usb_req);
}

static int dwc3_ep0_handle_feature(struct dwc3 *dwc,
		struct usb_ctrlrequest *ctrl, int set)
{
	struct dwc3_ep		*dep;
	u32			recip;
	u32			wValue;
	u32			wIndex;
	u32			reg;
	int			ret;
	enum usb_device_state	state;

	wValue = le16_to_cpu(ctrl->wValue);
	wIndex = le16_to_cpu(ctrl->wIndex);
	recip = ctrl->bRequestType & USB_RECIP_MASK;
	state = dwc->gadget.state;

	switch (recip) {
	case USB_RECIP_DEVICE:

		switch (wValue) {
		case USB_DEVICE_REMOTE_WAKEUP:
			pr_debug("%s(): remote wakeup :%s\n", __func__,
					(set ? "enabled" : "disabled"));
			dwc->gadget.remote_wakeup = set;
			break;
		/*
		 * 9.4.1 says only only for SS, in AddressState only for
		 * default control pipe
		 */
		case USB_DEVICE_U1_ENABLE:
			if (state != USB_STATE_CONFIGURED)
				return -EINVAL;
			if (dwc->speed != DWC3_DSTS_SUPERSPEED)
				return -EINVAL;

			if (dwc->usb3_u1u2_disable && !enable_dwc3_u1u2)
				return -EINVAL;

			reg = dwc3_readl(dwc->regs, DWC3_DCTL);
			if (set)
				reg |= DWC3_DCTL_INITU1ENA;
			else
				reg &= ~DWC3_DCTL_INITU1ENA;
			dwc3_writel(dwc->regs, DWC3_DCTL, reg);
			break;

		case USB_DEVICE_U2_ENABLE:
			if (state != USB_STATE_CONFIGURED)
				return -EINVAL;
			if (dwc->speed != DWC3_DSTS_SUPERSPEED)
				return -EINVAL;

			if (dwc->usb3_u1u2_disable && !enable_dwc3_u1u2)
				return -EINVAL;

			reg = dwc3_readl(dwc->regs, DWC3_DCTL);
			if (set)
				reg |= DWC3_DCTL_INITU2ENA;
			else
				reg &= ~DWC3_DCTL_INITU2ENA;
			dwc3_writel(dwc->regs, DWC3_DCTL, reg);
			break;

		case USB_DEVICE_LTM_ENABLE:
			return -EINVAL;
			break;

		case USB_DEVICE_TEST_MODE:
			if ((wIndex & 0xff) != 0)
				return -EINVAL;
			if (!set)
				return -EINVAL;

			dwc->test_mode_nr = wIndex >> 8;
			dwc->test_mode = true;
			break;
		default:
			return -EINVAL;
		}
		break;

	case USB_RECIP_INTERFACE:
		switch (wValue) {
		case USB_INTRF_FUNC_SUSPEND:
			if (wIndex & USB_INTRF_FUNC_SUSPEND_LP)
				/* XXX enable Low power suspend */
				;
			if (wIndex & USB_INTRF_FUNC_SUSPEND_RW)
				/* XXX enable remote wakeup */
				;
			ret = dwc3_ep0_delegate_req(dwc, ctrl);
			if (ret)
				return ret;
			break;
		default:
			return -EINVAL;
		}
		break;

	case USB_RECIP_ENDPOINT:
		switch (wValue) {
		case USB_ENDPOINT_HALT:
			dep = dwc3_wIndex_to_dep(dwc, wIndex);
			if (!dep)
				return -EINVAL;
			if (set == 0 && (dep->flags & DWC3_EP_WEDGE))
				break;
			ret = __dwc3_gadget_ep_set_halt(dep, set, true);
			if (ret)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int dwc3_ep0_set_address(struct dwc3 *dwc, struct usb_ctrlrequest *ctrl)
{
	enum usb_device_state state = dwc->gadget.state;
	u32 addr;
	u32 reg;

	addr = le16_to_cpu(ctrl->wValue);
	if (addr > 127) {
		dwc3_trace(trace_dwc3_ep0, "invalid device address %d", addr);
		return -EINVAL;
	}

	if (state == USB_STATE_CONFIGURED) {
		dwc3_trace(trace_dwc3_ep0,
				"trying to set address when configured");
		return -EINVAL;
	}

	reg = dwc3_readl(dwc->regs, DWC3_DCFG);
	reg &= ~(DWC3_DCFG_DEVADDR_MASK);
	reg |= DWC3_DCFG_DEVADDR(addr);
	dwc3_writel(dwc->regs, DWC3_DCFG, reg);

	if (addr)
		usb_gadget_set_state(&dwc->gadget, USB_STATE_ADDRESS);
	else
		usb_gadget_set_state(&dwc->gadget, USB_STATE_DEFAULT);

	return 0;
}

static int dwc3_ep0_set_config(struct dwc3 *dwc, struct usb_ctrlrequest *ctrl)
{
	enum usb_device_state state = dwc->gadget.state;
	u32 cfg;
	int ret, num;
	u32 reg;
	struct dwc3_ep	*dep;

	cfg = le16_to_cpu(ctrl->wValue);

	switch (state) {
	case USB_STATE_DEFAULT:
		return -EINVAL;
		break;

	case USB_STATE_ADDRESS:
		/*
		 * If tx-fifo-resize flag is not set for the controller, then
		 * do not clear existing allocated TXFIFO since we do not
		 * allocate it again in dwc3_gadget_resize_tx_fifos
		 */
		if (dwc->needs_fifo_resize) {
			/* Read ep0IN related TXFIFO size */
			dwc->last_fifo_depth = (dwc3_readl(dwc->regs,
						DWC3_GTXFIFOSIZ(0)) & 0xFFFF);
			/* Clear existing TXFIFO for all IN eps except ep0 */
			for (num = 0; num < dwc->num_in_eps; num++) {
				dep = dwc->eps[(num << 1) | 1];
				if (num) {
					dwc3_writel(dwc->regs,
						DWC3_GTXFIFOSIZ(num), 0);
					dep->fifo_depth = 0;
				} else {
					dep->fifo_depth = dwc->last_fifo_depth;
				}

				dev_dbg(dwc->dev, "%s(): %s fifo_depth:%x\n",
					__func__, dep->name, dep->fifo_depth);
				dbg_event(0xFF, "fifo_reset", dep->number);
			}
		}

		ret = dwc3_ep0_delegate_req(dwc, ctrl);
		/* if the cfg matches and the cfg is non zero */
		if (cfg && (!ret || (ret == USB_GADGET_DELAYED_STATUS))) {

			/*
			 * only change state if set_config has already
			 * been processed. If gadget driver returns
			 * USB_GADGET_DELAYED_STATUS, we will wait
			 * to change the state on the next usb_ep_queue()
			 */
			if (ret == 0)
				usb_gadget_set_state(&dwc->gadget,
						USB_STATE_CONFIGURED);

			if (!dwc->usb3_u1u2_disable || enable_dwc3_u1u2) {
				/*
				 * Enable transition to U1/U2 state when
				 * nothing is pending from application.
				 */
				reg = dwc3_readl(dwc->regs, DWC3_DCTL);
				reg |= (DWC3_DCTL_ACCEPTU1ENA |
							DWC3_DCTL_ACCEPTU2ENA);
				dwc3_writel(dwc->regs, DWC3_DCTL, reg);
			}
		}
		break;

	case USB_STATE_CONFIGURED:
		ret = dwc3_ep0_delegate_req(dwc, ctrl);
		if (!cfg && !ret)
			usb_gadget_set_state(&dwc->gadget,
					USB_STATE_ADDRESS);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
static int dwc3_ep0_set_interface(struct dwc3 *dwc, struct usb_ctrlrequest *ctrl)
{
	int ret;

	ret = dwc3_ep0_delegate_req(dwc, ctrl);

	return ret;
}
#endif
static void dwc3_ep0_set_sel_cmpl(struct usb_ep *ep, struct usb_request *req)
{
	struct dwc3_ep	*dep = to_dwc3_ep(ep);
	struct dwc3	*dwc = dep->dwc;

	u32		param = 0;
	u32		reg;

	struct timing {
		u8	u1sel;
		u8	u1pel;
		u16	u2sel;
		u16	u2pel;
	} __packed timing;

	int		ret;

	memcpy(&timing, req->buf, sizeof(timing));

	dwc->u1sel = timing.u1sel;
	dwc->u1pel = timing.u1pel;
	dwc->u2sel = le16_to_cpu(timing.u2sel);
	dwc->u2pel = le16_to_cpu(timing.u2pel);

	reg = dwc3_readl(dwc->regs, DWC3_DCTL);
	if (reg & DWC3_DCTL_INITU2ENA)
		param = dwc->u2pel;
	if (reg & DWC3_DCTL_INITU1ENA)
		param = dwc->u1pel;

	/*
	 * According to Synopsys Databook, if parameter is
	 * greater than 125, a value of zero should be
	 * programmed in the register.
	 */
	if (param > 125)
		param = 0;

	/* now that we have the time, issue DGCMD Set Sel */
	ret = dwc3_send_gadget_generic_command(dwc,
			DWC3_DGCMD_SET_PERIODIC_PAR, param);
	if (WARN_ON_ONCE(ret < 0))
		dbg_event(dep->number, "ESET_SELCMPL", ret);
}

static int dwc3_ep0_set_sel(struct dwc3 *dwc, struct usb_ctrlrequest *ctrl)
{
	struct dwc3_ep	*dep;
	enum usb_device_state state = dwc->gadget.state;
	u16		wLength;
	u16		wValue;

	if (state == USB_STATE_DEFAULT)
		return -EINVAL;

	wValue = le16_to_cpu(ctrl->wValue);
	wLength = le16_to_cpu(ctrl->wLength);

	if (wLength != 6) {
		dev_err(dwc->dev, "Set SEL should be 6 bytes, got %d\n",
				wLength);
		return -EINVAL;
	}

	/*
	 * To handle Set SEL we need to receive 6 bytes from Host. So let's
	 * queue a usb_request for 6 bytes.
	 *
	 * Remember, though, this controller can't handle non-wMaxPacketSize
	 * aligned transfers on the OUT direction, so we queue a request for
	 * wMaxPacketSize instead.
	 */
	dep = dwc->eps[0];
	dwc->ep0_usb_req.dep = dep;
	dwc->ep0_usb_req.request.length = dep->endpoint.maxpacket;
	dwc->ep0_usb_req.request.buf = dwc->setup_buf;
	dwc->ep0_usb_req.request.complete = dwc3_ep0_set_sel_cmpl;

	return __dwc3_gadget_ep0_queue(dep, &dwc->ep0_usb_req);
}

static int dwc3_ep0_set_isoch_delay(struct dwc3 *dwc, struct usb_ctrlrequest *ctrl)
{
	u16		wLength;
	u16		wValue;
	u16		wIndex;

	wValue = le16_to_cpu(ctrl->wValue);
	wLength = le16_to_cpu(ctrl->wLength);
	wIndex = le16_to_cpu(ctrl->wIndex);

	if (wIndex || wLength)
		return -EINVAL;

	/*
	 * REVISIT It's unclear from Databook what to do with this
	 * value. For now, just cache it.
	 */
	dwc->isoch_delay = wValue;

	return 0;
}

static int dwc3_ep0_std_request(struct dwc3 *dwc, struct usb_ctrlrequest *ctrl)
{
	int ret;

	switch (ctrl->bRequest) {
	case USB_REQ_GET_STATUS:
		dwc3_trace(trace_dwc3_ep0, "USB_REQ_GET_STATUS\n");
		ret = dwc3_ep0_handle_status(dwc, ctrl);
		break;
	case USB_REQ_CLEAR_FEATURE:
		dwc3_trace(trace_dwc3_ep0, "USB_REQ_CLEAR_FEATURE\n");
		ret = dwc3_ep0_handle_feature(dwc, ctrl, 0);
		break;
	case USB_REQ_SET_FEATURE:
		dwc3_trace(trace_dwc3_ep0, "USB_REQ_SET_FEATURE\n");
		ret = dwc3_ep0_handle_feature(dwc, ctrl, 1);
		break;
	case USB_REQ_SET_ADDRESS:
		dwc3_trace(trace_dwc3_ep0, "USB_REQ_SET_ADDRESS\n");
		ret = dwc3_ep0_set_address(dwc, ctrl);
		break;
	case USB_REQ_SET_CONFIGURATION:
		dwc3_trace(trace_dwc3_ep0, "USB_REQ_SET_CONFIGURATION\n");
#ifdef CONFIG_USB_CHARGING_EVENT
		if(dwc->gadget.speed == USB_SPEED_SUPER)
			dwc->vbus_current = USB_CURRENT_SUPER_SPEED;
		else
			dwc->vbus_current= USB_CURRENT_HIGH_SPEED;
		schedule_work(&dwc->set_vbus_current_work);
#endif
		ret = dwc3_ep0_set_config(dwc, ctrl);
		break;
	case USB_REQ_SET_SEL:
		dwc3_trace(trace_dwc3_ep0, "USB_REQ_SET_SEL\n");
		ret = dwc3_ep0_set_sel(dwc, ctrl);
		break;
	case USB_REQ_SET_ISOCH_DELAY:
		dwc3_trace(trace_dwc3_ep0, "USB_REQ_SET_ISOCH_DELAY\n");
		ret = dwc3_ep0_set_isoch_delay(dwc, ctrl);
		break;
#ifdef CONFIG_USB_ANDROID_SAMSUNG_COMPOSITE
	case USB_REQ_SET_INTERFACE:
		dev_vdbg(dwc->dev, "USB_REQ_SET_INTERFACE\n");
		ret = dwc3_ep0_set_interface(dwc, ctrl);
		break;
#endif
	default:
		dwc3_trace(trace_dwc3_ep0, "Forwarding to gadget driver\n");
		ret = dwc3_ep0_delegate_req(dwc, ctrl);
		break;
	}

	return ret;
}

static void dwc3_ep0_inspect_setup(struct dwc3 *dwc,
		const struct dwc3_event_depevt *event)
{
	struct usb_ctrlrequest *ctrl = dwc->ctrl_req;
	int ret = -EINVAL;
	u32 len;

	if (!dwc->gadget_driver)
		goto out;

	trace_dwc3_ctrl_req(ctrl);

	len = le16_to_cpu(ctrl->wLength);
	if (!len) {
		dwc->three_stage_setup = false;
		dwc->ep0_expect_in = false;
		dwc->ep0_next_event = DWC3_EP0_NRDY_STATUS;
	} else {
		dwc->three_stage_setup = true;
		dwc->ep0_expect_in = !!(ctrl->bRequestType & USB_DIR_IN);
		dwc->ep0_next_event = DWC3_EP0_NRDY_DATA;
	}

	dbg_setup(0x00, ctrl);
	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD)
		ret = dwc3_ep0_std_request(dwc, ctrl);
	else
		ret = dwc3_ep0_delegate_req(dwc, ctrl);

	if (ret == USB_GADGET_DELAYED_STATUS)
		dwc->delayed_status = true;

out:
	if (ret < 0) {
		dbg_event(0x0, "ERRSTAL", ret);
		dwc3_ep0_stall_and_restart(dwc);
	}
}

static bool zlp_required;
static void dwc3_ep0_complete_data(struct dwc3 *dwc,
		const struct dwc3_event_depevt *event)
{
	struct dwc3_request	*r = NULL;
	struct usb_request	*ur;
	struct dwc3_trb		*trb;
	struct dwc3_ep		*ep0;
	u32			transferred;
	u32			status;
	u32			length;
	u8			epnum;

	epnum = event->endpoint_number;
	ep0 = dwc->eps[0];

	dwc->ep0_next_event = DWC3_EP0_NRDY_STATUS;

	trb = dwc->ep0_trb;

	r = next_request(&ep0->request_list);
	if (!r)
		return;

	ur = &r->request;
	if ((epnum & 1) && ur->zero &&
		(ur->length % ep0->endpoint.maxpacket == 0)) {
		zlp_required = true;
		ur->zero = false;
	}

	status = DWC3_TRB_SIZE_TRBSTS(trb->size);
	if (status == DWC3_TRBSTS_SETUP_PENDING) {
		dwc3_trace(trace_dwc3_ep0, "Setup Pending received");
		zlp_required = false;

		if (r)
			dwc3_gadget_giveback(ep0, r, -ECONNRESET);

		return;
	}

	if (zlp_required)
		return;

	length = trb->size & DWC3_TRB_SIZE_MASK;

	if (dwc->ep0_bounced) {
		unsigned transfer_size = ur->length;
		unsigned maxp = ep0->endpoint.maxpacket;

		transfer_size += (maxp - (transfer_size % maxp));
		transferred = min_t(u32, ur->length,
				transfer_size - length);
		memcpy(ur->buf, dwc->ep0_bounce, transferred);
	} else {
		transferred = ur->length - length;
	}

	ur->actual += transferred;

	if ((epnum & 1) && ur->actual < ur->length) {
		/* for some reason we did not get everything out */
		dbg_event(epnum, "INDATSTAL", 0);
		dwc3_ep0_stall_and_restart(dwc);
	} else {
		dwc3_gadget_giveback(ep0, r, 0);

		if (IS_ALIGNED(ur->length, ep0->endpoint.maxpacket) &&
				ur->length && ur->zero) {
			int ret;

			dwc->ep0_next_event = DWC3_EP0_COMPLETE;

			ret = dwc3_ep0_start_trans(dwc, epnum,
					dwc->ctrl_req_addr, 0,
					DWC3_TRBCTL_CONTROL_DATA);
			if (WARN_ON_ONCE(ret < 0))
				dbg_event(epnum, "ECTRL_DATA", ret);
		}
	}
}

static void dwc3_ep0_complete_status(struct dwc3 *dwc,
		const struct dwc3_event_depevt *event)
{
	struct dwc3_request	*r;
	struct dwc3_ep		*dep;
	struct dwc3_trb		*trb;
	u32			status;

	dep = dwc->eps[0];
	trb = dwc->ep0_trb;

	if (!list_empty(&dep->request_list)) {
		r = next_request(&dep->request_list);

		dwc3_gadget_giveback(dep, r, 0);
	}

	if (dwc->test_mode) {
		int ret;

		ret = dwc3_gadget_set_test_mode(dwc, dwc->test_mode_nr);
		if (ret < 0) {
			dwc3_trace(trace_dwc3_ep0, "Invalid Test #%d",
					dwc->test_mode_nr);
			dbg_event(0x00, "INVALTEST", ret);
			dwc3_ep0_stall_and_restart(dwc);
			return;
		}
	}

	status = DWC3_TRB_SIZE_TRBSTS(trb->size);
	if (status == DWC3_TRBSTS_SETUP_PENDING)
		dwc3_trace(trace_dwc3_ep0, "Setup Pending received\n");

	dbg_print(dep->number, "DONE", status, "STATUS");
	dwc->ep0state = EP0_SETUP_PHASE;
	dwc3_ep0_out_start(dwc);
}

static void dwc3_ep0_xfer_complete(struct dwc3 *dwc,
			const struct dwc3_event_depevt *event)
{
	struct dwc3_ep		*dep = dwc->eps[event->endpoint_number];

	dep->flags &= ~DWC3_EP_BUSY;
	dep->resource_index = 0;
	dwc->setup_packet_pending = false;

	switch (dwc->ep0state) {
	case EP0_SETUP_PHASE:
		dwc3_trace(trace_dwc3_ep0, "Setup Phase");
		dwc3_ep0_inspect_setup(dwc, event);
		break;

	case EP0_DATA_PHASE:
		dwc3_trace(trace_dwc3_ep0, "Data Phase");
		dwc3_ep0_complete_data(dwc, event);
		break;

	case EP0_STATUS_PHASE:
		dwc3_trace(trace_dwc3_ep0, "Status Phase");
		dwc3_ep0_complete_status(dwc, event);
		break;
	default:
		WARN(true, "UNKNOWN ep0state %d\n", dwc->ep0state);
	}
}

static void __dwc3_ep0_do_control_data(struct dwc3 *dwc,
		struct dwc3_ep *dep, struct dwc3_request *req)
{
	int			ret;

	req->direction = !!dep->number;

	if (req->request.length == 0) {
		ret = dwc3_ep0_start_trans(dwc, dep->number,
				dwc->ctrl_req_addr, 0,
				DWC3_TRBCTL_CONTROL_DATA);
	} else if (!IS_ALIGNED(req->request.length, dep->endpoint.maxpacket)
			&& (dep->number == 0)) {
		u32	transfer_size;
		u32	maxpacket;

		ret = usb_gadget_map_request(&dwc->gadget, &req->request,
				dep->number);
		if (ret) {
			dev_dbg(dwc->dev, "failed to map request\n");
			return;
		}

		WARN_ON(req->request.length > DWC3_EP0_BOUNCE_SIZE);

		maxpacket = dep->endpoint.maxpacket;
		transfer_size = roundup(req->request.length, maxpacket);

		dwc->ep0_bounced = true;

		/*
		 * REVISIT in case request length is bigger than
		 * DWC3_EP0_BOUNCE_SIZE we will need two chained
		 * TRBs to handle the transfer.
		 */
		ret = dwc3_ep0_start_trans(dwc, dep->number,
				dwc->ep0_bounce_addr, transfer_size,
				DWC3_TRBCTL_CONTROL_DATA);
	} else {
		ret = usb_gadget_map_request(&dwc->gadget, &req->request,
				dep->number);
		if (ret) {
			dev_dbg(dwc->dev, "failed to map request\n");
			return;
		}

		if (dep->number &&
			!(req->request.length % dwc->gadget.ep0->maxpacket))
			req->request.zero = true;

		ret = dwc3_ep0_start_trans(dwc, dep->number, req->request.dma,
				req->request.length, DWC3_TRBCTL_CONTROL_DATA);
	}

	dbg_queue(dep->number, &req->request, ret);
}

static int dwc3_ep0_start_control_status(struct dwc3_ep *dep)
{
	struct dwc3		*dwc = dep->dwc;
	u32			type;

	type = dwc->three_stage_setup ? DWC3_TRBCTL_CONTROL_STATUS3
		: DWC3_TRBCTL_CONTROL_STATUS2;

	return dwc3_ep0_start_trans(dwc, dep->number,
			dwc->ctrl_req_addr, 0, type);
}

static void __dwc3_ep0_do_control_status(struct dwc3 *dwc, struct dwc3_ep *dep)
{
	int ret;

	ret = dwc3_ep0_start_control_status(dep);
	if (WARN_ON_ONCE(ret))
		dbg_event(dep->number, "ECTRLSTATUS", ret);
}

static void dwc3_ep0_do_control_status(struct dwc3 *dwc,
		const struct dwc3_event_depevt *event)
{
	struct dwc3_ep		*dep = dwc->eps[event->endpoint_number];

	__dwc3_ep0_do_control_status(dwc, dep);
}

static void dwc3_ep0_end_control_data(struct dwc3 *dwc, struct dwc3_ep *dep)
{
	struct dwc3_gadget_ep_cmd_params params;
	u32			cmd;
	int			ret;

	if (!dep->resource_index)
		return;

	cmd = DWC3_DEPCMD_ENDTRANSFER;
	cmd |= DWC3_DEPCMD_CMDIOC;
	cmd |= DWC3_DEPCMD_PARAM(dep->resource_index);
	memset(&params, 0, sizeof(params));
	ret = dwc3_send_gadget_ep_cmd(dwc, dep->number, cmd, &params);
	if (ret) {
		dev_dbg(dwc->dev, "%s: send ep cmd ENDTRANSFER failed",
			dep->name);
		dbg_event(dep->number, "EENDXFER", ret);
	}
	dep->resource_index = 0;
}

static void dwc3_ep0_xfernotready(struct dwc3 *dwc,
		const struct dwc3_event_depevt *event)
{
	u8			epnum;
	struct dwc3_ep		*dep;
	int			ret;

	dwc->setup_packet_pending = true;
	epnum = event->endpoint_number;
	dep = dwc->eps[epnum];

	switch (event->status) {
	case DEPEVT_STATUS_CONTROL_DATA:
		dep->dbg_ep_events.control_data++;

		/*
		 * When we issue a STALL and RESTART of EP0 OUT, then
		 * ep0_next_event is set as DWC3_EP0_COMPLETE and we wait for
		 * the next setup packet. We will ignore a XferNotReady (DATA)
		 * event until setup packet arrives, so as to avoid HW latency
		 * issues.
		 */
		if (dwc->ep0_next_event == DWC3_EP0_COMPLETE) {
			dwc3_trace(trace_dwc3_ep0, "Ignore Control Data");
			return;
		}

		dwc3_trace(trace_dwc3_ep0, "Control Data");

		/*
		 * We already have a DATA transfer in the controller's cache,
		 * if we receive a XferNotReady(DATA) we will ignore it, unless
		 * it's for the wrong direction.
		 *
		 * In that case, we must issue END_TRANSFER command to the Data
		 * Phase we already have started and issue SetStall on the
		 * control endpoint.
		 */
		if (dwc->ep0_expect_in != event->endpoint_number) {
			struct dwc3_ep	*dep = dwc->eps[dwc->ep0_expect_in];

			dwc3_trace(trace_dwc3_ep0,
					"Wrong direction for Data phase");
			dwc3_ep0_end_control_data(dwc, dep);
			dbg_event(epnum, "WRONGDR", 0);
			dwc3_ep0_stall_and_restart(dwc);
			return;
		}

		if (zlp_required) {
			zlp_required = false;
			ret = dwc3_ep0_start_trans(dwc, epnum,
					dwc->ctrl_req_addr, 0,
					DWC3_TRBCTL_CONTROL_DATA);
			dbg_event(epnum, "ZLP", ret);
			if (ret)
				dev_dbg(dwc->dev, "%s: start xfer cmd failed",
					dep->name);
		}

		break;

	case DEPEVT_STATUS_CONTROL_STATUS:
		dep->dbg_ep_events.control_status++;
		if (dwc->ep0_next_event != DWC3_EP0_NRDY_STATUS)
			return;

		dwc3_trace(trace_dwc3_ep0, "Control Status");

		zlp_required = false;
		dwc->ep0state = EP0_STATUS_PHASE;

		if (dwc->delayed_status &&
				list_empty(&dwc->eps[0]->request_list)) {
			if (event->endpoint_number != 1)
				dbg_event(epnum, "EEPNUM", event->status);
			dwc3_trace(trace_dwc3_ep0, "Delayed Status");
			return;
		}
		dwc->delayed_status = false;

		dwc3_ep0_do_control_status(dwc, event);
	}
}

void dwc3_ep0_interrupt(struct dwc3 *dwc,
		const struct dwc3_event_depevt *event)
{
	u8			epnum = event->endpoint_number;
	struct dwc3_ep		*dep;

	dwc3_trace(trace_dwc3_ep0, "%s while ep%d%s in state '%s'",
			dwc3_ep_event_string(event->endpoint_event),
			epnum >> 1, (epnum & 1) ? "in" : "out",
			dwc3_ep0_state_string(dwc->ep0state));

	dep = dwc->eps[epnum];
	switch (event->endpoint_event) {
	case DWC3_DEPEVT_XFERCOMPLETE:
		dwc3_ep0_xfer_complete(dwc, event);
		dep->dbg_ep_events.xfercomplete++;
		break;

	case DWC3_DEPEVT_XFERNOTREADY:
		dwc3_ep0_xfernotready(dwc, event);
		dep->dbg_ep_events.xfernotready++;
		break;

	case DWC3_DEPEVT_XFERINPROGRESS:
		dep->dbg_ep_events.xferinprogress++;
		break;
	case DWC3_DEPEVT_RXTXFIFOEVT:
		dep->dbg_ep_events.rxtxfifoevent++;
		break;
	case DWC3_DEPEVT_STREAMEVT:
		dep->dbg_ep_events.streamevent++;
		break;
	case DWC3_DEPEVT_EPCMDCMPLT:
		dep->dbg_ep_events.epcmdcomplete++;
		break;
	}
}
