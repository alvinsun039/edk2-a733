/** @file
  Android fastboot platform layer for the Cubie A7A: partitions as BlockIo.

  EmbeddedPkg's AndroidFastbootApp owns the protocol (fastboot_protocol.txt in
  AOSP's system/core/fastboot) and asks this file for the three things the
  platform has to know: which partitions exist, how to write one, and what the
  per-device getvar values are.  Nothing here parses a fastboot command - the
  application does that - and nothing here writes anything except in
  FlashPartition() and ErasePartition(), i.e. only after the host sent an
  explicit "flash" or "erase".

  Where the partitions come from.  PartitionDxe publishes EFI_PARTITION_INFO_
  PROTOCOL next to the EFI_BLOCK_IO_PROTOCOL of every GPT partition it finds, and
  that protocol carries the partition's GPT name - which is what fastboot's
  "flash <name>" means on this board.  So a partition is looked up by name, on
  a handle that carries both protocols, and the write goes through that
  partition's BlockIo: the driver never sees the whole device, so a wrong name
  cannot reach the partition table.

  Names and values are matched case-insensitively, ASCII, the way the fastboot
  host sends them (lowercase, and GPT names are uppercase on most images).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/AndroidFastbootPlatform.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>

//
// The value GetVar() may write.  The fastboot protocol caps a variable at 60
// characters and AndroidFastbootApp passes a 61-byte buffer (its "OKAY" plus
// this), so nothing here may be longer than that.
//
#define FASTBOOT_VALUE_MAX  60U

//
// Largest image a single "download" may hand to FlashPartition().  The
// fastboot protocol has no streaming write: the host sends the whole image
// before it says "flash", so this is one pool allocation in the application and
// one buffer here - a memory decision, not a protocol one.  128 MB holds a boot,
// dtbo, vendor_boot or a sparse system image, and is small enough not to matter
// against this board's DRAM.
//
#define FASTBOOT_MAX_DOWNLOAD_SIZE  0x08000000U

#define FASTBOOT_PRODUCT_NAME  "cubie_a7a"
#define FASTBOOT_SERIAL_NAME   "A7A00000001"

//
// Erasing writes in chunks this big; the buffer is reused for the whole
// partition and is one eMMC block, which is also the smallest write BlockIo
// accepts.
//
#define FASTBOOT_ERASE_CHUNK_BLOCKS  128U

typedef struct {
  EFI_BLOCK_IO_PROTOCOL           *BlockIo;
  EFI_PARTITION_INFO_PROTOCOL     *Info;
  CHAR16                          Name[36];
  UINT64                          Bytes;
} FASTBOOT_PARTITION;

/**
  Compare a fastboot partition name (CHAR8, as the host sent it) with a GPT
  partition name (CHAR16), ignoring case.

  @retval TRUE   The two name the same partition.
  @retval FALSE  They do not.
**/
STATIC
BOOLEAN
PartitionNameMatches (
  IN CONST CHAR8   *FastbootName,
  IN CONST CHAR16  *GptName
  )
{
  UINTN  Index;
  CHAR16 Left;
  CHAR16 Right;

  for (Index = 0; ; Index++) {
    if ((FastbootName[Index] == 0) && (GptName[Index] == 0)) {
      return TRUE;
    }

    if ((FastbootName[Index] == 0) || (GptName[Index] == 0)) {
      return FALSE;
    }

    Left  = (CHAR16)(UINT8)FastbootName[Index];
    Right = GptName[Index];
    if ((Left >= L'a') && (Left <= L'z')) {
      Left = (CHAR16)(Left - L'a' + L'A');
    }

    if ((Right >= L'a') && (Right <= L'z')) {
      Right = (CHAR16)(Right - L'a' + L'A');
    }

    if (Left != Right) {
      return FALSE;
    }
  }
}

/**
  Format a byte count as the hex string the fastboot host expects for a size.
**/
STATIC
VOID
FormatSize (
  OUT CHAR8   *Value,
  IN  UINT64  Bytes
  )
{
  AsciiSPrint (Value, FASTBOOT_VALUE_MAX + 1, "0x%Lx", Bytes);
}

/**
  Find the partition the host named.

  @param[in]  Name       Partition name, as fastboot sent it.
  @param[out] Partition  Filled in on success.

  @retval EFI_NOT_FOUND  No GPT partition has that name.
  @retval EFI_SUCCESS    Partition is usable.
**/
STATIC
EFI_STATUS
FindPartition (
  IN  CONST CHAR8        *Name,
  OUT FASTBOOT_PARTITION *Partition
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;
  EFI_PARTITION_INFO_PROTOCOL *Info;
  EFI_BLOCK_IO_PROTOCOL      *BlockIo;

  if ((Name == NULL) || (Name[0] == 0)) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Fastboot: no partition information protocol: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info))) {
      continue;
    }

    if (Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    if (!PartitionNameMatches (Name, Info->Info.Gpt.PartitionName)) {
      continue;
    }

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo))) {
      continue;
    }

    if ((BlockIo->Media == NULL) || !BlockIo->Media->MediaPresent) {
      continue;
    }

    Partition->BlockIo = BlockIo;
    Partition->Info    = Info;
    CopyMem (Partition->Name, Info->Info.Gpt.PartitionName, sizeof (Partition->Name));
    Partition->Name[ARRAY_SIZE (Partition->Name) - 1] = 0;
    Partition->Bytes = MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize);
    Status = EFI_SUCCESS;
    break;
  }

  FreePool (Handles);
  return Status;
}

/**
  Log the partitions this driver can reach, so the boot log says what a host may
  flash without the host having to ask.
**/
STATIC
VOID
LogPartitions (
  VOID
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;
  EFI_PARTITION_INFO_PROTOCOL *Info;
  EFI_BLOCK_IO_PROTOCOL      *BlockIo;
  UINTN                      Listed;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Fastboot: no partitions to list: %r\n", Status));
    return;
  }

  Listed = 0;
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info))) {
      continue;
    }

    if (Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo))) {
      continue;
    }

    DEBUG ((
      DEBUG_INFO,
      "Fastboot: partition %-16s %Lu bytes, %u x %u B blocks\n",
      Info->Info.Gpt.PartitionName,
      MultU64x32 (BlockIo->Media->LastBlock + 1, BlockIo->Media->BlockSize),
      (UINT32)(BlockIo->Media->LastBlock + 1),
      (UINT32)BlockIo->Media->BlockSize
      ));
    Listed++;
  }

  DEBUG ((DEBUG_INFO, "Fastboot: %u GPT partition(s) reachable\n", (UINT32)Listed));
  FreePool (Handles);
}

/**
  FASTBOOT_PLATFORM_PROTOCOL.Init.

  Only reports: nothing is connected, claimed or written until a host asks for
  something.  The partition list is printed here because this is the moment the
  application has just started, i.e. the line appears next to the fastboot
  banner.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformInit (
  VOID
  )
{
  DEBUG ((
    DEBUG_INFO,
    "Fastboot: platform up, product %a, max download 0x%x\n",
    FASTBOOT_PRODUCT_NAME,
    FASTBOOT_MAX_DOWNLOAD_SIZE
    ));
  LogPartitions ();
  return EFI_SUCCESS;
}

/**
  FASTBOOT_PLATFORM_PROTOCOL.UnInit.  Nothing was claimed, so there is nothing
  to give back; the boot services and the eMMC stay as they were.
**/
STATIC
VOID
EFIAPI
FastbootPlatformUnInit (
  VOID
  )
{
}

/**
  FASTBOOT_PLATFORM_PROTOCOL.FlashPartition.

  Writes at the start of the named GPT partition, through that partition's own
  BlockIo.  The image need not be a whole number of blocks: the trailing partial
  block is read, patched and written back, so the bytes outside the image in
  that block keep their contents.

  @param[in] PartitionName  GPT partition name, as fastboot sent it.
  @param[in] BufferSize     Image size in bytes.
  @param[in] Buffer         Image.

  @retval EFI_NOT_FOUND     No such partition, or the image does not fit.
  @retval EFI_DEVICE_ERROR  The medium refused a write.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformFlash (
  IN CHAR8  *PartitionName,
  IN UINTN  BufferSize,
  IN VOID   *Buffer
  )
{
  EFI_STATUS          Status;
  FASTBOOT_PARTITION  Partition;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN               BlockSize;
  UINTN               WholeBlocks;
  UINTN               Tail;
  UINT8               *TailBlock;

  Status = FindPartition (PartitionName, &Partition);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot: flash %a: no such partition\n", PartitionName));
    return EFI_NOT_FOUND;
  }

  BlockIo   = Partition.BlockIo;
  BlockSize = BlockIo->Media->BlockSize;

  if ((UINT64)BufferSize > Partition.Bytes) {
    DEBUG ((
      DEBUG_ERROR,
      "Fastboot: flash %a: image is %u bytes, partition holds %Lu\n",
      PartitionName,
      (UINT32)BufferSize,
      Partition.Bytes
      ));
    return EFI_NOT_FOUND;
  }

  WholeBlocks = BufferSize / BlockSize;
  Tail        = BufferSize % BlockSize;

  DEBUG ((
    DEBUG_INFO,
    "Fastboot: flashing %a: %u bytes (%u full blocks of %u B%s)\n",
    PartitionName,
    (UINT32)BufferSize,
    (UINT32)WholeBlocks,
    (UINT32)BlockSize,
    (Tail != 0) ? ", plus a partial block" : ""
    ));

  if (WholeBlocks != 0) {
    Status = BlockIo->WriteBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        0,
                        WholeBlocks * BlockSize,
                        Buffer
                        );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Fastboot: flash %a: write failed: %r\n", PartitionName, Status));
      return EFI_DEVICE_ERROR;
    }
  }

  if (Tail != 0) {
    TailBlock = AllocatePool (BlockSize);
    if (TailBlock == NULL) {
      return EFI_DEVICE_ERROR;
    }

    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, WholeBlocks, BlockSize, TailBlock);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Fastboot: flash %a: cannot read the last block: %r\n", PartitionName, Status));
      FreePool (TailBlock);
      return EFI_DEVICE_ERROR;
    }

    CopyMem (TailBlock, (UINT8 *)Buffer + (WholeBlocks * BlockSize), Tail);
    Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, WholeBlocks, BlockSize, TailBlock);
    FreePool (TailBlock);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Fastboot: flash %a: last block write failed: %r\n", PartitionName, Status));
      return EFI_DEVICE_ERROR;
    }
  }

  Status = BlockIo->FlushBlocks (BlockIo);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Fastboot: flash %a: flush failed: %r\n", PartitionName, Status));
  }

  DEBUG ((
    DEBUG_INFO,
    "Fastboot: flashed %a (%u bytes)\n",
    PartitionName,
    (UINT32)BufferSize
    ));
  return EFI_SUCCESS;
}

/**
  FASTBOOT_PLATFORM_PROTOCOL.ErasePartition.

  Writes zeroes through the whole partition.  BlockIo has no erase operation
  (EFI_ERASE_BLOCK_PROTOCOL is a separate protocol and an asynchronous one,
  which cannot be waited on at the TPL the fastboot application calls this
  from), so an erase is a write here - and it is bounded to the partition, so
  nothing outside it is touched.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformErase (
  IN CHAR8  *PartitionName
  )
{
  EFI_STATUS            Status;
  FASTBOOT_PARTITION    Partition;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN                 Blocks;
  UINT8                 *Zeroes;
  UINTN                 Chunk;
  UINTN                 Lba;
  UINTN                 Done;

  Status = FindPartition (PartitionName, &Partition);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot: erase %a: no such partition\n", PartitionName));
    return EFI_NOT_FOUND;
  }

  BlockIo = Partition.BlockIo;
  Blocks  = (UINTN)(Partition.Bytes / BlockIo->Media->BlockSize);

  DEBUG ((DEBUG_INFO, "Fastboot: erasing %a (%u blocks of %u B)\n",
          PartitionName, (UINT32)Blocks, (UINT32)BlockIo->Media->BlockSize));

  Zeroes = AllocateZeroPool (FASTBOOT_ERASE_CHUNK_BLOCKS * BlockIo->Media->BlockSize);
  if (Zeroes == NULL) {
    return EFI_DEVICE_ERROR;
  }

  Chunk  = FASTBOOT_ERASE_CHUNK_BLOCKS;
  Lba    = 0;
  Status = EFI_SUCCESS;
  while (Lba < Blocks) {
    Done   = MIN (Chunk, Blocks - Lba);
    Status = BlockIo->WriteBlocks (
                        BlockIo,
                        BlockIo->Media->MediaId,
                        Lba,
                        Done * BlockIo->Media->BlockSize,
                        Zeroes
                        );
    if (EFI_ERROR (Status)) {
      break;
    }

    Lba += Done;
  }

  FreePool (Zeroes);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot: erase %a: write at block %u failed: %r\n", PartitionName, (UINT32)Lba, Status));
    return EFI_DEVICE_ERROR;
  }

  BlockIo->FlushBlocks (BlockIo);
  DEBUG ((DEBUG_INFO, "Fastboot: erased %a\n", PartitionName));
  return EFI_SUCCESS;
}

/**
  FASTBOOT_PLATFORM_PROTOCOL.GetVar.

  Answers the variables a fastboot host uses to decide what it can do, and
  leaves unknown names as the empty string - which is what the protocol's
  documentation in EmbeddedPkg/Include/Protocol/AndroidFastbootPlatform.h asks
  for, and keeps "fastboot getvar <anything>" from being a failure.

  @param[in]  Name   Variable name, without the "getvar:" prefix.
  @param[out] Value  61-byte buffer; on return a null-terminated value.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformGetVar (
  IN  CHAR8  *Name,
  OUT CHAR8  *Value
  )
{
  FASTBOOT_PARTITION  Partition;
  EFI_STATUS          Status;
  CONST CHAR8         *Suffix;

  Value[0] = 0;

  if (!AsciiStrCmp (Name, "product")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, FASTBOOT_PRODUCT_NAME);
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "serialno")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, FASTBOOT_SERIAL_NAME);
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "version-bootloader")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, "edk2-a733");
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "version-baseband")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, "none");
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "max-download-size")) {
    FormatSize (Value, FASTBOOT_MAX_DOWNLOAD_SIZE);
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "secure")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, "no");
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "unlocked")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, "yes");
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "is-userspace")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, "no");
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Name, "slot-count")) {
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, "0");
    return EFI_SUCCESS;
  }

  //
  // The per-partition pair the host uses to size a plan before it downloads
  // anything: "partition-size:<name>" and "partition-type:<name>".
  //
  Suffix = NULL;
  if (!AsciiStrnCmp (Name, "partition-size:", sizeof ("partition-size:") - 1)) {
    Suffix = Name + sizeof ("partition-size:") - 1;
    Status = FindPartition (Suffix, &Partition);
    if (EFI_ERROR (Status)) {
      return EFI_SUCCESS;               // empty value: no such partition
    }

    FormatSize (Value, Partition.Bytes);
    return EFI_SUCCESS;
  }

  if (!AsciiStrnCmp (Name, "partition-type:", sizeof ("partition-type:") - 1)) {
    Suffix = Name + sizeof ("partition-type:") - 1;
    Status = FindPartition (Suffix, &Partition);
    if (EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    //
    // "raw" is the honest answer: this layer writes images as they arrive and
    // does not know what filesystem, if any, a partition holds.
    //
    AsciiStrCpyS (Value, FASTBOOT_VALUE_MAX + 1, "raw");
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}

/**
  FASTBOOT_PLATFORM_PROTOCOL.DoOemCommand.

  Reporting only, by design: an OEM command must not be able to write anything,
  so everything here logs and returns.  Unknown commands are reported as such,
  which is what makes the host print its own error rather than wait.

  @param[in] Command  Command string after "oem ".

  @retval EFI_SUCCESS    A command this platform knows was carried out.
  @retval EFI_NOT_FOUND  It is not one of them.
**/
STATIC
EFI_STATUS
EFIAPI
FastbootPlatformOem (
  IN CHAR8  *Command
  )
{
  if (!AsciiStrCmp (Command, "partitions")) {
    LogPartitions ();
    return EFI_SUCCESS;
  }

  if (!AsciiStrCmp (Command, "device-info")) {
    DEBUG ((
      DEBUG_INFO,
      "Fastboot: product %a, serial %a, max download 0x%x, storage eMMC via BlockIo\n",
      FASTBOOT_PRODUCT_NAME,
      FASTBOOT_SERIAL_NAME,
      FASTBOOT_MAX_DOWNLOAD_SIZE
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_WARN, "Fastboot: OEM command %a is not implemented (report-only driver)\n", Command));
  return EFI_NOT_FOUND;
}

STATIC FASTBOOT_PLATFORM_PROTOCOL  mFastbootPlatformProtocol = {
  FastbootPlatformInit,
  FastbootPlatformUnInit,
  FastbootPlatformFlash,
  FastbootPlatformErase,
  FastbootPlatformGetVar,
  FastbootPlatformOem
};

/**
  Publish the platform protocol for AndroidFastbootApp to locate.
**/
EFI_STATUS
EFIAPI
A733FastbootPlatformEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gAndroidFastbootPlatformProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mFastbootPlatformProtocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot: cannot publish the platform protocol: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "Fastboot: platform layer published (BlockIo over GPT names)\n"));
  return EFI_SUCCESS;
}