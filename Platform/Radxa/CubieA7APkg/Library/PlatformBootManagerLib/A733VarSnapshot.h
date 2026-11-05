/** @file
  Variable snapshot: restore the variable set from a FAT volume and mirror it
  back at ReadyToBoot.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_VAR_SNAPSHOT_H_
#define A733_VAR_SNAPSHOT_H_

/**
  Restore the variable set from \EFI\A7A\vars.bin when the store has never
  been written, then arm the ReadyToBoot export.  The SPI NOR store stays
  authoritative either way.

  Call once from PlatformBootManagerAfterConsole, after the storage stack is
  connected and before the boot options are built.
**/
VOID
EFIAPI
A733VarSnapshotInit (
  VOID
  );

#endif