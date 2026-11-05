/** @file

  Copyright (c) 2011 - 2019, Intel Corporaton. All rights reserved.
  Copyright (c) 2020, Arm Limited. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
 **/

#include "DwEmacSnpDxe.h"

#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DmaLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NetLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC
EFI_STATUS
EFIAPI
DriverSupported (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  );

STATIC
EFI_STATUS
EFIAPI
DriverStart (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  );

STATIC
EFI_STATUS
EFIAPI
DriverStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  UINTN                       NumberOfChildren,
  IN  EFI_HANDLE                  *ChildHandleBuffer
  );

STATIC
EFI_DRIVER_BINDING_PROTOCOL mDriverBinding = {
  DriverSupported,
  DriverStart,
  DriverStop,
  0xa,
  NULL,
  NULL
};

STATIC
SIMPLE_NETWORK_DEVICE_PATH PathTemplate = {
  {
    {
      MESSAGING_DEVICE_PATH, MSG_MAC_ADDR_DP,
      {(UINT8)(sizeof (MAC_ADDR_DEVICE_PATH)), (UINT8)((sizeof (MAC_ADDR_DEVICE_PATH)) >> 8)}
    },
    {{ 0 }},
    0
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}
  }
};

STATIC
EFI_STATUS
EFIAPI
DriverSupported (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  )
{
  NON_DISCOVERABLE_DEVICE    *Dev;
  EFI_STATUS                 Status;

  // Connect to the non-discoverable device
  Status = gBS->OpenProtocol (Controller,
                              &gEdkiiNonDiscoverableDeviceProtocolGuid,
                              (VOID **)&Dev,
                              This->DriverBindingHandle,
                              Controller,
                              EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Clean up.
  gBS->CloseProtocol (Controller,
                      &gEdkiiNonDiscoverableDeviceProtocolGuid,
                      This->DriverBindingHandle,
                      Controller);

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
DriverStart (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  )
{
  EFI_STATUS                       Status;
  SIMPLE_NETWORK_DRIVER            *Snp;
  EFI_SIMPLE_NETWORK_MODE          *SnpMode;
  SIMPLE_NETWORK_DEVICE_PATH       *DevicePath;
  UINT64                           DefaultMacAddress;
  EFI_MAC_ADDRESS                  *SwapMacAddressPtr;
  UINTN                            DescriptorSize;
  UINTN                            BufferSize;
  UINTN                            *RxBufferAddr;
  EFI_PHYSICAL_ADDRESS             RxBufferAddrMap;
  EFI_PHYSICAL_ADDRESS             SnpAddress;

  // The GMAC (dwmac-5.20 EQoS) DMA engine is 32-bit: descriptors and the
  // embedded TX/RX buffers must live below 4 GB (BSP sets
  // coherent_dma_mask = DMA_BIT_MASK(32)). The descriptor rings are already
  // DmaAllocateBuffer'd (<2 GB); the SIMPLE_NETWORK_DRIVER instance carries
  // MAC_BUFFER[] inline, so allocate the whole struct below 4 GB too.
  // Cap at 2 GB, not 4 GB: the 0xFFExxxxx-0xFFFFxxxx band below 4 GB is
  // valid DRAM but not reliably reachable on this SoC (page-table access
  // aborts there - see the SmbiosDxe/NonDiscoverable DmaLib fixes).
  SnpAddress = 0x80000000ULL - 1;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  EFI_SIZE_TO_PAGES (sizeof (SIMPLE_NETWORK_DRIVER)),
                  &SnpAddress
                  );
  if (EFI_ERROR (Status)) {
    return EFI_OUT_OF_RESOURCES;
  }
  Snp = (SIMPLE_NETWORK_DRIVER *)(UINTN)SnpAddress;

  DEBUG ((DEBUG_VERBOSE, "DwEmac: DriverStart on handle %p\n", Controller));

  Status = gBS->OpenProtocol (Controller,
                              &gEdkiiNonDiscoverableDeviceProtocolGuid,
                              (VOID **)&Snp->Dev,
                              This->DriverBindingHandle,
                              Controller,
                              EFI_OPEN_PROTOCOL_BY_DRIVER);
  DEBUG ((DEBUG_VERBOSE, "DwEmac: OpenProtocol NonDiscoverable: %r (dev %p)\n", Status, Snp->Dev));

  // Size for descriptor ring (64-byte pitched descriptors, see EQOS_DESC_SIZE)
  DescriptorSize = CONFIG_TX_DESCR_NUM * EQOS_DESC_SIZE;
  // Size for transmit and receive buffer
  BufferSize = ETH_BUFSIZE;

  //
  // EQoS DMAC ring mode addresses descriptors as one contiguous array:
  // descriptor N lives at base + N * 16, and the hardware walks the ring
  // sequentially.  Allocating each descriptor on its own page produced
  // non-contiguous rings (observed: TxdescRing[0]=0x7F7A6000,
  // TxdescRing[1]=0x7F7A8000), so the DMA engine read garbage at base+16,
  // never saw OWN, and never transmitted a frame.  Allocate the whole ring
  // once and point the per-index pointers into it.
  //
  for (int Index = 0; Index < CONFIG_TX_DESCR_NUM; Index++) {
    //DMA TxdescRing allocate buffer and map (one contiguous block)
    if (Index == 0) {
      Status = DmaAllocateBuffer (EfiBootServicesData,
                 EFI_SIZE_TO_PAGES (CONFIG_TX_DESCR_NUM * EQOS_DESC_SIZE),
                 (VOID *)&Snp->MacDriver.TxdescRing[Index]);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a () for TxdescRing: %r\n", __func__, Status));
        return Status;
      }

      Status = DmaMap (MapOperationBusMasterCommonBuffer, Snp->MacDriver.TxdescRing[Index],
                 &DescriptorSize, &Snp->MacDriver.TxdescRingMap[Index].AddrMap, &Snp->MacDriver.TxdescRingMap[Index].Mapping);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a () for TxdescRing: %r\n", __func__, Status));
        return Status;
      }
    } else {
      Snp->MacDriver.TxdescRing[Index] = (DESIGNWARE_HW_DESCRIPTOR *)
        ((UINTN)Snp->MacDriver.TxdescRing[0] + (Index * EQOS_DESC_SIZE));
      Snp->MacDriver.TxdescRingMap[Index].AddrMap = Snp->MacDriver.TxdescRingMap[0].AddrMap +
        (Index * EQOS_DESC_SIZE);
      Snp->MacDriver.TxdescRingMap[Index].Mapping = Snp->MacDriver.TxdescRingMap[0].Mapping;
    }

    // DMA RxdescRing allocte buffer and map (one contiguous block)
    if (Index == 0) {
      Status = DmaAllocateBuffer (EfiBootServicesData,
                 EFI_SIZE_TO_PAGES (CONFIG_RX_DESCR_NUM * EQOS_DESC_SIZE),
                 (VOID *)&Snp->MacDriver.RxdescRing[Index]);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a () for RxdescRing: %r\n", __func__, Status));
        return Status;
      }

      Status = DmaMap (MapOperationBusMasterCommonBuffer, Snp->MacDriver.RxdescRing[Index],
                 &DescriptorSize, &Snp->MacDriver.RxdescRingMap[Index].AddrMap, &Snp->MacDriver.RxdescRingMap[Index].Mapping);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "%a () for RxdescRing: %r\n", __func__, Status));
        return Status;
      }
    } else {
      Snp->MacDriver.RxdescRing[Index] = (DESIGNWARE_HW_DESCRIPTOR *)
        ((UINTN)Snp->MacDriver.RxdescRing[0] + (Index * EQOS_DESC_SIZE));
      Snp->MacDriver.RxdescRingMap[Index].AddrMap = Snp->MacDriver.RxdescRingMap[0].AddrMap +
        (Index * EQOS_DESC_SIZE);
      Snp->MacDriver.RxdescRingMap[Index].Mapping = Snp->MacDriver.RxdescRingMap[0].Mapping;
    }

    //DMA mapping for receive buffer
    RxBufferAddr = (UINTN*)((UINTN)Snp->MacDriver.RxBuffer + (Index * BufferSize));
    Status = DmaMap (MapOperationBusMasterWrite,  (VOID *) RxBufferAddr,
               &BufferSize, &RxBufferAddrMap, &Snp->MacDriver.RxBufNum[Index].Mapping);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a () for Rxbuffer: %r\n", __func__, Status));
      return Status;
    }
    Snp->MacDriver.RxBufNum[Index].AddrMap = RxBufferAddrMap;
  }

  DevicePath = (SIMPLE_NETWORK_DEVICE_PATH*)AllocateCopyPool (sizeof (SIMPLE_NETWORK_DEVICE_PATH), &PathTemplate);
  if (DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  // Initialized signature (used by INSTANCE_FROM_SNP_THIS macro)
  Snp->Signature = SNP_DRIVER_SIGNATURE;

  EfiInitializeLock (&Snp->Lock, TPL_CALLBACK);

  // Initialize pointers
  SnpMode = &Snp->SnpMode;
  Snp->Snp.Mode = SnpMode;

  // Get MAC controller base address
  Snp->MacBase = (UINTN)Snp->Dev->Resources[0].AddrRangeMin;

  // Assign fields and func pointers
  Snp->Snp.Revision = EFI_SIMPLE_NETWORK_PROTOCOL_REVISION;
  Snp->Snp.WaitForPacket = NULL;
  Snp->Snp.Initialize = SnpInitialize;
  Snp->Snp.Start = SnpStart;
  Snp->Snp.Stop = SnpStop;
  Snp->Snp.Reset = SnpReset;
  Snp->Snp.Shutdown = SnpShutdown;
  Snp->Snp.ReceiveFilters = SnpReceiveFilters;
  Snp->Snp.StationAddress = SnpStationAddress;
  Snp->Snp.Statistics = SnpStatistics;
  Snp->Snp.MCastIpToMac = SnpMcastIptoMac;
  Snp->Snp.NvData = SnpNvData;
  Snp->Snp.GetStatus = SnpGetStatus;
  Snp->Snp.Transmit = SnpTransmit;
  Snp->Snp.Receive = SnpReceive;

  Snp->RecycledTxBuf = AllocatePool (sizeof (UINT64) * SNP_TX_BUFFER_INCREASE);
  if (Snp->RecycledTxBuf == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
  }

  Snp->MaxRecycledTxBuf = SNP_TX_BUFFER_INCREASE;
  Snp->RecycledTxBufCount = 0;

  // Start completing simple network mode structure
  SnpMode->State = EfiSimpleNetworkStopped;
  SnpMode->HwAddressSize = NET_ETHER_ADDR_LEN;    // HW address is 6 bytes
  SnpMode->MediaHeaderSize = sizeof (ETHER_HEAD);
  SnpMode->MaxPacketSize = EFI_PAGE_SIZE;         // Preamble + SOF + Ether Frame (with VLAN tag +4bytes)
  SnpMode->NvRamSize = 0;                         // No NVRAM with this device
  SnpMode->NvRamAccessSize = 0;                   // No NVRAM with this device

  // Update network mode information
  SnpMode->ReceiveFilterMask = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST     |
                               EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST   |
                               EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST   |
                               EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS |
                               EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST;

  // We do not intend to receive anything for the time being.
  SnpMode->ReceiveFilterSetting = 0;

  // EMAC has 64bit hash table, can filter 64 MCast MAC Addresses
  SnpMode->MaxMCastFilterCount = MAX_MCAST_FILTER_CNT;
  SnpMode->MCastFilterCount = 0;
  ZeroMem (&SnpMode->MCastFilter, MAX_MCAST_FILTER_CNT * sizeof (EFI_MAC_ADDRESS));

  // Set the interface type (1: Ethernet or 6: IEEE 802 Networks)
  SnpMode->IfType = NET_IFTYPE_ETHERNET;

  // Mac address is changeable as it is loaded from erasable memory
  SnpMode->MacAddressChangeable = TRUE;

  // Can only transmit one packet at a time
  SnpMode->MultipleTxSupported = FALSE;

  // MediaPresent checks for cable connection and partner link
  SnpMode->MediaPresentSupported = TRUE;
  SnpMode->MediaPresent = FALSE;

  // Set broadcast address
  SetMem (&SnpMode->BroadcastAddress, sizeof (EFI_MAC_ADDRESS), 0xFF);

  //Set current address
  DefaultMacAddress = Snp->Dev->Resources[1].AddrRangeMin;
  // Swap PCD human readable form to correct endianess
  SwapMacAddressPtr = (EFI_MAC_ADDRESS *) &DefaultMacAddress;
  SnpMode->CurrentAddress.Addr[0] = SwapMacAddressPtr->Addr[5];
  SnpMode->CurrentAddress.Addr[1] = SwapMacAddressPtr->Addr[4];
  SnpMode->CurrentAddress.Addr[2] = SwapMacAddressPtr->Addr[3];
  SnpMode->CurrentAddress.Addr[3] = SwapMacAddressPtr->Addr[2];
  SnpMode->CurrentAddress.Addr[4] = SwapMacAddressPtr->Addr[1];
  SnpMode->CurrentAddress.Addr[5] = SwapMacAddressPtr->Addr[0];

  // Assign fields for device path
  CopyMem (&DevicePath->MacAddrDP.MacAddress, &Snp->Snp.Mode->CurrentAddress, NET_ETHER_ADDR_LEN);
  DevicePath->MacAddrDP.IfType = Snp->Snp.Mode->IfType;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Controller,
                  &gEfiSimpleNetworkProtocolGuid, &(Snp->Snp),
                  NULL
                  );
  DEBUG ((DEBUG_VERBOSE, "DwEmac: Install SNP on %p: %r (MacBase=0x%lx)\n",
    Controller, Status, Snp->MacBase));

  //
  // The Controller handle already carries a device path (the VenHw path
  // installed by RegisterNonDiscoverableMmioDevice), so installing a second
  // gEfiDevicePathProtocolGuid here would fail with EFI_INVALID_PARAMETER.
  // SNP consumers (MnpDxe and up) bind by protocol, not by device path, so
  // the MAC_ADDR path is not needed on this handle.
  //
  FreePool (DevicePath);

  if (EFI_ERROR(Status)) {
    gBS->CloseProtocol (Controller,
                        &gEdkiiNonDiscoverableDeviceProtocolGuid,
                        This->DriverBindingHandle,
                        Controller);

    FreePages (Snp, EFI_SIZE_TO_PAGES (sizeof (SIMPLE_NETWORK_DRIVER)));
  } else {
    Snp->ControllerHandle = Controller;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
DriverStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  UINTN                       NumberOfChildren,
  IN  EFI_HANDLE                  *ChildHandleBuffer
  )
{
  EFI_STATUS                   Status;
  EFI_SIMPLE_NETWORK_PROTOCOL  *SnpProtocol;
  SIMPLE_NETWORK_DRIVER        *Snp;

  //
  // NIC localisation aid: log every Stop so we can see who unbinds the
  // SNP after a successful boot-time install (2026-09-06 investigation).
  //
  DEBUG ((DEBUG_VERBOSE,
    "DwEmac: DriverBindingStop on controller %p (children %u)\n",
    Controller, (UINT32)NumberOfChildren));

  Status = gBS->HandleProtocol (
                  Controller,
                  &gEfiSimpleNetworkProtocolGuid,
                  (VOID **)&SnpProtocol
                );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a (): HandleProtocol: %r\n", __func__, Status));
    return Status;
  }

  Snp = INSTANCE_FROM_SNP_THIS(SnpProtocol);

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Controller,
                  &gEfiSimpleNetworkProtocolGuid,
                  &Snp->Snp,
                  NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a (): UninstallMultipleProtocolInterfaces: %r\n", __func__, Status));
    return Status;
  }

  FreePool (Snp->RecycledTxBuf);
  FreePages (Snp, EFI_SIZE_TO_PAGES (sizeof (SIMPLE_NETWORK_DRIVER)));

  return Status;
}


/**
   UEFI Driver Entry Point API
   @param  ImageHandle       EFI_HANDLE.
   @param  SystemTable       EFI_SYSTEM_TABLE.
   @return EFI_SUCCESS       Success.
   EFI_DEVICE_ERROR  Fail.
**/

EFI_STATUS
EFIAPI
DwEmacSnpDxeEntry (
  IN  EFI_HANDLE ImageHandle,
  IN  EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS        Status;

  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &mDriverBinding,
             ImageHandle,
             &gSnpComponentName,
             &gSnpComponentName2
             );

  return Status;

}
