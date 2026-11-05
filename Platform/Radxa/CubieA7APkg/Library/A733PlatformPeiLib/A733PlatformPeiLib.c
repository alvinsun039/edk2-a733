/** @file
  Radxa Cubie A7A platform PEI library.

  Replaces ArmPlatformPkg's PlatformPeiLib so the platform can add HOBs
  before the DXE core builds its memory map.  The default implementation
  only publishes the firmware volume HOB; this one also reserves the page
  boot0 uses for the ARISC configuration FDT.

  boot0 copies the boot package's "dtb" item to uboot_base + 2 MB, i.e. to
  0x41200000.  That address is inside the DRAM range declared as system
  memory (PcdSystemMemoryBase 0x40000000 + PcdSystemMemorySize), and the
  firmware volume itself ends well below it (1.5 MB), so before this HOB
  existed the DXE core was free to allocate the page for its own heap or
  image data.  Whichever happened first per boot decided whether
  AriscStartupDxe still found the FDT header: on the boots where it was
  overwritten the driver handed BL31 a NULL boot argument and the E902
  started without its /dram parameters and /standby_param power tree -
  different DRAM settings and a different suspend power plan on every such
  boot.

  Reserving the page here, before the DXE core exists, keeps boot0's copy
  intact for the whole boot and marks it EfiReservedMemoryType in the
  memory map the OS receives, so nothing after ExitBootServices reuses it
  either (the E902 parses the tree again at every super-standby).

  The early-measurement transfer list that the default library turns into
  TPM event log HOBs is not used on this platform (the DSC maps the TPM
  libraries to their Null instances), so that part is intentionally not
  replicated.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>

#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>

//
// boot0's boot-package layout: "u-boot" (this firmware) at the start of the
// BL33 window, "dtb" (the ARISC configuration FDT) 2 MB further up.
//
#define A733_UBOOT_BASE       0x41000000UL
#define A733_ARISC_DTB_BASE   (A733_UBOOT_BASE + 0x200000UL)

/**
  Publish the firmware volume HOB and reserve the ARISC configuration FDT
  page boot0 placed in DRAM.

  @retval EFI_SUCCESS  The HOBs were built.
**/
EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));

  BuildMemoryAllocationHob (
    (EFI_PHYSICAL_ADDRESS)A733_ARISC_DTB_BASE,
    EFI_PAGE_SIZE,
    EfiReservedMemoryType
    );

  DEBUG ((
    DEBUG_INFO,
    "A733PlatformPeiLib: reserved ARISC FDT page at 0x%lx\n",
    (UINT64)A733_ARISC_DTB_BASE
    ));

  return EFI_SUCCESS;
}
