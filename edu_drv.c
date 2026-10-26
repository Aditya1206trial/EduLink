// SPDX-License-Identifier: GPL-2.0
/*
 * EduLink: driver for the QEMU "edu" educational PCI device (1234:11e8).
 *
 * Phase 3: PCI probe, BAR0 MMIO mapping, identification check, liveness
 * ioctl, interrupt-driven factorial and DMA round trip on shared legacy
 * INTx, exposed as /dev/edu0. Only one device is supported.
 *
 * Register map: https://www.qemu.org/docs/master/specs/edu.html
 */

#include <linux/atomic.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "edu_ioctl.h"

/*
 * DMA registers need single 8-byte readq/writeq. QEMU edu decodes only
 * 0x80/0x88/0x90/0x98, so a split access's high half (0x84, ...) reads all
 * ones and its write is dropped.
 */
#ifndef CONFIG_64BIT
#error "EduLink needs a 64-bit kernel (native readq/writeq)"
#endif

#define DRV_NAME "edulink"

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

/* BAR0 register offsets. Offsets below 0x80 allow 4-byte access only. */
#define EDU_REG_ID       0x00 /* RO: 0xRRrr00ed, RR major, rr minor */
#define EDU_REG_LIVENESS 0x04 /* RW: reads back ~written value */
#define EDU_REG_FACT     0x08 /* RW: write n, read n! once not busy */
#define EDU_REG_STATUS   0x20 /* RW: bit 0 busy (RO), bit 7 IRQ on fact */
#define EDU_REG_IRQ_STATUS 0x24 /* RO: pending interrupt bits */
#define EDU_REG_IRQ_ACK  0x64 /* WO: clears the written interrupt bits */
#define EDU_REG_DMA_SRC  0x80 /* RW: DMA source address */
#define EDU_REG_DMA_DST  0x88 /* RW: DMA destination address */
#define EDU_REG_DMA_CNT  0x90 /* RW: DMA transfer count */
#define EDU_REG_DMA_CMD  0x98 /* RW: DMA command */

#define EDU_STATUS_COMPUTING 0x01
#define EDU_STATUS_IRQFACT   0x80
#define EDU_IRQ_FACT         0x01 /* interrupt status bit: factorial done */
#define EDU_IRQ_DMA          0x100 /* interrupt status bit: DMA done */

#define EDU_DMA_START  0x01 /* set to start; device clears it when done */
#define EDU_DMA_TO_RAM 0x02 /* direction: 0 RAM -> device, 1 device -> RAM */
#define EDU_DMA_IRQ    0x04 /* raise EDU_IRQ_DMA when done */

#define EDU_DMA_DEV_ADDR  0x40000 /* device-side address of its DMA buffer */
#define EDU_DMA_ADDR_BITS 28 /* device default dma_mask */

#define EDU_ID_MAGIC_MASK 0x0000ffffu
#define EDU_ID_MAGIC      0x000000edu

/* Smallest BAR0 that covers every register this phase touches. */
#define EDU_MMIO_MIN_LEN 0x100

#define EDU_FACT_TIMEOUT (HZ) /* 1 s */
#define EDU_DMA_TIMEOUT  (HZ) /* 1 s; QEMU runs each transfer 100 ms after start */

struct edu_dev {
	struct kref ref;
	struct mutex lock;	/* serializes ioctl MMIO; protects regs */
	void __iomem *regs;	/* NULL once removed; the IRQ is freed first */
	bool removed;		/* set by remove() before it wakes waiters */
	struct pci_dev *pdev;
	struct miscdevice misc;
	wait_queue_head_t fact_wq;	/* woken by the factorial IRQ */
	wait_queue_head_t dma_wq;	/* woken by the DMA IRQ */
	void *dma_buf;		/* EDU_DMA_MAX_LEN coherent bytes, under lock */
	dma_addr_t dma_addr;
};

/* Set while a device owns /dev/edu0. */
static atomic_t edu_bound = ATOMIC_INIT(0);

static void edu_free(struct kref *ref)
{
	struct edu_dev *edu = container_of(ref, struct edu_dev, ref);

	mutex_destroy(&edu->lock);
	kfree(edu);
}

/*
 * A surprise-removed PCI device reads back as all ones; no valid status
 * value has every bit set, so treat that as device gone. An orderly
 * removal can leave the device present and busy, so remove() also sets
 * edu->removed, checked first without touching MMIO.
 */
static bool edu_idle(struct edu_dev *edu, u32 *status)
{
	if (READ_ONCE(edu->removed)) {
		*status = U32_MAX;
		return true;
	}
	*status = readl(edu->regs + EDU_REG_STATUS);
	return *status == U32_MAX || !(*status & EDU_STATUS_COMPUTING);
}

/*
 * Sleep until the device is not computing. The factorial IRQ only wakes
 * us; the busy bit decides. The device clears busy before it raises the
 * IRQ, so the IRQ of an abandoned (timed out or interrupted) factorial can
 * arrive after the next one started. Its wakeup then sees busy still set
 * and sleeps again, so it cannot complete the later factorial.
 */
static int edu_wait_idle(struct edu_dev *edu)
{
	u32 status;
	long ret;

	ret = wait_event_interruptible_timeout(edu->fact_wq,
					       edu_idle(edu, &status),
					       EDU_FACT_TIMEOUT);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;
	if (status == U32_MAX)
		return -ENODEV;
	return 0;
}

/* All ones means the device is gone, so it can no longer do DMA. */
static bool edu_dma_busy(struct edu_dev *edu)
{
	u64 cmd = readq(edu->regs + EDU_REG_DMA_CMD);

	return cmd != U64_MAX && (cmd & EDU_DMA_START);
}

/*
 * Sleep until no DMA runs. As for factorial, the DMA IRQ only wakes us and
 * the START bit decides: a late IRQ from an abandoned transfer finds START
 * set by the new one and sleeps again. QEMU clears START before it raises
 * the IRQ. readq() orders this read before later CPU reads of dma_buf.
 */
static int edu_dma_wait(struct edu_dev *edu)
{
	long ret;

	ret = wait_event_interruptible_timeout(edu->dma_wq,
					       READ_ONCE(edu->removed) ||
					       !edu_dma_busy(edu),
					       EDU_DMA_TIMEOUT);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;
	if (READ_ONCE(edu->removed) ||
	    readq(edu->regs + EDU_REG_DMA_CMD) == U64_MAX)
		return -ENODEV;
	return 0;
}

static irqreturn_t edu_irq(int irq, void *data)
{
	struct edu_dev *edu = data;
	u32 pending;

	/* Shared line: zero means not ours, all ones means device gone. */
	pending = readl(edu->regs + EDU_REG_IRQ_STATUS);
	if (!pending || pending == U32_MAX)
		return IRQ_NONE;

	/* Ack every bit so INTx drops, then wake whoever waits for it. */
	writel(pending, edu->regs + EDU_REG_IRQ_ACK);
	if (pending & EDU_IRQ_FACT)
		wake_up(&edu->fact_wq);
	if (pending & EDU_IRQ_DMA)
		wake_up(&edu->dma_wq);
	return IRQ_HANDLED;
}

static int edu_do_liveness(struct edu_dev *edu, u32 in, u32 *out)
{
	writel(in, edu->regs + EDU_REG_LIVENESS);
	*out = readl(edu->regs + EDU_REG_LIVENESS);
	return 0;
}

static int edu_do_factorial(struct edu_dev *edu, u32 n, u32 *out)
{
	int ret;

	if (n > EDU_FACTORIAL_MAX_INPUT)
		return -ERANGE;

	/*
	 * The device ignores writes to the factorial register while busy.
	 * Normally idle at once; only an abandoned factorial makes us sleep.
	 */
	ret = edu_wait_idle(edu);
	if (ret)
		return ret;

	/* The write sets the busy bit synchronously before it returns. */
	writel(n, edu->regs + EDU_REG_FACT);

	ret = edu_wait_idle(edu);
	if (ret)
		return ret;

	*out = readl(edu->regs + EDU_REG_FACT);
	return 0;
}

static int edu_dma_leg(struct edu_dev *edu, u64 src, u64 dst, u32 len,
		       u64 dir)
{
	writeq(src, edu->regs + EDU_REG_DMA_SRC);
	writeq(dst, edu->regs + EDU_REG_DMA_DST);
	writeq(len, edu->regs + EDU_REG_DMA_CNT);
	/* writeq() orders earlier CPU stores to dma_buf before the start. */
	writeq(dir | EDU_DMA_START | EDU_DMA_IRQ, edu->regs + EDU_REG_DMA_CMD);
	return edu_dma_wait(edu);
}

/*
 * RAM -> device buffer, clear dma_buf, device buffer -> RAM. A transfer
 * abandoned on timeout or signal keeps running into dma_buf, which stays
 * allocated until remove() has stopped the engine. The device ignores DMA
 * register writes while START is set, so first wait for it to finish.
 */
static int edu_do_dma(struct edu_dev *edu, struct edu_dma __user *u, u32 len)
{
	int ret;

	ret = edu_dma_wait(edu);
	if (ret)
		return ret;

	if (copy_from_user(edu->dma_buf, u->data, len))
		return -EFAULT;

	ret = edu_dma_leg(edu, edu->dma_addr, EDU_DMA_DEV_ADDR, len, 0);
	if (ret)
		return ret;

	/* So the result can only come from the device. */
	memset(edu->dma_buf, 0, len);

	ret = edu_dma_leg(edu, EDU_DMA_DEV_ADDR, edu->dma_addr, len,
			  EDU_DMA_TO_RAM);
	if (ret)
		return ret;

	if (copy_to_user(u->data, edu->dma_buf, len))
		return -EFAULT;
	return 0;
}

static long edu_ioctl_dma(struct edu_dev *edu, struct edu_dma __user *u)
{
	u32 len;
	int ret;

	if (get_user(len, &u->len))
		return -EFAULT;
	if (!len || len > EDU_DMA_MAX_LEN)
		return -EINVAL;

	ret = mutex_lock_interruptible(&edu->lock);
	if (ret)
		return ret;
	ret = edu->regs ? edu_do_dma(edu, u, len) : -ENODEV;
	mutex_unlock(&edu->lock);
	return ret;
}

static int edu_open(struct inode *inode, struct file *file)
{
	/*
	 * misc_open() set private_data to our miscdevice and calls us under
	 * misc_mtx, which misc_deregister() also takes, so edu is still
	 * alive here and the reference below cannot race with remove.
	 */
	struct edu_dev *edu = container_of(file->private_data,
					   struct edu_dev, misc);

	kref_get(&edu->ref);
	file->private_data = edu;
	return 0;
}

static int edu_release(struct inode *inode, struct file *file)
{
	struct edu_dev *edu = file->private_data;

	kref_put(&edu->ref, edu_free);
	return 0;
}

static long edu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct edu_dev *edu = file->private_data;
	u32 __user *uarg = (u32 __user *)arg;
	u32 in, out = 0;
	int ret;

	if (cmd == EDU_IOC_DMA)
		return edu_ioctl_dma(edu, (struct edu_dma __user *)arg);
	if (cmd != EDU_IOC_LIVENESS && cmd != EDU_IOC_FACTORIAL)
		return -ENOTTY;

	if (get_user(in, uarg))
		return -EFAULT;

	ret = mutex_lock_interruptible(&edu->lock);
	if (ret)
		return ret;

	if (!edu->regs) {
		ret = -ENODEV;
	} else if (cmd == EDU_IOC_LIVENESS) {
		ret = edu_do_liveness(edu, in, &out);
	} else {
		ret = edu_do_factorial(edu, in, &out);
	}

	mutex_unlock(&edu->lock);

	if (ret)
		return ret;
	if (put_user(out, uarg))
		return -EFAULT;
	return 0;
}

static const struct file_operations edu_fops = {
	.owner		= THIS_MODULE,
	.open		= edu_open,
	.release	= edu_release,
	.unlocked_ioctl	= edu_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static int edu_check_hw(struct edu_dev *edu)
{
	struct device *dev = &edu->pdev->dev;
	const u32 probe = 0x5a5aa5a5;
	u32 id, live;

	id = readl(edu->regs + EDU_REG_ID);
	if ((id & EDU_ID_MAGIC_MASK) != EDU_ID_MAGIC) {
		dev_err(dev, "unexpected identification 0x%08x\n", id);
		return -ENODEV;
	}

	writel(probe, edu->regs + EDU_REG_LIVENESS);
	live = readl(edu->regs + EDU_REG_LIVENESS);
	if (live != ~probe) {
		dev_err(dev, "liveness check failed: wrote 0x%08x read 0x%08x\n",
			probe, live);
		return -EIO;
	}

	dev_info(dev, "edu device version %u.%u\n", id >> 24, (id >> 16) & 0xff);
	return 0;
}

static void edu_irq_off(struct edu_dev *edu)
{
	u32 pending;

	/*
	 * Stop factorial IRQs. A factorial that already read IRQFACT can still
	 * raise one (QEMU checks the bit before taking its lock), so mask INTx
	 * in PCI_COMMAND while our handler still runs; a later assertion then
	 * stays latched in the device instead of hitting the shared line. Then
	 * clear what is pending. A bit set after this ack is handled by the
	 * next probe, which unmasks INTx only after request_irq().
	 */
	writel(0, edu->regs + EDU_REG_STATUS);
	pci_intx(edu->pdev, 0);
	free_irq(edu->pdev->irq, edu);
	pending = readl(edu->regs + EDU_REG_IRQ_STATUS);
	if (pending && pending != U32_MAX)
		writel(pending, edu->regs + EDU_REG_IRQ_ACK);
}

/*
 * Stop DMA so dma_buf can be freed. EDU has no abort: a started transfer
 * always runs to completion. Clearing bus master stops new DMA requests;
 * waiting for START to clear (the DMA IRQ wakes us) shows none is left.
 * Returns false if START is still set after the timeout.
 */
static bool edu_dma_quiesce(struct edu_dev *edu)
{
	pci_clear_master(edu->pdev);
	return wait_event_timeout(edu->dma_wq, !edu_dma_busy(edu),
				  EDU_DMA_TIMEOUT);
}

static void edu_dma_free(struct edu_dev *edu)
{
	dma_free_coherent(&edu->pdev->dev, EDU_DMA_MAX_LEN, edu->dma_buf,
			  edu->dma_addr);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct edu_dev *edu;
	int ret;

	edu = kzalloc(sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;
	kref_init(&edu->ref);
	mutex_init(&edu->lock);
	init_waitqueue_head(&edu->fact_wq);
	init_waitqueue_head(&edu->dma_wq);
	edu->pdev = pdev;

	if (atomic_cmpxchg(&edu_bound, 0, 1)) {
		dev_err(&pdev->dev, "only one edu device is supported\n");
		ret = -EBUSY;
		goto err_put;
	}

	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto err_unbind;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 0) < EDU_MMIO_MIN_LEN) {
		dev_err(&pdev->dev, "BAR0 is not a usable MMIO region\n");
		ret = -ENODEV;
		goto err_disable;
	}

	ret = pci_request_region(pdev, 0, DRV_NAME);
	if (ret)
		goto err_disable;

	edu->regs = pci_iomap(pdev, 0, 0);
	if (!edu->regs) {
		ret = -ENOMEM;
		goto err_release;
	}

	ret = edu_check_hw(edu);
	if (ret)
		goto err_unmap;

	ret = dma_set_mask_and_coherent(&pdev->dev,
					DMA_BIT_MASK(EDU_DMA_ADDR_BITS));
	if (ret) {
		dev_err(&pdev->dev, "no usable %d-bit DMA\n", EDU_DMA_ADDR_BITS);
		goto err_unmap;
	}

	/* A transfer left by an earlier owner would hit memory we do not own. */
	if (edu_dma_busy(edu)) {
		dev_err(&pdev->dev, "DMA from an earlier owner still running\n");
		ret = -EBUSY;
		goto err_unmap;
	}

	edu->dma_buf = dma_alloc_coherent(&pdev->dev, EDU_DMA_MAX_LEN,
					  &edu->dma_addr, GFP_KERNEL);
	if (!edu->dma_buf) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	if (!pdev->irq) {
		dev_err(&pdev->dev, "no legacy INTx interrupt assigned\n");
		ret = -ENODEV;
		goto err_dma;
	}

	/*
	 * The handler may run as soon as this returns (shared line); regs and
	 * both wait queues are ready.
	 */
	ret = request_irq(pdev->irq, edu_irq, IRQF_SHARED, DRV_NAME, edu);
	if (ret)
		goto err_dma;

	/*
	 * A previous owner (or our own remove) may have left INTx masked.
	 * Unmask only now that the handler can ack bits left pending.
	 */
	pci_intx(pdev, 1);

	/* Raise an interrupt each time a factorial completes. */
	writel(EDU_STATUS_IRQFACT, edu->regs + EDU_REG_STATUS);

	pci_set_master(pdev);

	edu->misc.minor = MISC_DYNAMIC_MINOR;
	edu->misc.name = "edu0";
	edu->misc.fops = &edu_fops;
	edu->misc.parent = &pdev->dev;
	edu->misc.mode = 0600;

	ret = misc_register(&edu->misc);
	if (ret)
		goto err_irq;

	/* Only after success: remove() is its sole user and must see live state. */
	pci_set_drvdata(pdev, edu);

	dev_info(&pdev->dev, "registered /dev/%s\n", edu->misc.name);
	return 0;

err_irq:
	/* No DMA was started, so nothing can still use dma_buf. */
	pci_clear_master(pdev);
	edu_irq_off(edu);
err_dma:
	edu_dma_free(edu);
err_unmap:
	pci_iounmap(pdev, edu->regs);
err_release:
	pci_release_region(pdev, 0);
err_disable:
	pci_disable_device(pdev);
err_unbind:
	atomic_set(&edu_bound, 0);
err_put:
	kref_put(&edu->ref, edu_free);
	return ret;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *edu = pci_get_drvdata(pdev);
	void __iomem *regs;
	bool dma_stopped;

	/* After this returns no new open() can reach edu. */
	misc_deregister(&edu->misc);

	/*
	 * A removed device may never send the IRQ a waiter sleeps for. Make
	 * the waiter return ENODEV now; regs stays mapped until we hold the
	 * mutex below. wake_up() orders the store before the waiter's check.
	 */
	WRITE_ONCE(edu->removed, true);
	wake_up(&edu->fact_wq);
	wake_up(&edu->dma_wq);

	/*
	 * Wait for any in-flight ioctl, then make later ones fail. DMA is
	 * stopped while the IRQ that signals its end is still ours. The IRQ is
	 * freed while regs is still set because the handler reads it.
	 */
	mutex_lock(&edu->lock);
	dma_stopped = edu_dma_quiesce(edu);
	edu_irq_off(edu);
	regs = edu->regs;
	edu->regs = NULL;
	mutex_unlock(&edu->lock);

	pci_iounmap(pdev, regs);
	pci_release_region(pdev, 0);

	/*
	 * shortcut: EDU has no DMA abort, so an engine still busy after the
	 * timeout leaks this 4 KiB buffer; revisit if the device gains one.
	 */
	if (dma_stopped)
		edu_dma_free(edu);
	else
		dev_warn(&pdev->dev, "DMA did not stop; leaking its buffer\n");

	pci_disable_device(pdev);
	atomic_set(&edu_bound, 0);

	/* Open files keep their own reference; the last close frees edu. */
	kref_put(&edu->ref, edu_free);
}

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
	.name		= DRV_NAME,
	.id_table	= edu_ids,
	.probe		= edu_probe,
	.remove		= edu_remove,
};
module_pci_driver(edu_driver);

MODULE_DESCRIPTION("EduLink driver for the QEMU edu PCI device");
MODULE_LICENSE("GPL");
