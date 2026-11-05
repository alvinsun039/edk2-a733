/** @file
  Board device-tree fix-ups for the A733 platforms.

  The vendor DTS enables every block the SoC has, not every block a board
  mounts.  A node left "okay" for hardware that is not there does not fail
  quietly: the kernel probes it, the probe times out against an absent PHY,
  and the driver retries for the rest of the boot (the UFS node alone
  produced ~50 "Phy setup failed (-110)" lines on the Cubie A7A).

  Only node presence is changed, never a value that is already right, so the
  fix-ups are idempotent and safe to run on a tree that was fixed already.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733BoardFdtLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>

/**
  Apply the board fix-ups to a device tree in place.

  @param[in,out] Fdt  Device tree blob.
**/
VOID
EFIAPI
A733FdtApplyBoardFixups (
  IN OUT VOID  *Fdt
  )
{
  static CONST CHAR8  Disabled[]  = "disabled";
  static CONST CHAR8  UfsPath[]   = "/soc@3000000/ufs@04520000";
  static CONST CHAR8  EmmcPath[]  = "/soc@3000000/sdmmc@4022000";
  INT32               Offset;

  //
  // UFS: the board boots from eMMC and mounts no UFS device.  The node's PHY
  // never comes ready, which costs the probe a timeout plus a retry loop.
  //
  Offset = FdtPathOffset (Fdt, UfsPath);
  if (Offset >= 0) {
    if (FdtSetProp (Fdt, Offset, "status", Disabled, sizeof (Disabled)) == 0) {
      DEBUG ((DEBUG_INFO, "A733Fdt: fix-up: ufs@04520000 -> disabled\n"));
    } else {
      DEBUG ((DEBUG_WARN, "A733Fdt: fix-up: ufs status\n"));
    }
  }

  //
  // eMMC command queue: the vendor node asks for it, but on this board the
  // kernel's CQE bring-up faults on the first data command - "smc 0 p2 err,
  // DCE" followed by a CMD12 timeout - and the driver has to disable CQE and
  // re-run the identification.  It works without the queue, so the queue is
  // what goes.  Not a property value either way: the binding is "present and
  // we use CQE", so the property is removed.
  //
  Offset = FdtPathOffset (Fdt, EmmcPath);
  if (Offset >= 0) {
    if (FdtDelProp (Fdt, Offset, "cqe-on") == 0) {
      DEBUG ((DEBUG_INFO, "A733Fdt: fix-up: sdmmc@4022000 cqe-on removed\n"));
    } else {
      DEBUG ((DEBUG_INFO, "A733Fdt: fix-up: sdmmc@4022000 has no cqe-on\n"));
    }
  }
}
