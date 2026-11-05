/** @file
  Keep a copy of the non-volatile variable set on a FAT volume.

  The SPI NOR store remains authoritative: VariableRuntimeDxe and A733FvbDxe
  keep serving GetVariable/SetVariable from it, including at OS runtime.  This
  file only mirrors the variables to \EFI\A7A\vars.bin on the SD card (or the
  first FAT volume when no card is present), so a board's boot order and
  settings travel with its card and can be restored onto a fresh store.

  Restore runs once, from A733VarSnapshotInit, when BootOrder is absent - the
  signature of a store that has never been written.  Export runs from a
  ReadyToBoot event and is skipped when the file already matches.  Every path
  fails silently: a missing card, an unreadable volume, a corrupt or
  truncated file simply leaves the NOR store as the only source of truth.

  Which volume holds the file is a build choice: PcdA733BootMedium is 0 for the
  microSD build and 1 for the eMMC build, and the scan tries that medium's
  volume class first (removable for the card, fixed for the eMMC), falling back
  to the other only when it has no usable FAT volume.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>
#include <Guid/Gpt.h>

#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "A733VarSnapshot.h"

//
// The snapshot is a copy of a 64 KB NOR store, so it cannot legitimately be
// larger than that; the cap only bounds a corrupt or hostile file.
//
#define A733_VAR_SNAPSHOT_MAX_SIZE   (256 * 1024)
#define A733_VAR_SNAPSHOT_MAX_RECORDS  1024

//
// UEFI caps a variable name at 1024 bytes; the record extent check below uses
// the same number.
//
#define A733_VAR_SNAPSHOT_NAME_MAX  1024

#define A733_VAR_SNAPSHOT_DIR      L"\\EFI\\A7A"
#define A733_VAR_SNAPSHOT_FILE     L"\\EFI\\A7A\\vars.bin"
#define A733_VAR_SNAPSHOT_BACKUP   L"\\EFI\\A7A\\vars.bak"
#define A733_VAR_SNAPSHOT_TEMP     L"\\EFI\\A7A\\vars.tmp"

#define A733_VAR_SNAPSHOT_MAGIC    SIGNATURE_32 ('A', '7', 'V', 'S')
#define A733_VAR_SNAPSHOT_VERSION  1

//
// PcdA733BootMedium values.  The build picks the medium its firmware boots
// from, and the snapshot follows it: the SD build keeps the file on the card,
// the eMMC build on the eMMC's FAT volume.  Both are "removable or not" to the
// block layer, which is what the scan below sorts on.
//
#define A733_BOOT_MEDIUM_SD    0
#define A733_BOOT_MEDIUM_EMMC  1

//
// Attributes a snapshot record must never carry: the restore path writes
// records straight through SetVariable, so anything that expects an
// authenticated payload would be rejected by design.  APPEND_WRITE is a
// SetVariable modifier, not a stored attribute.
//
#define A733_VAR_SNAPSHOT_BAD_ATTRIBUTES                 \
  (EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS |             \
   EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS |  \
   EFI_VARIABLE_APPEND_WRITE)

#pragma pack(1)

typedef struct {
  UINT32    Magic;
  UINT8     Version;
  UINT8     Flags;
  UINT16    Reserved;
  UINT32    RecordCount;
  UINT32    Crc32;
} A733_VAR_SNAPSHOT_HEADER;

typedef struct {
  UINT16    NameSize;
  UINT32    Attributes;
  UINT32    DataSize;
  EFI_GUID  Guid;
} A733_VAR_SNAPSHOT_RECORD;

#pragma pack()

typedef struct {
  UINT8    *Data;
  UINTN    Size;
  UINTN    Capacity;
} A733_VAR_SNAPSHOT_BUFFER;

//
// Variables that describe this boot rather than this board.  Console paths
// are rebuilt from the connected devices on every boot, BootNext is a
// one-shot request, and OsIndications can trigger capsule processing; none
// of them belong in a snapshot.
//
STATIC CONST CHAR16  *CONST  mExcludedNames[] = {
  L"BootNext",
  L"BootCurrent",
  L"ConIn",
  L"ConOut",
  L"ErrOut",
  L"OsIndications"
};

/**
  CRC-32 (reflected, polynomial 0xEDB88320), fed incrementally.

  @param[in]  Crc   Running value, start with 0xFFFFFFFF.
  @param[in]  Data  Bytes to add.
  @param[in]  Size  Number of bytes.

  @return Updated running value; the final checksum is the bitwise NOT.
**/
STATIC
UINT32
A733VarSnapshotCrc32 (
  IN UINT32       Crc,
  IN CONST VOID   *Data,
  IN UINTN        Size
  )
{
  CONST UINT8  *Bytes;
  UINTN        Index;
  UINTN        Bit;

  Bytes = (CONST UINT8 *)Data;
  for (Index = 0; Index < Size; Index++) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ ((Crc & 1) != 0 ? 0xEDB88320U : 0U);
    }
  }

  return Crc;
}

/**
  Whether a variable is left out of the snapshot.

  @param[in]  Name        Variable name.
  @param[in]  Attributes  Variable attributes.

  @retval TRUE   The variable must not be exported or restored.
  @retval FALSE  The variable belongs in the snapshot.
**/
STATIC
BOOLEAN
A733VarSnapshotSkip (
  IN CONST CHAR16  *Name,
  IN UINT32        Attributes
  )
{
  UINTN  Index;

  if ((Attributes & EFI_VARIABLE_NON_VOLATILE) == 0) {
    return TRUE;
  }

  if ((Attributes & A733_VAR_SNAPSHOT_BAD_ATTRIBUTES) != 0) {
    return TRUE;
  }

  for (Index = 0; Index < ARRAY_SIZE (mExcludedNames); Index++) {
    if (StrCmp (Name, mExcludedNames[Index]) == 0) {
      return TRUE;
    }
  }

  //
  // PlatformRecovery#### is a boot request, not a setting; restoring a stale
  // one would send the next boot into recovery.
  //
  if (StrnCmp (Name, L"PlatformRecovery", 16) == 0) {
    return TRUE;
  }

  return FALSE;
}

/**
  Append bytes to a growing buffer.

  @param[in,out]  Buffer  Buffer to extend.
  @param[in]      Data    Bytes to append.
  @param[in]      Size    Number of bytes.

  @retval EFI_SUCCESS            Appended.
  @retval EFI_BAD_BUFFER_SIZE    The result would exceed the snapshot cap.
  @retval EFI_OUT_OF_RESOURCES   Allocation failed.
**/
STATIC
EFI_STATUS
A733VarSnapshotAppend (
  IN OUT A733_VAR_SNAPSHOT_BUFFER  *Buffer,
  IN CONST VOID                    *Data,
  IN UINTN                         Size
  )
{
  UINTN  Capacity;
  VOID   *NewData;

  if (Size > A733_VAR_SNAPSHOT_MAX_SIZE - Buffer->Size) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (Buffer->Size + Size > Buffer->Capacity) {
    Capacity = (Buffer->Capacity == 0) ? 4096 : Buffer->Capacity;
    while (Capacity < Buffer->Size + Size) {
      if (Capacity >= A733_VAR_SNAPSHOT_MAX_SIZE / 2) {
        Capacity = A733_VAR_SNAPSHOT_MAX_SIZE;
        break;
      }

      Capacity *= 2;
    }

    NewData = ReallocatePool (Buffer->Capacity, Capacity, Buffer->Data);
    if (NewData == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Buffer->Data     = NewData;
    Buffer->Capacity = Capacity;
  }

  CopyMem (Buffer->Data + Buffer->Size, Data, Size);
  Buffer->Size += Size;
  return EFI_SUCCESS;
}

/**
  Serialize the non-volatile variable set.

  @param[out]  Buffer       Receives the full snapshot (header included).
  @param[out]  RecordCount  Receives the number of records.

  @retval EFI_SUCCESS  The snapshot is in Buffer->Data.
  @return              Allocation or size failure; Buffer is not allocated.
**/
STATIC
EFI_STATUS
A733VarSnapshotBuild (
  OUT A733_VAR_SNAPSHOT_BUFFER  *Buffer,
  OUT UINT32                    *RecordCount
  )
{
  EFI_STATUS                 Status;
  CHAR16                     *Name;
  UINTN                      NameSize;
  EFI_GUID                   Guid;
  UINT32                     Attributes;
  UINTN                      DataSize;
  VOID                       *Data;
  UINT16                     NameLength;
  A733_VAR_SNAPSHOT_RECORD   Record;
  A733_VAR_SNAPSHOT_HEADER   Header;
  UINT32                     Count;
  UINT32                     Crc;

  ZeroMem (Buffer, sizeof (*Buffer));
  ZeroMem (&Header, sizeof (Header));
  Status = A733VarSnapshotAppend (Buffer, &Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Name = AllocateZeroPool (A733_VAR_SNAPSHOT_NAME_MAX);
  if (Name == NULL) {
    FreePool (Buffer->Data);
    Buffer->Data = NULL;
    return EFI_OUT_OF_RESOURCES;
  }

  Count = 0;
  for ( ; ; ) {
    //
    // NameSize is in/out: reset the capacity before every call, the contents
    // carry the iteration state (the previous name).
    //
    NameSize = A733_VAR_SNAPSHOT_NAME_MAX;
    Status   = gRT->GetNextVariableName (&NameSize, Name, &Guid);
    if (Status == EFI_NOT_FOUND) {
      break;
    }

    if (Status == EFI_BUFFER_TOO_SMALL) {
      //
      // A name longer than the UEFI limit is a broken store; stop here and
      // keep the records collected so far.
      //
      break;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    Attributes = 0;
    DataSize   = 0;
    Status     = gRT->GetVariable (Name, &Guid, &Attributes, &DataSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      Data = AllocatePool (DataSize);
      if (Data == NULL) {
        continue;
      }

      Status = gRT->GetVariable (Name, &Guid, &Attributes, &DataSize, Data);
      if (EFI_ERROR (Status)) {
        FreePool (Data);
        continue;
      }
    } else if (Status == EFI_SUCCESS) {
      Data     = NULL;
      DataSize = 0;
    } else {
      continue;
    }

    if (A733VarSnapshotSkip (Name, Attributes)) {
      if (Data != NULL) {
        FreePool (Data);
      }

      continue;
    }

    NameLength         = (UINT16)(StrLen (Name) * sizeof (CHAR16));
    Record.NameSize    = NameLength;
    Record.Attributes  = Attributes;
    Record.DataSize    = (UINT32)DataSize;
    Record.Guid        = Guid;

    Status = A733VarSnapshotAppend (Buffer, &Record, sizeof (Record));
    if (!EFI_ERROR (Status)) {
      Status = A733VarSnapshotAppend (Buffer, Name, NameLength);
    }

    if (!EFI_ERROR (Status) && (DataSize != 0)) {
      Status = A733VarSnapshotAppend (Buffer, Data, DataSize);
    }

    if (Data != NULL) {
      FreePool (Data);
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    Count++;
  }

  FreePool (Name);

  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    FreePool (Buffer->Data);
    Buffer->Data = NULL;
    return Status;
  }

  Header.Magic       = A733_VAR_SNAPSHOT_MAGIC;
  Header.Version     = A733_VAR_SNAPSHOT_VERSION;
  Header.Flags       = 0;
  Header.Reserved    = 0;
  Header.RecordCount = Count;
  Crc                = A733VarSnapshotCrc32 (
                         0xFFFFFFFF,
                         Buffer->Data + sizeof (Header),
                         Buffer->Size - sizeof (Header)
                         );
  Header.Crc32 = ~Crc;
  CopyMem (Buffer->Data, &Header, sizeof (Header));

  *RecordCount = Count;
  return EFI_SUCCESS;
}

/**
  Walk a snapshot; either validate it or apply it to the variable store.

  @param[in]  Buffer  Snapshot bytes.
  @param[in]  Size    Snapshot size.
  @param[in]  Apply   TRUE writes every record through SetVariable.

  @retval EFI_SUCCESS            The snapshot is well formed.
  @retval EFI_VOLUME_CORRUPTED   Magic, CRC, or a record extent is wrong.
  @retval EFI_UNSUPPORTED        The format version is not this one.
  @retval EFI_OUT_OF_RESOURCES   Allocation failed while applying.
**/
STATIC
EFI_STATUS
A733VarSnapshotProcess (
  IN CONST VOID  *Buffer,
  IN UINTN       Size,
  IN BOOLEAN     Apply
  )
{
  CONST A733_VAR_SNAPSHOT_HEADER  *Header;
  CONST A733_VAR_SNAPSHOT_RECORD  *Record;
  CONST UINT8                     *Bytes;
  UINTN                           Offset;
  UINT32                          Index;
  UINT32                          Crc;
  UINT32                          Applied;
  CHAR16                          *Name;
  EFI_GUID                        Guid;

  Bytes = (CONST UINT8 *)Buffer;
  if (Size < sizeof (*Header)) {
    return EFI_VOLUME_CORRUPTED;
  }

  Header = (CONST A733_VAR_SNAPSHOT_HEADER *)Bytes;
  if (Header->Magic != A733_VAR_SNAPSHOT_MAGIC) {
    return EFI_VOLUME_CORRUPTED;
  }

  if (Header->Version != A733_VAR_SNAPSHOT_VERSION) {
    return EFI_UNSUPPORTED;
  }

  Crc = ~A733VarSnapshotCrc32 (0xFFFFFFFF, Bytes + sizeof (*Header), Size - sizeof (*Header));
  if (Crc != Header->Crc32) {
    return EFI_VOLUME_CORRUPTED;
  }

  if (Header->RecordCount > A733_VAR_SNAPSHOT_MAX_RECORDS) {
    return EFI_VOLUME_CORRUPTED;
  }

  Offset  = sizeof (*Header);
  Applied = 0;
  for (Index = 0; Index < Header->RecordCount; Index++) {
    if (Offset + sizeof (*Record) > Size) {
      return EFI_VOLUME_CORRUPTED;
    }

    Record  = (CONST A733_VAR_SNAPSHOT_RECORD *)(Bytes + Offset);
    Offset += sizeof (*Record);

    if ((Record->NameSize == 0) ||
        ((Record->NameSize & (sizeof (CHAR16) - 1)) != 0) ||
        (Record->NameSize > A733_VAR_SNAPSHOT_NAME_MAX) ||
        (Record->DataSize > A733_VAR_SNAPSHOT_MAX_SIZE) ||
        (Offset + Record->NameSize + Record->DataSize > Size))
    {
      return EFI_VOLUME_CORRUPTED;
    }

    if (!Apply) {
      Offset += Record->NameSize + Record->DataSize;
      continue;
    }

    Name = AllocatePool (Record->NameSize + sizeof (CHAR16));
    if (Name == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    CopyMem (Name, Bytes + Offset, Record->NameSize);
    Name[Record->NameSize / sizeof (CHAR16)] = 0;
    Offset                                  += Record->NameSize;

    Guid = Record->Guid;
    if (!A733VarSnapshotSkip (Name, Record->Attributes) &&
        !EFI_ERROR (gRT->SetVariable (
                          Name,
                          &Guid,
                          Record->Attributes,
                          Record->DataSize,
                          (VOID *)(Bytes + Offset)
                          )))
    {
      Applied++;
    }

    FreePool (Name);
    Offset += Record->DataSize;
  }

  if (Apply) {
    DEBUG ((
      DEBUG_INFO,
      "A733VarSnapshot: restored %u of %u variables\n",
      Applied,
      Header->RecordCount
      ));
  }

  return EFI_SUCCESS;
}

/**
  Delete a file if it exists.
**/
STATIC
VOID
A733VarSnapshotDeleteFile (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  )
{
  EFI_FILE_PROTOCOL  *File;

  if (!EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
    File->Delete (File);
  }
}

/**
  Read a whole file.

  @param[in]   Root    Volume root.
  @param[in]   Path    Volume-relative path.
  @param[out]  Buffer  Receives an allocated copy.
  @param[out]  Size    Receives the file size.

  @retval EFI_SUCCESS  The file was read.
  @return              It is missing, too large/small, or unreadable.
**/
STATIC
EFI_STATUS
A733VarSnapshotReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT VOID               **Buffer,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              FileSize;
  VOID               *Data;

  *Buffer = NULL;
  *Size   = 0;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_NOT_FOUND;
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

  if ((FileSize < sizeof (A733_VAR_SNAPSHOT_HEADER)) || (FileSize > A733_VAR_SNAPSHOT_MAX_SIZE)) {
    File->Close (File);
    return EFI_VOLUME_CORRUPTED;
  }

  Data = AllocatePool (FileSize);
  if (Data == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = File->Read (File, &FileSize, Data);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (Data);
    return Status;
  }

  *Buffer = Data;
  *Size   = FileSize;
  return EFI_SUCCESS;
}

/**
  Replace a file with the given bytes.

  @param[in]  Root    Volume root.
  @param[in]  Path    Volume-relative path.
  @param[in]  Buffer  Bytes to write.
  @param[in]  Size    Number of bytes.

  @retval EFI_SUCCESS  The file is on the volume and flushed.
  @return              Open, write, or flush failed.
**/
STATIC
EFI_STATUS
A733VarSnapshotWriteFile (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path,
  IN CONST VOID         *Buffer,
  IN UINTN              Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINTN              Written;

  //
  // Remove the old node first: an overwrite that is cut short would leave a
  // tail of the previous file behind, and a missing file is easier to detect
  // than a silently longer one.
  //
  A733VarSnapshotDeleteFile (Root, Path);

  Status = Root->Open (
                   Root,
                   &File,
                   (CHAR16 *)Path,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   0
                   );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733VarSnapshot: cannot open %s: %r\n", Path, Status));
    return Status;
  }

  Written = Size;
  Status  = File->Write (File, &Written, (VOID *)Buffer);
  if (!EFI_ERROR (Status) && (Written != Size)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    //
    // EnhancedFatDxe keeps dirty blocks in its cache; without the explicit
    // Flush the bytes can still be on their way when the next boot starts.
    //
    Status = File->Flush (File);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "A733VarSnapshot: cannot write %s (%u of %u bytes): %r\n",
      Path,
      (UINT32)Written,
      (UINT32)Size,
      Status
      ));
  }

  File->Close (File);
  return Status;
}

/**
  Create one directory (already-existing counts as success).

  A directory has to be created through `Open()`'s Attributes argument; the
  file protocol cannot turn a file into a directory afterwards.  This matters:
  EnhancedFatDxe returns EFI_ACCESS_DENIED for a SetInfo that changes the
  directory bit, and an earlier version of this function left a zero-byte file
  named after the directory on the card, which made every later open of a path
  below it fail with EFI_NOT_FOUND.

  @param[in]  Root  Volume root.
  @param[in]  Path  Volume-relative directory path.

  @retval EFI_SUCCESS  The directory exists.
  @return              Creation failed.
**/
STATIC
EFI_STATUS
A733VarSnapshotCreateDirectory (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Dir;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;

  Status = Root->Open (Root, &Dir, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    Info     = NULL;
    InfoSize = 0;
    Status   = Dir->GetInfo (Dir, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      Info = AllocatePool (InfoSize);
      if (Info != NULL) {
        Status = Dir->GetInfo (Dir, &gEfiFileInfoGuid, &InfoSize, Info);
      }
    }

    if (!EFI_ERROR (Status) && (Info != NULL) &&
        ((Info->Attribute & EFI_FILE_DIRECTORY) != 0))
    {
      FreePool (Info);
      Dir->Close (Dir);
      return EFI_SUCCESS;
    }

    if (Info != NULL) {
      FreePool (Info);
    }

    //
    // A stale plain file bears this name; remove it before creating the
    // directory in its place.
    //
    Dir->Close (Dir);
    if (!EFI_ERROR (
          Root->Open (
                  Root,
                  &Dir,
                  (CHAR16 *)Path,
                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                  0
                  )
          ))
    {
      Dir->Delete (Dir);
    }
  }

  return Root->Open (
                Root,
                &Dir,
                (CHAR16 *)Path,
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                EFI_FILE_DIRECTORY
                );
}

/**
  Make sure \EFI\A7A exists.

  @param[in]  Root  Volume root.

  @retval EFI_SUCCESS  The directory exists.
  @return              Creation failed.
**/
STATIC
EFI_STATUS
A733VarSnapshotEnsureDirectory (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS  Status;

  Status = A733VarSnapshotCreateDirectory (Root, L"\\EFI");
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return A733VarSnapshotCreateDirectory (Root, A733_VAR_SNAPSHOT_DIR);
}

/**
  Whether a volume is an EFI System Partition.

  The card's layout has two FAT partitions - a 16 MB "config" one and the
  300 MB ESP - and both look removable to the block layer, so the medium alone
  cannot say which one the settings belong on.  PartitionDxe's partition info
  protocol carries the answer: an ESP is flagged as such, and its GPT type GUID
  is the ESP one even when the flag is not set.

  @param[in]  Handle  Handle that carries the SimpleFileSystem protocol.

  @retval TRUE   The volume is an EFI System Partition.
  @retval FALSE  It is not, or the partition information is unavailable.
**/
STATIC
BOOLEAN
A733VarSnapshotVolumeIsEsp (
  IN EFI_HANDLE  Handle
  )
{
  EFI_PARTITION_INFO_PROTOCOL  *Info;

  if (EFI_ERROR (
        gBS->HandleProtocol (
               Handle,
               &gEfiPartitionInfoProtocolGuid,
               (VOID **)&Info
               )
        ))
  {
    return FALSE;
  }

  if (Info->Revision != EFI_PARTITION_INFO_PROTOCOL_REVISION) {
    return FALSE;
  }

  if (Info->System != 0) {
    return TRUE;
  }

  return (BOOLEAN)((Info->Type == PARTITION_TYPE_GPT) &&
                   CompareGuid (&Info->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid));
}

/**
  Whether a volume lives on removable media.

  SdDxe marks the SD card's block media removable; partition children inherit
  it.  The restore/export scans put the volume class PcdA733BootMedium names
  first, and fall back to the other when that class has no usable volume.

  @param[in]  Handle  Handle that carries the SimpleFileSystem protocol.

  @retval TRUE   Removable media.
  @retval FALSE  Fixed media, or no BlockIo to tell.
**/
STATIC
BOOLEAN
A733VarSnapshotVolumeIsRemovable (
  IN EFI_HANDLE  Handle
  )
{
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;

  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo))) {
    return FALSE;
  }

  return (BOOLEAN)((BlockIo->Media != NULL) && BlockIo->Media->RemovableMedia);
}

/**
  Restore the first well-formed snapshot found on a FAT volume.
**/
STATIC
VOID
A733VarSnapshotRestoreFromVolumes (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  UINTN                            PathIndex;
  UINTN                            Pass;
  BOOLEAN                          Removable;
  BOOLEAN                          PreferRemovable;
  UINTN                            Rank;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  VOID                             *Data;
  UINTN                            Size;
  STATIC CONST CHAR16  *CONST      Paths[] = {
    A733_VAR_SNAPSHOT_FILE,
    A733_VAR_SNAPSHOT_BACKUP
  };

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  PreferRemovable = (BOOLEAN)(PcdGet8 (PcdA733BootMedium) == A733_BOOT_MEDIUM_SD);

  //
  // Four passes, best first: the build's medium and an EFI System Partition,
  // that medium without one, then the other medium with and without.  The
  // card's two FAT partitions both answer to "removable", and only the
  // partition type separates the settings' ESP from the small config one.
  //
  for (Pass = 0; Pass < 4; Pass++) {
    for (Index = 0; Index < HandleCount; Index++) {
      Removable = A733VarSnapshotVolumeIsRemovable (Handles[Index]);
      Rank      = (UINTN)((Removable == PreferRemovable) ? 2 : 0) |
                  (UINTN)(A733VarSnapshotVolumeIsEsp (Handles[Index]) ? 1 : 0);
      if (Rank != (3 - Pass)) {
        continue;
      }

      Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
      if (EFI_ERROR (Status) || (Fs == NULL)) {
        continue;
      }

      if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
        continue;
      }

      for (PathIndex = 0; PathIndex < ARRAY_SIZE (Paths); PathIndex++) {
        Data = NULL;
        Size = 0;
        if (EFI_ERROR (A733VarSnapshotReadFile (Root, Paths[PathIndex], &Data, &Size))) {
          continue;
        }

        if (!EFI_ERROR (A733VarSnapshotProcess (Data, Size, FALSE))) {
          DEBUG ((
            DEBUG_INFO,
            "A733VarSnapshot: restoring from %s\n",
            Paths[PathIndex]
            ));
          A733VarSnapshotProcess (Data, Size, TRUE);
          FreePool (Data);
          Root->Close (Root);
          FreePool (Handles);
          return;
        }

        FreePool (Data);
      }

      Root->Close (Root);
    }
  }

  FreePool (Handles);
}

/**
  Write a snapshot to the first writable FAT volume, removable first.

  @param[in]  Buffer  Snapshot bytes.
  @param[in]  Size    Snapshot size.
**/
STATIC
VOID
A733VarSnapshotExportToVolumes (
  IN CONST VOID  *Buffer,
  IN UINTN       Size
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  UINTN                            Pass;
  BOOLEAN                          Removable;
  BOOLEAN                          PreferRemovable;
  UINTN                            Rank;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  VOID                             *Old;
  UINTN                            OldSize;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  PreferRemovable = (BOOLEAN)(PcdGet8 (PcdA733BootMedium) == A733_BOOT_MEDIUM_SD);

  //
  // Four passes, best first: the build's medium and an EFI System Partition,
  // that medium without one, then the other medium with and without.  The
  // card's two FAT partitions both answer to "removable", and only the
  // partition type separates the settings' ESP from the small config one.
  //
  for (Pass = 0; Pass < 4; Pass++) {
    for (Index = 0; Index < HandleCount; Index++) {
      Removable = A733VarSnapshotVolumeIsRemovable (Handles[Index]);
      Rank      = (UINTN)((Removable == PreferRemovable) ? 2 : 0) |
                  (UINTN)(A733VarSnapshotVolumeIsEsp (Handles[Index]) ? 1 : 0);
      if (Rank != (3 - Pass)) {
        continue;
      }

      Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
      if (EFI_ERROR (Status) || (Fs == NULL)) {
        continue;
      }

      if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
        continue;
      }

      Old     = NULL;
      OldSize = 0;
      if (!EFI_ERROR (A733VarSnapshotReadFile (Root, A733_VAR_SNAPSHOT_FILE, &Old, &OldSize))) {
        if ((OldSize == Size) && (CompareMem (Old, Buffer, Size) == 0)) {
          //
          // Already current: do not touch the card.
          //
          FreePool (Old);
          Root->Close (Root);
          FreePool (Handles);
          return;
        }

        //
        // A corrupt previous file is not worth backing up.
        //
        if (EFI_ERROR (A733VarSnapshotProcess (Old, OldSize, FALSE))) {
          FreePool (Old);
          Old     = NULL;
          OldSize = 0;
        }
      }

      if (EFI_ERROR (A733VarSnapshotEnsureDirectory (Root))) {
        DEBUG ((DEBUG_WARN, "A733VarSnapshot: cannot create \\EFI\\A7A\n"));
        if (Old != NULL) {
          FreePool (Old);
        }

        Root->Close (Root);
        continue;
      }

      //
      // Keep the last good file as vars.bak before replacing vars.bin, so a
      // power cut during the write leaves a restore source behind.
      //
      if ((Old != NULL) &&
          EFI_ERROR (A733VarSnapshotWriteFile (Root, A733_VAR_SNAPSHOT_BACKUP, Old, OldSize)))
      {
        FreePool (Old);
        Root->Close (Root);
        continue;
      }

      if (Old != NULL) {
        FreePool (Old);
      }

      Status = A733VarSnapshotWriteFile (Root, A733_VAR_SNAPSHOT_FILE, Buffer, Size);
      Root->Close (Root);
      if (!EFI_ERROR (Status)) {
        DEBUG ((DEBUG_INFO, "A733VarSnapshot: exported %u bytes\n", (UINT32)Size));
        FreePool (Handles);
        return;
      }
    }
  }

  FreePool (Handles);
}

/**
  ReadyToBoot: mirror the current variable set to the card.
**/
STATIC
VOID
EFIAPI
A733VarSnapshotExportNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A733_VAR_SNAPSHOT_BUFFER  Buffer;
  UINT32                    RecordCount;

  if (EFI_ERROR (A733VarSnapshotBuild (&Buffer, &RecordCount)) || (Buffer.Data == NULL)) {
    return;
  }

  A733VarSnapshotExportToVolumes (Buffer.Data, Buffer.Size);
  FreePool (Buffer.Data);
}

/**
  Whether the store has never had a boot order written.

  BDS creates BootOrder on the first boot after the store is formatted; its
  absence is what marks a store that a snapshot may populate.
**/
STATIC
BOOLEAN
A733VarSnapshotStoreIsEmpty (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Attributes;
  UINTN       Size;

  Attributes = 0;
  Size       = 0;
  Status     = gRT->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, &Attributes, &Size, NULL);
  if (Status == EFI_NOT_FOUND) {
    return TRUE;
  }

  return (BOOLEAN)(Size == 0);
}

/**
  See A733VarSnapshot.h.
**/
VOID
EFIAPI
A733VarSnapshotInit (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  if (!PcdGetBool (PcdA733VarSnapshotEnable)) {
    return;
  }

  if (A733VarSnapshotStoreIsEmpty ()) {
    A733VarSnapshotRestoreFromVolumes ();
  }

  //
  // ReadyToBoot fires before the selected boot option runs, which is late
  // enough for the option list the user may have changed in UiApp (that
  // change is already in NOR; the next boot's export picks it up).
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  A733VarSnapshotExportNotify,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733VarSnapshot: no ReadyToBoot event: %r\n", Status));
  }
}