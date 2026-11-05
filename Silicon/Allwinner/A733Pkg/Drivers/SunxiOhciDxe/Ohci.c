/** @file
  This file contains the implementation of the internal OHCI core
  transfer routines.

  Ported from rk3588 OhciDxe Ohci.c. Legacy EFI_USB_HC_PROTOCOL entry
  points are converted to internal routines taking the USB_OHCI_HC_DEV
  private-data pointer directly; DmaLib calls are replaced with PciIo
  equivalents (identity-mapped non-coherent DMA on A733).

  Copyright (c) 2013-2016 Intel Corporation.
  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "Ohci.h"

/**
  Provides software reset for the USB host controller.

**/
EFI_STATUS
OhciReset (
  IN USB_OHCI_HC_DEV  *Ohc,
  IN UINT16           Attributes
  )
{
  EFI_STATUS  Status;
  UINT8       Index;
  UINT8       NumOfPorts;
  UINT32      PowerOnGoodTime;
  UINT32      Data32;
  BOOLEAN     Flag = FALSE;

  if ((Attributes & ~(EFI_USB_HC_RESET_GLOBAL | EFI_USB_HC_RESET_HOST_CONTROLLER)) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EFI_SUCCESS;

  if ((Attributes & EFI_USB_HC_RESET_HOST_CONTROLLER) != 0) {
    gBS->Stall (50 * 1000);
    Status = OhciSetHcCommandStatus (Ohc, HC_RESET, HC_RESET);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    gBS->Stall (50 * 1000);
    //
    // Wait for host controller reset.
    //
    PowerOnGoodTime = 50;
    do {
      gBS->Stall (1 * 1000);
      Data32 = OhciGetOperationalReg (Ohc, HC_COMMAND_STATUS);
      if (EFI_ERROR (Status)) {
        return EFI_DEVICE_ERROR;
      }

      if ((Data32 & HC_RESET) == 0) {
        Flag = TRUE;
        break;
      }
    } while (PowerOnGoodTime--);

    if (!Flag) {
      return EFI_DEVICE_ERROR;
    }
  }

  OhciFreeIntTransferMemory (Ohc);
  Status = OhciInitializeInterruptList (Ohc);
  OhciSetFrameInterval (Ohc, FRAME_INTERVAL, 0x2edf);
  if ((Attributes & EFI_USB_HC_RESET_GLOBAL) != 0) {
    Status = OhciSetHcControl (Ohc, HC_FUNCTIONAL_STATE, HC_STATE_RESET);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    gBS->Stall (50 * 1000);
  }

  //
  // Initialize host controller operational registers
  //
  OhciSetFrameInterval (Ohc, FS_LARGEST_DATA_PACKET, 0x2778);
  OhciSetFrameInterval (Ohc, FRAME_INTERVAL, 0x2edf);
  OhciSetPeriodicStart (Ohc, 0x2a2f);
  OhciSetHcControl (Ohc, CONTROL_BULK_RATIO, 0x3);
  OhciSetHcCommandStatus (Ohc, CONTROL_LIST_FILLED | BULK_LIST_FILLED, 0);
  OhciSetRootHubDescriptor (Ohc, RH_PSWITCH_MODE, 0);
  OhciSetRootHubDescriptor (Ohc, RH_NO_PSWITCH | RH_NOC_PROT, 1);

  OhciSetRootHubDescriptor (Ohc, RH_DEV_REMOVABLE, 0);
  OhciSetRootHubDescriptor (Ohc, RH_PORT_PWR_CTRL_MASK, 0xffff);
  OhciSetRootHubStatus (Ohc, RH_LOCAL_PSTAT_CHANGE);
  OhciSetRootHubPortStatus (Ohc, 0, RH_SET_PORT_POWER);
  OhciGetRootHubNumOfPorts (Ohc, &NumOfPorts);
  for (Index = 0; Index < NumOfPorts; Index++) {
    if (!EFI_ERROR (OhciSetRootHubPortFeature (Ohc, Index, EfiUsbPortReset))) {
      gBS->Stall (200 * 1000);
      OhciClearRootHubPortFeature (Ohc, Index, EfiUsbPortReset);
      gBS->Stall (1000);
      OhciSetRootHubPortFeature (Ohc, Index, EfiUsbPortEnable);
      gBS->Stall (1000);
    }
  }

  OhciSetMemoryPointer (Ohc, HC_HCCA, Ohc->HccaMemoryBlock);
  OhciSetMemoryPointer (Ohc, HC_CONTROL_HEAD, NULL);
  OhciSetMemoryPointer (Ohc, HC_BULK_HEAD, NULL);
  OhciSetHcControl (Ohc, PERIODIC_ENABLE | CONTROL_ENABLE | BULK_ENABLE, 1); /*ISOCHRONOUS_ENABLE*/
  OhciSetHcControl (Ohc, HC_FUNCTIONAL_STATE, HC_STATE_OPERATIONAL);
  gBS->Stall (50 * 1000);
  //
  // Wait till first SOF occurs, and then clear it
  //
  while (OhciGetHcInterruptStatus (Ohc, START_OF_FRAME) == 0) {
  }

  OhciClearInterruptStatus (Ohc, START_OF_FRAME);
  gBS->Stall (1000);

  return Status;
}

/**
  Retrieve the current state of the USB host controller.

**/
EFI_STATUS
OhciGetState (
  IN  USB_OHCI_HC_DEV  *Ohc,
  OUT EFI_USB_HC_STATE *State
  )
{
  UINT32  FuncState;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  FuncState = OhciGetHcControl (Ohc, HC_FUNCTIONAL_STATE);

  switch (FuncState) {
    case HC_STATE_RESET:
    case HC_STATE_RESUME:
      *State = EfiUsbHcStateHalt;
      break;

    case HC_STATE_OPERATIONAL:
      *State = EfiUsbHcStateOperational;
      break;

    case HC_STATE_SUSPEND:
      *State = EfiUsbHcStateSuspend;
      break;

    default:
      ASSERT (FALSE);
  }

  return EFI_SUCCESS;
}

/**
  Sets the USB host controller to a specific state.

**/
EFI_STATUS
OhciSetState (
  IN USB_OHCI_HC_DEV  *Ohc,
  IN EFI_USB_HC_STATE State
  )
{
  EFI_STATUS  Status;

  switch (State) {
    case EfiUsbHcStateHalt:
      Status = OhciSetHcControl (Ohc, HC_FUNCTIONAL_STATE, HC_STATE_RESET);
      break;

    case EfiUsbHcStateOperational:
      Status = OhciSetHcControl (Ohc, HC_FUNCTIONAL_STATE, HC_STATE_OPERATIONAL);
      break;

    case EfiUsbHcStateSuspend:
      Status = OhciSetHcControl (Ohc, HC_FUNCTIONAL_STATE, HC_STATE_SUSPEND);
      break;

    default:
      Status = EFI_INVALID_PARAMETER;
  }

  gBS->Stall (1000);

  return Status;
}

/**

  Submits control transfer to a target USB device.

**/
EFI_STATUS
OhciControlTransfer (
  IN     USB_OHCI_HC_DEV          *Ohc,
  IN     UINT8                    DeviceAddress,
  IN     BOOLEAN                  IsSlowDevice,
  IN     UINT8                    MaxPacketLength,
  IN     EFI_USB_DEVICE_REQUEST   *Request,
  IN     EFI_USB_DATA_DIRECTION   TransferDirection,
  IN OUT VOID                     *Data                 OPTIONAL,
  IN OUT UINTN                    *DataLength           OPTIONAL,
  IN     UINTN                    TimeOut,
  OUT    UINT32                   *TransferResult
  )
{
  ED_DESCRIPTOR  *HeadEd;
  ED_DESCRIPTOR  *Ed;
  TD_DESCRIPTOR  *HeadTd;
  TD_DESCRIPTOR  *SetupTd;
  TD_DESCRIPTOR  *DataTd;
  TD_DESCRIPTOR  *StatusTd;
  TD_DESCRIPTOR  *EmptyTd;
  EFI_STATUS     Status;
  UINT32         DataPidDir;
  UINT32         StatusPidDir;
  UINTN          TimeCount;
  OHCI_ED_RESULT EdResult;

  EFI_PCI_IO_PROTOCOL_OPERATION  MapOp;

  UINTN                 ActualSendLength;
  UINTN                 LeftLength;
  UINT8                 DataToggle;

  VOID                  *ReqMapping   = NULL;
  UINTN                 ReqMapLength  = 0;
  EFI_PHYSICAL_ADDRESS  ReqMapPhyAddr = 0;

  VOID                  *DataMapping   = NULL;
  UINTN                 DataMapLength  = 0;
  EFI_PHYSICAL_ADDRESS  DataMapPhyAddr = 0;

  HeadTd = NULL;
  DataTd = NULL;

  if (((TransferDirection != EfiUsbDataOut) && (TransferDirection != EfiUsbDataIn) &&
       (TransferDirection != EfiUsbNoData)) ||
      (Request == NULL) || (DataLength == NULL) || (TransferResult == NULL) ||
      ((TransferDirection == EfiUsbNoData) && ((*DataLength != 0) || (Data != NULL))) ||
      ((TransferDirection != EfiUsbNoData) && ((*DataLength == 0) || (Data == NULL))) ||
      (IsSlowDevice && (MaxPacketLength != 8)) ||
      ((MaxPacketLength != 8) && (MaxPacketLength != 16) &&
       (MaxPacketLength != 32) && (MaxPacketLength != 64)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (*DataLength > MAX_BYTES_PER_TD) {
    DEBUG ((DEBUG_ERROR, "OhciControlTransfer: Request data size is too large\r\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (TransferDirection == EfiUsbDataIn) {
    DataPidDir   = TD_IN_PID;
    StatusPidDir = TD_OUT_PID;
  } else {
    DataPidDir   = TD_OUT_PID;
    StatusPidDir = TD_IN_PID;
  }

  Status = OhciSetHcControl (Ohc, CONTROL_ENABLE, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OhciControlTransfer: fail to disable CONTROL_ENABLE\r\n"));
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_DEVICE_ERROR;
  }

  Status = OhciSetHcCommandStatus (Ohc, CONTROL_LIST_FILLED, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OhciControlTransfer: fail to disable CONTROL_LIST_FILLED\r\n"));
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_DEVICE_ERROR;
  }

  gBS->Stall (20 * 1000);

  OhciSetMemoryPointer (Ohc, HC_CONTROL_HEAD, NULL);
  Ed = OhciCreateED (Ohc);
  if (Ed == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_INFO, "OhciControlTransfer: Fail to allocate ED buffer\r\n"));
    goto CTRL_EXIT;
  }

  OhciSetEDField (Ed, ED_SKIP, 1);
  OhciSetEDField (Ed, ED_FUNC_ADD, DeviceAddress);
  OhciSetEDField (Ed, ED_ENDPT_NUM, 0);
  OhciSetEDField (Ed, ED_DIR, ED_FROM_TD_DIR);
  OhciSetEDField (Ed, ED_SPEED, IsSlowDevice);
  OhciSetEDField (Ed, ED_FORMAT | ED_HALTED | ED_DTTOGGLE, 0);
  OhciSetEDField (Ed, ED_MAX_PACKET, MaxPacketLength);
  OhciSetEDField (Ed, ED_PDATA, 0);
  OhciSetEDField (Ed, ED_ZERO, 0);
  OhciSetEDField (Ed, ED_TDHEAD_PTR, 0);
  OhciSetEDField (Ed, ED_TDTAIL_PTR, 0);
  OhciSetEDField (Ed, ED_NEXT_EDPTR, 0);
  HeadEd = OhciAttachEDToList (Ohc, CONTROL_LIST, Ed, NULL);
  //
  // Setup Stage
  //
  if (Request != NULL) {
    ReqMapLength = sizeof (EFI_USB_DEVICE_REQUEST);
    MapOp        = EfiPciIoOperationBusMasterRead;
    Status       = Ohc->PciIo->Map (
                                Ohc->PciIo,
                                MapOp,
                                (UINT8 *)Request,
                                &ReqMapLength,
                                &ReqMapPhyAddr,
                                &ReqMapping
                                );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "OhciControlTransfer: Fail to Map Request Buffer\r\n"));
      goto FREE_ED_BUFF;
    }
  }

  SetupTd = OhciCreateTD (Ohc);
  if (SetupTd == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_INFO, "OhciControlTransfer: Fail to allocate Setup TD buffer\r\n"));
    goto UNMAP_SETUP_BUFF;
  }

  HeadTd = SetupTd;
  OhciSetTDField (SetupTd, TD_PDATA, 0);
  OhciSetTDField (SetupTd, TD_BUFFER_ROUND, 1);
  OhciSetTDField (SetupTd, TD_DIR_PID, TD_SETUP_PID);
  OhciSetTDField (SetupTd, TD_DELAY_INT, TD_NO_DELAY);
  OhciSetTDField (SetupTd, TD_DT_TOGGLE, 2);
  OhciSetTDField (SetupTd, TD_ERROR_CNT, 0);
  OhciSetTDField (SetupTd, TD_COND_CODE, TD_TOBE_PROCESSED);
  OhciSetTDField (SetupTd, TD_CURR_BUFFER_PTR, (UINT32)ReqMapPhyAddr);
  OhciSetTDField (SetupTd, TD_NEXT_PTR, 0);
  OhciSetTDField (SetupTd, TD_BUFFER_END_PTR, (UINT32)(ReqMapPhyAddr + sizeof (EFI_USB_DEVICE_REQUEST) - 1));
  SetupTd->ActualSendLength = sizeof (EFI_USB_DEVICE_REQUEST);
  SetupTd->DataBuffer       = (UINT32)ReqMapPhyAddr;
  SetupTd->NextTDPointer    = 0;

  if (TransferDirection == EfiUsbDataIn) {
    MapOp = EfiPciIoOperationBusMasterWrite;
  } else {
    MapOp = EfiPciIoOperationBusMasterRead;
  }

  DataMapLength = *DataLength;
  if ((Data != NULL) && (DataMapLength != 0)) {
    Status = Ohc->PciIo->Map (
                            Ohc->PciIo,
                            MapOp,
                            Data,
                            &DataMapLength,
                            &DataMapPhyAddr,
                            &DataMapping
                            );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "OhciControlTransfer: Fail To Map Data Buffer\r\n"));
      goto FREE_TD_BUFF;
    }
  }

  //
  // Data Stage
  //
  LeftLength       = DataMapLength;
  ActualSendLength = DataMapLength;
  DataToggle       = 1;
  while (LeftLength > 0) {
    ActualSendLength = LeftLength;
    if (LeftLength > MaxPacketLength) {
      ActualSendLength = MaxPacketLength;
    }

    DataTd = OhciCreateTD (Ohc);
    if (DataTd == NULL) {
      DEBUG ((DEBUG_INFO, "OhciControlTransfer: Fail to allocate buffer for Data Stage TD\r\n"));
      Status = EFI_OUT_OF_RESOURCES;
      goto UNMAP_DATA_BUFF;
    }

    OhciSetTDField (DataTd, TD_PDATA, 0);
    OhciSetTDField (DataTd, TD_BUFFER_ROUND, 1);
    OhciSetTDField (DataTd, TD_DIR_PID, DataPidDir);
    OhciSetTDField (DataTd, TD_DELAY_INT, TD_NO_DELAY);
    OhciSetTDField (DataTd, TD_DT_TOGGLE, DataToggle);
    OhciSetTDField (DataTd, TD_ERROR_CNT, 0);
    OhciSetTDField (DataTd, TD_COND_CODE, TD_TOBE_PROCESSED);
    OhciSetTDField (DataTd, TD_CURR_BUFFER_PTR, (UINT32)DataMapPhyAddr);
    OhciSetTDField (DataTd, TD_BUFFER_END_PTR, (UINT32)(DataMapPhyAddr + ActualSendLength - 1));
    OhciSetTDField (DataTd, TD_NEXT_PTR, 0);
    DataTd->ActualSendLength = (UINT32)ActualSendLength;
    DataTd->DataBuffer       = (UINT32)DataMapPhyAddr;
    DataTd->NextTDPointer    = 0;
    OhciLinkTD (HeadTd, DataTd);
    DataToggle     ^= 1;
    DataMapPhyAddr += ActualSendLength;
    LeftLength     -= ActualSendLength;
  }

  //
  // Status Stage
  //
  StatusTd = OhciCreateTD (Ohc);
  if (StatusTd == NULL) {
    DEBUG ((DEBUG_INFO, "OhciControlTransfer: Fail to allocate buffer for Status Stage TD\r\n"));
    Status = EFI_OUT_OF_RESOURCES;
    goto UNMAP_DATA_BUFF;
  }

  OhciSetTDField (StatusTd, TD_PDATA, 0);
  OhciSetTDField (StatusTd, TD_BUFFER_ROUND, 1);
  OhciSetTDField (StatusTd, TD_DIR_PID, StatusPidDir);
  OhciSetTDField (StatusTd, TD_DELAY_INT, 7);
  OhciSetTDField (StatusTd, TD_DT_TOGGLE, 3);
  OhciSetTDField (StatusTd, TD_ERROR_CNT, 0);
  OhciSetTDField (StatusTd, TD_COND_CODE, TD_TOBE_PROCESSED);
  OhciSetTDField (StatusTd, TD_CURR_BUFFER_PTR, 0);
  OhciSetTDField (StatusTd, TD_NEXT_PTR, 0);
  OhciSetTDField (StatusTd, TD_BUFFER_END_PTR, 0);
  StatusTd->ActualSendLength = 0;
  StatusTd->DataBuffer       = 0;
  StatusTd->NextTDPointer    = 0;
  OhciLinkTD (HeadTd, StatusTd);
  //
  // Empty Stage
  //
  EmptyTd = OhciCreateTD (Ohc);
  if (EmptyTd == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto UNMAP_DATA_BUFF;
  }

  OhciSetTDField (EmptyTd, TD_PDATA, 0);
  OhciSetTDField (EmptyTd, TD_BUFFER_ROUND, 0);
  OhciSetTDField (EmptyTd, TD_DIR_PID, 0);
  OhciSetTDField (EmptyTd, TD_DELAY_INT, 0);
  EmptyTd->Word0.DataToggle = 0;
  OhciSetTDField (EmptyTd, TD_ERROR_CNT, 0);
  OhciSetTDField (EmptyTd, TD_COND_CODE, 0);
  OhciSetTDField (EmptyTd, TD_CURR_BUFFER_PTR, 0);
  OhciSetTDField (EmptyTd, TD_BUFFER_END_PTR, 0);
  OhciSetTDField (EmptyTd, TD_NEXT_PTR, 0);
  EmptyTd->ActualSendLength = 0;
  EmptyTd->DataBuffer       = 0;
  EmptyTd->NextTDPointer    = 0;
  OhciLinkTD (HeadTd, EmptyTd);
  Ed->TdTailPointer = (UINT32)(UINTN)EmptyTd;
  OhciAttachTDListToED (Ed, HeadTd);

  OhciSetEDField (Ed, ED_SKIP, 0);
  Status = OhciSetHcControl (Ohc, CONTROL_ENABLE, 1);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OhciControlTransfer: fail to enable CONTROL_ENABLE\r\n"));
    *TransferResult = EFI_USB_ERR_SYSTEM;
    Status          = EFI_DEVICE_ERROR;
    goto UNMAP_DATA_BUFF;
  }

  Status = OhciSetHcCommandStatus (Ohc, CONTROL_LIST_FILLED, 1);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OhciControlTransfer: fail to enable CONTROL_LIST_FILLED\r\n"));
    *TransferResult = EFI_USB_ERR_SYSTEM;
    Status          = EFI_DEVICE_ERROR;
    goto UNMAP_DATA_BUFF;
  }

  gBS->Stall (20 * 1000);

  TimeCount = 0;
  Status    = CheckIfDone (Ohc, CONTROL_LIST, Ed, HeadTd, &EdResult);

  while (Status == EFI_NOT_READY && TimeCount <= TimeOut) {
    gBS->Stall (1000);
    TimeCount++;
    Status = CheckIfDone (Ohc, CONTROL_LIST, Ed, HeadTd, &EdResult);
  }

  *TransferResult = ConvertErrorCode (EdResult.ErrorCode);

  if (EdResult.ErrorCode != TD_NO_ERROR) {
    if (EdResult.ErrorCode == TD_TOBE_PROCESSED) {
      DEBUG ((DEBUG_ERROR, "Control pipe timeout, > %d mS\r\n", TimeOut));
    } else {
      DEBUG ((DEBUG_ERROR, "Control pipe broken\r\n"));
    }

    *DataLength = 0;
  } else {
    DEBUG ((DEBUG_INFO, "Control transfer successed\r\n"));
  }

UNMAP_DATA_BUFF:
  OhciSetEDField (Ed, ED_SKIP, 1);
  if (HeadEd == Ed) {
    OhciSetMemoryPointer (Ohc, HC_CONTROL_HEAD, NULL);
  } else {
    HeadEd->NextED = Ed->NextED;
  }

  if (DataMapping != NULL) {
    Ohc->PciIo->Unmap (Ohc->PciIo, DataMapping);
  }

FREE_TD_BUFF:
  while (HeadTd) {
    DataTd = HeadTd;
    HeadTd = (TD_DESCRIPTOR *)(UINTN)(HeadTd->NextTDPointer);
    UsbHcFreeMem (Ohc->MemPool, DataTd, sizeof (TD_DESCRIPTOR));
  }

UNMAP_SETUP_BUFF:
  if (ReqMapping != NULL) {
    Ohc->PciIo->Unmap (Ohc->PciIo, ReqMapping);
  }

FREE_ED_BUFF:
  UsbHcFreeMem (Ohc->MemPool, Ed, sizeof (ED_DESCRIPTOR));

CTRL_EXIT:
  return Status;
}

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
  )
{
  ED_DESCRIPTOR    *HeadEd;
  ED_DESCRIPTOR    *Ed;
  UINT32           DataPidDir;
  TD_DESCRIPTOR    *HeadTd;
  TD_DESCRIPTOR    *DataTd;
  TD_DESCRIPTOR    *EmptyTd;
  EFI_STATUS       Status;
  UINT8            EndPointNum;
  UINTN            TimeCount;
  OHCI_ED_RESULT   EdResult;

  EFI_PCI_IO_PROTOCOL_OPERATION  MapOp;
  VOID                  *Mapping;
  UINTN                 MapLength;
  EFI_PHYSICAL_ADDRESS  MapPyhAddr;
  UINTN                 LeftLength;
  UINTN                 ActualSendLength;
  BOOLEAN               FirstTD;

  Mapping    = NULL;
  MapLength  = 0;
  MapPyhAddr = 0;
  LeftLength = 0;
  Status     = EFI_SUCCESS;

  if ((Data == NULL) || (DataLength == NULL) || (DataToggle == NULL) || (TransferResult == NULL) ||
      (*DataLength == 0) || ((*DataToggle != 0) && (*DataToggle != 1)) ||
      ((MaxPacketLength != 8) && (MaxPacketLength != 16) &&
       (MaxPacketLength != 32) && (MaxPacketLength != 64)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((EndPointAddress & 0x80) != 0) {
    DataPidDir = TD_IN_PID;
    MapOp      = EfiPciIoOperationBusMasterWrite;
  } else {
    DataPidDir = TD_OUT_PID;
    MapOp      = EfiPciIoOperationBusMasterRead;
  }

  EndPointNum         = (EndPointAddress & 0xF);
  EdResult.NextToggle = *DataToggle;

  Status = OhciSetHcControl (Ohc, BULK_ENABLE, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OhciBulkTransfer: fail to disable BULK_ENABLE\r\n"));
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_DEVICE_ERROR;
  }

  Status = OhciSetHcCommandStatus (Ohc, BULK_LIST_FILLED, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "OhciBulkTransfer: fail to disable BULK_LIST_FILLED\r\n"));
    *TransferResult = EFI_USB_ERR_SYSTEM;
    return EFI_DEVICE_ERROR;
  }

  gBS->Stall (20 * 1000);

  OhciSetMemoryPointer (Ohc, HC_BULK_HEAD, NULL);

  Ed = OhciCreateED (Ohc);
  if (Ed == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  OhciSetEDField (Ed, ED_SKIP, 1);
  OhciSetEDField (Ed, ED_FUNC_ADD, DeviceAddress);
  OhciSetEDField (Ed, ED_ENDPT_NUM, EndPointNum);
  OhciSetEDField (Ed, ED_DIR, ED_FROM_TD_DIR);
  OhciSetEDField (Ed, ED_SPEED, HI_SPEED);
  OhciSetEDField (Ed, ED_FORMAT | ED_HALTED | ED_DTTOGGLE, 0);
  OhciSetEDField (Ed, ED_MAX_PACKET, MaxPacketLength);
  OhciSetEDField (Ed, ED_PDATA, 0);
  OhciSetEDField (Ed, ED_ZERO, 0);
  OhciSetEDField (Ed, ED_TDHEAD_PTR, 0);
  OhciSetEDField (Ed, ED_TDTAIL_PTR, 0);
  OhciSetEDField (Ed, ED_NEXT_EDPTR, 0);
  HeadEd = OhciAttachEDToList (Ohc, BULK_LIST, Ed, NULL);

  if (Data != NULL) {
    MapLength = *DataLength;
    Status    = Ohc->PciIo->Map (
                               Ohc->PciIo,
                               MapOp,
                               (UINT8 *)Data,
                               &MapLength,
                               &MapPyhAddr,
                               &Mapping
                               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "OhciBulkTransfer: Fail to Map Data Buffer for Bulk\r\n"));
      goto FREE_ED_BUFF;
    }
  }

  //
  // Data Stage
  //
  LeftLength       = MapLength;
  ActualSendLength = MapLength;
  HeadTd           = NULL;
  FirstTD          = TRUE;
  while (LeftLength > 0) {
    ActualSendLength = LeftLength;
    if (LeftLength > MaxPacketLength) {
      ActualSendLength = MaxPacketLength;
    }

    DataTd = OhciCreateTD (Ohc);
    if (DataTd == NULL) {
      DEBUG ((DEBUG_INFO, "OhciBulkTransfer: Fail to allocate buffer for Data Stage TD\r\n"));
      Status = EFI_OUT_OF_RESOURCES;
      goto FREE_OHCI_TDBUFF;
    }

    OhciSetTDField (DataTd, TD_PDATA, 0);
    OhciSetTDField (DataTd, TD_BUFFER_ROUND, 1);
    OhciSetTDField (DataTd, TD_DIR_PID, DataPidDir);
    OhciSetTDField (DataTd, TD_DELAY_INT, TD_NO_DELAY);
    OhciSetTDField (DataTd, TD_DT_TOGGLE, *DataToggle);
    OhciSetTDField (DataTd, TD_ERROR_CNT, 0);
    OhciSetTDField (DataTd, TD_COND_CODE, TD_TOBE_PROCESSED);
    OhciSetTDField (DataTd, TD_CURR_BUFFER_PTR, (UINT32)MapPyhAddr);
    OhciSetTDField (DataTd, TD_BUFFER_END_PTR, (UINT32)(MapPyhAddr + ActualSendLength - 1));
    OhciSetTDField (DataTd, TD_NEXT_PTR, 0);
    DataTd->ActualSendLength = (UINT32)ActualSendLength;
    DataTd->DataBuffer       = (UINT32)MapPyhAddr;
    DataTd->NextTDPointer    = 0;
    if (FirstTD) {
      HeadTd  = DataTd;
      FirstTD = FALSE;
    } else {
      OhciLinkTD (HeadTd, DataTd);
    }

    *DataToggle ^= 1;
    MapPyhAddr  += ActualSendLength;
    LeftLength  -= ActualSendLength;
  }

  //
  // Empty Stage
  //
  EmptyTd = OhciCreateTD (Ohc);
  if (EmptyTd == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_INFO, "OhciBulkTransfer: Fail to allocate buffer for Empty TD\r\n"));
    goto FREE_OHCI_TDBUFF;
  }

  OhciSetTDField (EmptyTd, TD_PDATA, 0);
  OhciSetTDField (EmptyTd, TD_BUFFER_ROUND, 0);
  OhciSetTDField (EmptyTd, TD_DIR_PID, 0);
  OhciSetTDField (EmptyTd, TD_DELAY_INT, 0);
  EmptyTd->Word0.DataToggle = 0;
  OhciSetTDField (EmptyTd, TD_ERROR_CNT, 0);
  OhciSetTDField (EmptyTd, TD_COND_CODE, 0);
  OhciSetTDField (EmptyTd, TD_CURR_BUFFER_PTR, 0);
  OhciSetTDField (EmptyTd, TD_BUFFER_END_PTR, 0);
  OhciSetTDField (EmptyTd, TD_NEXT_PTR, 0);
  EmptyTd->ActualSendLength = 0;
  EmptyTd->DataBuffer       = 0;
  EmptyTd->NextTDPointer    = 0;
  OhciLinkTD (HeadTd, EmptyTd);
  Ed->TdTailPointer = (UINT32)(UINTN)EmptyTd;
  OhciAttachTDListToED (Ed, HeadTd);

  OhciSetEDField (Ed, ED_SKIP, 0);
  Status = OhciSetHcCommandStatus (Ohc, BULK_LIST_FILLED, 1);
  if (EFI_ERROR (Status)) {
    *TransferResult = EFI_USB_ERR_SYSTEM;
    Status          = EFI_DEVICE_ERROR;
    DEBUG ((DEBUG_INFO, "OhciBulkTransfer: Fail to enable BULK_LIST_FILLED\r\n"));
    goto FREE_OHCI_TDBUFF;
  }

  Status = OhciSetHcControl (Ohc, BULK_ENABLE, 1);
  if (EFI_ERROR (Status)) {
    *TransferResult = EFI_USB_ERR_SYSTEM;
    Status          = EFI_DEVICE_ERROR;
    DEBUG ((DEBUG_INFO, "OhciBulkTransfer: Fail to enable BULK_ENABLE\r\n"));
    goto FREE_OHCI_TDBUFF;
  }

  gBS->Stall (20 * 1000);

  TimeCount = 0;
  Status    = CheckIfDone (Ohc, BULK_LIST, Ed, HeadTd, &EdResult);
  while (Status == EFI_NOT_READY && TimeCount <= TimeOut) {
    gBS->Stall (1000);
    TimeCount++;
    Status = CheckIfDone (Ohc, BULK_LIST, Ed, HeadTd, &EdResult);
  }

  *TransferResult = ConvertErrorCode (EdResult.ErrorCode);

  if (EdResult.ErrorCode != TD_NO_ERROR) {
    if (EdResult.ErrorCode == TD_TOBE_PROCESSED) {
      DEBUG ((DEBUG_ERROR, "Bulk pipe timeout, > %d mS\r\n", TimeOut));
    } else {
      DEBUG ((DEBUG_ERROR, "Bulk pipe broken\r\n"));
      *DataToggle = EdResult.NextToggle;
    }

    *DataLength = 0;
  } else {
    DEBUG ((DEBUG_INFO, "Bulk transfer successed\r\n"));
  }

FREE_OHCI_TDBUFF:
  OhciSetEDField (Ed, ED_SKIP, 1);
  if (HeadEd == Ed) {
    OhciSetMemoryPointer (Ohc, HC_BULK_HEAD, NULL);
  } else {
    HeadEd->NextED = Ed->NextED;
  }

  while (HeadTd) {
    DataTd = HeadTd;
    HeadTd = (TD_DESCRIPTOR *)(UINTN)(HeadTd->NextTDPointer);
    UsbHcFreeMem (Ohc->MemPool, DataTd, sizeof (TD_DESCRIPTOR));
  }

  if (Mapping != NULL) {
    Ohc->PciIo->Unmap (Ohc->PciIo, Mapping);
  }

FREE_ED_BUFF:
  UsbHcFreeMem (Ohc->MemPool, Ed, sizeof (ED_DESCRIPTOR));

  return Status;
}

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
  )
{
  ED_DESCRIPTOR            *Ed;
  UINT8                    EdDir;
  ED_DESCRIPTOR            *HeadEd;
  TD_DESCRIPTOR            *HeadTd;
  TD_DESCRIPTOR            *DataTd;
  TD_DESCRIPTOR            *EmptTd;
  UINTN                    Depth;
  UINTN                    Index;
  EFI_STATUS               Status;
  UINT8                    EndPointNum;
  UINT32                   DataPidDir;
  INTERRUPT_CONTEXT_ENTRY  *Entry;
  EFI_TPL                  OldTpl;
  BOOLEAN                  FirstTD;

  VOID                  *Mapping;
  UINTN                 MapLength;
  EFI_PHYSICAL_ADDRESS  MapPyhAddr;
  UINTN                 LeftLength;
  UINTN                 ActualSendLength;

  if (DataLength > MAX_BYTES_PER_TD) {
    DEBUG ((DEBUG_ERROR, "OhciInterruptTransfer: Error param\r\n"));
    return EFI_INVALID_PARAMETER;
  }

  if ((EndPointAddress & 0x80) != 0) {
    EdDir      = ED_IN_DIR;
    DataPidDir = TD_IN_PID;
  } else {
    EdDir      = ED_OUT_DIR;
    DataPidDir = TD_OUT_PID;
  }

  EndPointNum = (EndPointAddress & 0xF);
  if (!IsNewTransfer) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    OhciSetHcControl (Ohc, PERIODIC_ENABLE, 0);
    OhciFreeInterruptContext (Ohc, DeviceAddress, EndPointAddress, DataToggle);
    Status = OhciFreeInterruptEdByAddr (Ohc, DeviceAddress, EndPointNum);
    OhciSetHcControl (Ohc, PERIODIC_ENABLE, 1);
    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  Mapping = NULL;
  if ((UCBuffer != NULL) && (DataLength != 0)) {
    MapLength = DataLength;
    Status    = Ohc->PciIo->Map (
                               Ohc->PciIo,
                               EfiPciIoOperationBusMasterWrite,
                               UCBuffer,
                               &MapLength,
                               &MapPyhAddr,
                               &Mapping
                               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OhciInterruptTransfer: Fail to Map buffer \r\n"));
      goto EXIT;
    }
  } else {
    MapLength = 0;
    MapPyhAddr = 0;
  }

  Depth = 5;
  Index = 1;
  while (PollingInterval >= Index * 2 && Depth > 0) {
    Index *= 2;
    Depth--;
  }

  //
  // ED Stage
  //
  HeadEd = OhciFindMinInterruptEDList (Ohc, (UINT32)Depth);
  if ((Ed = OhciFindWorkingEd (HeadEd, DeviceAddress, EndPointNum, EdDir)) != NULL) {
    OhciSetEDField (Ed, ED_SKIP, 1);
  } else {
    Ed = OhciCreateED (Ohc);
    if (Ed == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      DEBUG ((DEBUG_ERROR, "OhciInterruptTransfer: Fail to allocate buffer for ED\r\n"));
      goto UNMAP_OHCI_XBUFF;
    }

    OhciSetEDField (Ed, ED_SKIP, 1);
    OhciSetEDField (Ed, ED_FUNC_ADD, DeviceAddress);
    OhciSetEDField (Ed, ED_ENDPT_NUM, EndPointNum);
    OhciSetEDField (Ed, ED_DIR, ED_FROM_TD_DIR);
    OhciSetEDField (Ed, ED_SPEED, IsSlowDevice);
    OhciSetEDField (Ed, ED_FORMAT, 0);
    OhciSetEDField (Ed, ED_MAX_PACKET, MaxPacketLength);
    OhciSetEDField (Ed, ED_PDATA | ED_ZERO | ED_HALTED | ED_DTTOGGLE, 0);
    OhciSetEDField (Ed, ED_TDHEAD_PTR, 0);
    OhciSetEDField (Ed, ED_TDTAIL_PTR, 0);
    OhciSetEDField (Ed, ED_NEXT_EDPTR, 0);
    OhciAttachEDToList (Ohc, INTERRUPT_LIST, Ed, HeadEd);
  }

  //
  // Data Stage
  //
  LeftLength       = MapLength;
  ActualSendLength = MapLength;
  HeadTd           = NULL;
  FirstTD          = TRUE;
  while (LeftLength > 0) {
    ActualSendLength = LeftLength;
    if (LeftLength > MaxPacketLength) {
      ActualSendLength = MaxPacketLength;
    }

    DataTd = OhciCreateTD (Ohc);
    if (DataTd == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      DEBUG ((DEBUG_ERROR, "OhciInterruptTransfer: Fail to allocate buffer for Data Stage TD\r\n"));
      goto FREE_OHCI_TDBUFF;
    }

    OhciSetTDField (DataTd, TD_PDATA, 0);
    OhciSetTDField (DataTd, TD_BUFFER_ROUND, 1);
    OhciSetTDField (DataTd, TD_DIR_PID, DataPidDir);
    OhciSetTDField (DataTd, TD_DELAY_INT, TD_NO_DELAY);
    OhciSetTDField (DataTd, TD_DT_TOGGLE, *DataToggle);
    OhciSetTDField (DataTd, TD_ERROR_CNT, 0);
    OhciSetTDField (DataTd, TD_COND_CODE, TD_TOBE_PROCESSED);
    OhciSetTDField (DataTd, TD_CURR_BUFFER_PTR, (UINT32)MapPyhAddr);
    OhciSetTDField (DataTd, TD_BUFFER_END_PTR, (UINT32)(MapPyhAddr + ActualSendLength - 1));
    OhciSetTDField (DataTd, TD_NEXT_PTR, 0);
    DataTd->ActualSendLength = (UINT32)ActualSendLength;
    DataTd->DataBuffer       = (UINT32)(UINTN)MapPyhAddr;
    DataTd->NextTDPointer    = 0;
    if (FirstTD) {
      HeadTd  = DataTd;
      FirstTD = FALSE;
    } else {
      OhciLinkTD (HeadTd, DataTd);
    }

    *DataToggle ^= 1;
    MapPyhAddr  += ActualSendLength;
    LeftLength  -= ActualSendLength;
  }

  EmptTd = OhciCreateTD (Ohc);
  if (EmptTd == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    DEBUG ((DEBUG_ERROR, "OhciInterruptTransfer: Fail to allocate buffer for Empty Stage TD\r\n"));
    goto FREE_OHCI_TDBUFF;
  }

  OhciSetTDField (EmptTd, TD_PDATA, 0);
  OhciSetTDField (EmptTd, TD_BUFFER_ROUND, 0);
  OhciSetTDField (EmptTd, TD_DIR_PID, 0);
  OhciSetTDField (EmptTd, TD_DELAY_INT, 0);
  EmptTd->Word0.DataToggle = 0;
  OhciSetTDField (EmptTd, TD_ERROR_CNT, 0);
  OhciSetTDField (EmptTd, TD_COND_CODE, 0);
  OhciSetTDField (EmptTd, TD_CURR_BUFFER_PTR, 0);
  OhciSetTDField (EmptTd, TD_BUFFER_END_PTR, 0);
  OhciSetTDField (EmptTd, TD_NEXT_PTR, 0);
  EmptTd->ActualSendLength = 0;
  EmptTd->DataBuffer       = 0;
  EmptTd->NextTDPointer    = 0;
  OhciLinkTD (HeadTd, EmptTd);
  Ed->TdTailPointer = (UINT32)(UINTN)EmptTd;
  OhciAttachTDListToED (Ed, HeadTd);

  if (OutputED != NULL) {
    *OutputED = Ed;
  }

  if (OutputTD != NULL) {
    *OutputTD = HeadTd;
  }

  if (CallBackFunction != NULL) {
    Entry = AllocatePool (sizeof (INTERRUPT_CONTEXT_ENTRY));
    if (Entry == NULL) {
      goto FREE_OHCI_TDBUFF;
    }

    Entry->DeviceAddress    = DeviceAddress;
    Entry->EndPointAddress  = EndPointAddress;
    Entry->Ed               = Ed;
    Entry->DataTd           = HeadTd;
    Entry->IsSlowDevice     = IsSlowDevice;
    Entry->MaxPacketLength  = MaxPacketLength;
    Entry->PollingInterval  = PollingInterval;
    Entry->CallBackFunction = CallBackFunction;
    Entry->Context          = Context;
    Entry->IsPeriodic       = IsPeriodic;
    Entry->UCBuffer         = UCBuffer;
    Entry->UCBufferMapping  = Mapping;
    Entry->DataLength       = DataLength;
    Entry->Toggle           = DataToggle;
    Entry->NextEntry        = NULL;
    OhciAddInterruptContextEntry (Ohc, Entry);
  }

  OhciSetEDField (Ed, ED_SKIP, 0);

  if (OhciGetHcControl (Ohc, PERIODIC_ENABLE) == 0) {
    Status = OhciSetHcControl (Ohc, PERIODIC_ENABLE, 1);
    gBS->Stall (1000);
  }

  return EFI_SUCCESS;

FREE_OHCI_TDBUFF:
  while (HeadTd) {
    DataTd = HeadTd;
    HeadTd = (TD_DESCRIPTOR *)(UINTN)(HeadTd->NextTDPointer);
    UsbHcFreeMem (Ohc->MemPool, DataTd, sizeof (TD_DESCRIPTOR));
  }

  // FREE_OHCI_EDBUFF:
  if ((HeadEd != Ed) && HeadEd && Ed) {
    while (HeadEd->NextED != (UINT32)(UINTN)Ed) {
      HeadEd = (ED_DESCRIPTOR *)(UINTN)(HeadEd->NextED);
    }

    HeadEd->NextED = Ed->NextED;
    UsbHcFreeMem (Ohc->MemPool, Ed, sizeof (ED_DESCRIPTOR));
  }

UNMAP_OHCI_XBUFF:
  if (Mapping != NULL) {
    Ohc->PciIo->Unmap (Ohc->PciIo, Mapping);
  }

EXIT:
  return Status;
}

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
  )
{
  EFI_STATUS  Status;
  VOID        *UCBuffer;
  UINTN       Pages;

  if ((DataToggle == NULL) || ((EndPointAddress & 0x80) == 0) ||
      (IsNewTransfer && ((DataLength == 0) ||
                         ((*DataToggle != 0) && (*DataToggle != 1)) || ((PollingInterval < 1) || (PollingInterval > 255)))))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (IsNewTransfer) {
    Pages  = EFI_SIZE_TO_PAGES (DataLength);
    Status = Ohc->PciIo->AllocateBuffer (
                           Ohc->PciIo,
                           AllocateAnyPages,
                           EfiBootServicesData,
                           Pages,
                           &UCBuffer,
                           0
                           );
    if (EFI_ERROR (Status)) {
      return EFI_OUT_OF_RESOURCES;
    }
  } else {
    UCBuffer = NULL;
  }

  Status = OhciInterruptTransfer (
             Ohc,
             DeviceAddress,
             EndPointAddress,
             IsSlowDevice,
             MaxPacketLength,
             IsNewTransfer,
             DataToggle,
             PollingInterval,
             UCBuffer,
             DataLength,
             CallBackFunction,
             Context,
             TRUE,
             NULL,
             NULL
             );

  if (UCBuffer) {
    if (EFI_ERROR (Status)) {
      Ohc->PciIo->FreeBuffer (Ohc->PciIo, Pages, UCBuffer);
    }
  }

  return Status;
}

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
  )
{
  EFI_STATUS     Status;
  ED_DESCRIPTOR  *Ed;
  TD_DESCRIPTOR  *HeadTd;
  OHCI_ED_RESULT EdResult;
  VOID           *UCBuffer;

  if (((EndPointAddress & 0x80) == 0) || (Data == NULL) || (DataLength == NULL) || (*DataLength == 0) ||
      (IsSlowDevice && (MaxPacketLength > 8)) || (!IsSlowDevice && (MaxPacketLength > 64)) ||
      (DataToggle == NULL) || ((*DataToggle != 0) && (*DataToggle != 1)) || (TransferResult == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  UCBuffer = AllocatePool (*DataLength);
  if (UCBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = OhciInterruptTransfer (
             Ohc,
             DeviceAddress,
             EndPointAddress,
             IsSlowDevice,
             MaxPacketLength,
             TRUE,
             DataToggle,
             1,
             UCBuffer,
             *DataLength,
             NULL,
             NULL,
             FALSE,
             &Ed,
             &HeadTd
             );

  if (!EFI_ERROR (Status)) {
    Status = CheckIfDone (Ohc, INTERRUPT_LIST, Ed, HeadTd, &EdResult);
    while (Status == EFI_NOT_READY && TimeOut > 0) {
      gBS->Stall (1000);
      TimeOut--;
      Status = CheckIfDone (Ohc, INTERRUPT_LIST, Ed, HeadTd, &EdResult);
    }

    *TransferResult = ConvertErrorCode (EdResult.ErrorCode);
  }

  CopyMem (Data, UCBuffer, *DataLength);
  Status = OhciInterruptTransfer (
             Ohc,
             DeviceAddress,
             EndPointAddress,
             IsSlowDevice,
             MaxPacketLength,
             FALSE,
             DataToggle,
             0,
             NULL,
             0,
             NULL,
             NULL,
             FALSE,
             NULL,
             NULL
             );

  FreePool (UCBuffer);

  return Status;
}

/**

  Retrieves the number of root hub ports.

**/
EFI_STATUS
OhciGetRootHubNumOfPorts (
  IN  USB_OHCI_HC_DEV  *Ohc,
  OUT UINT8            *NumOfPorts
  )
{
  if (NumOfPorts == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *NumOfPorts = (UINT8)OhciGetRootHubDescriptor (Ohc, RH_NUM_DS_PORTS);

  return EFI_SUCCESS;
}

/**

  Retrieves the current status of a USB root hub port.

**/
EFI_STATUS
OhciGetRootHubPortStatus (
  IN  USB_OHCI_HC_DEV     *Ohc,
  IN  UINT8               PortNumber,
  OUT EFI_USB_PORT_STATUS *PortStatus
  )
{
  UINT8  NumOfPorts;

  OhciGetRootHubNumOfPorts (Ohc, &NumOfPorts);
  if (PortNumber >= NumOfPorts) {
    return EFI_INVALID_PARAMETER;
  }

  PortStatus->PortStatus       = 0;
  PortStatus->PortChangeStatus = 0;

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_CURR_CONNECT_STAT)) {
    PortStatus->PortStatus |= USB_PORT_STAT_CONNECTION;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_ENABLE_STAT)) {
    PortStatus->PortStatus |= USB_PORT_STAT_ENABLE;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_SUSPEND_STAT)) {
    PortStatus->PortStatus |= USB_PORT_STAT_SUSPEND;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_OC_INDICATOR)) {
    PortStatus->PortStatus |= USB_PORT_STAT_OVERCURRENT;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_RESET_STAT)) {
    PortStatus->PortStatus |= USB_PORT_STAT_RESET;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_POWER_STAT)) {
    PortStatus->PortStatus |= USB_PORT_STAT_POWER;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_LSDEVICE_ATTACHED)) {
    PortStatus->PortStatus |= USB_PORT_STAT_LOW_SPEED;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_ENABLE_STAT_CHANGE)) {
    PortStatus->PortChangeStatus |= USB_PORT_STAT_C_ENABLE;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_CONNECT_STATUS_CHANGE)) {
    PortStatus->PortChangeStatus |= USB_PORT_STAT_C_CONNECTION;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_SUSPEND_STAT_CHANGE)) {
    PortStatus->PortChangeStatus |= USB_PORT_STAT_C_SUSPEND;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_OC_INDICATOR_CHANGE)) {
    PortStatus->PortChangeStatus |= USB_PORT_STAT_C_OVERCURRENT;
  }

  if (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_RESET_STAT_CHANGE)) {
    PortStatus->PortChangeStatus |= USB_PORT_STAT_C_RESET;
  }

  return EFI_SUCCESS;
}

/**

  Sets a feature for the specified root hub port.

**/
EFI_STATUS
OhciSetRootHubPortFeature (
  IN USB_OHCI_HC_DEV      *Ohc,
  IN UINT8                PortNumber,
  IN EFI_USB_PORT_FEATURE PortFeature
  )
{
  EFI_STATUS  Status;
  UINT8       NumOfPorts;
  UINTN       RetryTimes;

  OhciGetRootHubNumOfPorts (Ohc, &NumOfPorts);
  if (PortNumber >= NumOfPorts) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EFI_SUCCESS;

  switch (PortFeature) {
    case EfiUsbPortPower:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_SET_PORT_POWER);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_POWER_STAT) == 0 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortReset:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_SET_PORT_RESET);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while ((OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_RESET_STAT_CHANGE) == 0 ||
                OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_RESET_STAT) == 1) &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      OhciSetRootHubPortStatus (Ohc, PortNumber, RH_PORT_RESET_STAT_CHANGE);
      break;

    case EfiUsbPortEnable:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_SET_PORT_ENABLE);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_ENABLE_STAT) == 0 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortSuspend:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_SET_PORT_SUSPEND);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_SUSPEND_STAT) == 0 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    default:
      return EFI_INVALID_PARAMETER;
  }

  return Status;
}

/**

  Clears a feature for the specified root hub port.

**/
EFI_STATUS
OhciClearRootHubPortFeature (
  IN USB_OHCI_HC_DEV      *Ohc,
  IN UINT8                PortNumber,
  IN EFI_USB_PORT_FEATURE PortFeature
  )
{
  EFI_STATUS  Status;
  UINT8       NumOfPorts;
  UINTN       RetryTimes;

  OhciGetRootHubNumOfPorts (Ohc, &NumOfPorts);
  if (PortNumber >= NumOfPorts) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EFI_SUCCESS;

  switch (PortFeature) {
    case EfiUsbPortEnable:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_CLEAR_PORT_ENABLE);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_ENABLE_STAT) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortSuspend:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_CLEAR_SUSPEND_STATUS);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_SUSPEND_STAT) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortReset:
      break;

    case EfiUsbPortPower:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_CLEAR_PORT_POWER);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_POWER_STAT) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortConnectChange:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_CONNECT_STATUS_CHANGE);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_CONNECT_STATUS_CHANGE) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortResetChange:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_PORT_RESET_STAT_CHANGE);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_RESET_STAT_CHANGE) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortEnableChange:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_PORT_ENABLE_STAT_CHANGE);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_ENABLE_STAT_CHANGE) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortSuspendChange:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_PORT_SUSPEND_STAT_CHANGE);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_PORT_SUSPEND_STAT_CHANGE) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    case EfiUsbPortOverCurrentChange:
      Status = OhciSetRootHubPortStatus (Ohc, PortNumber, RH_OC_INDICATOR_CHANGE);

      //
      // Verify the state
      //
      RetryTimes = 0;
      do {
        gBS->Stall (1000);
        RetryTimes++;
      } while (OhciReadRootHubPortStatus (Ohc, PortNumber, RH_OC_INDICATOR_CHANGE) == 1 &&
               RetryTimes < MAX_RETRY_TIMES);

      if (RetryTimes >= MAX_RETRY_TIMES) {
        return EFI_DEVICE_ERROR;
      }

      break;

    default:
      return EFI_INVALID_PARAMETER;
  }

  return Status;
}

EFI_STATUS
OhcInitHC (
  IN USB_OHCI_HC_DEV  *Ohc
  )
{
  EFI_STATUS  Status;
  UINT8       Index;
  UINT8       NumOfPorts;
  UINT32      PowerOnGoodTime;
  UINT32      Data32;
  BOOLEAN     Flag = FALSE;

  Status = OhciSetHcCommandStatus (Ohc, HC_RESET, HC_RESET);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  gBS->Stall (50 * 1000);
  //
  // Wait for host controller reset.
  //
  PowerOnGoodTime = 50;
  do {
    gBS->Stall (1000);
    Data32 = OhciGetOperationalReg (Ohc, HC_COMMAND_STATUS);
    if ((Data32 & HC_RESET) == 0) {
      Flag = TRUE;
      break;
    }
  } while (PowerOnGoodTime--);

  if (!Flag) {
    return EFI_DEVICE_ERROR;
  }

  OhciInitializeInterruptList (Ohc);

  OhciSetFrameInterval (Ohc, FRAME_INTERVAL, 0x2edf);
  Status = OhciSetHcControl (Ohc, HC_FUNCTIONAL_STATE, HC_STATE_RESET);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  gBS->Stall (50 * 1000);
  //
  // Initialize host controller operational registers
  //
  OhciSetFrameInterval (Ohc, FS_LARGEST_DATA_PACKET, 0x2778);
  OhciSetFrameInterval (Ohc, FRAME_INTERVAL, 0x2edf);
  OhciSetPeriodicStart (Ohc, 0x2a2f);
  OhciSetHcControl (Ohc, CONTROL_BULK_RATIO, 0x3);
  OhciSetHcCommandStatus (Ohc, CONTROL_LIST_FILLED | BULK_LIST_FILLED, 0);
  OhciSetRootHubDescriptor (Ohc, RH_PSWITCH_MODE, 0);
  OhciSetRootHubDescriptor (Ohc, RH_NO_PSWITCH | RH_NOC_PROT, 1);

  OhciSetRootHubDescriptor (Ohc, RH_DEV_REMOVABLE, 0);
  OhciSetRootHubDescriptor (Ohc, RH_PORT_PWR_CTRL_MASK, 0xffff);
  OhciSetRootHubStatus (Ohc, RH_LOCAL_PSTAT_CHANGE);
  OhciSetRootHubPortStatus (Ohc, 0, RH_SET_PORT_POWER);
  NumOfPorts = OhciGetRootHubDescriptor (Ohc, RH_NUM_DS_PORTS);

  for (Index = 0; Index < NumOfPorts; Index++) {
    if (!EFI_ERROR (OhciSetRootHubPortFeature (Ohc, Index, EfiUsbPortReset))) {
      gBS->Stall (200 * 1000);
      OhciClearRootHubPortFeature (Ohc, Index, EfiUsbPortReset);
      gBS->Stall (1000);
      OhciSetRootHubPortFeature (Ohc, Index, EfiUsbPortEnable);
      gBS->Stall (1000);
    }
  }

  OhciSetMemoryPointer (Ohc, HC_HCCA, Ohc->HccaMemoryBlock);
  OhciSetMemoryPointer (Ohc, HC_CONTROL_HEAD, NULL);
  OhciSetMemoryPointer (Ohc, HC_BULK_HEAD, NULL);
  OhciSetHcControl (Ohc, PERIODIC_ENABLE | CONTROL_ENABLE | BULK_ENABLE, 1);
  OhciSetHcControl (Ohc, HC_FUNCTIONAL_STATE, HC_STATE_OPERATIONAL);

  //
  // Wait till first SOF occurs, and then clear it
  //
  while (OhciGetHcInterruptStatus (Ohc, START_OF_FRAME) == 0) {
  }

  OhciClearInterruptStatus (Ohc, START_OF_FRAME);
  gBS->Stall (1000);

  return EFI_SUCCESS;
}

/**

  Free the OHCI device and release its associated resources.

  @param  Ohc                   The OHCI device to release.

**/
VOID
OhciFreeDev (
  IN USB_OHCI_HC_DEV  *Ohc
  )
{
  OhciFreeFixedIntMemory (Ohc);

  if (Ohc->HouseKeeperTimer != NULL) {
    gBS->CloseEvent (Ohc->HouseKeeperTimer);
  }

  if (Ohc->ExitBootServiceEvent != NULL) {
    gBS->CloseEvent (Ohc->ExitBootServiceEvent);
  }

  if (Ohc->MemPool != NULL) {
    UsbHcFreeMemPool (Ohc->MemPool);
  }

  if (Ohc->HccaMemoryMapping != NULL) {
    Ohc->PciIo->Unmap (Ohc->PciIo, Ohc->HccaMemoryMapping);
  }

  if (Ohc->HccaMemoryBuf != NULL) {
    Ohc->PciIo->FreeBuffer (Ohc->PciIo, Ohc->HccaMemoryPages, Ohc->HccaMemoryBuf);
  }

  FreePool (Ohc);
}

/**

  One notified function to stop the Host Controller when gBS->ExitBootServices() called.

  @param  Event                 Pointer to this event
  @param  Context               Event handler private data
**/
VOID
EFIAPI
OhcExitBootService (
  EFI_EVENT  Event,
  VOID       *Context
  )
{
  USB_OHCI_HC_DEV  *Ohc;

  Ohc = (USB_OHCI_HC_DEV *)Context;

  //
  // Stop the Host Controller
  //
  OhciSetHcControl (Ohc, PERIODIC_ENABLE | CONTROL_ENABLE | ISOCHRONOUS_ENABLE | BULK_ENABLE, 0);
  OhciReset (Ohc, EFI_USB_HC_RESET_GLOBAL);
  OhciSetState (Ohc, EfiUsbHcStateHalt);

  return;
}
