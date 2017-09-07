// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hypervisor/bits.h>
#include <hypervisor/vcpu.h>
#include <hypervisor/virtio.h>
#include <magenta/syscalls/port.h>
#include <mxtl/unique_ptr.h>

#include <virtio/virtio.h>
#include <virtio/virtio_ring.h>

// clang-format off

/* PCI macros. */
#define PCI_VENDOR_ID_VIRTIO            0x1af4u

// clang-format on

static constexpr uint16_t virtio_pci_legacy_id(uint16_t virtio_id) {
    return static_cast<uint16_t>(virtio_id + 0xfffu);
}

static virtio_device_t* pci_device_to_virtio(const pci_device_t* device) {
    return static_cast<virtio_device_t*>(device->impl);
}

static virtio_queue_t* selected_queue(const virtio_device_t* device) {
    return device->queue_sel < device->num_queues ? &device->queues[device->queue_sel] : nullptr;
}

// Virtio 1.0 Section 4.1.5.1.3:
//
// When using the legacy interface, the queue layout follows 2.4.2 Legacy
// Interfaces: A Note on Virtqueue Layout with an alignment of 4096. Driver
// writes the physical address, divided by 4096 to the Queue Address field 2.
static mx_status_t virtio_queue_set_pfn(virtio_queue_t* queue, uint32_t pfn) {
    uintptr_t desc_paddr = pfn * PAGE_SIZE;
    uintptr_t desc_size = queue->size * sizeof(queue->desc[0]);
    virtio_queue_set_desc_addr(queue, desc_paddr);

    uintptr_t avail_paddr = desc_paddr + desc_size;
    uintptr_t avail_size = sizeof(*queue->avail)
        + (queue->size * sizeof(queue->avail->ring[0]))
        + sizeof(*queue->used_event);
    virtio_queue_set_avail_addr(queue, avail_paddr);

    uintptr_t used_paddr = align(avail_paddr + avail_size, 4096);
    virtio_queue_set_used_addr(queue, used_paddr);

    return MX_OK;
}


static mx_status_t virtio_pci_legacy_read(const pci_device_t* pci_device, uint8_t bar,
                                          uint16_t port, uint8_t access_size,
                                          mx_vcpu_io_t* vcpu_io) {
    if (bar != 0)
        return MX_ERR_NOT_SUPPORTED;

    virtio_device_t* device = pci_device_to_virtio(pci_device);
    const virtio_queue_t* queue = selected_queue(device);
    switch (port) {
    case VIRTIO_PCI_DEVICE_FEATURES:
        vcpu_io->access_size = 4;
        vcpu_io->u32 = device->features;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_PFN:
        if (!queue)
            return MX_ERR_NOT_SUPPORTED;
        vcpu_io->access_size = 4;
        vcpu_io->u32 = static_cast<uint32_t>(queue->addr.desc / PAGE_SIZE);
        return MX_OK;
    case VIRTIO_PCI_QUEUE_SIZE:
        if (!queue)
            return MX_ERR_NOT_SUPPORTED;
        vcpu_io->access_size = 2;
        vcpu_io->u16 = queue->size;
        return MX_OK;
    case VIRTIO_PCI_DEVICE_STATUS:
        vcpu_io->access_size = 1;
        vcpu_io->u8 = device->status;
        return MX_OK;
    case VIRTIO_PCI_ISR_STATUS:
        vcpu_io->access_size = 1;
        mtx_lock(&device->mutex);
        vcpu_io->u8 = device->isr_status;

        // From VIRTIO 1.0 Section 4.1.4.5:
        //
        // To avoid an extra access, simply reading this register resets it to
        // 0 and causes the device to de-assert the interrupt.
        device->isr_status = 0;
        mtx_unlock(&device->mutex);
        return MX_OK;
    }

    // Handle device-specific accesses.
    if (port >= VIRTIO_PCI_DEVICE_CFG_BASE) {
        uint16_t device_offset = static_cast<uint16_t>(port - VIRTIO_PCI_DEVICE_CFG_BASE);
        return device->ops->read(device, device_offset, access_size, vcpu_io);
    }

    fprintf(stderr, "Unhandled virtio device read %#x\n", port);
    return MX_ERR_NOT_SUPPORTED;
}

static mx_status_t virtio_pci_legacy_write(pci_device_t* pci_device, uint8_t bar, uint16_t port,
                                           const mx_packet_guest_io_t* io) {
    if (bar != 0)
        return MX_ERR_NOT_SUPPORTED;

    virtio_device_t* device = pci_device_to_virtio(pci_device);
    virtio_queue_t* queue = selected_queue(device);
    switch (port) {
    case VIRTIO_PCI_DRIVER_FEATURES:
        if (io->access_size != 4)
            return MX_ERR_IO_DATA_INTEGRITY;
        // Currently we expect the driver to accept all our features.
        if (io->u32 != device->features)
            return MX_ERR_INVALID_ARGS;
        return MX_OK;
    case VIRTIO_PCI_DEVICE_STATUS:
        if (io->access_size != 1)
            return MX_ERR_IO_DATA_INTEGRITY;
        device->status = io->u8;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_PFN: {
        if (io->access_size != 4)
            return MX_ERR_IO_DATA_INTEGRITY;
        if (!queue)
            return MX_ERR_NOT_SUPPORTED;
        return virtio_queue_set_pfn(queue, io->u32);
    }
    case VIRTIO_PCI_QUEUE_SIZE:
        if (io->access_size != 2)
            return MX_ERR_IO_DATA_INTEGRITY;
        queue->size = io->u16;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_SELECT:
        if (io->access_size != 2)
            return MX_ERR_IO_DATA_INTEGRITY;
        if (io->u16 >= device->num_queues) {
            fprintf(stderr, "Selected queue does not exist.\n");
            return MX_ERR_NOT_SUPPORTED;
        }
        device->queue_sel = io->u16;
        return MX_OK;
    case VIRTIO_PCI_QUEUE_NOTIFY: {
        if (io->access_size != 2)
            return MX_ERR_IO_DATA_INTEGRITY;
        if (io->u16 >= device->num_queues) {
            fprintf(stderr, "Notify queue does not exist.\n");
            return MX_ERR_NOT_SUPPORTED;
        }

        // Invoke the device callback if one has been provided.
        uint16_t queue_sel = io->u16;
        if (device->ops->queue_notify != NULL) {
            mx_status_t status = device->ops->queue_notify(device, queue_sel);
            if (status != MX_OK) {
                fprintf(stderr, "Failed to handle queue notify event. Error %d\n", status);
                return status;
            }

            // Send an interrupt back to the guest if we've generated one while
            // processing the queue.
            if (device->isr_status > 0) {
                return pci_interrupt(&device->pci_device);
            }
        }

        // Notify threads waiting on a descriptor.
        virtio_queue_signal(&device->queues[queue_sel]);
        return MX_OK;
    }
    }

    // Handle device-specific accesses.
    if (port >= VIRTIO_PCI_DEVICE_CFG_BASE) {
        uint16_t device_offset = static_cast<uint16_t>(port - VIRTIO_PCI_DEVICE_CFG_BASE);
        return device->ops->write(device, device_offset, io);
    }

    fprintf(stderr, "Unhandled virtio device write %#x\n", port);
    return MX_ERR_NOT_SUPPORTED;
}

static const pci_device_ops_t kVirtioLegacyPciDeviceOps = {
    .read_bar = &virtio_pci_legacy_read,
    .write_bar = &virtio_pci_legacy_write,
};

void virtio_pci_init(virtio_device_t* device) {
    device->pci_device.vendor_id = PCI_VENDOR_ID_VIRTIO;
    device->pci_device.device_id = virtio_pci_legacy_id(device->device_id);
    device->pci_device.subsystem_vendor_id = 0;
    device->pci_device.subsystem_id = device->device_id;
    device->pci_device.class_code = 0;
    device->pci_device.bar[0].size = static_cast<uint16_t>(
        sizeof(virtio_pci_legacy_config_t) + device->config_size);
    device->pci_device.impl = device;
    device->pci_device.ops = &kVirtioLegacyPciDeviceOps;
}