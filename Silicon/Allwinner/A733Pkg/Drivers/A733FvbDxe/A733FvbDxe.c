/** @file
  EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL over the A733's SPI NOR variable window.

  Why this driver exists: VariableRuntimeDxe reads the variable store as plain
  memory at PcdFlashNvStorageVariableBase64 and only uses the block protocol
  for erase and program (VariableDxe.c: the GetVariable path never touches it,
  the write path always does).  A store on media that is not memory mapped -
  SPI NOR here - therefore needs a RAM window at exactly that address, kept in
  step with the media, and the window has to be EfiRuntimeServicesData or the
  OS reclaims it at ExitBootServices and an OS SetVariable writes into memory
  nobody owns.  This driver reserves that window, fills it from the NOR, and
  installs the block protocol on top of it.  Every write and erase goes to the
  NOR synchronously, so a variable is on the flash before SetVariable returns;
  there is no deferred flush to lose on a power cut.

  The window: 192 KB, laid out as the variable store (64 KB), the FTW working
  block (64 KB) and the FTW spare block (64 KB), at DRAM offset
  PcdFlashNvStorageVariableBase64 and at the *end* of the board's SPI NOR.  The
  offset in the flash follows the capacity the JEDEC identification reports -
  0x7D0000 on the W25Q64FW this board's schematic fits, 0xF00000 on a 16 MB
  part - so the window is always the last 192 KB of whatever answers, low
  memory (where a NOR boot image would live) is never touched, and the eMMC is
  not involved at all.  Block size is 4 KB: the NOR's sector erase unit, the
  granularity FTW works in, and the granularity the variable driver's own block
  map is formatted with.

  Three outcomes, and the driver never hangs on any of them:

  - The window holds a valid store: it is read in, and every write goes to the
    NOR.
  - The window is blank: it is erased and formatted, then used.
  - The NOR is missing, refuses the JEDEC identification, cannot be read, or
    the window is neither blank nor a valid store: nothing is erased, one line
    says why, and the window becomes a RAM-only store for this boot.  Variables
    then behave exactly as they did when the platform ran with
    PcdEmuVariableNvModeEnable, they just do not survive a power cycle.  The
    block protocol is still installed in that case: with the emulated store
    switched off, a store the variable driver can read but cannot write would
    be worse than one that works and forgets.

  The store is formatted here rather than shipped in a flash descriptor
  because the window is not part of the firmware volume: the header is built at
  run time from the PCD sizes, so the two cannot disagree.

  Written from the PI specification's firmware volume and variable store
  layouts and from this platform's own needs; no FVB driver was carried over
  from another tree, so there is no attribution line to make.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/VariableFormat.h>
#include <Pi/PiFirmwareVolume.h>
#include <PiDxe.h>
#include <Protocol/FirmwareVolumeBlock.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiRuntimeLib.h>

#include <A733Spi.h>

#include "A733SpiNor.h"
#include <A733.h>
#include <A733Ccu.h>
#include <Library/IoLib.h>
#include <Library/IoLib.h>
#include <A733Ccu.h>

//
// The block protocol's unit is the NOR's sector erase size, which is also what
// the store's own block map has to say for the variable driver's address to
// LBA arithmetic (Variable.c, UpdateVariableStore) to line up.
//
#define A733_FVB_BLOCK_SIZE  A733_SPI_NOR_SECTOR_BYTES

//
// A header plus one block map entry, i.e. room for the terminator entry after
// the one inside EFI_FIRMWARE_VOLUME_HEADER.  This is the length the store's
// variable store header sits at, and the length the FV checksum covers.
//
#define A733_FVB_HEADER_LENGTH  (sizeof (EFI_FIRMWARE_VOLUME_HEADER) + \
                                 sizeof (EFI_FV_BLOCK_MAP_ENTRY))

//
// A header plus the variable store header: what the format path programs and
// what the validation path parses.  Everything after it is erased store data.
//
#define A733_FVB_FORMAT_BYTES  (A733_FVB_HEADER_LENGTH + \
                                sizeof (VARIABLE_STORE_HEADER))

//
// The media is erased to all ones, it has no lock bits exposed to this
// interface, and writes only clear bits.  Both consumers of this protocol
// (FaultTolerantWriteDxe and VariableRuntimeDxe) require WRITE_STATUS.
//
#define A733_FVB_ATTRIBUTES  (EFI_FVB2_READ_ENABLED_CAP | \
                              EFI_FVB2_READ_STATUS        | \
                              EFI_FVB2_WRITE_ENABLED_CAP  | \
                              EFI_FVB2_WRITE_STATUS       | \
                              EFI_FVB2_ERASE_POLARITY)

//
// The window's three regions, as the PCDs describe them, and the RAM copy of
// them.  Shadow is a pointer, not an address, so the virtual address change
// handler has something to convert.
//
typedef struct {
  UINT8    *Shadow;
  UINTN    Base;
  UINTN    VariableSize;
  UINTN    WorkingSize;
  UINTN    SpareSize;
  UINTN    TotalSize;
  UINTN    BlockCount;
  UINTN    WindowOffset;
  BOOLEAN  NorBacked;
} A733_FVB_PRIVATE;

STATIC A733_FVB_PRIVATE                    mFvbPrivate;
STATIC EFI_EVENT                           mVirtualAddressChangeEvent;
STATIC EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  mFvbProtocol;

/**
  The offset in the NOR that corresponds to an offset in the window.

  @param[in] Offset  Offset in the window.

  @return The chip offset.
**/
STATIC
UINTN
FvbNorOffset (
  IN UINTN  Offset
  )
{
  return mFvbPrivate.WindowOffset + Offset;
}

/**
  Sum a firmware volume header as 16-bit words.

  A valid header sums to zero, which is both how it is validated and how the
  value to store in its Checksum field is derived.

  @param[in] Header  The header, starting at the volume's base.
  @param[in] Length  HeaderLength bytes, a multiple of two.

  @return The 16-bit sum.
**/
STATIC
UINT16
FvbHeaderSum (
  IN CONST UINT8  *Header,
  IN UINTN        Length
  )
{
  UINT16  Sum;
  UINTN   Index;

  Sum = 0;
  for (Index = 0; Index + 1 < Length; Index += 2) {
    Sum = (UINT16)(Sum + (UINT16)(Header[Index] | (Header[Index + 1] << 8)));
  }

  return Sum;
}

/**
  Fill in a firmware volume header and the variable store header behind it.

  The buffer must already be 0xFF where it is not a header field, so the store
  starts out erased.  Field values that are not sizes come from the PI
  specification: an NV data volume, revision 2, one block map entry covering
  the whole window, and an authenticated variable store, which is the format
  the variable driver's own emulated store uses and therefore the one this
  port has been running.

  @param[out] Buffer  At least A733_FVB_FORMAT_BYTES long.
**/
STATIC
VOID
FvbBuildHeader (
  OUT UINT8  *Buffer
  )
{
  EFI_FIRMWARE_VOLUME_HEADER  *FvHeader;
  EFI_FV_BLOCK_MAP_ENTRY      *BlockMap;
  VARIABLE_STORE_HEADER       *Store;
  UINT16                      Checksum;

  SetMem (Buffer, A733_FVB_FORMAT_BYTES, 0xFF);

  FvHeader = (EFI_FIRMWARE_VOLUME_HEADER *)Buffer;
  SetMem (FvHeader->ZeroVector, sizeof (FvHeader->ZeroVector), 0);
  CopyGuid (&FvHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid);
  FvHeader->FvLength        = mFvbPrivate.TotalSize;
  FvHeader->Signature       = EFI_FVH_SIGNATURE;
  FvHeader->Attributes      = (UINT32)A733_FVB_ATTRIBUTES;
  FvHeader->HeaderLength    = (UINT16)A733_FVB_HEADER_LENGTH;
  FvHeader->ExtHeaderOffset = 0;
  FvHeader->Reserved[0]     = 0;
  FvHeader->Revision        = EFI_FVH_REVISION;

  //
  // Two entries, the second the terminator, which is what HeaderLength above
  // counts room for.  Addressed through a pointer because the structure
  // declares only one.
  //
  BlockMap              = (EFI_FV_BLOCK_MAP_ENTRY *)(FvHeader->BlockMap);
  BlockMap[0].NumBlocks = (UINT32)mFvbPrivate.BlockCount;
  BlockMap[0].Length    = A733_FVB_BLOCK_SIZE;
  BlockMap[1].NumBlocks = 0;
  BlockMap[1].Length    = 0;

  //
  // The checksum field has to be zero while the sum is taken; the value
  // written is then the one that makes the whole header sum to zero.
  //
  FvHeader->Checksum = 0;
  Checksum           = FvbHeaderSum (Buffer, FvHeader->HeaderLength);
  FvHeader->Checksum = (UINT16)(0x10000U - Checksum);

  Store = (VARIABLE_STORE_HEADER *)(Buffer + FvHeader->HeaderLength);
  CopyGuid (&Store->Signature, &gEfiAuthenticatedVariableGuid);
  Store->Size      = (UINT32)(mFvbPrivate.VariableSize -
                              FvHeader->HeaderLength);
  Store->Format    = VARIABLE_STORE_FORMATTED;
  Store->State     = VARIABLE_STORE_HEALTHY;
  Store->Reserved  = 0;
  Store->Reserved1 = 0;
}

/**
  Decide whether a window holds a store this driver can use.

  Every field the variable driver will act on is checked, so that a store that
  would fail later fails here, while nothing has been erased yet.

  @param[in] Base  The window.

  @retval TRUE   A consistent store header and block map are present.
  @retval FALSE  It is not, or it is not the store these PCDs describe.
**/
STATIC
BOOLEAN
FvbHeaderIsValid (
  IN CONST UINT8  *Base
  )
{
  CONST EFI_FIRMWARE_VOLUME_HEADER  *FvHeader;
  CONST VARIABLE_STORE_HEADER       *Store;

  FvHeader = (CONST EFI_FIRMWARE_VOLUME_HEADER *)Base;

  if (FvHeader->Signature != EFI_FVH_SIGNATURE) {
    return FALSE;
  }

  if (!CompareGuid (&FvHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid)) {
    return FALSE;
  }

  if (FvHeader->Revision != EFI_FVH_REVISION) {
    return FALSE;
  }

  if (FvHeader->HeaderLength != A733_FVB_HEADER_LENGTH) {
    return FALSE;
  }

  if (FvHeader->FvLength != mFvbPrivate.TotalSize) {
    return FALSE;
  }

  if ((FvHeader->BlockMap[0].NumBlocks != mFvbPrivate.BlockCount) ||
      (FvHeader->BlockMap[0].Length != A733_FVB_BLOCK_SIZE))
  {
    return FALSE;
  }

  if (FvbHeaderSum (Base, FvHeader->HeaderLength) != 0) {
    return FALSE;
  }

  Store = (CONST VARIABLE_STORE_HEADER *)(Base + FvHeader->HeaderLength);
  if (!CompareGuid (&Store->Signature, &gEfiAuthenticatedVariableGuid)) {
    return FALSE;
  }

  if (Store->Size != (UINT32)(mFvbPrivate.VariableSize -
                              FvHeader->HeaderLength))
  {
    return FALSE;
  }

  if ((Store->Format != VARIABLE_STORE_FORMATTED) ||
      (Store->State != VARIABLE_STORE_HEALTHY))
  {
    return FALSE;
  }

  return TRUE;
}

/**
  Check that a window reads as erased.

  @param[in] Base    The window.
  @param[in] Length  Byte count.

  @retval TRUE   Every byte is 0xFF.
  @retval FALSE  Some byte is not.
**/
STATIC
BOOLEAN
FvbIsBlank (
  IN CONST UINT8  *Base,
  IN UINTN        Length
  )
{
  UINTN  Index;

  for (Index = 0; Index < Length; Index++) {
    if (Base[Index] != 0xFF) {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Erase the variable store region and program a fresh header into it.

  Only the variable store region is touched: the FTW working and spare blocks
  are left erased for FaultTolerantWriteDxe to initialise, which is what it
  does on a virgin flash anyway.

  The header is read back and compared before the window is used, because a
  program path that reports success without writing - a write-protected part,
  a mis-sampled bus - would otherwise turn into a store that fails to parse on
  the next boot with no record of why.

  @retval EFI_SUCCESS  The region is erased, formatted and verified.
  @retval other        The NOR refused an operation; nothing may be trusted.
**/
STATIC
EFI_STATUS
FvbFormatNor (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Header[A733_FVB_FORMAT_BYTES];
  UINT8       Verify[A733_FVB_FORMAT_BYTES];

  FvbBuildHeader (Header);

  Status = A733SpiNorErase (
             FvbNorOffset (0),
             mFvbPrivate.VariableSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Fvb: cannot erase the store: %r\n", Status));
    return Status;
  }

  Status = A733SpiNorProgram (
             FvbNorOffset (0),
             Header,
             sizeof (Header)
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Fvb: cannot write the store header: %r\n",
            Status));
    return Status;
  }

  Status = A733SpiNorRead (
             FvbNorOffset (0),
             Verify,
             sizeof (Verify)
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Fvb: cannot read the store header back: %r\n",
            Status));
    return Status;
  }

  if (CompareMem (Header, Verify, sizeof (Header)) != 0) {
    DEBUG ((DEBUG_ERROR, "A733Fvb: the store header did not read back\n"));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Bring the NOR up and, if it can carry the store, fill the window from it.

  This is where every safety rule of the driver lives: the part has to identify
  itself as a W25Q128, the window has to read, and it has to be either blank or
  hold a store this driver recognises before anything is erased.  A window that
  is neither is left exactly as it is.

  On success the window holds the store and mFvbPrivate.NorBacked is set.  On
  any failure the window's contents are undefined and the caller replaces them
  with a RAM-only store.

  @retval TRUE   The window is NOR-backed.
  @retval FALSE  It is not; the reason has been logged.
**/
STATIC
BOOLEAN
FvbTryNor (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Id[3];
  UINTN       Capacity;
  UINTN       SfdpCapacity;
  UINTN       Offset;

  Status = A733SpiNorInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Fvb: SPI0 will not come up: %r\n", Status));
    return FALSE;
  }

  Status = A733SpiNorReadId (Id);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Fvb: no JEDEC ID from the NOR: %r\n", Status));
    return FALSE;
  }

  //
  // Winbond, a W25Q 25-series part, and an 8 MB or 16 MB die: the board's U7
  // is a W25Q64FWZPIG (8 MB, ID EF 60 17), a W25Q128 would be EF 40 18, and
  // anything else is not a part this window was laid out for.  The capacity
  // byte is log2(bytes), so it is also what says where the part ends - and the
  // window is the last A733_SPI_NOR_WINDOW_SIZE bytes of it.  Getting this
  // wrong is the one mistake that could erase somebody else's data, which is
  // why it is a refusal here rather than a guess.
  //
  if ((Id[0] != 0xEF) || ((Id[1] != 0x40) && (Id[1] != 0x60)) ||
      ((Id[2] != 0x17) && (Id[2] != 0x18)))
  {
    DEBUG ((
      DEBUG_WARN,
      "A733Fvb: JEDEC ID %02x %02x %02x is not a W25Q64/W25Q128, NOR unused\n",
      Id[0],
      Id[1],
      Id[2]
      ));

    //
    // DIAGNOSTIC: the ID is the first transaction this driver makes, so a wrong
    // one says nothing about which layer is at fault.  Print the three that
    // could be: the CCU's view of SPI0's clock and gate, the PIO's view of the
    // four pin multiplexers, and the controller's own state after the burst.
    //
    {
      UINTN  Ccu;
      UINT32  Cfg0;
      UINT32  Cfg1;

      Ccu  = (UINTN)PcdGet64 (PcdA733CcuBase);
      Cfg0 = MmioRead32 ((UINTN)PcdGet64 (PcdA733GpioBase) + 0x180U);
      Cfg1 = MmioRead32 ((UINTN)PcdGet64 (PcdA733GpioBase) + 0x190U);

      DEBUG ((
        DEBUG_ERROR,
        "A733Fvb diag: CCU SPI0_CLK %08x BGR %08x\n",
        MmioRead32 (Ccu + A733_CCU_SPI0_CLK),
        MmioRead32 (Ccu + A733_CCU_SPI0_BGR)
        ));
      DEBUG ((
        DEBUG_ERROR,
        "A733Fvb diag: PC_CFG0 %08x (PC2 mux %u, PC3 mux %u, PC4 mux %u) PC_CFG1 %08x (PC12 mux %u)\n",
        Cfg0,
        (Cfg0 >> 8) & 0xFU,
        (Cfg0 >> 12) & 0xFU,
        (Cfg0 >> 16) & 0xFU,
        Cfg1,
        (Cfg1 >> 16) & 0xFU
        ));
      DEBUG ((
        DEBUG_ERROR,
        "A733Fvb diag: SPI GCR %08x TCR %08x ISR %08x FSR %08x MBC %08x MTC %08x BCC %08x\n",
        MmioRead32 (A733_SPI0_BASE + A733_SPI_GCR),
        MmioRead32 (A733_SPI0_BASE + A733_SPI_TCR),
        MmioRead32 (A733_SPI0_BASE + A733_SPI_ISR),
        MmioRead32 (A733_SPI0_BASE + A733_SPI_FSR),
        MmioRead32 (A733_SPI0_BASE + A733_SPI_MBC),
        MmioRead32 (A733_SPI0_BASE + A733_SPI_MTC),
        MmioRead32 (A733_SPI0_BASE + A733_SPI_BCC)
        ));
    }

    A733SpiNorDiag ("JEDEC id rejected");

    return FALSE;
  }

  //
  // Prefer what the part reports through SFDP (JESD216) over the JEDEC
  // capacity byte: it is a direct statement of the die size, and it also
  // supplies the erase instructions.  The byte is the fallback for parts
  // without a usable SFDP table.
  //
  Capacity = (UINTN)1 << Id[2];
  if (!EFI_ERROR (A733SpiNorSfdpInfo (&SfdpCapacity)) && (SfdpCapacity != 0)) {
    Capacity = SfdpCapacity;
  } else {
    DEBUG ((
      DEBUG_INFO,
      "A733Fvb: no SFDP table, using the JEDEC capacity byte (1 << %u)\n",
      (UINT32)Id[2]
      ));
  }

  Status = A733SpiNorSetCapacity (Capacity);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Fvb: capacity 1<<%u is unusable: %r\n",
            (UINT32)Id[2], Status));
    return FALSE;
  }

  //
  // Where in the part the store lives.  Not the tail: on this board the last
  // 192 KB already holds another firmware's variable store (the reads show a
  // store's data area there, and no `_FVH` of the geometry above), while the
  // first four megabytes read blank.  A window at 4 MB is therefore both free
  // and far from the low region a SPI-NOR boot image would occupy.
  //
  mFvbPrivate.WindowOffset = A733_SPI_NOR_STORE_OFFSET;
  if (mFvbPrivate.WindowOffset + A733_SPI_NOR_WINDOW_SIZE > ((UINTN)1 << Id[2])) {
    DEBUG ((
      DEBUG_WARN,
      "A733Fvb: the part is too small for a store at 0x%x\n",
      (UINT32)A733_SPI_NOR_STORE_OFFSET
      ));
    return FALSE;
  }

  Status = A733SpiNorRead (
             FvbNorOffset (0),
             mFvbPrivate.Shadow,
             mFvbPrivate.TotalSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Fvb: cannot read the NOR window: %r\n", Status));
    return FALSE;
  }

  if (FvbIsBlank (mFvbPrivate.Shadow, mFvbPrivate.TotalSize)) {
    Status = FvbFormatNor ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "A733Fvb: NOR window left unformatted\n"));
      return FALSE;
    }

    FvbBuildHeader (mFvbPrivate.Shadow);

    DEBUG ((
      DEBUG_INFO,
      "A733Fvb: formatted %u KB at NOR 0x%06x (ID %02x %02x %02x)\n",
      (UINT32)(mFvbPrivate.VariableSize / SIZE_1KB),
      (UINT32)mFvbPrivate.WindowOffset,
      Id[0],
      Id[1],
      Id[2]
      ));
    return TRUE;
  }

  if (FvbHeaderIsValid (mFvbPrivate.Shadow)) {
    DEBUG ((
      DEBUG_INFO,
      "A733Fvb: variables at 0x%08x backed by NOR 0x%06x (ID %02x %02x %02x)\n",
      (UINT32)mFvbPrivate.Base,
      (UINT32)mFvbPrivate.WindowOffset,
      Id[0],
      Id[1],
      Id[2]
      ));
    return TRUE;
  }

  //
  // Not blank and not a store: erase nothing.  Whatever is there was not put
  // there by this driver, or was damaged, and guessing would cost data that
  // may still mean something to somebody.
  //
  Offset = 0;
  while ((Offset < mFvbPrivate.TotalSize) &&
         (mFvbPrivate.Shadow[Offset] == 0xFF))
  {
    Offset++;
  }

  DEBUG ((
    DEBUG_WARN,
    "A733Fvb: NOR window is neither blank nor a store (0x%02x at +0x%x),"
    " left untouched\n",
    (UINT32)((Offset < mFvbPrivate.TotalSize) ?
             mFvbPrivate.Shadow[Offset] : 0xFF),
    (UINT32)Offset
    ));


  return FALSE;
}

/**
  Describe SPI0 as runtime device memory.

  A variable written from the OS runs this driver's write path after
  ExitBootServices, so the controller's registers have to be in the memory map
  the OS is given, marked runtime.  Without this the map has no descriptor for
  the window at all and the OS maps nothing there.

  Failure is logged and not fatal: reads and writes still work during boot, and
  the variable driver is far more useful with a store that might fault at
  runtime than with none.
**/
STATIC
VOID
FvbMarkSpiRuntime (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeMemoryMappedIo,
                  A733_SPI0_BASE,
                  A733_SPI0_WINDOW_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                  );
  if (Status == EFI_ACCESS_DENIED) {
    // Somebody already described the window; the attributes below still apply.
    Status = EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Fvb: SPI0 not added to the memory map: %r\n",
      Status
      ));
    return;
  }

  Status = gDS->SetMemorySpaceAttributes (
                  A733_SPI0_BASE,
                  A733_SPI0_WINDOW_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Fvb: SPI0 is not runtime memory: %r\n",
      Status
      ));
  }

}

/**
  Read the window's geometry from the PCDs and check that it is one region.

  The driver treats the three areas as one contiguous span, at one NOR offset,
  because that is what makes an address in the window an offset in the flash
  with no table in between.  A platform that moves one of them has to move the
  others with it, and this is where it finds that out.

  @retval EFI_SUCCESS            The geometry is consistent.
  @retval EFI_INVALID_PARAMETER  A size is zero, misaligned or the areas are
                                 not contiguous.
  @retval EFI_UNSUPPORTED        The window is not the size the NOR layout
                                 reserves.
**/
STATIC
EFI_STATUS
FvbReadGeometry (
  VOID
  )
{
  UINTN  Base;
  UINTN  WorkBase;
  UINTN  SpareBase;

  Base = (UINTN)(PcdGet64 (PcdFlashNvStorageVariableBase64) != 0 ?
                 PcdGet64 (PcdFlashNvStorageVariableBase64) :
                 PcdGet32 (PcdFlashNvStorageVariableBase));
  WorkBase = (UINTN)(PcdGet64 (PcdFlashNvStorageFtwWorkingBase64) != 0 ?
                     PcdGet64 (PcdFlashNvStorageFtwWorkingBase64) :
                     PcdGet32 (PcdFlashNvStorageFtwWorkingBase));
  SpareBase = (UINTN)(PcdGet64 (PcdFlashNvStorageFtwSpareBase64) != 0 ?
                      PcdGet64 (PcdFlashNvStorageFtwSpareBase64) :
                      PcdGet32 (PcdFlashNvStorageFtwSpareBase));

  mFvbPrivate.Base         = Base;
  mFvbPrivate.VariableSize = PcdGet32 (PcdFlashNvStorageVariableSize);
  mFvbPrivate.WorkingSize  = PcdGet32 (PcdFlashNvStorageFtwWorkingSize);
  mFvbPrivate.SpareSize    = PcdGet32 (PcdFlashNvStorageFtwSpareSize);

  if ((mFvbPrivate.VariableSize == 0) || (mFvbPrivate.WorkingSize == 0) ||
      (mFvbPrivate.SpareSize == 0))
  {
    DEBUG ((DEBUG_ERROR, "A733Fvb: a window region has no size\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (((Base % A733_FVB_BLOCK_SIZE) != 0) ||
      ((mFvbPrivate.VariableSize % A733_FVB_BLOCK_SIZE) != 0) ||
      ((mFvbPrivate.WorkingSize % A733_FVB_BLOCK_SIZE) != 0) ||
      ((mFvbPrivate.SpareSize % A733_FVB_BLOCK_SIZE) != 0))
  {
    DEBUG ((DEBUG_ERROR, "A733Fvb: window is not block size aligned\n"));
    return EFI_INVALID_PARAMETER;
  }

  if ((Base + mFvbPrivate.VariableSize != WorkBase) ||
      (WorkBase + mFvbPrivate.WorkingSize != SpareBase))
  {
    DEBUG ((
      DEBUG_ERROR,
      "A733Fvb: window is not contiguous: 0x%x+0x%x, 0x%x+0x%x, 0x%x+0x%x\n",
      (UINT32)Base,
      (UINT32)mFvbPrivate.VariableSize,
      (UINT32)WorkBase,
      (UINT32)mFvbPrivate.WorkingSize,
      (UINT32)SpareBase,
      (UINT32)mFvbPrivate.SpareSize
      ));
    return EFI_INVALID_PARAMETER;
  }

  mFvbPrivate.TotalSize = mFvbPrivate.VariableSize +
                          mFvbPrivate.WorkingSize +
                          mFvbPrivate.SpareSize;
  mFvbPrivate.BlockCount = mFvbPrivate.TotalSize / A733_FVB_BLOCK_SIZE;

  if (mFvbPrivate.TotalSize != A733_SPI_NOR_WINDOW_SIZE) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Fvb: a 0x%x-byte window does not fit the NOR layout's 0x%x\n",
      (UINT32)mFvbPrivate.TotalSize,
      (UINT32)A733_SPI_NOR_WINDOW_SIZE
      ));
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

/**
  Reserve the window as runtime memory and fill it from the NOR or with a
  fresh store.

  @retval EFI_SUCCESS  The window is reserved and holds a usable store.
  @retval other        The window could not be reserved; nothing else can be
                       done without it.
**/
STATIC
EFI_STATUS
FvbReserveWindow (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_PHYSICAL_ADDRESS             Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;

  Address = (EFI_PHYSICAL_ADDRESS)mFvbPrivate.Base;
  Status  = gBS->AllocatePages (
                   AllocateAddress,
                   EfiRuntimeServicesData,
                   EFI_SIZE_TO_PAGES (mFvbPrivate.TotalSize),
                   &Address
                   );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Fvb: cannot reserve 0x%x at 0x%08x: %r\n",
      (UINT32)mFvbPrivate.TotalSize,
      (UINT32)mFvbPrivate.Base,
      Status
      ));

    if (!EFI_ERROR (gDS->GetMemorySpaceDescriptor (mFvbPrivate.Base,
                                                   &Descriptor)))
    {
      DEBUG ((
        DEBUG_ERROR,
        "A733Fvb: 0x%08x is already GCD type %u attributes 0x%lx\n",
        (UINT32)mFvbPrivate.Base,
        (UINT32)Descriptor.GcdMemoryType,
        Descriptor.Attributes
        ));
    }

    return Status;
  }

  mFvbPrivate.Shadow = (UINT8 *)(UINTN)Address;

  //
  // 0xFF, not zero: this window is a mirror of erased flash, and it is what
  // the variable driver parses when the NOR cannot be reached.
  //
  SetMem (mFvbPrivate.Shadow, mFvbPrivate.TotalSize, 0xFF);

  mFvbPrivate.NorBacked = FvbTryNor ();
  if (!mFvbPrivate.NorBacked) {
    SetMem (mFvbPrivate.Shadow, mFvbPrivate.TotalSize, 0xFF);
    FvbBuildHeader (mFvbPrivate.Shadow);
    DEBUG ((
      DEBUG_WARN,
      "A733Fvb: variables at 0x%08x are RAM-only this boot\n",
      (UINT32)mFvbPrivate.Base
      ));
  }

  return EFI_SUCCESS;
}

/**
  Convert this driver's pointers when the OS sets a virtual address map.

  The window pointer is the one that matters: it is dereferenced by the write
  path at runtime.  The protocol's entry points are converted too - the
  variable driver converts its own copy of them, but a runtime driver owes the
  protocol it published the same courtesy.

  @param[in] Event    The virtual address change event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
FvbVirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  (VOID)Context;

  EfiConvertPointer (0x0, (VOID **)&mFvbPrivate.Shadow);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.GetAttributes);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.SetAttributes);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.GetPhysicalAddress);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.GetBlockSize);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.Read);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.Write);
  EfiConvertPointer (0x0, (VOID **)&mFvbProtocol.EraseBlocks);
}

/**
  Return the volume's attributes.

  @param[in]  This        The protocol instance.
  @param[out] Attributes  The attributes.

  @retval EFI_SUCCESS            Attributes was filled in.
  @retval EFI_INVALID_PARAMETER  Attributes is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
A733FvbGetAttributes (
  IN  CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  OUT EFI_FVB_ATTRIBUTES_2                      *Attributes
  )
{
  (VOID)This;

  if (Attributes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Attributes = A733_FVB_ATTRIBUTES;
  return EFI_SUCCESS;
}

/**
  Accept an attribute change.

  The NOR's own protection bits are not exposed through this interface, so
  every attribute the caller can ask for here is already the state of the
  media: nothing is written and nothing is refused.

  @param[in]     This        The protocol instance.
  @param[in,out] Attributes  The requested attributes.

  @retval EFI_SUCCESS  Always.
**/
STATIC
EFI_STATUS
EFIAPI
A733FvbSetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN OUT EFI_FVB_ATTRIBUTES_2                  *Attributes
  )
{
  (VOID)This;
  (VOID)Attributes;

  return EFI_SUCCESS;
}

/**
  Return the address of the window this volume is backed by.

  This is the address the variable driver compares its own PCD-derived base
  against, so it is the window's base, not the NOR's.

  @param[in]  This     The protocol instance.
  @param[out] Address  The window's base address.

  @retval EFI_SUCCESS            Address was filled in.
  @retval EFI_INVALID_PARAMETER  Address is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
A733FvbGetPhysicalAddress (
  IN  CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  OUT EFI_PHYSICAL_ADDRESS                      *Address
  )
{
  (VOID)This;

  if (Address == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Address = (EFI_PHYSICAL_ADDRESS)(UINTN)mFvbPrivate.Shadow;
  return EFI_SUCCESS;
}

/**
  Return the block size and count of the volume.

  One block size for the whole window: every consumer of this protocol assumes
  that, and the window is laid out so that it holds.

  @param[in]  This            The protocol instance.
  @param[in]  Lba             Block to ask about.
  @param[out] BlockSize       Bytes per block.
  @param[out] NumberOfBlocks  Blocks in the volume.

  @retval EFI_SUCCESS            The geometry was filled in.
  @retval EFI_INVALID_PARAMETER  A pointer is NULL or Lba is out of range.
**/
STATIC
EFI_STATUS
EFIAPI
A733FvbGetBlockSize (
  IN  CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN  EFI_LBA                                   Lba,
  OUT UINTN                                     *BlockSize,
  OUT UINTN                                     *NumberOfBlocks
  )
{
  (VOID)This;

  if ((BlockSize == NULL) || (NumberOfBlocks == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Lba >= (EFI_LBA)mFvbPrivate.BlockCount) {
    return EFI_INVALID_PARAMETER;
  }

  *BlockSize      = A733_FVB_BLOCK_SIZE;
  *NumberOfBlocks = mFvbPrivate.BlockCount;
  return EFI_SUCCESS;
}

/**
  Check that a request lies inside one block of the window.

  @param[in] Lba         Target block.
  @param[in] Offset      Offset inside the block.
  @param[in] BufferSize  Bytes requested.

  @retval EFI_SUCCESS            The range is inside the block.
  @retval EFI_INVALID_PARAMETER  It is not.
**/
STATIC
EFI_STATUS
FvbCheckRange (
  IN EFI_LBA  Lba,
  IN UINTN    Offset,
  IN UINTN    BufferSize
  )
{
  if (Lba >= (EFI_LBA)mFvbPrivate.BlockCount) {
    return EFI_INVALID_PARAMETER;
  }

  if (Offset >= A733_FVB_BLOCK_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize > A733_FVB_BLOCK_SIZE - Offset) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Read from the window.

  The window is the authority for reads: it is written in the same call that
  programs the NOR, so a caller that has just written sees its own data even
  if the NOR could not be reached, and no read here can fail.

  @param[in]     This        The protocol instance.
  @param[in]     Lba         Block to read.
  @param[in]     Offset      Offset inside the block.
  @param[in,out] BufferSize  Bytes to read, then bytes read.
  @param[out]    Buffer      Destination.

  @retval EFI_SUCCESS            The bytes were copied.
  @retval EFI_INVALID_PARAMETER  A pointer is NULL or the range is invalid.
**/
STATIC
EFI_STATUS
EFIAPI
A733FvbRead (
  IN     CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN     EFI_LBA                                   Lba,
  IN     UINTN                                     Offset,
  IN OUT UINTN                                     *BufferSize,
  OUT    UINT8                                     *Buffer
  )
{
  EFI_STATUS  Status;

  (VOID)This;

  if ((BufferSize == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvbCheckRange (Lba, Offset, *BufferSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (
    Buffer,
    mFvbPrivate.Shadow + (UINTN)Lba * A733_FVB_BLOCK_SIZE + Offset,
    *BufferSize
    );

  return EFI_SUCCESS;
}

/**
  Write to the window and to the NOR.

  The range must already be erased: a 25-series part can only clear bits, so a
  byte that would have to gain a one is refused rather than programmed into
  something neither the caller nor this driver asked for.  The check is made
  against the window, which is the same data the NOR holds, and it is what
  turns a caller that forgot to erase into an error instead of a corrupt
  store.

  @param[in]     This        The protocol instance.
  @param[in]     Lba         Block to write.
  @param[in]     Offset      Offset inside the block.
  @param[in,out] NumBytes    Bytes to write, then bytes written.
  @param[in]     Buffer      Source.

  @retval EFI_SUCCESS            The bytes are in the NOR and in the window.
  @retval EFI_INVALID_PARAMETER  A pointer is NULL or the range is invalid.
  @retval EFI_ACCESS_DENIED      The range is not erased.
  @retval EFI_DEVICE_ERROR       The NOR refused or failed the program.
**/
STATIC
EFI_STATUS
EFIAPI
A733FvbWrite (
  IN     CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  IN     EFI_LBA                                   Lba,
  IN     UINTN                                     Offset,
  IN OUT UINTN                                     *NumBytes,
  IN     UINT8                                     *Buffer
  )
{
  EFI_STATUS  Status;
  UINT8       *Target;
  UINTN       Index;

  (VOID)This;

  if ((NumBytes == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = FvbCheckRange (Lba, Offset, *NumBytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Target = mFvbPrivate.Shadow + (UINTN)Lba * A733_FVB_BLOCK_SIZE + Offset;

  for (Index = 0; Index < *NumBytes; Index++) {
    if ((Target[Index] & Buffer[Index]) != Buffer[Index]) {
      DEBUG ((
        DEBUG_ERROR,
        "A733Fvb: write of 0x%x bytes at 0x%08x is not into erased space"
        " (byte 0x%x: 0x%02x over 0x%02x)\n",
        (UINT32)*NumBytes,
        (UINT32)(mFvbPrivate.Base + (UINTN)Lba * A733_FVB_BLOCK_SIZE + Offset),
        (UINT32)Index,
        Buffer[Index],
        Target[Index]
        ));
      return EFI_ACCESS_DENIED;
    }
  }

  if (mFvbPrivate.NorBacked) {
    Status = A733SpiNorProgram (
               FvbNorOffset ((UINTN)Lba * A733_FVB_BLOCK_SIZE + Offset),
               Buffer,
               *NumBytes
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "A733Fvb: program at 0x%08x failed: %r\n",
        (UINT32)(mFvbPrivate.Base + (UINTN)Lba * A733_FVB_BLOCK_SIZE + Offset),
        Status
        ));
      return Status;
    }
  }

  CopyMem (Target, Buffer, *NumBytes);
  return EFI_SUCCESS;
}

/**
  Erase blocks of the window and of the NOR.

  The block list is the protocol's own shape: (Lba, NumBlocks) pairs ended by
  EFI_LBA_LIST_TERMINATOR.  The whole list is erased even when the caller
  breaks it into single blocks, so a run of adjacent 4 KB sectors becomes one
  64 KB block erase where the alignment allows it.

  @param[in] This  The protocol instance.
  @param[in] ...   (EFI_LBA Lba, UINTN NumBlocks) pairs, then
                   EFI_LBA_LIST_TERMINATOR.

  @retval EFI_SUCCESS            Every listed range is erased.
  @retval EFI_INVALID_PARAMETER  A listed range is outside the window.
  @retval EFI_DEVICE_ERROR       The NOR refused or failed an erase.
**/
STATIC
EFI_STATUS
EFIAPI
A733FvbEraseBlocks (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL  *This,
  ...
  )
{
  VA_LIST     Args;
  EFI_STATUS  Status;
  EFI_LBA     Lba;
  UINTN       NumBlocks;
  UINTN       Offset;
  UINTN       Length;

  (VOID)This;

  VA_START (Args, This);
  for ( ; ;) {
    Lba = VA_ARG (Args, EFI_LBA);
    if (Lba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    NumBlocks = VA_ARG (Args, UINTN);
    if ((NumBlocks == 0) ||
        (Lba + NumBlocks > (EFI_LBA)mFvbPrivate.BlockCount))
    {
      VA_END (Args);
      return EFI_INVALID_PARAMETER;
    }

    Offset = (UINTN)Lba * A733_FVB_BLOCK_SIZE;
    Length = NumBlocks * A733_FVB_BLOCK_SIZE;

    if (mFvbPrivate.NorBacked) {
      Status = A733SpiNorErase (FvbNorOffset (Offset), Length);
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "A733Fvb: erase of 0x%x bytes at 0x%08x failed: %r\n",
          (UINT32)Length,
          (UINT32)(mFvbPrivate.Base + Offset),
          Status
          ));
        VA_END (Args);
        return Status;
      }
    }

    SetMem (mFvbPrivate.Shadow + Offset, Length, 0xFF);
  }

  VA_END (Args);
  return EFI_SUCCESS;
}

/**
  Reserve the variable window and publish it as a firmware volume block device.

  Order matters here, and it is the order the two consumers need: the window is
  reserved and filled first, the block protocol is published next (so
  FaultTolerantWriteDxe can find the volume its depex waits for), and
  gEdkiiNvVarStoreFormattedGuid last - that GUID is the platform's statement
  that the store's headers are in place, and VariableRuntimeDxe is linked
  against the library that makes it wait for exactly that.

  @param[in] ImageHandle  This image.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  The store is initialised and published.
  @retval other        The window could not be reserved; the boot continues
                       without variable services.
**/
EFI_STATUS
EFIAPI
A733FvbDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Status = FvbReadGeometry ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FvbReserveWindow ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (mFvbPrivate.NorBacked) {
    FvbMarkSpiRuntime ();
  }

  mFvbProtocol.GetAttributes      = A733FvbGetAttributes;
  mFvbProtocol.SetAttributes      = A733FvbSetAttributes;
  mFvbProtocol.GetPhysicalAddress = A733FvbGetPhysicalAddress;
  mFvbProtocol.GetBlockSize       = A733FvbGetBlockSize;
  mFvbProtocol.Read               = A733FvbRead;
  mFvbProtocol.Write              = A733FvbWrite;
  mFvbProtocol.EraseBlocks        = A733FvbEraseBlocks;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  FvbVirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &mVirtualAddressChangeEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Fvb: no address change event: %r\n", Status));
    return Status;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiFirmwareVolumeBlockProtocolGuid,
                  &mFvbProtocol,
                  &gEdkiiNvVarStoreFormattedGuid,
                  NULL,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Fvb: cannot publish the volume: %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}