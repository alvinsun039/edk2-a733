/** @file
  SunxiOhciDxe - OHCI USB host controller driver for Allwinner A733
  (sun60iw2). Ports the rk3588 OhciDxe core logic onto the EDK2
  NonDiscoverableDevice + PciIo + EFI_USB2_HC_PROTOCOL infrastructure,
  following the same pattern as MdeModulePkg EhciDxe / UhciDxe.

  The legacy EFI_USB_HC_PROTOCOL has been removed from this EDK2 tree,
  so the driver directly implements EFI_USB2_HC_PROTOCOL. Internal core
  routines keep the rk3588 Ohci* signatures but take the private
  USB_OHCI_HC_DEV pointer instead of the legacy protocol pointer.

  Copyright (c) 2013-2016 Intel Corporation.
  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef _SUNXI_OHCI_H
#define _SUNXI_OHCI_H

#include <Uefi.h>

#include <Protocol/Usb2HostController.h>
#include <Protocol/PciIo.h>

#include <Guid/EventGroup.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>

typedef struct _USB_OHCI_HC_DEV USB_OHCI_HC_DEV;

#include "UsbHcMem.h"
#include "OhciReg.h"
#include "OhciSched.h"
#include "OhciUrb.h"
#include "Descriptor.h"

extern EFI_DRIVER_BINDING_PROTOCOL   gSunxiOhciDriverBinding;
extern EFI_COMPONENT_NAME_PROTOCOL   gSunxiOhciComponentName;
extern EFI_COMPONENT_NAME2_PROTOCOL  gSunxiOhciComponentName2;

#define USB_OHCI_HC_DEV_SIGNATURE  SIGNATURE_32 ('o', 'h', 'c', 'i')

#define OHC_TPL  TPL_NOTIFY

typedef struct _HCCA_MEMORY_BLOCK {
  UINT32    HccaInterruptTable[32];                    // 32-bit Physical Address to ED_DESCRIPTOR
  UINT16    HccaFrameNumber;
  UINT16    HccaPad;
  UINT32    HccaDoneHead;                              // 32-bit Physical Address to TD_DESCRIPTOR
  UINT8     Reserved[116];
} HCCA_MEMORY_BLOCK;

struct _USB_OHCI_HC_DEV {
  UINTN                       Signature;
  EFI_USB2_HC_PROTOCOL        Usb2Hc;
  EFI_PCI_IO_PROTOCOL         *PciIo;
  UINT32                      UsbHcBaseAddress;
  HCCA_MEMORY_BLOCK           *HccaMemoryBlock;
  VOID                        *HccaMemoryBuf;
  VOID                        *HccaMemoryMapping;
  UINTN                       HccaMemoryPages;
  ED_DESCRIPTOR               *IntervalList[6][32];
  INTERRUPT_CONTEXT_ENTRY     *InterruptContextList;
  USBHC_MEM_POOL              *MemPool;

  EFI_EVENT                   HouseKeeperTimer;
  //
  // ExitBootServicesEvent is used to stop the OHC DMA operation
  // after exit boot service.
  //
  EFI_EVENT                   ExitBootServiceEvent;

  EFI_UNICODE_STRING_TABLE    *ControllerNameTable;
};

#define USB_OHCI_HC_DEV_FROM_THIS(a) \
  CR (a, USB_OHCI_HC_DEV, Usb2Hc, USB_OHCI_HC_DEV_SIGNATURE)

//
// Internal core routines (rk3588 OhciDxe logic, private-data based)
//

/**
  Reset the OHCI host controller.

**/
EFI_STATUS
OhciReset (
  IN USB_OHCI_HC_DEV  *Ohc,
  IN UINT16           Attributes
  );

/**
  Retrieve the current state of the USB host controller.

**/
EFI_STATUS
OhciGetState (
  IN  USB_OHCI_HC_DEV  *Ohc,
  OUT EFI_USB_HC_STATE *State
  );

/**
  Sets the USB host controller to a specific state.

**/
EFI_STATUS
OhciSetState (
  IN USB_OHCI_HC_DEV  *Ohc,
  IN EFI_USB_HC_STATE State
  );

/**

  Submits control transfer to a target USB device.

**/
EFI_STATUS
OhciControlTransfer (
  IN     USB_OHCI_HC_DEV      *Ohc,
  IN     UINT8                DeviceAddress,
  IN     BOOLEAN              IsSlowDevice,
  IN     UINT8                MaxPacketLength,
  IN     EFI_USB_DEVICE_REQUEST *Request,
  IN     EFI_USB_DATA_DIRECTION TransferDirection,
  IN OUT VOID                 *Data                 OPTIONAL,
  IN OUT UINTN                *DataLength           OPTIONAL,
  IN     UINTN                TimeOut,
  OUT    UINT32               *TransferResult
  );

/**

  Submits bulk transfer to a bulk endpoint of a USB device.

**/
EFI_STATUS
OhciBulkTransfer (
  IN     USB_OHCI_HC_DEV  *Ohc,
  IN     UINT8            DeviceAddress,
  IN     UINT8            EndPointAddress,
  IN     UINT8            MaxPacketLength,
  IN OUT VOID             *Data,
  IN OUT UINTN            *DataLength,
  IN OUT UINT8            *DataToggle,
  IN     UINTN            TimeOut,
  OUT    UINT32           *TransferResult
  );

/**

  Submits an interrupt transfer to an interrupt endpoint of a USB device.

**/
EFI_STATUS
OhciInterruptTransfer (
  IN     USB_OHCI_HC_DEV                 *Ohc,
  IN     UINT8                           DeviceAddress,
  IN     UINT8                           EndPointAddress,
  IN     BOOLEAN                         IsSlowDevice,
  IN     UINT8                           MaxPacketLength,
  IN     BOOLEAN                         IsNewTransfer,
  IN OUT UINT8                           *DataToggle        OPTIONAL,
  IN     UINTN                           PollingInterval    OPTIONAL,
  IN     VOID                            *UCBuffer          OPTIONAL,
  IN     UINTN                           DataLength         OPTIONAL,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK CallBackFunction   OPTIONAL,
  IN     VOID                            *Context           OPTIONAL,
  IN     BOOLEAN                         IsPeriodic         OPTIONAL,
  OUT    ED_DESCRIPTOR                   **OutputED         OPTIONAL,
  OUT    TD_DESCRIPTOR                   **OutputTD         OPTIONAL
  );

/**

  Submits an asynchronous interrupt transfer to an interrupt endpoint of
  a USB device.

**/
EFI_STATUS
OhciAsyncInterruptTransfer (
  IN     USB_OHCI_HC_DEV                 *Ohc,
  IN     UINT8                           DeviceAddress,
  IN     UINT8                           EndPointAddress,
  IN     BOOLEAN                         IsSlowDevice,
  IN     UINT8                           MaxPacketLength,
  IN     BOOLEAN                         IsNewTransfer,
  IN OUT UINT8                           *DataToggle        OPTIONAL,
  IN     UINTN                           PollingInterval    OPTIONAL,
  IN     UINTN                           DataLength         OPTIONAL,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK CallBackFunction   OPTIONAL,
  IN     VOID                            *Context           OPTIONAL
  );

/**

  Submits synchronous interrupt transfer to an interrupt endpoint
  of a USB device.

**/
EFI_STATUS
OhciSyncInterruptTransfer (
  IN     USB_OHCI_HC_DEV  *Ohc,
  IN     UINT8            DeviceAddress,
  IN     UINT8            EndPointAddress,
  IN     BOOLEAN          IsSlowDevice,
  IN     UINT8            MaxPacketLength,
  IN OUT VOID             *Data,
  IN OUT UINTN            *DataLength,
  IN OUT UINT8            *DataToggle,
  IN     UINTN            TimeOut,
  OUT    UINT32           *TransferResult
  );

/**

  Retrieves the number of root hub ports.

**/
EFI_STATUS
OhciGetRootHubNumOfPorts (
  IN  USB_OHCI_HC_DEV  *Ohc,
  OUT UINT8            *NumOfPorts
  );

/**

  Retrieves the current status of a USB root hub port.

**/
EFI_STATUS
OhciGetRootHubPortStatus (
  IN  USB_OHCI_HC_DEV     *Ohc,
  IN  UINT8               PortNumber,
  OUT EFI_USB_PORT_STATUS *PortStatus
  );

/**

  Sets a feature for the specified root hub port.

**/
EFI_STATUS
OhciSetRootHubPortFeature (
  IN USB_OHCI_HC_DEV     *Ohc,
  IN UINT8               PortNumber,
  IN EFI_USB_PORT_FEATURE PortFeature
  );

/**

  Clears a feature for the specified root hub port.

**/
EFI_STATUS
OhciClearRootHubPortFeature (
  IN USB_OHCI_HC_DEV     *Ohc,
  IN UINT8               PortNumber,
  IN EFI_USB_PORT_FEATURE PortFeature
  );

/**
  One notified function to stop the Host Controller when
  gBS->ExitBootServices() is called.
**/
VOID
EFIAPI
OhcExitBootService (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  );

/**
  Initialize the OHCI host controller hardware.
**/
EFI_STATUS
OhcInitHC (
  IN USB_OHCI_HC_DEV  *Ohc
  );

/**
  Free the OHCI device and release its associated resources.
**/
VOID
OhciFreeDev (
  IN USB_OHCI_HC_DEV  *Ohc
  );

#endif
