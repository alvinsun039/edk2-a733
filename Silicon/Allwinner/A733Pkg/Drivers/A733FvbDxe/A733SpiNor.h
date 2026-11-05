/** @file
  SPI NOR access for the Radxa Cubie A7A's Winbond 25-series flash.

  The board's NOR is on SPI0, chip select 0, on PC2 (MOSI), PC3 (CS0), PC4
  (MISO) and PC12 (CLK), all on the pins' SPI0 alternate function (ALT3, the
  manual's PCn select 0011).  The schematic's page 09 (EMMC_UFS_NOR) makes it
  U7, a W25Q64FWZPIG: 64 Mbit = 8 MB, 1.8 V, SOIC-8, WP# and HOLD# tied high.
  The variable window is therefore the *last* 192 KB of whatever part answers,
  which is 0x7D0000 on this one and 0xF00000 on a 16 MB part - see
  A733SpiNorSetCapacity.  Keeping it at the tail is what leaves any boot image
  in the low part alone.

  The command set is the standard 25-series one: 0x03 read, 0x02 page program
  with 0x06 write enable, 0x05 read status with WIP polling, 0x20 4 KB sector
  erase, 0xD8 64 KB block erase, 0x9F JEDEC ID.  Nothing here is vendor
  specific; the only part-specific knowledge is the 256-byte page, the 4 KB
  sector, the 64 KB block, and the identification bytes the caller checks.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_SPI_NOR_H_
#define A733_SPI_NOR_H__


#include <Uefi.h>

//
// The variable window: the variable store, the FTW working block and the FTW
// spare block, one 64 KB region each, at the end of the part.  Its chip offset
// depends on the capacity the JEDEC ID reports, so it is not a constant here;
// A733SpiNorSetCapacity bounds the command layer and the caller derives the
// offset.
//
//
// Where the variable window sits in the part.  The tail is not usable on this
// board (another firmware's store is there); 4 MB is blank and clear of the
// low region a boot image would occupy.
//
#define A733_SPI_NOR_STORE_OFFSET  0x400000U

#define A733_SPI_NOR_WINDOW_SIZE  0x00030000UL

//
// Geometry the command set is written against: a page is the largest single
// program operation and must not be crossed, a sector is the smallest erase
// unit (the FVB block size), and a block is the largest.
//
#define A733_SPI_NOR_PAGE_BYTES    0x100U
#define A733_SPI_NOR_SECTOR_BYTES  0x1000U
#define A733_SPI_NOR_BLOCK_BYTES   0x10000U

//
// Status register 1 bits that matter here.
//
#define A733_SPI_NOR_SR_WIP  BIT0
#define A733_SPI_NOR_SR_WEL  BIT1

/**
  Bring SPI0 up: CCU clock and gate, the four pads, then the controller.

  The controller is left in master mode, SPI mode 0 (CPOL = CPHA = 0), at
  SYS_CLK24M / 1 = 24 MHz, with the burst discarded rather than hashed and no
  sample-delay selection.  The chip select is left de-asserted under software
  control on chip select 0.

  @retval EFI_SUCCESS            SPI0 is up.
  @retval EFI_UNSUPPORTED        The CCU does not model a clock for SPI0.
  @retval other                  A pad, the module gate or the controller
                                 could not be configured.
**/
EFI_STATUS
EFIAPI
A733SpiNorInit (
  VOID
  );

/**
  DIAGNOSTIC: what a failing identification read did, burst by burst.

  Only reached when the JEDEC id is still not a W25Q64/W25Q128, which with the
  send-then-receive exchange should not happen.  Repeats the read over the four
  SPI modes x the two sample-data selections and, for each, prints whether the
  transmit burst and the receive burst each reported completion, how many bytes
  the RX FIFO gave up, and the bytes themselves, so a burst that never ran, a
  burst that ran against an empty FIFO, and a burst that brought back the wrong
  bytes can be told apart from the log alone.

  @param[in] Why  Short tag naming the caller.
**/
VOID
EFIAPI
A733SpiNorDiag (
  IN CONST CHAR8  *Why
  );

/**
  Read the JEDEC identification (0x9F).

  @param[out] Id  Three bytes: manufacturer, memory type, capacity.  The
                  capacity byte is the base-2 logarithm of the part's size in
                  bytes, which is what A733SpiNorSetCapacity expects.

  @retval EFI_SUCCESS      The three bytes were returned.
  @retval EFI_TIMEOUT      A burst did not complete.
  @retval EFI_DEVICE_ERROR The controller reported a FIFO error.
**/
EFI_STATUS
EFIAPI
A733SpiNorReadId (
  OUT UINT8  Id[3]
  );

/**
  Tell the command layer how large the part is.

  Every read, program and erase is bounds-checked against this, and the caller
  derives the variable window from it, so the two cannot disagree about where
  the part ends.  Until it is called, the command layer refuses every request.

  @param[in] CapacityBytes  Part size, a power of two.  At least the variable
                            window.

  @retval EFI_SUCCESS            The capacity was accepted.
  @retval EFI_INVALID_PARAMETER  It is zero, not a power of two, or smaller
                                 than one window.
**/
EFI_STATUS
EFIAPI
A733SpiNorSetCapacity (
  IN UINTN  CapacityBytes
  );

/**
  Read from the part.

  @param[in]  Offset  Byte offset in the part.
  @param[out] Buffer  Destination.
  @param[in]  Length  Byte count.

  @retval EFI_SUCCESS            The bytes were read.
  @retval EFI_INVALID_PARAMETER  The range is past the end of the part.
  @retval EFI_TIMEOUT            A burst did not complete.
  @retval EFI_DEVICE_ERROR       The controller reported a FIFO error.
**/
EFI_STATUS
EFIAPI
A733SpiNorRead (
  IN  UINTN  Offset,
  OUT VOID   *Buffer,
  IN  UINTN  Length
  );

/**
  Program the part, splitting at page boundaries.

  The target range must be erased: every byte written has to be reachable by
  clearing bits only.  That is the caller's contract, and it is the reason this
  function never erases.  Write enable is issued and checked per page, and each
  page's completion is waited out with a bounded timeout.

  @param[in] Offset  Byte offset in the part.
  @param[in] Buffer  Source.
  @param[in] Length  Byte count.

  @retval EFI_SUCCESS            The bytes were programmed.
  @retval EFI_INVALID_PARAMETER  The range is past the end of the part.
  @retval EFI_DEVICE_ERROR       The part refused write enable, did not finish,
                                 or the controller reported a FIFO error.
**/
EFI_STATUS
EFIAPI
A733SpiNorProgram (
  IN UINTN        Offset,
  IN CONST VOID   *Buffer,
  IN UINTN        Length
  );

/**
  Erase a range.

  The range must be sector aligned; a 64 KB unit is erased with 0xD8 when the
  unit is both aligned and complete, and with 0x20 sector erases otherwise.
  Every unit is verified to read back erased before the next one starts, so a
  part that silently ignores an erase is reported here rather than becoming a
  corrupt variable store.

  @param[in] Offset  Byte offset in the part.
  @param[in] Length  Byte count, a multiple of the sector size.

  @retval EFI_SUCCESS            The range is erased.
  @retval EFI_INVALID_PARAMETER  The range is past the end of the part or not
                                 sector aligned.
  @retval EFI_DEVICE_ERROR       The part refused write enable, did not finish,
                                 read back unerased, or the controller reported
                                 a FIFO error.
**/
EFI_STATUS
EFIAPI
A733SpiNorErase (
  IN UINTN  Offset,
  IN UINTN  Length
  );

/**
  Read the part's size and erase instructions from its SFDP table.

  @param[out] CapacityBytes  Die size in bytes.

  @retval EFI_SUCCESS      Parsed.
  @retval EFI_NOT_FOUND    No usable table; keep the JEDEC defaults.
  @retval other            A bus transaction failed.
**/
EFI_STATUS
EFIAPI
A733SpiNorSfdpInfo (
  OUT UINTN  *CapacityBytes
  );

#endif