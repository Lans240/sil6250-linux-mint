/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UAPI_SIL6250_H
#define _UAPI_SIL6250_H

/*
 * sil6250 - userspace ABI for the Silead SIL6250 fingerprint mailbox broker.
 *
 * This module is deliberately a *resource broker*, not a protocol driver: it
 * claims the ACPI device (HID "SIL6250"), maps its mailbox window, and exposes
 * the two GpioIo strobe outputs and the GpioInt RX-ready doorbell. All of the
 * Petaic protocol lives entirely in userspace.
 *
 * Transport primitives exposed on /dev/sil6250:
 *   - mmap()              maps the mailbox window (write region at +0x000, EC
 *                         response region at +0x200). Access it with 64-bit
 *                         aligned reads/writes, mirroring the firmware contract.
 *   - SIL6250_SET_GPIO    drive one of the two output strobes high/low. The
 *                         pulse *timing* (high/post-delay) is owned by userspace.
 *   - SIL6250_WAIT_IRQ    re-arm and block on the EC's RX-ready GpioInt (the
 *                         level line is masked after each fire and re-armed by
 *                         this call, exactly the contract the EC expects).
 */

#include <linux/types.h>
#include <linux/ioctl.h>

#define SIL6250_IOCTL_MAGIC	0x5C

/* GpioIo output strobe lines (ACPI _CRS GpioIo order). */
#define SIL6250_LINE_WRITE_DONE	0	/* strobed after a request is written  */
#define SIL6250_LINE_READ_DONE	1	/* strobed after a response is consumed */

struct sil6250_gpio {
	__u32 line;	        /* SIL6250_LINE_* */
	__u32 value;	    /* logical level: 0 = deassert, 1 = assert */
};

struct sil6250_wait_irq {
	__u32 timeout_ms;	/* 0 = driver default (500 ms) */
	__u32 _pad;
};

/* IN: drive a strobe output to the given logical level. */
#define SIL6250_SET_GPIO	_IOW(SIL6250_IOCTL_MAGIC, 0x01, struct sil6250_gpio)

/*
 * Re-arm the GpioInt level line and block until the EC asserts it (a response /
 * continuation packet has been staged in the window) or the timeout elapses.
 * Returns 0 on IRQ, -ETIMEDOUT on timeout.
 */
#define SIL6250_WAIT_IRQ	_IOW(SIL6250_IOCTL_MAGIC, 0x02, struct sil6250_wait_irq)

/* OUT: byte size of the mmap-able mailbox window. */
#define SIL6250_GET_WINDOW_SIZE	_IOR(SIL6250_IOCTL_MAGIC, 0x03, __u32)

#endif /* _UAPI_SIL6250_H */
