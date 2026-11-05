/** @file
  BootExtlinux — the U-Boot `sysboot` path, native in EDK2.

  Scans every SimpleFileSystem volume for the two configuration paths
  U-Boot's distro_bootcmd checks (`extlinux/extlinux.conf` at the volume
  root and under `boot/`), parses the syslinux-style file, shows a menu,
  and hands the selected entry to the EFI stub:

    - the entry's FDT/FDTDIR tree replaces the platform FDT table, so a
      distro kernel boots with the tree it was built against;
    - INITRD is exposed over LoadFile2 on
      VenMedia(LINUX_EFI_INITRD_MEDIA_GUID);
    - APPEND becomes the kernel command line in LoadOptions.

  The kernel itself is read by LoadImage through the same volume, so
  nothing is copied into a temporary buffer.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
#include <Library/A733BoardFdtLib.h>
#include <Library/PcdLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/DevicePath.h>
#include <Guid/FileInfo.h>
#include "Extlinux.h"

#define EXTLINUX_CONF_ROOT   L"\\extlinux\\extlinux.conf"
#define EXTLINUX_CONF_BOOT   L"\\boot\\extlinux\\extlinux.conf"
#define EXTLINUX_MAX_INITRD  8
#define EXTLINUX_DIR_BUFFER  4096

//
// EFI_DTB_TABLE_GUID - the arm64 EFI stub reads the DTB from this table.
//
STATIC EFI_GUID  mFdtTableGuid =
  { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          VenMedia;
  EFI_DEVICE_PATH_PROTOCOL    End;
} INITRD_DEVICE_PATH;
#pragma pack()

//
// LINUX_EFI_INITRD_MEDIA_GUID - the stub matches a LoadFile2 handle on this
// device path.
//
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

STATIC
EFI_STATUS
GetFileSize (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (!EFI_ERROR (Status)) {
    *Size = (UINTN)Info->FileSize;
  }

  FreePool (Info);
  File->Close (File);
  return Status;
}

/**
  Read a whole file into pool memory and append a NUL so text files can be
  parsed in place.
**/
STATIC
EFI_STATUS
ReadWholeFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT VOID               **Data,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  VOID               *Buffer;
  UINTN              FileSize;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    File->Close (File);
    return Status;
  }

  FileSize = (UINTN)Info->FileSize;
  FreePool (Info);

  Buffer = AllocatePool (FileSize + 1);
  if (Buffer == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read (File, &FileSize, Buffer);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    return Status;
  }

  ((UINT8 *)Buffer)[FileSize] = 0;
  *Data                       = Buffer;
  *Size                       = FileSize;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReadInto (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT UINT8              *Buffer,
  IN  UINTN              Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINTN              ReadSize;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ReadSize = Size;
  Status   = File->Read (File, &ReadSize, Buffer);
  File->Close (File);
  if (!EFI_ERROR (Status) && (ReadSize != Size)) {
    Status = EFI_DEVICE_ERROR;
  }

  return Status;
}

STATIC
VOID
JoinPath (
  OUT CHAR16       *Out,
  IN  UINTN        OutChars,
  IN  CONST CHAR16 *Directory,
  IN  CONST CHAR16 *Name
  )
{
  UINTN  Length;

  Length = 0;
  while ((*Directory != L'\0') && (Length + 1 < OutChars)) {
    Out[Length++] = *Directory++;
  }

  if ((Length > 0) && (Out[Length - 1] != L'\\') && (Length + 1 < OutChars)) {
    Out[Length++] = L'\\';
  }

  while ((*Name != L'\0') && (Length + 1 < OutChars)) {
    Out[Length++] = *Name++;
  }

  Out[Length] = L'\0';
}

STATIC
BOOLEAN
IsDtbName (
  IN CONST CHAR16  *Name
  )
{
  UINTN  Length;

  Length = StrLen (Name);
  if (Length <= 4) {
    return FALSE;
  }

  return ExtlinuxStriCmp (Name + Length - 4, L".dtb") == 0;
}

/**
  Look for a device tree in one directory.

  The file named by Preferred wins; the first .dtb is kept as First for the
  caller to fall back to when no directory holds the preferred name.  First is
  only written when it is still empty, so the first tree found anywhere in the
  search stays the fallback.

  @param[in]     Root       Volume root.
  @param[in]     Directory  Directory to scan.
  @param[in]     Preferred  File name to prefer, or NULL.
  @param[out]    Match      Receives the preferred tree's full path, or an
                            empty string when it is not here.
  @param[in]     MatchChars Capacity of Match in characters.
  @param[in,out] First      Receives the first tree's full path when empty.
  @param[in]     FirstChars Capacity of First in characters.
**/
STATIC
VOID
ScanDtbDir (
  IN     EFI_FILE_PROTOCOL  *Root,
  IN     CONST CHAR16       *Directory,
  IN     CONST CHAR16       *Preferred,
  OUT    CHAR16             *Match,
  IN     UINTN              MatchChars,
  IN OUT CHAR16             *First,
  IN     UINTN              FirstChars
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Dir;
  CHAR16             Path[EXTLINUX_PATH_LEN];
  UINT8              *Buffer;
  UINTN              BufferSize;
  UINTN              Offset;
  EFI_FILE_INFO      *Info;

  Match[0] = L'\0';

  Status = Root->Open (Root, &Dir, (CHAR16 *)Directory, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return;
  }

  Buffer = AllocatePool (EXTLINUX_DIR_BUFFER);
  if (Buffer == NULL) {
    Dir->Close (Dir);
    return;
  }

  for (;;) {
    BufferSize = EXTLINUX_DIR_BUFFER;
    Status     = Dir->Read (Dir, &BufferSize, Buffer);
    if (EFI_ERROR (Status) || (BufferSize == 0)) {
      break;
    }

    Offset = 0;
    while (Offset + sizeof (EFI_FILE_INFO) <= BufferSize) {
      Info = (EFI_FILE_INFO *)(Buffer + Offset);
      if (Info->Size == 0) {
        break;
      }

      if ((!(Info->Attribute & EFI_FILE_DIRECTORY)) &&
          IsDtbName (Info->FileName))
      {
        JoinPath (Path, ARRAY_SIZE (Path), Directory, Info->FileName);
        if (First[0] == L'\0') {
          StrCpyS (First, FirstChars, Path);
        }

        if ((Preferred != NULL) && (Preferred[0] != L'\0') &&
            (ExtlinuxStriCmp (Info->FileName, Preferred) == 0))
        {
          StrCpyS (Match, MatchChars, Path);
          break;
        }
      }

      Offset += Info->Size;
    }

    if (Match[0] != L'\0') {
      break;
    }
  }

  FreePool (Buffer);
  Dir->Close (Dir);
}

/**
  Find the entry's device tree under the directory FDTDIR names.

  FDTDIR is the directory a distribution installs its trees in, and
  distributions do not agree on its shape: some put the .dtb files straight in
  it, and Debian lays them out one level down, in a directory named after the
  SoC family (allwinner/sun60i-a733-cubie-a7a.dtb).  Both are searched, with
  the board's own file name (PcdBootExtlinuxFdtFile) preferred over whatever
  happens to be first.  The fallback matters only for a directory that holds a
  single tree, because a scan cannot choose between many boards.

  @param[in]  Root       Volume root.
  @param[in]  Directory  Directory named by FDTDIR, in UEFI form.
  @param[in]  Preferred  Board tree's file name, or NULL.
  @param[out] Out        Receives the tree's full path.
  @param[in]  OutChars   Capacity of Out in characters.

  @retval EFI_SUCCESS    A tree was found.
  @retval EFI_NOT_FOUND  The directory and its immediate subdirectories have
                         no .dtb file.
**/
STATIC
EFI_STATUS
FindDtbInDir (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Directory,
  IN  CONST CHAR16       *Preferred,
  OUT CHAR16             *Out,
  IN  UINTN              OutChars
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Dir;
  CHAR16             Normalized[EXTLINUX_PATH_LEN];
  CHAR16             SubPath[EXTLINUX_PATH_LEN];
  CHAR16             Match[EXTLINUX_PATH_LEN];
  CHAR16             First[EXTLINUX_PATH_LEN];
  UINT8              *Buffer;
  UINTN              BufferSize;
  UINTN              Offset;
  UINTN              Length;
  EFI_FILE_INFO      *Info;

  //
  // FDTDIR's trailing slash is part of the extlinux spelling (it names a
  // directory), but a file protocol opens the path without it.
  //
  Length = StrLen (Directory);
  while ((Length > 1) && (Directory[Length - 1] == L'\\')) {
    Length--;
  }

  if ((Length == 0) || (Length >= EXTLINUX_PATH_LEN)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Normalized, Directory, Length * sizeof (CHAR16));
  Normalized[Length] = L'\0';

  First[0] = L'\0';

  ScanDtbDir (Root, Normalized, Preferred, Match, ARRAY_SIZE (Match), First, ARRAY_SIZE (First));
  if (Match[0] != L'\0') {
    StrCpyS (Out, OutChars, Match);
    return EFI_SUCCESS;
  }

  Status = Root->Open (Root, &Dir, Normalized, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Buffer = AllocatePool (EXTLINUX_DIR_BUFFER);
  if (Buffer == NULL) {
    Dir->Close (Dir);
    return EFI_OUT_OF_RESOURCES;
  }

  for (;;) {
    BufferSize = EXTLINUX_DIR_BUFFER;
    Status     = Dir->Read (Dir, &BufferSize, Buffer);
    if (EFI_ERROR (Status) || (BufferSize == 0)) {
      break;
    }

    Offset = 0;
    while (Offset + sizeof (EFI_FILE_INFO) <= BufferSize) {
      Info = (EFI_FILE_INFO *)(Buffer + Offset);
      if (Info->Size == 0) {
        break;
      }

      if ((Info->Attribute & EFI_FILE_DIRECTORY) &&
          (StrCmp (Info->FileName, L".") != 0) &&
          (StrCmp (Info->FileName, L"..") != 0))
      {
        JoinPath (SubPath, ARRAY_SIZE (SubPath), Normalized, Info->FileName);
        ScanDtbDir (Root, SubPath, Preferred, Match, ARRAY_SIZE (Match),
                    First, ARRAY_SIZE (First));
        if (Match[0] != L'\0') {
          StrCpyS (Out, OutChars, Match);
          FreePool (Buffer);
          Dir->Close (Dir);
          return EFI_SUCCESS;
        }
      }

      Offset += Info->Size;
    }
  }

  FreePool (Buffer);
  Dir->Close (Dir);

  if (First[0] != L'\0') {
    StrCpyS (Out, OutChars, First);
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

/**
  Read one or more comma-separated INITRD files into one buffer.  Linux
  accepts concatenated cpio archives, which is how syslinux expresses
  multiple initrds.
**/
STATIC
EFI_STATUS
LoadInitrd (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *BaseDir,
  IN  CONST CHAR16       *List,
  OUT VOID               **Data,
  OUT UINTN              *Size
  )
{
  CHAR16      Work[EXTLINUX_PATH_LEN];
  CHAR16      *Item[EXTLINUX_MAX_INITRD];
  UINTN       ItemCount;
  CHAR16      *Cursor;
  CHAR16      UefiPath[EXTLINUX_PATH_LEN];
  UINTN       Total;
  UINTN       Index;
  UINTN       FileSize;
  UINT8       *Buffer;
  UINTN       Offset;
  EFI_STATUS  Status;

  *Data = NULL;
  *Size = 0;

  if ((List == NULL) || (List[0] == L'\0')) {
    return EFI_SUCCESS;
  }

  ExtlinuxCopyString (Work, EXTLINUX_PATH_LEN, List);
  Work[EXTLINUX_PATH_LEN - 1] = L'\0';

  ItemCount = 0;
  Cursor    = Work;
  while ((*Cursor != L'\0') && (ItemCount < EXTLINUX_MAX_INITRD)) {
    Item[ItemCount++] = Cursor;
    while ((*Cursor != L'\0') && (*Cursor != L',')) {
      Cursor++;
    }

    if (*Cursor == L',') {
      *Cursor = L'\0';
      Cursor++;
      while (*Cursor == L' ') {
        Cursor++;
      }
    }
  }

  Total = 0;
  for (Index = 0; Index < ItemCount; Index++) {
    ExtlinuxToUefiPath (BaseDir, Item[Index], UefiPath, ARRAY_SIZE (UefiPath));
    Status = GetFileSize (Root, UefiPath, &FileSize);
    if (EFI_ERROR (Status)) {
      Print (L"BootExtlinux: initrd %s: %r\n", UefiPath, Status);
      return Status;
    }

    Total += FileSize;
  }

  if (Total == 0) {
    return EFI_NOT_FOUND;
  }

  Buffer = AllocatePool (Total);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Offset = 0;
  for (Index = 0; Index < ItemCount; Index++) {
    ExtlinuxToUefiPath (BaseDir, Item[Index], UefiPath, ARRAY_SIZE (UefiPath));
    Status = GetFileSize (Root, UefiPath, &FileSize);
    if (!EFI_ERROR (Status)) {
      Status = ReadInto (Root, UefiPath, Buffer + Offset, FileSize);
    }

    if (EFI_ERROR (Status)) {
      FreePool (Buffer);
      return Status;
    }

    Offset += FileSize;
  }

  *Data = Buffer;
  *Size = Total;
  return EFI_SUCCESS;
}

/**
  Load and start one extlinux entry.

  The caller owns the volume and keeps it open, so a failed boot returns
  here with the filesystem still mounted for the next attempt.
**/
STATIC
EFI_STATUS
BootEntry (
  IN EFI_HANDLE             ImageHandle,
  IN EFI_HANDLE             FsHandle,
  IN EFI_FILE_PROTOCOL      *Root,
  IN CONST EXTLINUX_ENTRY   *Entry,
  IN CONST CHAR16           *BaseDir
  )
{
  EFI_STATUS                 Status;
  CHAR16                     KernelPath[EXTLINUX_PATH_LEN];
  CHAR16                     DtbPath[EXTLINUX_PATH_LEN];
  VOID                       *Dtb;
  UINTN                      DtbSize;
  EFI_PHYSICAL_ADDRESS       DtbAddress;
  BOOLEAN                    DtbPublished;
  VOID                       *PreviousFdt;
  BOOLEAN                    HadPreviousFdt;
  EFI_HANDLE                 InitrdHandle;
  VOID                       *Initrd;
  UINTN                      InitrdSize;
  EFI_DEVICE_PATH_PROTOCOL   *KernelDevicePath;
  EFI_HANDLE                 KernelHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *KernelImage;

  Dtb              = NULL;
  DtbSize          = 0;
  DtbAddress       = 0;
  DtbPublished     = FALSE;
  PreviousFdt      = NULL;
  HadPreviousFdt   = FALSE;
  InitrdHandle     = NULL;
  Initrd           = NULL;
  InitrdSize       = 0;
  KernelDevicePath = NULL;
  KernelHandle     = NULL;
  Status           = EFI_SUCCESS;

  if (Entry->Kernel[0] == L'\0') {
    Print (L"BootExtlinux: entry has no linux/kernel line\n");
    return EFI_INVALID_PARAMETER;
  }

  ExtlinuxToUefiPath (BaseDir, Entry->Kernel, KernelPath, ARRAY_SIZE (KernelPath));
  Print (L"BootExtlinux: kernel %s\n", KernelPath);

  //
  // 1. Device tree.  An entry that names one wins over the tree the platform
  //    published: a distro kernel is built against the tree the distro ships.
  //
  if ((Entry->Fdt[0] != L'\0') || (Entry->FdtDir[0] != L'\0')) {
    if (Entry->Fdt[0] != L'\0') {
      ExtlinuxToUefiPath (BaseDir, Entry->Fdt, DtbPath, ARRAY_SIZE (DtbPath));
      Status = ReadWholeFile (Root, DtbPath, &Dtb, &DtbSize);
    } else {
      CHAR16        Directory[EXTLINUX_PATH_LEN];
      CONST CHAR16  *FdtName;

      ExtlinuxToUefiPath (BaseDir, Entry->FdtDir, Directory, ARRAY_SIZE (Directory));
      FdtName = (CONST CHAR16 *)PcdGetPtr (PcdBootExtlinuxFdtFile);
      if ((FdtName != NULL) && (FdtName[0] != L'\0')) {
        JoinPath (DtbPath, ARRAY_SIZE (DtbPath), Directory, FdtName);
        Status = ReadWholeFile (Root, DtbPath, &Dtb, &DtbSize);
        if (!EFI_ERROR (Status)) {
          Print (L"BootExtlinux: DTB %s (PCD name)\n", DtbPath);
        }
      } else {
        Status = EFI_NOT_FOUND;
      }

      if (EFI_ERROR (Status)) {
        Status = FindDtbInDir (Root, Directory, FdtName, DtbPath, ARRAY_SIZE (DtbPath));
        if (!EFI_ERROR (Status)) {
          Print (L"BootExtlinux: DTB %s (directory scan)\n", DtbPath);
          Status = ReadWholeFile (Root, DtbPath, &Dtb, &DtbSize);
        }
      }
    }

    if (EFI_ERROR (Status)) {
      Print (L"BootExtlinux: cannot load the entry's DTB: %r\n", Status);
      goto Cleanup;
    }

    //
    // Room for the board fix-ups: they add properties, and libfdt can only
    // grow a tree inside the buffer its header describes.
    //
    Status = gBS->AllocatePages (
                    AllocateAnyPages,
                    EfiACPIReclaimMemory,
                    EFI_SIZE_TO_PAGES (DtbSize + SIZE_4KB),
                    &DtbAddress
                    );
    if (EFI_ERROR (Status)) {
      Print (L"BootExtlinux: AllocatePages(DTB) failed: %r\n", Status);
      goto Cleanup;
    }

    if (FdtOpenInto (Dtb, (VOID *)(UINTN)DtbAddress, (INT32)(DtbSize + SIZE_4KB)) != 0) {
      Print (L"BootExtlinux: cannot resize the entry's DTB, using it as-is\n");
      CopyMem ((VOID *)(UINTN)DtbAddress, Dtb, DtbSize);
    } else {
      A733FdtApplyBoardFixups ((VOID *)(UINTN)DtbAddress);
    }

    FreePool (Dtb);
    Dtb = NULL;

    if (!EFI_ERROR (EfiGetSystemConfigurationTable (&mFdtTableGuid, &PreviousFdt)) &&
        (PreviousFdt != NULL))
    {
      HadPreviousFdt = TRUE;
    }

    Status = gBS->InstallConfigurationTable (&mFdtTableGuid, (VOID *)(UINTN)DtbAddress);
    if (EFI_ERROR (Status)) {
      Print (L"BootExtlinux: install FDT table failed: %r\n", Status);
      goto Cleanup;
    }

    DtbPublished = TRUE;
    Print (L"BootExtlinux: FDT installed @ 0x%lx (%u bytes)\n", DtbAddress, (UINTN)DtbSize);
  } else {
    Print (L"BootExtlinux: no FDT/FDTDIR, keeping the platform tree\n");
  }

  //
  // 2. INITRD -> LoadFile2.
  //
  if (Entry->Initrd[0] != L'\0') {
    Status = LoadInitrd (Root, BaseDir, Entry->Initrd, &Initrd, &InitrdSize);
    if (EFI_ERROR (Status)) {
      Print (L"BootExtlinux: cannot load the initrd: %r\n", Status);
      goto Cleanup;
    }

    mInitrdData = Initrd;
    mInitrdSize = InitrdSize;

    Status = gBS->InstallMultipleProtocolInterfaces (
                    &InitrdHandle,
                    &gEfiDevicePathProtocolGuid, &mInitrdDevicePath,
                    &gEfiLoadFile2ProtocolGuid,  &mInitrdLoadFile2,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      Print (L"BootExtlinux: install initrd LoadFile2 failed: %r\n", Status);
      goto Cleanup;
    }

    Print (L"BootExtlinux: initrd registered (%u bytes)\n", (UINTN)InitrdSize);
  }

  //
  // 3. Kernel -> LoadImage, APPEND -> LoadOptions.
  //
  KernelDevicePath = FileDevicePath (FsHandle, KernelPath);
  if (KernelDevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Cleanup;
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, KernelDevicePath, NULL, 0, &KernelHandle);
  if (EFI_ERROR (Status)) {
    Print (L"BootExtlinux: LoadImage(%s) failed: %r\n", KernelPath, Status);
    goto Cleanup;
  }

  Status = gBS->HandleProtocol (KernelHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&KernelImage);
  if (!EFI_ERROR (Status) && (Entry->Append[0] != L'\0')) {
    KernelImage->LoadOptions     = (VOID *)Entry->Append;
    KernelImage->LoadOptionsSize = (UINT32)((StrLen (Entry->Append) + 1) * sizeof (CHAR16));
  }

  Print (L"BootExtlinux: cmdline: %s\n", Entry->Append);
  Status = gBS->StartImage (KernelHandle, NULL, NULL);
  Print (L"BootExtlinux: kernel returned: %r\n", Status);
  if (!EFI_ERROR (Status)) {
    Status = EFI_DEVICE_ERROR;
  }

Cleanup:
  if (KernelHandle != NULL) {
    gBS->UnloadImage (KernelHandle);
  }

  if (KernelDevicePath != NULL) {
    FreePool (KernelDevicePath);
  }

  if (InitrdHandle != NULL) {
    gBS->UninstallMultipleProtocolInterfaces (
          InitrdHandle,
          &gEfiDevicePathProtocolGuid, &mInitrdDevicePath,
          &gEfiLoadFile2ProtocolGuid,  &mInitrdLoadFile2,
          NULL
          );
  }

  mInitrdData = NULL;
  mInitrdSize = 0;

  if (Initrd != NULL) {
    FreePool (Initrd);
  }

  if (DtbPublished) {
    if (HadPreviousFdt) {
      gBS->InstallConfigurationTable (&mFdtTableGuid, PreviousFdt);
    } else {
      gBS->InstallConfigurationTable (&mFdtTableGuid, NULL);
    }
  }

  if (DtbAddress != 0) {
    gBS->FreePages (DtbAddress, EFI_SIZE_TO_PAGES (DtbSize));
  }

  if (Dtb != NULL) {
    FreePool (Dtb);
  }

  return Status;
}

STATIC
BOOLEAN
DevicePathMatches (
  IN EFI_DEVICE_PATH_PROTOCOL  *First,
  IN EFI_DEVICE_PATH_PROTOCOL  *Second
  )
{
  UINTN  FirstSize;
  UINTN  SecondSize;

  if ((First == NULL) || (Second == NULL)) {
    return FALSE;
  }

  FirstSize  = GetDevicePathSize (First);
  SecondSize = GetDevicePathSize (Second);
  if (FirstSize != SecondSize) {
    return FALSE;
  }

  return (BOOLEAN)(CompareMem (First, Second, FirstSize) == 0);
}

STATIC
EFI_STATUS
FindConfig (
  IN  EFI_DEVICE_PATH_PROTOCOL  *TargetDp  OPTIONAL,
  OUT EFI_HANDLE                *FsHandle,
  OUT EFI_FILE_PROTOCOL         **Root,
  OUT EXTLINUX_CONFIG           *Config,
  OUT CHAR16                    *BaseDir
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       HandleIndex;
  UINTN       Candidate;

  STATIC CONST CHAR16  *Candidates[2] = {
    EXTLINUX_CONF_ROOT,
    EXTLINUX_CONF_BOOT
  };
  STATIC CONST CHAR16  *BaseDirs[2] = {
    L"\\extlinux",
    L"\\boot\\extlinux"
  };

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (HandleIndex = 0; HandleIndex < HandleCount; HandleIndex++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
    EFI_FILE_PROTOCOL                *VolumeRoot;

    if (TargetDp != NULL) {
      //
      // The boot option names one volume: only that one can serve it, or a
      // card reader with two slots would boot the wrong card's configuration.
      //
      if (!DevicePathMatches (DevicePathFromHandle (Handles[HandleIndex]), TargetDp)) {
        continue;
      }
    }

    Status = gBS->HandleProtocol (
                    Handles[HandleIndex],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = FileSystem->OpenVolume (FileSystem, &VolumeRoot);
    if (EFI_ERROR (Status)) {
      continue;
    }

    for (Candidate = 0; Candidate < ARRAY_SIZE (Candidates); Candidate++) {
      CHAR8  *Text;
      UINTN  TextSize;

      Status = ReadWholeFile (VolumeRoot, Candidates[Candidate], (VOID **)&Text, &TextSize);
      if (EFI_ERROR (Status)) {
        continue;
      }

      Status = ExtlinuxParse (Text, TextSize, Config);
      FreePool (Text);
      if (!EFI_ERROR (Status)) {
        break;
      }
    }

    if (!EFI_ERROR (Status)) {
      *FsHandle = Handles[HandleIndex];
      *Root     = VolumeRoot;
      ExtlinuxCopyString (BaseDir, EXTLINUX_PATH_LEN, BaseDirs[Candidate]);
      BaseDir[EXTLINUX_PATH_LEN - 1] = L'\0';
      Print (L"BootExtlinux: %s on volume %u\n", Candidates[Candidate], (UINTN)HandleIndex);
      FreePool (Handles);
      return EFI_SUCCESS;
    }

    VolumeRoot->Close (VolumeRoot);
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
BootExtlinuxEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_HANDLE            FsHandle;
  EFI_FILE_PROTOCOL         *Root;
  EXTLINUX_CONFIG           *Config;
  CHAR16                    BaseDir[EXTLINUX_PATH_LEN];
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL  *TargetDp;

  Root     = NULL;
  TargetDp = NULL;

  Config = AllocatePool (sizeof (*Config));
  if (Config == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // The platform boot manager registers one option per volume that carries a
  // configuration and passes that volume's device path in OptionalData.  No
  // data means "scan every volume", which is what a hand-started copy or an
  // older boot option gets.
  //
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (!EFI_ERROR (Status) &&
      (LoadedImage->LoadOptions != NULL) &&
      (LoadedImage->LoadOptionsSize >= sizeof (EFI_DEVICE_PATH_PROTOCOL)))
  {
    TargetDp = (EFI_DEVICE_PATH_PROTOCOL *)LoadedImage->LoadOptions;
    if ((TargetDp->Type != HARDWARE_DEVICE_PATH) &&
        (TargetDp->Type != MESSAGING_DEVICE_PATH) &&
        (TargetDp->Type != MEDIA_DEVICE_PATH))
    {
      TargetDp = NULL;
    }
  }

  Status = FindConfig (TargetDp, &FsHandle, &Root, Config, BaseDir);
  if (EFI_ERROR (Status) && (TargetDp != NULL)) {
    Print (L"BootExtlinux: the volume this entry names is gone; scanning instead\n");
    TargetDp = NULL;
    Status   = FindConfig (NULL, &FsHandle, &Root, Config, BaseDir);
  }

  if (EFI_ERROR (Status)) {
    Print (L"BootExtlinux: no usable extlinux/extlinux.conf found\n");
    FreePool (Config);
    return Status;
  }

  for (;;) {
    UINTN         Selected;
    CONST CHAR16  *Label;

    if (!Config->Prompt && (Config->Timeout == 0)) {
      Selected = ExtlinuxDefaultIndex (Config);
    } else {
      Status = ExtlinuxMenuSelect (Config, &Selected);
      if (Status == EFI_ABORTED) {
        Root->Close (Root);
        FreePool (Config);
        return EFI_ABORTED;
      }

      if (EFI_ERROR (Status)) {
        Root->Close (Root);
        FreePool (Config);
        return Status;
      }
    }

    Status = BootEntry (
               ImageHandle,
               FsHandle,
               Root,
               &Config->Entry[Selected],
               BaseDir
               );

    Label = Config->Entry[Selected].MenuLabel[0] != L'\0'
              ? Config->Entry[Selected].MenuLabel
              : Config->Entry[Selected].Label;
    Print (L"BootExtlinux: %s failed: %r\n", Label, Status);
    Print (L"BootExtlinux: back to the menu in 3 s; Esc exits\n");
    gBS->Stall (3 * 1000 * 1000);

    //
    // Drop keys pressed while the error message was up, then show the menu
    // without a countdown: a boot that failed should not auto-repeat.
    //
    {
      EFI_INPUT_KEY  Key;

      while (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key) == EFI_SUCCESS) {
      }
    }

    Config->Prompt  = TRUE;
    Config->Timeout = 0;
  }
}
