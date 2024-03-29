/* Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/diagchar.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/ratelimit.h>
#include "diagchar.h"
#include "diagfwd.h"
#include "diag_mux.h"
#include "diag_usb.h"
#include "diag_memorydevice.h"
#include "diag_ipc_logging.h"

#ifdef CONFIG_LGE_DIAG_BYPASS
#include "lg_diag_bypass.h"
#endif

struct diag_mux_state_t *diag_mux;
static struct diag_logger_t usb_logger;
static struct diag_logger_t md_logger;

static struct diag_logger_ops usb_log_ops = {
	.open = diag_usb_connect_all,
	.close = diag_usb_disconnect_all,
	.queue_read = diag_usb_queue_read,
	.write = diag_usb_write,
	.close_peripheral = NULL
};

static struct diag_logger_ops md_log_ops = {
	.open = diag_md_open_all,
	.close = diag_md_close_all,
	.queue_read = NULL,
	.write = diag_md_write,
	.close_peripheral = diag_md_close_peripheral,
};

int diag_mux_init(void)
{
	diag_mux = kzalloc(sizeof(struct diag_mux_state_t),
			 GFP_KERNEL);
	if (!diag_mux)
		return -ENOMEM;
	kmemleak_not_leak(diag_mux);

	usb_logger.mode = DIAG_USB_MODE;
	usb_logger.log_ops = &usb_log_ops;

	md_logger.mode = DIAG_MEMORY_DEVICE_MODE;
	md_logger.log_ops = &md_log_ops;
	diag_md_init();

	/*
	 * Set USB logging as the default logger. This is the mode
	 * Diag should be in when it initializes.
	 */
	diag_mux->usb_ptr = &usb_logger;
	diag_mux->md_ptr = &md_logger;
	diag_mux->logger = &usb_logger;
	diag_mux->mux_mask = 0;
	diag_mux->mode = DIAG_USB_MODE;
	return 0;
}

void diag_mux_exit(void)
{
	kfree(diag_mux);
}

int diag_mux_register(int proc, int ctx, struct diag_mux_ops *ops)
{
	int err = 0;
	if (!ops)
		return -EINVAL;

	if (proc < 0 || proc >= NUM_MUX_PROC)
		return 0;

	/* Register with USB logger */
	usb_logger.ops[proc] = ops;
	err = diag_usb_register(proc, ctx, ops);
	if (err) {
		pr_err("diag: MUX: unable to register usb operations for proc: %d, err: %d\n",
		       proc, err);
		return err;
	}

	md_logger.ops[proc] = ops;
	err = diag_md_register(proc, ctx, ops);
	if (err) {
		pr_err("diag: MUX: unable to register md operations for proc: %d, err: %d\n",
		       proc, err);
		return err;
	}

	return 0;
}

int diag_mux_queue_read(int proc)
{
	struct diag_logger_t *logger = NULL;

	if (proc < 0 || proc >= NUM_MUX_PROC)
		return -EINVAL;
	if (!diag_mux)
		return -EIO;

/* [LGE_S][BSP_Modem] LGSSL to support testmode cmd */
#ifdef CONFIG_LGE_DM_APP
	if (diag_mux->mode == DIAG_MEMORY_DEVICE_MODE || diag_mux->mode == DIAG_MULTI_MODE)
#else
	if (diag_mux->mode == DIAG_MULTI_MODE)
#endif
/* [LGE_E][BSP_Modem] LGSSL to support testmode cmd */
		logger = diag_mux->usb_ptr;
	else
		logger = diag_mux->logger;

	if (logger && logger->log_ops && logger->log_ops->queue_read)
		return logger->log_ops->queue_read(proc);

	return 0;
}

int diag_mux_write(int proc, unsigned char *buf, int len, int ctx)
{
	struct diag_logger_t *logger = NULL;
	int peripheral, upd;

	if (proc < 0 || proc >= NUM_MUX_PROC)
		return -EINVAL;
	if (!diag_mux)
		return -EIO;

	upd = GET_PD_CTXT(ctx);
	if (upd) {
		switch (upd) {
		case DIAG_ID_MPSS:
			upd = PERIPHERAL_MODEM;
			break;
		case DIAG_ID_LPASS:
			upd = PERIPHERAL_LPASS;
			break;
		case DIAG_ID_CDSP:
			upd = PERIPHERAL_CDSP;
			break;
		case UPD_WLAN:
			if (!driver->pd_logging_mode[0])
				upd = PERIPHERAL_MODEM;
			break;
		case UPD_AUDIO:
			if (!driver->pd_logging_mode[1])
				upd = PERIPHERAL_LPASS;
			break;
		case UPD_SENSORS:
			if (!driver->pd_logging_mode[2])
				upd = PERIPHERAL_LPASS;
			break;
		default:
			pr_err("diag: invalid pd ctxt= %d\n", upd);
			return -EINVAL;
		}
		if (((MD_PERIPHERAL_MASK(upd)) &
			(diag_mux->mux_mask)) &&
			driver->md_session_map[upd])
			logger = diag_mux->md_ptr;
		else
			logger = diag_mux->usb_ptr;
	} else {

		peripheral = GET_BUF_PERIPHERAL(ctx);
		if (peripheral > NUM_PERIPHERALS)
			return -EINVAL;

		if (MD_PERIPHERAL_MASK(peripheral) &
			diag_mux->mux_mask)
			logger = diag_mux->md_ptr;
		else
			logger = diag_mux->usb_ptr;
	}

#ifdef CONFIG_LGE_DIAG_BYPASS
    if(diag_bypass_response(buf, len, proc, ctx, logger) > 0) {
        return 0;
    }
#endif

/* [LGE_S][BSP_Modem] LGSSL to support testmode cmd */
#ifdef CONFIG_LGE_DM_APP
	if (driver->logging_mode == DIAG_MEMORY_DEVICE_MODE)
	{
		if ((*((char *)buf) == 0xFA) || (*((char *)buf) == 0x29))
		{
//			pr_debug("diag: In %s, Testmode cmd uses the diag usb. cmd = 0x%2x\n", __func__, *((char *)buf));
			logger = diag_mux->usb_ptr;
		}
	}
#endif
/* [LGE_S][BSP_Modem]  LGSSL to support testmode cmd */
	if (logger && logger->log_ops && logger->log_ops->write)
		return logger->log_ops->write(proc, buf, len, ctx);
	return 0;
}

int diag_mux_close_peripheral(int proc, uint8_t peripheral)
{
	struct diag_logger_t *logger = NULL;
	if (proc < 0 || proc >= NUM_MUX_PROC)
		return -EINVAL;

	/* Peripheral should account for Apps data as well */
	if (peripheral > NUM_PERIPHERALS) {
		if (driver->num_pd_session) {
			if (peripheral > NUM_MD_SESSIONS)
				return -EINVAL;
		} else {
			return -EINVAL;
		}
	}

	if (!diag_mux)
		return -EIO;

	if (MD_PERIPHERAL_MASK(peripheral) & diag_mux->mux_mask)
		logger = diag_mux->md_ptr;
	else
		logger = diag_mux->logger;

	if (logger && logger->log_ops && logger->log_ops->close_peripheral)
		return logger->log_ops->close_peripheral(proc, peripheral);
	return 0;
}

int diag_mux_switch_logging(int *req_mode, int *peripheral_mask)
{
	unsigned int new_mask = 0;

	if (!req_mode)
		return -EINVAL;

	if (*peripheral_mask <= 0 ||
		(*peripheral_mask > (DIAG_CON_ALL | DIAG_CON_UPD_ALL))) {
		pr_err("diag: mask %d in %s\n", *peripheral_mask, __func__);
		return -EINVAL;
	}

	switch (*req_mode) {
	case DIAG_USB_MODE:
		new_mask = ~(*peripheral_mask) & diag_mux->mux_mask;
		if (new_mask != DIAG_CON_NONE)
			*req_mode = DIAG_MULTI_MODE;
		if (new_mask == DIAG_CON_ALL)
			*req_mode = DIAG_MEMORY_DEVICE_MODE;
		break;
	case DIAG_MEMORY_DEVICE_MODE:
		new_mask = (*peripheral_mask) | diag_mux->mux_mask;
/* [LGE_S][BSP_Modem] LGSSL to support testmode cmd */
#ifndef CONFIG_LGE_DM_APP
		if (new_mask != DIAG_CON_ALL)
			*req_mode = DIAG_MULTI_MODE;
#endif
/* [LGE_E][BSP_Modem] LGSSL to support testmode cmd */
		break;
	default:
		pr_err("diag: Invalid mode %d in %s\n", *req_mode, __func__);
		return -EINVAL;
	}

	switch (diag_mux->mode) {
	case DIAG_USB_MODE:
		if (*req_mode == DIAG_MEMORY_DEVICE_MODE) {
/* [LGE_S][BSP_Modem] LGSSL to support testmode cmd */
#ifndef CONFIG_LGE_DM_APP
			diag_mux->usb_ptr->log_ops->close();
#endif
/* [LGE_E][BSP_Modem] LGSSL to support testmode cmd */
			diag_mux->logger = diag_mux->md_ptr;
			diag_mux->md_ptr->log_ops->open();
		} else if (*req_mode == DIAG_MULTI_MODE) {
			diag_mux->md_ptr->log_ops->open();
			diag_mux->logger = NULL;
		}
		break;
	case DIAG_MEMORY_DEVICE_MODE:
		if (*req_mode == DIAG_USB_MODE) {
/* [LGE_S][BSP_Modem] LGSSL to support testmode cmd */
#ifdef CONFIG_LGE_DM_APP
			diag_mux->usb_ptr->log_ops->close();
#endif
/* [LGE_E][BSP_Modem] LGSSL to support testmode cmd */
			diag_mux->md_ptr->log_ops->close();
			diag_mux->logger = diag_mux->usb_ptr;
			diag_mux->usb_ptr->log_ops->open();
		} else if (*req_mode == DIAG_MULTI_MODE) {
			diag_mux->usb_ptr->log_ops->open();
			diag_mux->logger = NULL;
		}
		break;
	case DIAG_MULTI_MODE:
		if (*req_mode == DIAG_USB_MODE) {
			diag_mux->md_ptr->log_ops->close();
			diag_mux->logger = diag_mux->usb_ptr;
		} else if (*req_mode == DIAG_MEMORY_DEVICE_MODE) {
			diag_mux->usb_ptr->log_ops->close();
			diag_mux->logger = diag_mux->md_ptr;
		}
		break;
	}
	diag_mux->mode = *req_mode;
	diag_mux->mux_mask = new_mask;
	*peripheral_mask = new_mask;
	return 0;
}
