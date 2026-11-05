/** @file
  Board device-tree fix-ups for the A733 platforms.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_BOARD_FDT_LIB_H_
#define A733_BOARD_FDT_LIB_H_

/**
  Apply the board fix-ups to a device tree in place.

  The tree must be writable and have room for the properties the fix-ups
  add; fdt_open_into() into a buffer larger than the input is the usual way
  to prepare it.

  @param[in,out] Fdt  Device tree blob.
**/
VOID
EFIAPI
A733FdtApplyBoardFixups (
  IN OUT VOID  *Fdt
  );

#endif
