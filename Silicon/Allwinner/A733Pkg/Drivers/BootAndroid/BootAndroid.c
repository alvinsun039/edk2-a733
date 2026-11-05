/** @file
  BootAndroid — EDK2 hand-off to an AOSP16 GKI kernel on eMMC.

  Native EDK2 boot of an Android GKI2.0 image set (boot_a / init_boot_a /
  vendor_boot_a, boot header v4) straight from the eMMC, no GRUB and no
  U-Boot involved:

    1. locate the eMMC BlockIo (SMHC2) and read the three GPT partitions:
         boot_a        -> v4 header + kernel Image (EFI stub)
         init_boot_a   -> generic ramdisk (lz4)
         vendor_boot_a -> vendor ramdisk + dtb + vendor_cmdline + bootconfig
    2. assemble initrd = [vendor ramdisk][generic ramdisk][bootconfig+trailer]
       (concatenated, no page alignment — kernel decompressor auto-splits)
    3. expose initrd via Linux EFI_LOAD_FILE2 on VenMedia(LINUX_EFI_INITRD_MEDIA_GUID)
    4. kernel cmdline = vendor_cmdline + PcdBootAndroidExtraCmdline
    5. LoadImage()/StartImage() with the kernel, LoadOptions = cmdline.

  The DTB is NOT read here: PlatformBootManagerLib::InstallPlatformFdt()
  already publishes the board DTB as the EFI FDT configuration table, which
  the arm64 EFI stub finds via get_fdt().  (vendor_boot_a's own dtb is
  therefore redundant on this board, but we still parse the header.)

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/DevicePath.h>

//
// Partition positions are discovered at runtime: FindAndroidPartition walks
// the GPT entries by name (boot_a / init_boot_a / vendor_boot_a / misc) and
// returns each partition's starting LBA and size, so no disk offset is baked
// into this file.  Offsets inside an image are relative to its partition
// start; the v4 boot header is exactly one 4096B page, so kernel/ramdisk sit
// at +0x1000 (BOOT_HEADER_PAGE, fixed by the v3/v4 format, not by the disk).
//
#define BOOT_HEADER_PAGE      0x1000ULL    // 4096, fixed for v3/v4
#define VENDOR_BOOT_HDR_SIZE  2128U
#define ANDROID_MAGIC         "ANDROID!"
#define VNDRBOOT_MAGIC        "VNDRBOOT"
#define BOOTCONFIG_MAGIC      "#BOOTCONFIG\n"   // 12 bytes
#define BOOTCONFIG_TRAILER    20U               // size+csum+magic

#pragma pack(1)
// boot_img_hdr v3/v4 (only the fields we need).
typedef struct {
  UINT8     Magic[8];
  UINT32    KernelSize;        // 0x08
  UINT32    RamdiskSize;       // 0x0C
  UINT32    OsVersion;         // 0x10
  UINT32    HeaderSize;        // 0x14
  UINT32    Reserved[4];       // 0x18
  UINT32    HeaderVersion;     // 0x28 = 4
  UINT8     Cmdline[1536];     // 0x2C
  // v4: signature_size @ 0x62C (not needed)
} ANDROID_BOOT_HDR_V4;

// vendor_boot_img_hdr v4 (fields we need).
typedef struct {
  UINT8     Magic[8];          // 0x00
  UINT32    HeaderVersion;     // 0x08
  UINT32    PageSize;          // 0x0C
  UINT32    KernelAddr;        // 0x10
  UINT32    RamdiskAddr;       // 0x14
  UINT32    VendorRamdiskSize; // 0x18
  UINT8     Cmdline[2048];     // 0x1C
  UINT32    TagsAddr;          // 0x81C
  UINT8     Name[16];          // 0x820
  UINT32    HdrSize;           // 0x830
  UINT32    DtbSize;           // 0x834
  UINT64    DtbAddr;           // 0x838
  UINT32    VramTableSize;     // 0x840
  UINT32    VramTableNum;      // 0x844
  UINT32    VramTableEntrySize;// 0x848
  UINT32    BootconfigSize;    // 0x84C
} VENDOR_BOOT_HDR_V4;
#pragma pack()

// EFI_DTB_TABLE_GUID — informational only; InstallPlatformFdt() in
// PlatformBootManagerLib already publishes the board DTB into this config
// table, and the arm64 EFI stub reads it via get_fdt().  (The vendor_boot
// dtb is therefore redundant on this board, but we still parse the header.)

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          VenMedia;
  EFI_DEVICE_PATH_PROTOCOL    End;
} INITRD_DEVICE_PATH;
#pragma pack()

STATIC INITRD_DEVICE_PATH  mInitrdDevicePath = {
  {
    { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC VOID   *mInitrdData = NULL;
STATIC UINTN   mInitrdSize = 0;

/**
  LoadFile2 callback the EFI stub uses to pull in the initrd.
**/
STATIC
EFI_STATUS
EFIAPI
InitrdLoadFile2 (
  IN     EFI_LOAD_FILE2_PROTOCOL   *This,
  IN     EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  IN     BOOLEAN                   BootPolicy,
  IN OUT UINTN                     *BufferSize,
  OUT    VOID                      *Buffer  OPTIONAL
  )
{
  if (BootPolicy) {
    return EFI_UNSUPPORTED;
  }
  if ((BufferSize == NULL) || (mInitrdData == NULL) || (mInitrdSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Buffer == NULL) || (*BufferSize < mInitrdSize)) {
    *BufferSize = mInitrdSize;
    return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem (Buffer, mInitrdData, mInitrdSize);
  *BufferSize = mInitrdSize;
  return EFI_SUCCESS;
}

STATIC EFI_LOAD_FILE2_PROTOCOL  mInitrdLoadFile2 = { InitrdLoadFile2 };

/**
  Locate the disk that hosts the Android partitions, by GPT partition name.

  Enumerates EFI_PARTITION_INFO_PROTOCOL (GPT) children and matches the
  ASCII partition name against the GPT entry's CHAR16 name; the disk whose
  GPT contains "boot_a" is the Android boot medium (eMMC, SATA, NVMe - any
  GPT disk works; no hard-coded LBA).  For each requested partition the
  starting LBA and size are returned.  When the name exists on several
  disks the eMMC slot (SMHC2 vendor device-path GUID) wins, like the
  fastboot library does.
**/
STATIC
BOOLEAN
BootAndroidIsEmmcDp (
  IN EFI_DEVICE_PATH_PROTOCOL  *Dp
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  VENDOR_DEVICE_PATH        *Vendor;
  EFI_GUID                  EmmcGuid = { 0x6fb9fcd6, 0xe02c, 0x4f75,
                                         { 0xb6, 0x62, 0xe8, 0x43, 0x3e, 0x51, 0x43, 0xd4 } };

  if (Dp == NULL) {
    return FALSE;
  }
  Node = Dp;
  while (!IsDevicePathEnd (Node)) {
    if ((DevicePathType (Node) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType (Node) == HW_VENDOR_DP)) {
      Vendor = (VENDOR_DEVICE_PATH *)Node;
      if (CompareGuid (&Vendor->Guid, &EmmcGuid)) {
        return TRUE;
      }
    }
    Node = NextDevicePathNode (Node);
  }
  return FALSE;
}

STATIC
EFI_STATUS
FindAndroidPartition (
  IN  CHAR8                   *Name,
  OUT EFI_BLOCK_IO_PROTOCOL  **OutBio,
  OUT EFI_LBA                *OutStartLba,
  OUT UINTN                  *OutSizeBytes
  )
{
  EFI_STATUS                   Status;
  EFI_HANDLE                   *Handles;
  UINTN                        Count;
  UINTN                        Index;
  EFI_PARTITION_INFO_PROTOCOL  *PartInfo;
  EFI_DEVICE_PATH_PROTOCOL     *ChildDp;
  EFI_DEVICE_PATH_PROTOCOL     *ParentDp;
  EFI_DEVICE_PATH_PROTOCOL     *FreeDp;
  EFI_HANDLE                   ParentHandle;
  EFI_BLOCK_IO_PROTOCOL        *Io;
  BOOLEAN                      IsEmmc;
  EFI_BLOCK_IO_PROTOCOL        *BestIo = NULL;
  EFI_LBA                      BestStart = 0;
  UINTN                        BestBytes = 0;
  BOOLEAN                      BestIsEmmc = FALSE;
  UINTN                        I;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index],
                                  &gEfiPartitionInfoProtocolGuid, (VOID **)&PartInfo);
    if (EFI_ERROR (Status) || (PartInfo->Type != PARTITION_TYPE_GPT)) {
      continue;
    }
    // Match ASCII name (case-insensitive) against the GPT CHAR16 name.
    for (I = 0; I < 36; I++) {
      CHAR16 G = PartInfo->Info.Gpt.PartitionName[I];
      CHAR8  A = Name[I];
      if ((G == 0) && (A == 0)) {
        break;                      // match
      }
      if (G == 0 || A == 0) {
        goto NextPart;
      }
      if (G > 0x7F) {
        goto NextPart;
      }
      if ((G >= 'a') && (G <= 'z')) {
        G -= ('a' - 'A');
      }
      if ((A >= 'a') && (A <= 'z')) {
        A -= ('a' - 'A');
      }
      if (G != (CHAR16)A) {
        goto NextPart;
      }
    }

    ChildDp  = DevicePathFromHandle (Handles[Index]);
    ParentDp = DuplicateDevicePath (ChildDp);
    if (ParentDp == NULL) {
      continue;
    }
    {
      // Strip the partition node to reach the parent disk.
      EFI_DEVICE_PATH_PROTOCOL *P = ParentDp;
      UINTN  NodeLen;
      while (!IsDevicePathEnd (P)) {
        NodeLen = DevicePathNodeLength (P);
        if (IsDevicePathEnd ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)P + NodeLen))) {
          break;
        }
        P = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)P + NodeLen);
      }
      P->Type    = END_DEVICE_PATH_TYPE;
      P->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
      SetDevicePathNodeLength (P, sizeof (EFI_DEVICE_PATH_PROTOCOL));
    }
    FreeDp   = ParentDp;
    Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &ParentDp, &ParentHandle);
    FreePool (FreeDp);
    if (EFI_ERROR (Status)) {
      continue;
    }
    Status = gBS->HandleProtocol (ParentHandle, &gEfiBlockIoProtocolGuid, (VOID **)&Io);
    if (EFI_ERROR (Status) || (Io->Media == NULL)) {
      continue;
    }

    IsEmmc = BootAndroidIsEmmcDp (DevicePathFromHandle (ParentHandle));
    if ((BestIo == NULL) || (IsEmmc && !BestIsEmmc)) {
      BestIo     = Io;
      BestStart  = PartInfo->Info.Gpt.StartingLBA;
      BestBytes  = (UINTN)(PartInfo->Info.Gpt.EndingLBA - PartInfo->Info.Gpt.StartingLBA + 1)
                   * Io->Media->BlockSize;
      BestIsEmmc = IsEmmc;
    }
    if (BestIsEmmc) {
      break;
    }
NextPart:
    continue;
  }

  FreePool (Handles);
  if (BestIo == NULL) {
    return EFI_NOT_FOUND;
  }
  *OutBio      = BestIo;
  *OutStartLba = BestStart;
  *OutSizeBytes = BestBytes;
  return EFI_SUCCESS;
}


/**
  Read a block-aligned partition range into a caller-provided buffer.

  The chunking exists because multi-block reads at a high LBA are fragile on
  this SMHC/HS200 path (single-block CMD17 works, large CMD18 bursts fail).
**/
STATIC
EFI_STATUS
ReadPartitionInto (
  IN  EFI_BLOCK_IO_PROTOCOL  *Bio,
  IN  UINT64                  Lba,
  IN  UINTN                   ByteLen,
  OUT VOID                    *Buffer
  )
{
  UINTN   Remaining;
  UINTN   ChunkBlocks;
  UINT8   *Ptr;
  UINT64  CurLba;
  const UINTN CHUNK_BLOCKS = 16;   /* 8 KiB per burst */

  if ((ByteLen % Bio->Media->BlockSize) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  Remaining = ByteLen;
  Ptr       = (UINT8 *)Buffer;
  CurLba    = Lba;
  while (Remaining > 0) {
    ChunkBlocks = Remaining / Bio->Media->BlockSize;
    if (ChunkBlocks > CHUNK_BLOCKS) {
      ChunkBlocks = CHUNK_BLOCKS;
    }
    if (Bio->ReadBlocks (Bio, Bio->Media->MediaId, CurLba,
                         ChunkBlocks * Bio->Media->BlockSize,
                         Ptr) != EFI_SUCCESS) {
      return EFI_DEVICE_ERROR;
    }
    CurLba    += ChunkBlocks;
    Ptr       += ChunkBlocks * Bio->Media->BlockSize;
    Remaining -= ChunkBlocks * Bio->Media->BlockSize;
  }
  return EFI_SUCCESS;
}

/**
  Read a partition range into pool.
**/
STATIC
EFI_STATUS
ReadPartition (
  IN  EFI_BLOCK_IO_PROTOCOL  *Bio,
  IN  UINT64                  Lba,
  IN  UINTN                   ByteLen,
  OUT VOID                   **Data
  )
{
  EFI_STATUS  Status;
  UINTN       Blocks;
  UINT8       *Buf;

  Blocks = (ByteLen + Bio->Media->BlockSize - 1) / Bio->Media->BlockSize;
  Buf    = AllocatePool (Blocks * Bio->Media->BlockSize);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = ReadPartitionInto (Bio, Lba, Blocks * Bio->Media->BlockSize, Buf);
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return Status;
  }
  *Data = Buf;
  return EFI_SUCCESS;
}

/**
  Grow a partition image from its already-read header to the length the
  header declares.

  The old image is freed and replaced by a buffer of ImageBytes (rounded up
  to the block size, never past the partition end), with HeaderBytes copied
  over and the rest read from the partition.  This keeps a 64 MB boot
  partition from being read in full when only the header page and the kernel
  it names are needed.
**/
STATIC
EFI_STATUS
ExtendPartitionImage (
  IN OUT VOID                  **Image,
  IN     UINTN                 HeaderBytes,
  IN     UINTN                 ImageBytes,
  IN     EFI_LBA               StartLba,
  IN     UINTN                 PartitionBytes,
  IN     EFI_BLOCK_IO_PROTOCOL *Bio
  )
{
  EFI_STATUS  Status;
  UINTN       ReadLen;
  UINT8       *Full;

  if ((HeaderBytes > PartitionBytes) || (ImageBytes > PartitionBytes)) {
    return EFI_VOLUME_CORRUPTED;
  }
  if ((HeaderBytes % Bio->Media->BlockSize) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  ReadLen = ALIGN_VALUE (ImageBytes, Bio->Media->BlockSize);
  if (ReadLen > PartitionBytes) {
    ReadLen = PartitionBytes;
  }
  if (ReadLen <= HeaderBytes) {
    return EFI_SUCCESS;
  }

  Full = AllocatePool (ReadLen);
  if (Full == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (Full, *Image, HeaderBytes);
  Status = ReadPartitionInto (
             Bio,
             StartLba + HeaderBytes / Bio->Media->BlockSize,
             ReadLen - HeaderBytes,
             Full + HeaderBytes
             );
  if (EFI_ERROR (Status)) {
    FreePool (Full);
    return Status;
  }

  FreePool (*Image);
  *Image = Full;
  return EFI_SUCCESS;
}

/**
  Parse a boot.img v4 header; return kernel offset/size within Image.
**/
STATIC
EFI_STATUS
ParseBootHeader (
  IN  CONST ANDROID_BOOT_HDR_V4  *Hdr,
  OUT UINT32                     *KernelSize
  )
{
  if (CompareMem (Hdr->Magic, ANDROID_MAGIC, 8) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (Hdr->HeaderVersion < 3) {
    Print (L"BootAndroid: boot header v%d, need v3/v4\n", Hdr->HeaderVersion);
    return EFI_UNSUPPORTED;
  }
  *KernelSize = Hdr->KernelSize;
  return EFI_SUCCESS;
}

/**
  Parse vendor_boot v4; return vendor ramdisk / bootconfig byte offsets within
  the partition image (all relative to partition start).
**/
STATIC
VOID
ParseVendorBoot (
  IN  CONST VENDOR_BOOT_HDR_V4  *Vh,
  OUT UINTN                     *VramOff,
  OUT UINTN                     *VramLen,
  OUT UINTN                     *BcOff,
  OUT UINTN                     *BcLen,
  OUT UINT32                    *PageSize
  )
{
  UINTN  Page;
  UINTN  VramOffT, BcOffT;

  Page    = Vh->PageSize ? Vh->PageSize : 4096;
  VramOffT = (VENDOR_BOOT_HDR_SIZE + Page - 1) / Page * Page;   // ALIGN(2128, P)

  *VramOff = VramOffT;
  *VramLen = Vh->VendorRamdiskSize;
  *BcLen   = Vh->BootconfigSize;
  *PageSize = (UINT32)Page;

  // dtb follows vram, then table, then bootconfig.
  {
    UINTN  DtbOff = VramOffT + ALIGN_VALUE (Vh->VendorRamdiskSize, Page);
    UINTN  TblOff = DtbOff + ALIGN_VALUE (Vh->DtbSize, Page);
    BcOffT = TblOff + ALIGN_VALUE (Vh->VramTableSize, Page);
  }
  *BcOff = BcOffT;
}

/**
  Append the 20-byte bootconfig trailer: [pad to 4B][size le32][csum le32]["#BOOTCONFIG\n"].
  csum = uint32 sum of the (padded) bootconfig bytes.
**/
STATIC
UINTN
AddBootconfigTrailer (
  IN  VOID   *BcData,
  IN  UINTN   BcLen,
  OUT UINT8  *Out   // >= BcLen + 4 + 20
  )
{
  UINTN   Pad;
  UINTN   BodyLen;
  UINT32  Sum = 0;
  UINTN   I;

  Pad     = (4 - (BcLen % 4)) % 4;
  BodyLen = BcLen + Pad;
  CopyMem (Out, BcData, BcLen);
  ZeroMem (Out + BcLen, Pad);
  for (I = 0; I < BodyLen; I++) {
    Sum += Out[I];
  }
  CopyMem (Out + BodyLen, &BodyLen, sizeof (UINT32));          // size le32
  CopyMem (Out + BodyLen + 4, &Sum, sizeof (UINT32));          // csum le32
  CopyMem (Out + BodyLen + 8, BOOTCONFIG_MAGIC, 12);           // magic
  return BodyLen + 20;
}

//
// Android A/B: the active slot suffix lives in the "misc" partition at
// offset 0 in the bootloader_message (the first 32 bytes are the command
// field, the slot suffix is the second 32-byte field).  Parse it so
// boot_a/boot_b etc. resolve to the active slot; default to "_a".
//
typedef struct {
  CHAR8  Command[32];
  CHAR8  Status[32];
  CHAR8  Recovery[768];
  CHAR8  SlotSuffix[32];
  CHAR8  Reserved[32];
} BOOTLOADER_MESSAGE;

STATIC
EFI_STATUS
GetActiveSlot (
  OUT CHAR8  *SlotSuffix,     // "_a" or "_b"
  IN  UINTN   SlotSuffixSize
  )
{
  EFI_STATUS                   Status;
  EFI_BLOCK_IO_PROTOCOL        *Bio;
  EFI_LBA                      MiscLba;
  UINTN                        MiscLen;
  VOID                         *Misc;
  BOOTLOADER_MESSAGE           *Bm;

  // Default: _a (current behaviour).
  AsciiStrCpyS (SlotSuffix, SlotSuffixSize, "_a");

  // misc lives on the same disk as boot_a.
  Status = FindAndroidPartition ("misc", &Bio, &MiscLba, &MiscLen);
  if (EFI_ERROR (Status) || (MiscLen == 0)) {
    return EFI_SUCCESS;   // no misc -> assume _a
  }

  if (MiscLen < sizeof (BOOTLOADER_MESSAGE)) {
    return EFI_SUCCESS;   // too small to hold the slot message -> assume _a
  }

  //
  // The bootloader_message sits at offset 0; the rest of the partition is not
  // part of the protocol and does not need to be read.
  //
  Status = ReadPartition (
             Bio,
             MiscLba,
             (MiscLen < BOOT_HEADER_PAGE) ? MiscLen : BOOT_HEADER_PAGE,
             &Misc
             );
  if (EFI_ERROR (Status) || (Misc == NULL)) {
    return EFI_SUCCESS;
  }

  Bm = (BOOTLOADER_MESSAGE *)Misc;
  if (Bm->SlotSuffix[0] == '_' &&
      ((Bm->SlotSuffix[1] == 'a') || (Bm->SlotSuffix[1] == 'b')) &&
      (Bm->SlotSuffix[2] == '\0'))
  {
    AsciiStrCpyS (SlotSuffix, SlotSuffixSize, Bm->SlotSuffix);
    Print (L"BootAndroid: active slot %a\n", Bm->SlotSuffix);
  } else {
    Print (L"BootAndroid: no valid slot in misc, using _a\n");
  }

  FreePool (Misc);
  return EFI_SUCCESS;
}

/**
  Replicate the vendor U-Boot's bootconfig augmentation.

  The vendor's U-Boot passes the vendor_boot bootconfig verbatim and then
  appends the AVB-verification / platform nodes it would normally generate
  (androidboot.hardware, vbmeta.*, serialno, ...).  Without these the kernel
  gets an empty ro.hardware -> init never imports init.sun60iw2p1.rc -> the
  "on fs"/"on late-fs" mount_all actions never run and boot stalls before
  zygote.  Rebuild the same augmented bootconfig here so the EDK2 chain
  behaves like the vendor path.

  Format is the Linux bootconfig text syntax: "key=value\n" per node, then
  AddBootconfigTrailer() appends the [pad][size][csum]["#BOOTCONFIG\n"].
**/
STATIC
VOID
AppendUbootBootconfig (
  IN  VOID   *Base,      // start of the shared initrd buffer
  IN  VOID   *BcData,    // original vendor_boot bootconfig
  IN  UINTN   BcLen,
  IN  CONST CHAR8 *SlotSuffix,   // "_a" / "_b"
  OUT UINT8  *Out,       // >= BcLen + APPEND_LEN + 32
  OUT UINTN  *OutLen
  )
{
  STATIC CONST CHAR8  Append[] =
    "androidboot.hardware=sun60iw2p1\n"
    "androidboot.serialno=4c058a3c418047a1dd4\n"
    "androidboot.boot_type=2\n"
    "androidboot.mode=normal\n"
    "androidboot.secure_os_exist=0\n"
    "androidboot.verifiedbootstate=orange\n"
    "androidboot.trustchain=false\n"
    "androidboot.drmkey=false\n"
    "androidboot.dramfreq=1800\n"
    "androidboot.dramsize=8192\n"
    "androidboot.veritymode=enforcing\n"
    "androidboot.force_normal_boot=1\n"
    "androidboot.slot_suffix=_a\n"     // overwritten below with SlotSuffix
    "androidboot.vbmeta.avb_version=2.0\n"
    "androidboot.vbmeta.hash_alg=sha256\n"
    "androidboot.vbmeta.size=8960\n"
    "androidboot.vbmeta.digest=6f0339da30a8917e77f0a3ca5009a368d75c7a04835cfdfafa69bfdb6b1a86c6\n"
    "androidboot.vbmeta.device_state=locked\n";
  CHAR8  SlotLine[32];

  CopyMem (Out, BcData, BcLen);
  /* ensure the original body ends with a newline before appending */
  if ((BcLen == 0) || (((CHAR8 *)BcData)[BcLen - 1] != '\n')) {
    Out[BcLen++] = '\n';
  }
  CopyMem (Out + BcLen, Append, sizeof (Append) - 1);
  *OutLen = BcLen + sizeof (Append) - 1;

  //
  // Replace the hard-coded "androidboot.slot_suffix=_a\n" with the active
  // slot so Android mounts the right super/system slot.
  //
  AsciiSPrint (SlotLine, sizeof (SlotLine), "androidboot.slot_suffix=%a\n", SlotSuffix);
  {
    CHAR8  *Body = (CHAR8 *)Out;
    UINTN  I;
    for (I = 0; (I + sizeof (SlotLine) - 1) <= *OutLen; I++) {
      if (AsciiStrnCmp (&Body[I], "androidboot.slot_suffix=_a\n", sizeof ("androidboot.slot_suffix=_a\n") - 1) == 0) {
        CopyMem (&Body[I], SlotLine, AsciiStrLen (SlotLine));
        break;
      }
    }
  }
}

/**
  BootAndroid entry: read GKI partitions, assemble initrd, LoadImage/StartImage.
**/
EFI_STATUS
EFIAPI
BootAndroidEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *Bio;
  EFI_LBA                StartLba;
  VOID                   *BootImg;      // boot_a partition (kernel page + Image)
  UINTN                   BootImgLen;
  VOID                   *InitBoot;     // init_boot_a partition
  UINTN                   InitBootLen;
  VOID                   *VbImg;        // vendor_boot_a partition
  UINTN                   VbImgLen;
  CONST ANDROID_BOOT_HDR_V4 *BootHdr;
  CONST VENDOR_BOOT_HDR_V4  *VbHdr;
  UINT32                   KernelSize;
  UINT32                   GenRsz;       // generic ramdisk size
  UINTN                    VramOff, VramLen, BcOff, BcLen;
  UINT32                   Page;
  UINTN                    InitrdCap, InitrdLen;
  UINT8                   *Initrd;
  UINT8                   *BcBlob;
  UINTN                    BcBlobLen;
  EFI_HANDLE              InitrdHandle, KernelHandle;
  EFI_LOADED_IMAGE_PROTOCOL *KernelLi;
  CHAR16                  *ExtraCmd;
  UINTN                    CmdBaseLen;
  UINT8                   *Kernel;
  UINTN                    KernelLen;
  CHAR8                    SlotSuffix[8];
  CHAR8                    PartitionName[40];

  Print (L"BootAndroid: probing GPT partitions by name...\n");
  // Determine the active A/B slot (misc bootloader_message), default _a.
  GetActiveSlot (SlotSuffix, sizeof (SlotSuffix));
  Print (L"BootAndroid: active slot %a\n", SlotSuffix);

  // 1. boot%a -> header + kernel
  AsciiSPrint (PartitionName, sizeof (PartitionName), "boot%a", SlotSuffix);
  Status = FindAndroidPartition (PartitionName, &Bio, &StartLba, &BootImgLen);
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: no %a partition (%r)\n", PartitionName, Status);
    return Status;
  }
  if (BootImgLen < BOOT_HEADER_PAGE) {
    Print (L"BootAndroid: %a partition is too small (%lu)\n", PartitionName, BootImgLen);
    return EFI_VOLUME_CORRUPTED;
  }
  Status = ReadPartition (Bio, StartLba, BOOT_HEADER_PAGE, &BootImg);
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: read %a header failed (%r)\n", PartitionName, Status);
    return Status;
  }
  BootHdr = (CONST ANDROID_BOOT_HDR_V4 *)BootImg;
  Status  = ParseBootHeader (BootHdr, &KernelSize);
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: bad boot_a header (%r)\n", Status);
    return Status;
  }
  Print (L"BootAndroid: boot_a v%d kernel %u bytes\n",
         BootHdr->HeaderVersion, KernelSize);
  Status = ExtendPartitionImage (
             &BootImg,
             BOOT_HEADER_PAGE,
             BOOT_HEADER_PAGE + (UINTN)KernelSize,
             StartLba,
             BootImgLen,
             Bio
             );
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: read %a kernel failed (%r)\n", PartitionName, Status);
    return Status;
  }
  Kernel     = (UINT8 *)BootImg + BOOT_HEADER_PAGE;
  KernelLen  = KernelSize;

  // 2. init_boot%a -> generic ramdisk (kernel_size = 0)
  AsciiSPrint (PartitionName, sizeof (PartitionName), "init_boot%a", SlotSuffix);
  Status = FindAndroidPartition (PartitionName, &Bio, &StartLba, &InitBootLen);
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: no %a partition (%r)\n", PartitionName, Status);
    return Status;
  }
  if (InitBootLen < BOOT_HEADER_PAGE) {
    Print (L"BootAndroid: %a partition is too small (%lu)\n", PartitionName, InitBootLen);
    return EFI_VOLUME_CORRUPTED;
  }
  Status = ReadPartition (Bio, StartLba, BOOT_HEADER_PAGE, &InitBoot);
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: read %a header failed (%r)\n", PartitionName, Status);
    return Status;
  }
  if (CompareMem (InitBoot, ANDROID_MAGIC, 8) != 0) {
    Print (L"BootAndroid: bad %a magic\n", PartitionName);
    return EFI_INVALID_PARAMETER;
  }
  GenRsz = ((ANDROID_BOOT_HDR_V4 *)InitBoot)->RamdiskSize;
  Print (L"BootAndroid: %a generic ramdisk %u bytes\n", PartitionName, GenRsz);
  Status = ExtendPartitionImage (
             &InitBoot,
             BOOT_HEADER_PAGE,
             BOOT_HEADER_PAGE + (UINTN)GenRsz,
             StartLba,
             InitBootLen,
             Bio
             );
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: read %a ramdisk failed (%r)\n", PartitionName, Status);
    return Status;
  }

  // 3. vendor_boot%a -> vendor ramdisk + bootconfig + cmdline
  AsciiSPrint (PartitionName, sizeof (PartitionName), "vendor_boot%a", SlotSuffix);
  Status = FindAndroidPartition (PartitionName, &Bio, &StartLba, &VbImgLen);
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: no %a partition (%r)\n", PartitionName, Status);
    return Status;
  }
  if (VbImgLen < BOOT_HEADER_PAGE) {
    Print (L"BootAndroid: %a partition is too small (%lu)\n", PartitionName, VbImgLen);
    return EFI_VOLUME_CORRUPTED;
  }
  Status = ReadPartition (Bio, StartLba, BOOT_HEADER_PAGE, &VbImg);
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: read %a header failed (%r)\n", PartitionName, Status);
    return Status;
  }
  VbHdr = (CONST VENDOR_BOOT_HDR_V4 *)VbImg;
  if (CompareMem (VbHdr->Magic, VNDRBOOT_MAGIC, 8) != 0) {
    Print (L"BootAndroid: bad %a magic\n", PartitionName);
    return EFI_INVALID_PARAMETER;
  }
  ParseVendorBoot (VbHdr, &VramOff, &VramLen, &BcOff, &BcLen, &Page);
  Print (L"BootAndroid: vendor_boot v%d vram @0x%lx+%lu bc @0x%lx+%lu page %u\n",
         VbHdr->HeaderVersion, VramOff, VramLen, BcOff, BcLen, Page);
  //
  // bootconfig is the last region the boot path consumes, so its end is the
  // image end; everything before it (vram, dtb, table) is inside that range.
  //
  Status = ExtendPartitionImage (
             &VbImg,
             BOOT_HEADER_PAGE,
             BcOff + BcLen,
             StartLba,
             VbImgLen,
             Bio
             );
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: read %a payload failed (%r)\n", PartitionName, Status);
    return Status;
  }
  //
  // ExtendPartitionImage copies the header page into the new buffer, so the
  // header pointer from before is stale; the cmdline is read from it later.
  //
  VbHdr = (CONST VENDOR_BOOT_HDR_V4 *)VbImg;

  // 3b. vendor_boot DTB -> EFI FDT config table.
  //
  // This is the DT the Android kernel is validated with: the vendor's boot
  // chain passes it (the U-Boot boot image path does the same), and the
  // platform's built-in board DTB - published earlier by
  // PlatformBootManagerLib::InstallPlatformFdt() - is replaced here when the
  // vendor_boot image carries one.
  //
  // The platform DT is not equivalent for Android: with it the second
  // system suspend never resumes (the AP stops in the BL31/ARISC standby
  // hand-off), while this tree survives repeated suspend/resume cycles.
  // Keep the vendor tree whenever the image provides it, and fall back to
  // the platform tree when it does not.
  if (VbHdr->DtbSize > 0) {
    UINTN   DtbOff = VramOff + ALIGN_VALUE (VramLen, Page);
    VOID   *Dtb    = AllocateCopyPool ((UINTN)VbHdr->DtbSize,
                                       (UINT8 *)VbImg + DtbOff);
    if (Dtb != NULL) {
      EFI_GUID DtTableGuid = { 0xb1b621d5, 0xf19c, 0x41a5,
                               { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
      Status = gBS->InstallConfigurationTable (&DtTableGuid, Dtb);
      if (!EFI_ERROR (Status)) {
        Print (L"BootAndroid: vendor_boot DTB installed (%u bytes)\n",
               (UINTN)VbHdr->DtbSize);
      }
    }
  }
  // 4. assemble initrd = [vram][generic][bootconfig+trailer]
  //    bootconfig is augmented with the vendor-U-Boot-style androidboot.*
  //    nodes (see AppendUbootBootconfig), +1KiB headroom for the trailer.
  InitrdCap = VramLen + GenRsz + BcLen + 1024;
  Initrd    = AllocatePool (InitrdCap);
  if (Initrd == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (Initrd, (UINT8 *)VbImg + VramOff, VramLen);
  CopyMem (Initrd + VramLen, (UINT8 *)InitBoot + BOOT_HEADER_PAGE, GenRsz);
  BcBlob    = Initrd + VramLen + GenRsz;
  {
    UINTN  BcAugLen;
    AppendUbootBootconfig (Initrd, (UINT8 *)VbImg + BcOff, BcLen, SlotSuffix, BcBlob, &BcAugLen);
    BcBlobLen = AddBootconfigTrailer (BcBlob, BcAugLen, BcBlob);
  }
  InitrdLen = VramLen + GenRsz + BcBlobLen;
  mInitrdData = Initrd;
  mInitrdSize = InitrdLen;
  Print (L"BootAndroid: initrd assembled: %lu + %u + %lu = %lu bytes\n",
         VramLen, GenRsz, BcBlobLen, InitrdLen);

  // 5. initrd via Linux LoadFile2
  InitrdHandle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &InitrdHandle,
                  &gEfiDevicePathProtocolGuid, &mInitrdDevicePath,
                  &gEfiLoadFile2ProtocolGuid,  &mInitrdLoadFile2,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: install initrd LoadFile2 failed (%r)\n", Status);
    return Status;
  }
  Print (L"BootAndroid: initrd registered via LoadFile2\n");

  // 6. kernel -> LoadImage + cmdline + StartImage
  // Memory-mapped device path for the in-pool kernel image (same template
  // as EmbeddedPkg AndroidBootImgLib mMemoryDevicePathTemplate, which is a
  // MEMMAP node followed by an END node; the END node is required or
  // LoadImage walks past the structure and hangs).
  {
    struct {
      MEMMAP_DEVICE_PATH       Node1;
      EFI_DEVICE_PATH_PROTOCOL End;
    } KernelDevicePath;

    KernelDevicePath.Node1.Header.Type    = HARDWARE_DEVICE_PATH;
    KernelDevicePath.Node1.Header.SubType = HW_MEMMAP_DP;
    SetDevicePathNodeLength (&KernelDevicePath.Node1.Header,
                             sizeof (MEMMAP_DEVICE_PATH));
    KernelDevicePath.Node1.MemoryType     = EfiLoaderData;
    KernelDevicePath.Node1.StartingAddress =
      (EFI_PHYSICAL_ADDRESS)(UINTN)Kernel;
    KernelDevicePath.Node1.EndingAddress =
      (EFI_PHYSICAL_ADDRESS)(UINTN)Kernel + KernelLen;
    KernelDevicePath.End.Type    = END_DEVICE_PATH_TYPE;
    KernelDevicePath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    SetDevicePathNodeLength (&KernelDevicePath.End,
                             sizeof (EFI_DEVICE_PATH_PROTOCOL));

    Print (L"BootAndroid: LoadImage(kernel @0x%lx +%u) begin\n",
           (UINTN)Kernel, (UINTN)KernelLen);
    Print (L"BootAndroid: kernel[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
           ((UINT8 *)Kernel)[0], ((UINT8 *)Kernel)[1], ((UINT8 *)Kernel)[2],
           ((UINT8 *)Kernel)[3], ((UINT8 *)Kernel)[4], ((UINT8 *)Kernel)[5],
           ((UINT8 *)Kernel)[6], ((UINT8 *)Kernel)[7]);
    Status = gBS->LoadImage (TRUE, ImageHandle,
                             (EFI_DEVICE_PATH_PROTOCOL *)&KernelDevicePath,
                             (VOID *)Kernel, KernelLen, &KernelHandle);
    Print (L"BootAndroid: LoadImage done %r\n", Status);
  }
  if (EFI_ERROR (Status)) {
    Print (L"BootAndroid: LoadImage(kernel) failed (%r)\n", Status);
    return Status;
  }

  Status = gBS->HandleProtocol (KernelHandle,
                                &gEfiLoadedImageProtocolGuid, (VOID **)&KernelLi);
  if (!EFI_ERROR (Status)) {
    // cmdline = vendor_boot cmdline + PcdBootAndroidExtraCmdline
    ExtraCmd   = (CHAR16 *)PcdGetPtr (PcdBootAndroidExtraCmdline);
    CmdBaseLen = AsciiStrLen ((CHAR8 *)VbHdr->Cmdline);
    KernelLi->LoadOptions = AllocatePool (
                              (CmdBaseLen + StrLen (ExtraCmd) + 2) * sizeof (CHAR16));
    if (KernelLi->LoadOptions != NULL) {
      CHAR16  *Tmp = KernelLi->LoadOptions;
      UINTN   I;
      for (I = 0; I < CmdBaseLen; I++) {
        Tmp[I] = (CHAR16)VbHdr->Cmdline[I];
      }
      Tmp[CmdBaseLen] = L' ';
      StrCpyS (Tmp + CmdBaseLen + 1, StrLen (ExtraCmd) + 1, ExtraCmd);
      KernelLi->LoadOptionsSize =
        (UINT32)((CmdBaseLen + 1 + StrLen (ExtraCmd) + 1) * sizeof (CHAR16));
    }
  }

  Print (L"BootAndroid: starting kernel\n");
  Status = gBS->StartImage (KernelHandle, NULL, NULL);

  Print (L"BootAndroid: kernel returned (%r)\n", Status);
  return Status;
}
