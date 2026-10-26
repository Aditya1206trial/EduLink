/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * User-space ABI for the EduLink driver (QEMU "edu" PCI device, 1234:11e8).
 *
 * Phase 3 adds EDU_IOC_DMA. Liveness and factorial are unchanged from
 * phase 1.
 */
#ifndef EDU_IOCTL_H
#define EDU_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * The device computes factorial in a 32-bit register and wraps silently.
 * 12! = 479001600 is the largest result that fits; larger inputs are
 * rejected with -ERANGE.
 */
#define EDU_FACTORIAL_MAX_INPUT 12

#define EDU_IOC_MAGIC 'e'

/*
 * EDU_IOC_LIVENESS: in: value written to the liveness register (0x04).
 *                   out: raw value read back. A healthy device returns ~in.
 */
#define EDU_IOC_LIVENESS  _IOWR(EDU_IOC_MAGIC, 1, __u32)

/*
 * EDU_IOC_FACTORIAL: in: n (0..EDU_FACTORIAL_MAX_INPUT).
 *                    out: n! as computed by the device.
 */
#define EDU_IOC_FACTORIAL _IOWR(EDU_IOC_MAGIC, 2, __u32)

/* Size of the device's internal DMA buffer. */
#define EDU_DMA_MAX_LEN 4096

struct edu_dma {
	__u32 len;			/* 1..EDU_DMA_MAX_LEN */
	__u8 data[EDU_DMA_MAX_LEN];
};

/*
 * EDU_IOC_DMA: in: len and the first len bytes of data.
 *              out: the first len bytes of data are replaced with what the
 *              device returned after a RAM -> device -> RAM round trip.
 *              len and the rest of data are not written.
 */
#define EDU_IOC_DMA _IOWR(EDU_IOC_MAGIC, 3, struct edu_dma)

#endif /* EDU_IOCTL_H */
