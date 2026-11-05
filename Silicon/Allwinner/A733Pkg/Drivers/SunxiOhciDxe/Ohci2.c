/** @file
  EFI_USB2_HC_PROTOCOL implementation and UEFI Driver Model entry for
  SunxiOhciDxe.

  The legacy EFI_USB_HC_PROTOCOL was removed from this EDK2 tree, so this
  driver directly produces gEfiUsb2HcProtocolGuid. The Ohci2* wrappers
  follow the UhciDxe Uhci2* pattern: they adapt the EFI_USB2_HC_PROTOCOL
  signatures onto the internal Ohci* core routines (which take the private
  USB_OHCI_HC_DEV pointer). Like EhciDxe, the driver binds against
  gEfiPciIoProtocolGuid (provided by SunxiNonDiscoverablePciDeviceDxe for
  MMIO-only OHCI controllers).

  Copyright (c) 2013-2016 Intel Corporation.
  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Ohci.h"

#include <IndustryStandard/Pci.h>
#include <IndustryStandard/Acpi.h>
#include <Library/TimerLib.h>

//
// EFI_USB2_HC_PROTOCOL wrapper functions
//
STATIC
EFI_STATUS
EFIAPI
Ohci2GetCapability (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  OUT UINT8                 *MaxSpeed,
  OUT UINT8                 *PortNumber,
  OUT UINT8                 *Is64BitCapable
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_STATUS       Status;

  if ((MaxSpeed == NULL) || (PortNumber == NULL) || (Is64BitCapable == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ohc = USB_OHCI_HC_DEV_FROM_THIS (This);

  *MaxSpeed       = EFI_USB_SPEED_FULL;
  *Is64BitCapable = (UINT8)FALSE;

  Status = OhciGetRootHubNumOfPorts (Ohc, PortNumber);

  //
  // EFI_USB_SPEED_FULL is 0 in the USB2 host controller protocol, so print
  // the name rather than a number that reads like "no speed".
  //
  DEBUG ((
    DEBUG_INFO,
    "Ohci2GetCapability: %d port(s), full speed\n",
    (UINT32)*PortNumber
    ));

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2Reset (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN UINT16                Attributes
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;

  if ((Attributes == EFI_USB_HC_RESET_GLOBAL_WITH_DEBUG) ||
      (Attributes == EFI_USB_HC_RESET_HOST_WITH_DEBUG))
  {
    return EFI_UNSUPPORTED;
  }

  Ohc   = USB_OHCI_HC_DEV_FROM_THIS (This);
  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciReset (Ohc, Attributes);

  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2GetState (
  IN   EFI_USB2_HC_PROTOCOL  *This,
  OUT  EFI_USB_HC_STATE      *State
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_THIS (This);

  return OhciGetState (Ohc, State);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2SetState (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN EFI_USB_HC_STATE      State
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;

  Ohc    = USB_OHCI_HC_DEV_FROM_THIS (This);
  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciSetState (Ohc, State);

  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2ControlTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     EFI_USB_DEVICE_REQUEST              *Request,
  IN     EFI_USB_DATA_DIRECTION              TransferDirection,
  IN OUT VOID                                *Data,
  IN OUT UINTN                               *DataLength,
  IN     UINTN                               TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;
  BOOLEAN          IsSlowDevice;

  Ohc           = USB_OHCI_HC_DEV_FROM_THIS (This);
  IsSlowDevice  = (BOOLEAN)((EFI_USB_SPEED_LOW == DeviceSpeed) ? TRUE : FALSE);

  //
  // OHCI only handles low/full speed devices
  //
  if (DeviceSpeed == EFI_USB_SPEED_HIGH) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciControlTransfer (
             Ohc,
             DeviceAddress,
             IsSlowDevice,
             (UINT8)MaximumPacketLength,
             Request,
             TransferDirection,
             Data,
             DataLength,
             TimeOut,
             TransferResult
             );

  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2BulkTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     UINT8                               DataBuffersNumber,
  IN OUT VOID                                *Data[EFI_USB_MAX_BULK_BUFFER_NUM],
  IN OUT UINTN                               *DataLength,
  IN OUT UINT8                               *DataToggle,
  IN     UINTN                               TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;

  Ohc = USB_OHCI_HC_DEV_FROM_THIS (This);

  //
  // Bulk transfers are not supported on low speed, and OHCI cannot
  // handle high speed devices at all.
  //
  if ((DeviceSpeed == EFI_USB_SPEED_LOW) || (DeviceSpeed == EFI_USB_SPEED_HIGH)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((DataBuffersNumber == 0) || (Data == NULL) || (Data[0] == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciBulkTransfer (
             Ohc,
             DeviceAddress,
             EndPointAddress,
             (UINT8)MaximumPacketLength,
             Data[0],
             DataLength,
             DataToggle,
             TimeOut,
             TransferResult
             );

  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2AsyncInterruptTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     BOOLEAN                             IsNewTransfer,
  IN OUT UINT8                               *DataToggle,
  IN     UINTN                               PollingInterval,
  IN     UINTN                               DataLength,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK     CallBackFunction,
  IN     VOID                                *Context
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;
  BOOLEAN          IsSlowDevice;

  Ohc          = USB_OHCI_HC_DEV_FROM_THIS (This);
  IsSlowDevice = (BOOLEAN)((EFI_USB_SPEED_LOW == DeviceSpeed) ? TRUE : FALSE);

  //
  // OHCI only handles low/full speed devices
  //
  if (DeviceSpeed == EFI_USB_SPEED_HIGH) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciAsyncInterruptTransfer (
             Ohc,
             DeviceAddress,
             EndPointAddress,
             IsSlowDevice,
             (UINT8)MaximumPacketLength,
             IsNewTransfer,
             DataToggle,
             PollingInterval,
             DataLength,
             CallBackFunction,
             Context
             );

  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2SyncInterruptTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN OUT VOID                                *Data,
  IN OUT UINTN                               *DataLength,
  IN OUT UINT8                               *DataToggle,
  IN     UINTN                               TimeOut,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;
  BOOLEAN          IsSlowDevice;

  Ohc          = USB_OHCI_HC_DEV_FROM_THIS (This);
  IsSlowDevice = (BOOLEAN)((EFI_USB_SPEED_LOW == DeviceSpeed) ? TRUE : FALSE);

  //
  // OHCI only handles low/full speed devices
  //
  if (DeviceSpeed == EFI_USB_SPEED_HIGH) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciSyncInterruptTransfer (
             Ohc,
             DeviceAddress,
             EndPointAddress,
             IsSlowDevice,
             (UINT8)MaximumPacketLength,
             Data,
             DataLength,
             DataToggle,
             TimeOut,
             TransferResult
             );

  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2IsochronousTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     UINT8                               DataBuffersNumber,
  IN OUT VOID                                *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN     UINTN                               DataLength,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT    UINT32                              *TransferResult
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2AsyncIsochronousTransfer (
  IN     EFI_USB2_HC_PROTOCOL                *This,
  IN     UINT8                               DeviceAddress,
  IN     UINT8                               EndPointAddress,
  IN     UINT8                               DeviceSpeed,
  IN     UINTN                               MaximumPacketLength,
  IN     UINT8                               DataBuffersNumber,
  IN OUT VOID                                *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN     UINTN                               DataLength,
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK     IsochronousCallBack,
  IN     VOID                                *Context OPTIONAL
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2GetRootHubPortStatus (
  IN   EFI_USB2_HC_PROTOCOL  *This,
  IN   UINT8                 PortNumber,
  OUT  EFI_USB_PORT_STATUS   *PortStatus
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = USB_OHCI_HC_DEV_FROM_THIS (This);

  return OhciGetRootHubPortStatus (Ohc, PortNumber, PortStatus);
}

STATIC
EFI_STATUS
EFIAPI
Ohci2SetRootHubPortFeature (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN UINT8                 PortNumber,
  IN EFI_USB_PORT_FEATURE  PortFeature
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;

  Ohc    = USB_OHCI_HC_DEV_FROM_THIS (This);
  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciSetRootHubPortFeature (Ohc, PortNumber, PortFeature);

  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ohci2ClearRootHubPortFeature (
  IN EFI_USB2_HC_PROTOCOL  *This,
  IN UINT8                 PortNumber,
  IN EFI_USB_PORT_FEATURE  PortFeature
  )
{
  USB_OHCI_HC_DEV  *Ohc;
  EFI_TPL          OldTpl;
  EFI_STATUS       Status;

  Ohc    = USB_OHCI_HC_DEV_FROM_THIS (This);
  OldTpl = gBS->RaiseTPL (OHC_TPL);

  Status = OhciClearRootHubPortFeature (Ohc, PortNumber, PortFeature);

  gBS->RestoreTPL (OldTpl);
  return Status;
}

//
// UEFI Driver Model
//

/**
  Get the MMIO base address of the OHCI controller from BAR0 of the
  (NonDiscoverable) PciIo instance.

**/
STATIC
EFI_STATUS
OhciGetMmioBase (
  IN  EFI_PCI_IO_PROTOCOL  *PciIo,
  OUT UINT32               *Base
  )
{
  EFI_STATUS                         Status;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Resources;

  Status = PciIo->GetBarAttributes (PciIo, 0, NULL, (VOID **)&Resources);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Base = (UINT32)Resources->AddrRangeMin;

  gBS->FreePool (Resources);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SunxiOhciDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  USB_CLASSC           UsbClassCReg;

  //
  // Test whether there is PCI IO Protocol attached on the controller handle.
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint8,
                        PCI_CLASSCODE_OFFSET,
                        sizeof (USB_CLASSC) / sizeof (UINT8),
                        &UsbClassCReg
                        );
  if (EFI_ERROR (Status)) {
    Status = EFI_UNSUPPORTED;
    goto ON_EXIT;
  }

  //
  // Test whether the controller belongs to OHCI type
  //
  if ((UsbClassCReg.BaseCode != PCI_CLASS_SERIAL) ||
      (UsbClassCReg.SubClassCode != PCI_CLASS_SERIAL_USB) ||
      (UsbClassCReg.ProgInterface != PCI_IF_OHCI))
  {
    Status = EFI_UNSUPPORTED;
  }

ON_EXIT:
  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

EFI_STATUS
EFIAPI
SunxiOhciDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS            Status;
  EFI_PCI_IO_PROTOCOL   *PciIo;
  USB_OHCI_HC_DEV       *Ohc;
  VOID                  *Buf;
  EFI_PHYSICAL_ADDRESS  PhyAddr;
  VOID                  *Map;
  UINTN                 Pages;
  UINTN                 Bytes;

  Ohc = NULL;
  Buf = NULL;
  Map = NULL;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Ohc = AllocateZeroPool (sizeof (USB_OHCI_HC_DEV));
  if (Ohc == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CLOSE_PCIIO;
  }

  Ohc->Signature = USB_OHCI_HC_DEV_SIGNATURE;

  Ohc->Usb2Hc.GetCapability            = Ohci2GetCapability;
  Ohc->Usb2Hc.Reset                    = Ohci2Reset;
  Ohc->Usb2Hc.GetState                 = Ohci2GetState;
  Ohc->Usb2Hc.SetState                 = Ohci2SetState;
  Ohc->Usb2Hc.ControlTransfer          = Ohci2ControlTransfer;
  Ohc->Usb2Hc.BulkTransfer             = Ohci2BulkTransfer;
  Ohc->Usb2Hc.AsyncInterruptTransfer   = Ohci2AsyncInterruptTransfer;
  Ohc->Usb2Hc.SyncInterruptTransfer    = Ohci2SyncInterruptTransfer;
  Ohc->Usb2Hc.IsochronousTransfer      = Ohci2IsochronousTransfer;
  Ohc->Usb2Hc.AsyncIsochronousTransfer = Ohci2AsyncIsochronousTransfer;
  Ohc->Usb2Hc.GetRootHubPortStatus     = Ohci2GetRootHubPortStatus;
  Ohc->Usb2Hc.SetRootHubPortFeature    = Ohci2SetRootHubPortFeature;
  Ohc->Usb2Hc.ClearRootHubPortFeature  = Ohci2ClearRootHubPortFeature;
  Ohc->Usb2Hc.MajorRevision            = 0x1;
  Ohc->Usb2Hc.MinorRevision            = 0x1;

  Ohc->PciIo                = PciIo;
  Ohc->InterruptContextList = NULL;
  Ohc->ControllerNameTable  = NULL;
  Ohc->HouseKeeperTimer     = NULL;

  Status = OhciGetMmioBase (PciIo, &Ohc->UsbHcBaseAddress);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "SunxiOhciDxe: failed to get MMIO base: %r\n", Status));
    goto FREE_OHC;
  }

  DEBUG ((
    DEBUG_INFO,
    "SunxiOhciDxe: controller @ 0x%08x, HCIVERSION 0x%04x\n",
    Ohc->UsbHcBaseAddress,
    MmioRead32 (Ohc->UsbHcBaseAddress + HC_REVISION)
    ));

  Ohc->MemPool = UsbHcInitMemPool (PciIo);
  if (Ohc->MemPool == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FREE_OHC;
  }

  //
  // Allocate the HCCA (host controller communication area, 256 bytes).
  // PciIo->AllocateBuffer on a NonCoherent NonDiscoverable device returns
  // uncached memory, so the CommonBuffer mapping is legal and the DMA is
  // identity-mapped (device address == host address on A733).
  //
  Bytes = 4096;
  Pages = EFI_SIZE_TO_PAGES (Bytes);

  Status = PciIo->AllocateBuffer (
                    PciIo,
                    AllocateAnyPages,
                    EfiBootServicesData,
                    Pages,
                    &Buf,
                    0
                    );
  if (EFI_ERROR (Status)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FREE_MEM_POOL;
  }

  Status = PciIo->Map (
                    PciIo,
                    EfiPciIoOperationBusMasterCommonBuffer,
                    Buf,
                    &Bytes,
                    &PhyAddr,
                    &Map
                    );
  if (EFI_ERROR (Status) || (Bytes != 4096)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FREE_MEM_PAGE;
  }

  Ohc->HccaMemoryBlock   = (HCCA_MEMORY_BLOCK *)(UINTN)PhyAddr;
  Ohc->HccaMemoryMapping = Map;
  Ohc->HccaMemoryBuf     = (VOID *)(UINTN)Buf;
  Ohc->HccaMemoryPages   = Pages;

  //
  // Install the USB2 host controller protocol
  //
  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  &gEfiUsb2HcProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Ohc->Usb2Hc
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "SunxiOhciDxe: install protocol error\n"));
    goto FREE_MEM_PAGE;
  }

  //
  // Create event to stop the HC on exit boot services.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  OhcExitBootService,
                  Ohc,
                  &gEfiEventExitBootServicesGuid,
                  &Ohc->ExitBootServiceEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "SunxiOhciDxe: create exit boot event error\n"));
    goto UNINSTALL_USBHC;
  }

  //
  // Set a 10 ms periodic timer to poll the done list / periodic EDs.
  //
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  OhciHouseKeeper,
                  Ohc,
                  &Ohc->HouseKeeperTimer
                  );
  if (EFI_ERROR (Status)) {
    goto UNINSTALL_USBHC;
  }

  Status = OhcInitHC (Ohc);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "SunxiOhciDxe: failed to init OHCI host controller\n"));
    goto UNINSTALL_USBHC;
  }

  Status = gBS->SetTimer (Ohc->HouseKeeperTimer, TimerPeriodic, 10 * 1000 * 10);
  if (EFI_ERROR (Status)) {
    goto UNINSTALL_USBHC;
  }

  //
  // Log the root-port state once.  Nothing hangs off this controller on the
  // Cubie A7A (the hub is on the xHCI side), so this is normally "no
  // connection change" - what the verbose trace wants to show, and what made
  // the CCS/PRSC behaviour during bring-up readable.
  //
  {
    UINT8               NumPorts;
    UINT8               PortIndex;
    EFI_USB_PORT_STATUS PortStatus;

    NumPorts = 0;
    OhciGetRootHubNumOfPorts (Ohc, &NumPorts);
    for (PortIndex = 0; PortIndex < NumPorts; PortIndex++) {
      PortStatus.PortStatus       = 0;
      PortStatus.PortChangeStatus = 0;
      OhciGetRootHubPortStatus (Ohc, PortIndex, &PortStatus);
      DEBUG ((
        DEBUG_INFO,
        "SunxiOhciDxe: port %d status=0x%04x change=0x%04x\n",
        (UINT32)PortIndex,
        (UINT32)PortStatus.PortStatus,
        (UINT32)PortStatus.PortChangeStatus
        ));
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "SunxiOhciDxe: OHCI started @ 0x%08x\n",
    Ohc->UsbHcBaseAddress
    ));

  return EFI_SUCCESS;

UNINSTALL_USBHC:
  gBS->UninstallMultipleProtocolInterfaces (
         Controller,
         &gEfiUsb2HcProtocolGuid,
         &Ohc->Usb2Hc,
         NULL
         );

FREE_MEM_PAGE:
  if (Map != NULL) {
    PciIo->Unmap (PciIo, Map);
  }

  if (Buf != NULL) {
    PciIo->FreeBuffer (PciIo, Pages, Buf);
  }

FREE_MEM_POOL:
  UsbHcFreeMemPool (Ohc->MemPool);

FREE_OHC:
  FreePool (Ohc);

CLOSE_PCIIO:
  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

EFI_STATUS
EFIAPI
SunxiOhciDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_STATUS           Status;
  EFI_USB2_HC_PROTOCOL *Usb2Hc;
  USB_OHCI_HC_DEV      *Ohc;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsb2HcProtocolGuid,
                  (VOID **)&Usb2Hc,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Ohc = USB_OHCI_HC_DEV_FROM_THIS (Usb2Hc);

  Status = gBS->UninstallProtocolInterface (
                  Controller,
                  &gEfiUsb2HcProtocolGuid,
                  &Ohc->Usb2Hc
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  OhciFreeDev (Ohc);

  gBS->CloseProtocol (
         Controller,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  gSunxiOhciDriverBinding = {
  SunxiOhciDriverBindingSupported,
  SunxiOhciDriverBindingStart,
  SunxiOhciDriverBindingStop,
  0x10,
  NULL,
  NULL
};

EFI_STATUS
EFIAPI
SunxiOhciDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &gSunxiOhciDriverBinding,
           ImageHandle,
           &gSunxiOhciComponentName,
           &gSunxiOhciComponentName2
           );
}
