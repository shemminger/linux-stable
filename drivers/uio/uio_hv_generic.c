// SPDX-License-Identifier: GPL-2.0
/*
 * uio_hv_generic - generic UIO driver for VMBus
 *
 * Copyright (c) 2013-2016 Brocade Communications Systems, Inc.
 * Copyright (c) 2016, Microsoft Corporation.
 *
 * Since the driver does not declare any device ids, you must allocate
 * id and bind the device to the driver yourself.  For example:
 *
 * Associate Network GUID with UIO device
 * # echo "f8615163-df3e-46c5-913f-f2d2f965ed0e" \
 *    > /sys/bus/vmbus/drivers/uio_hv_generic/new_id
 * Then rebind
 * # echo -n "ed963694-e847-4b2a-85af-bc9cfc11d6f3" \
 *    > /sys/bus/vmbus/drivers/hv_netvsc/unbind
 * # echo -n "ed963694-e847-4b2a-85af-bc9cfc11d6f3" \
 *    > /sys/bus/vmbus/drivers/uio_hv_generic/bind
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uio_driver.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/hyperv.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "../hv/hyperv_vmbus.h"

#define DRIVER_VERSION	"0.02.1"
#define DRIVER_AUTHOR	"Stephen Hemminger <sthemmin at microsoft.com>"
#define DRIVER_DESC	"Generic UIO driver for VMBus devices"

#define HV_RING_SIZE	 512	/* pages */
#define SEND_BUFFER_SIZE (16 * 1024 * 1024)
#define RECV_BUFFER_SIZE (31 * 1024 * 1024)

/*
 * List of resources to be mapped to user space
 * can be extended up to MAX_UIO_MAPS(5) items
 */
enum hv_uio_map {
	TXRX_RING_MAP = 0,
	INT_PAGE_MAP,
	MON_PAGE_MAP,
	RECV_BUF_MAP,
	SEND_BUF_MAP
};

struct hv_uio_private_data {
	struct uio_info info;
	struct hv_device *device;
	atomic_t refcnt;

	void	*recv_buf;
	u32	recv_gpadl;
	char	recv_name[32];	/* "recv_4294967295" */

	void	*send_buf;
	u32	send_gpadl;
	char	send_name[32];
};

/*
 * This is the irqcontrol callback to be registered to uio_info.
 * It can be used to disable/enable interrupt from user space processes.
 *
 * @param info
 *  pointer to uio_info.
 * @param irq_state
 *  state value. 1 to enable interrupt, 0 to disable interrupt.
 */
static int
hv_uio_irqcontrol(struct uio_info *info, s32 irq_state)
{
	struct hv_uio_private_data *pdata = info->priv;
	struct hv_device *dev = pdata->device;

	dev->channel->inbound.ring_buffer->interrupt_mask = !irq_state;
	mb();

	return 0;
}

/*
 * Callback from vmbus_event when something is in inbound ring.
 */
static void hv_uio_channel_cb(void *context)
{
	struct vmbus_channel *chan = context;
	struct hv_device *hv_dev = chan->device_obj;
	struct hv_uio_private_data *pdata = hv_get_drvdata(hv_dev);

	chan->inbound.ring_buffer->interrupt_mask = 1;
	mb();

	uio_event_notify(&pdata->info);
}

/* free the reserved buffers for send and receive */
static void
hv_uio_cleanup(struct hv_device *dev, struct hv_uio_private_data *pdata)
{
	if (pdata->send_gpadl) {
		vmbus_teardown_gpadl(dev->channel, pdata->send_gpadl);
		pdata->send_gpadl = 0;
		vfree(pdata->send_buf);
	}

	if (pdata->recv_gpadl) {
		vmbus_teardown_gpadl(dev->channel, pdata->recv_gpadl);
		pdata->recv_gpadl = 0;
		vfree(pdata->recv_buf);
	}
}

/* VMBus primary channel is opened on first use */
static int
hv_uio_open(struct uio_info *info, struct inode *inode)
{
	struct hv_uio_private_data *pdata
		= container_of(info, struct hv_uio_private_data, info);
	struct hv_device *dev = pdata->device;
	int ret;

	if (atomic_inc_return(&pdata->refcnt) != 1)
		return 0;

	ret = vmbus_connect_ring(dev->channel,
				 hv_uio_channel_cb, dev->channel);
	if (ret == 0) {
		dev->channel->inbound.ring_buffer->interrupt_mask = 1;
		dev->channel->batched_reading = false;
	} else {
		atomic_dec(&pdata->refcnt);
	}

	return ret;
}

/* VMBus primary channel is closed on last close */
static int
hv_uio_release(struct uio_info *info, struct inode *inode)
{
	struct hv_uio_private_data *pdata
		= container_of(info, struct hv_uio_private_data, info);
	struct hv_device *dev = pdata->device;
	int ret = 0;

	if (atomic_dec_and_test(&pdata->refcnt))
		ret = vmbus_disconnect_ring(dev->channel);

	return ret;
}

static int
hv_uio_probe(struct hv_device *dev,
	     const struct hv_vmbus_device_id *dev_id)
{
	struct vmbus_channel *channel = dev->channel;
	struct hv_uio_private_data *pdata;
	int ret;

	/* Communicating with host has to be via shared memory not hypercall */
	if (!channel->offermsg.monitor_allocated) {
		dev_err(&dev->device, "vmbus channel requires hypercall\n");
		return -ENOTSUPP;
	}

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	ret = vmbus_alloc_ring(channel, HV_RING_SIZE * PAGE_SIZE,
			       HV_RING_SIZE * PAGE_SIZE);
	if (ret)
		goto fail;

	/* Fill general uio info */
	pdata->info.name = "uio_hv_generic";
	pdata->info.version = DRIVER_VERSION;
	pdata->info.irqcontrol = hv_uio_irqcontrol;
	pdata->info.open = hv_uio_open;
	pdata->info.release = hv_uio_release;
	pdata->info.irq = UIO_IRQ_CUSTOM;
	atomic_set(&pdata->refcnt, 0);

	/* mem resources */
	pdata->info.mem[TXRX_RING_MAP].name = "txrx_rings";
	pdata->info.mem[TXRX_RING_MAP].addr
		= PFN_PHYS(page_to_pfn(channel->ringbuffer_page));
	pdata->info.mem[TXRX_RING_MAP].size
		= channel->ringbuffer_pagecount << PAGE_SHIFT;
	pdata->info.mem[TXRX_RING_MAP].memtype = UIO_MEM_PHYS;

	pdata->info.mem[INT_PAGE_MAP].name = "int_page";
	pdata->info.mem[INT_PAGE_MAP].addr
		= (uintptr_t)vmbus_connection.int_page;
	pdata->info.mem[INT_PAGE_MAP].size = PAGE_SIZE;
	pdata->info.mem[INT_PAGE_MAP].memtype = UIO_MEM_LOGICAL;

	pdata->info.mem[MON_PAGE_MAP].name = "monitor_page";
	pdata->info.mem[MON_PAGE_MAP].addr
		= (uintptr_t)vmbus_connection.monitor_pages[1];
	pdata->info.mem[MON_PAGE_MAP].size = PAGE_SIZE;
	pdata->info.mem[MON_PAGE_MAP].memtype = UIO_MEM_LOGICAL;

	pdata->recv_buf = vzalloc(RECV_BUFFER_SIZE);
	if (pdata->recv_buf == NULL) {
		ret = -ENOMEM;
		goto fail_close;
	}

	ret = vmbus_establish_gpadl(channel, pdata->recv_buf,
				    RECV_BUFFER_SIZE, &pdata->recv_gpadl);
	if (ret)
		goto fail_close;

	/* put Global Physical Address Label in name */
	snprintf(pdata->recv_name, sizeof(pdata->recv_name),
		 "recv:%u", pdata->recv_gpadl);
	pdata->info.mem[RECV_BUF_MAP].name = pdata->recv_name;
	pdata->info.mem[RECV_BUF_MAP].addr
		= (uintptr_t)pdata->recv_buf;
	pdata->info.mem[RECV_BUF_MAP].size = RECV_BUFFER_SIZE;
	pdata->info.mem[RECV_BUF_MAP].memtype = UIO_MEM_VIRTUAL;

	pdata->send_buf = vzalloc(SEND_BUFFER_SIZE);
	if (pdata->send_buf == NULL) {
		ret = -ENOMEM;
		goto fail_close;
	}

	ret = vmbus_establish_gpadl(channel, pdata->send_buf,
				    SEND_BUFFER_SIZE, &pdata->send_gpadl);
	if (ret)
		goto fail_close;

	snprintf(pdata->send_name, sizeof(pdata->send_name),
		 "send:%u", pdata->send_gpadl);
	pdata->info.mem[SEND_BUF_MAP].name = pdata->send_name;
	pdata->info.mem[SEND_BUF_MAP].addr
		= (uintptr_t)pdata->send_buf;
	pdata->info.mem[SEND_BUF_MAP].size = SEND_BUFFER_SIZE;
	pdata->info.mem[SEND_BUF_MAP].memtype = UIO_MEM_VIRTUAL;

	pdata->info.priv = pdata;
	pdata->device = dev;

	ret = uio_register_device(&dev->device, &pdata->info);
	if (ret) {
		dev_err(&dev->device, "hv_uio register failed\n");
		goto fail_close;
	}

	hv_set_drvdata(dev, pdata);

	return 0;

fail_close:
	hv_uio_cleanup(dev, pdata);
fail:
	kfree(pdata);

	return ret;
}

static int
hv_uio_remove(struct hv_device *dev)
{
	struct hv_uio_private_data *pdata = hv_get_drvdata(dev);

	if (!pdata)
		return 0;

	uio_unregister_device(&pdata->info);
	hv_uio_cleanup(dev, pdata);
	hv_set_drvdata(dev, NULL);

	vmbus_free_ring(dev->channel);
	kfree(pdata);
	return 0;
}

static struct hv_driver hv_uio_drv = {
	.name = "uio_hv_generic",
	.id_table = NULL, /* only dynamic id's */
	.probe = hv_uio_probe,
	.remove = hv_uio_remove,
};

static int __init
hyperv_module_init(void)
{
	return vmbus_driver_register(&hv_uio_drv);
}

static void __exit
hyperv_module_exit(void)
{
	vmbus_driver_unregister(&hv_uio_drv);
}

module_init(hyperv_module_init);
module_exit(hyperv_module_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
