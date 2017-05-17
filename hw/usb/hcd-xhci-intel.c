/*
 * USB xHCI controller emulation
 *
 * Copyright (c) 2011 Securiforest
 * Date: 2011-05-11 ;  Author: Hector Martin <hector@marcansoft.com>
 * Based on usb-ohci.c, emulates Renesas NEC USB 3.0
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/usb.h"
#include "hw/pci/pci.h"

#include "hcd-xhci.h"

/*

00:14.0 USB controller [0c03]: Intel Corporation 7 Series/C210 Series \
                Chipset Family USB xHCI Host Controller \
                [8086:1e31] (rev 04) (prog-if 30 [XHCI])
        Subsystem: Lenovo Device [17aa:21f6]
        Flags: bus master, medium devsel, latency 0, IRQ 26
        Memory at f2520000 (64-bit, non-prefetchable) [size=64K]
        Capabilities: [70] Power Management version 2
        Capabilities: [80] MSI: Enable+ Count=1/8 Maskable- 64bit+
        Kernel driver in use: xhci_hcd

 */

#define TYPE_INTEL_XHCI "intel-xhci"

static void intel_xhci_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id    = PCI_VENDOR_ID_INTEL;
    k->device_id    = 0x1e31;
    k->revision     = 0x04;
}

static void intel_xhci_instance_init(Object *obj)
{
    XHCIState *xhci = XHCI(obj);

    xhci->msi      = ON_OFF_AUTO_AUTO;
    xhci->msix     = ON_OFF_AUTO_OFF;
    xhci->numintrs = MAXINTRS;
    xhci->numslots = MAXSLOTS;
    xhci_set_flag(xhci, XHCI_FLAG_SS_FIRST);
    xhci_set_flag(xhci, XHCI_FLAG_ENABLE_STREAMS);
}

static const TypeInfo intel_xhci_info = {
    .name          = TYPE_INTEL_XHCI,
    .parent        = TYPE_XHCI,
    .class_init    = intel_xhci_class_init,
    .instance_init = intel_xhci_instance_init,
};

static void intel_xhci_register_types(void)
{
    type_register_static(&intel_xhci_info);
}

type_init(intel_xhci_register_types);
