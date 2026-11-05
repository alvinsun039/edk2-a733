/** @file
 *
 *  ACPI platform driver — Allwinner A733
 *
 *  Simplified rk3588 AcpiPlatformDxe (edk2-rockchip): on the EndOfDxe
 *  callback, search the FV by FILE_GUID
 *  7E374E25-8E01-4FEE-87F2-390C23C606CD (AcpiTables.inf) and install the
 *  ACPI tables (LocateAndInstallAcpiFromFvConditional).
 *
 *  Gated by gA733TokenSpaceGuid.PcdConfigTableMode (same encoding as
 *  edk2-rockchip PcdConfigTableMode):
 *    bit0 (0x1) = ACPI
 *    bit1 (0x2) = FDT
 *    0x3        = both
 *  Default 0x2 (FDT only): the skeleton DSDT would break the existing
 *  DTB boot path, so ACPI must be enabled explicitly.  The PCD is bound
 *  to [PcdsDynamicHii] by the platform DSC and points at the
 *  "ConfigTableMode" variable of the A733SetupDxe formset ("OS Config
 *  Table" page, RESET_REQUIRED); changing it in Setup takes effect after
 *  a reboot.  The driver installs a DxePcdLib override for this reason,
 *  following BdsDxe.
 *
 *  Copyright (c) 2020, Jeremy Linton
 *  Copyright (c) 2024, Mario Bălănică <mariobalanica02@gmail.com> (template)
 *
 *  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
 *  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Library/AcpiLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <VarStoreData.h>

//
// FILE_GUID of the AcpiTables.inf FFS file holding the raw ACPI tables.
//
STATIC CONST EFI_GUID  mAcpiTableFile = {
  0x7E374E25, 0x8E01, 0x4FEE, { 0x87, 0xF2, 0x39, 0x0C, 0x23, 0xC6, 0x06, 0xCD }
};

STATIC
VOID
EFIAPI
NotifyEndOfDxeEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  Status = LocateAndInstallAcpiFromFvConditional (&mAcpiTableFile, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "AcpiPlatform: Failed to install firmware ACPI as config table. Status=%r\n",
      Status
      ));
  }
}

EFI_STATUS
EFIAPI
AcpiPlatformDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  if ((PcdGet32 (PcdConfigTableModeDefault) & CONFIG_TABLE_MODE_ACPI) == 0) {
    DEBUG ((DEBUG_WARN, "AcpiPlatform: ACPI support is disabled by the settings.\n"));
    return EFI_UNSUPPORTED;
  }

  //
  // AcpiTableDxe publishes gEfiAcpiTableProtocolGuid at dispatch; EndOfDxe
  // (signalled by BDS) is guaranteed to run after that, so the FV tables
  // (incl. the DSDT, whose address AcpiTableDxe wires into the FADT) are
  // installed before the OS hand-off.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,               // Type
                  TPL_CALLBACK,                    // NotifyTpl
                  NotifyEndOfDxeEvent,             // NotifyFunction
                  NULL,                            // NotifyContext
                  &gEfiEndOfDxeEventGroupGuid,     // EventGroup
                  &Event                           // Event
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
