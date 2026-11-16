/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/* Configuration of NXP's USB device stack for the qjsx link of the i.MX
 * RT1062 (usb_serial_imxrt1062.cpp): one EHCI controller (USB1, the board's
 * USB port), one CDC ACM function, bare-metal mode (everything in the USB
 * interrupt: our OSA has no tasks, fsl_os_abstraction.h). The names are the
 * stack's (its template: middleware/usb/config/device/ehci). */
#ifndef _USB_DEVICE_CONFIG_H_
#define _USB_DEVICE_CONFIG_H_

/* controllers: one EHCI */
#define USB_DEVICE_CONFIG_KHCI (0U)
#define USB_DEVICE_CONFIG_EHCI (1U)
#define USB_DEVICE_CONFIG_LPCIP3511FS (0U)
#define USB_DEVICE_CONFIG_LPCIP3511HS (0U)
#define USB_DEVICE_CONFIG_NUM \
    (USB_DEVICE_CONFIG_KHCI + USB_DEVICE_CONFIG_EHCI + USB_DEVICE_CONFIG_LPCIP3511FS + USB_DEVICE_CONFIG_LPCIP3511HS)

/* classes: one CDC ACM (the serial port of the link) */
#define USB_DEVICE_CONFIG_HID (0U)
#define USB_DEVICE_CONFIG_CDC_ACM (1U)
#define USB_DEVICE_CONFIG_CDC_RNDIS (0U)
#define USB_DEVICE_CONFIG_MSC (0U)
#define USB_DEVICE_CONFIG_AUDIO (0U)
#define USB_DEVICE_CONFIG_PHDC (0U)
#define USB_DEVICE_CONFIG_VIDEO (0U)
#define USB_DEVICE_CONFIG_CCID (0U)
#define USB_DEVICE_CONFIG_PRINTER (0U)
#define USB_DEVICE_CONFIG_DFU (0U)

/* powered by the bus */
#define USB_DEVICE_CONFIG_SELF_POWER (0U)

/* endpoints 0 (control), 1 (CDC notifications), 2 (CDC data) */
#define USB_DEVICE_CONFIG_ENDPOINTS (4U)

/* bare-metal: no device task, no message queue */
#define USB_DEVICE_CONFIG_USE_TASK (0U)
#define USB_DEVICE_CONFIG_MAX_MESSAGES (8U)

/* no test modes */
#define USB_DEVICE_CONFIG_USB20_TEST_MODE (0U)
#define USB_DEVICE_CONFIG_CV_TEST (0U)
#define USB_DEVICE_CONFIG_COMPLIANCE_TEST (0U)

/* EHCI: transfer descriptors, no ID pin */
#define USB_DEVICE_CONFIG_EHCI_MAX_DTD (16U)
#define USB_DEVICE_CONFIG_EHCI_ID_PIN_DETECT (0U)

/* the transfer buffers are in the DTCM, which has no cache */
#define USB_DEVICE_CONFIG_BUFFER_PROPERTY_CACHEABLE (0U)

/* no keep alive, low power, remote wake up, detach, error handling, SOF */
#define USB_DEVICE_CONFIG_KEEP_ALIVE_MODE (0U)
#define USB_DEVICE_CONFIG_LOW_POWER_MODE (0U)
#define USB_DEVICE_CONFIG_REMOTE_WAKEUP (0U)
#define USB_DEVICE_CONFIG_DETACH_ENABLE (0U)
#define USB_DEVICE_CONFIG_ERROR_HANDLING (0U)
#define USB_DEVICE_CONFIG_SOF_NOTIFICATIONS (0U)

#endif /* _USB_DEVICE_CONFIG_H_ */
