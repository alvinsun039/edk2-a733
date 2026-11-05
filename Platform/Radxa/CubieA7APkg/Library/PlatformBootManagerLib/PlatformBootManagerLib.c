/** @file
  Minimal boot manager policy for the Cubie A7A.

  Two jobs: make the serial terminal the console, and give BDS the UEFI
  Shell that lives in the firmware volume as its boot target.  This port
  has no storage driver yet, so nothing else can produce a boot option.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>

#include <Protocol/DevicePath.h>

#include <Protocol/FirmwareVolume2.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>

#include <Library/A733BoardFdtLib.h>
#include <Library/A733CcuLib.h>
#include <Library/A733GpioLib.h>
#include <Library/A733PmicLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootLogoLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "A733VarSnapshot.h"

//
// ShellPkg/Application/Shell/Shell.inf FILE_GUID (gUefiShellFileGuid).
//
STATIC CONST EFI_GUID  mShellFileGuid = {
  0x7C04A583, 0x9E3E, 0x4F1C, { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }
};

//
// EmbeddedPkg/Application/AndroidFastboot/AndroidFastbootApp.inf FILE_GUID.
// The fastboot command engine has no device path of its own, so this is the
// only way to name it as a boot target: the FV file, reached through the volume
// that carries it.
//
STATIC CONST EFI_GUID  mFastbootFileGuid = {
  0x9588502A, 0x5370, 0x11E3, { 0x86, 0x31, 0xD7, 0xC5, 0x95, 0x13, 0x64, 0xC8 }
};

//
// Silicon/Allwinner/A733Pkg/Drivers/BootDebian/BootDebian.inf FILE_GUID.
//
STATIC CONST EFI_GUID  mBootDebianFileGuid = {
  0x9E1D4A7B, 0x3C2F, 0x4B6E, { 0xA8, 0xD1, 0x5F, 0x0C, 0x9B, 0x2E, 0x7A, 0x34 }
};

//
// Silicon/Allwinner/A733Pkg/Drivers/BootExtlinux/BootExtlinux.inf FILE_GUID.
//
STATIC CONST EFI_GUID  mBootExtlinuxFileGuid = {
  0x6B2E9C41, 0x8A57, 0x4D3F, { 0x9E, 0x02, 0x71, 0xB5, 0xC8, 0x3A, 0x64, 0x1D }
};

/**
  Seed the console variables with every real text device.

  ConPlatform installs gEfiConsoleOutDeviceGuid / gEfiConsoleInDeviceGuid
  only on a device path already listed in ConOut/ConIn, and ConSplitter
  routes only to devices carrying those GUIDs.  A fresh variable store
  starts with both variables empty, so the loop never closes: ConOut
  exists, accepts every write and outputs nothing.  Put the device paths
  in the variables and let ConPlatform do the binding - the standard
  order, instead of installing the console-device GUIDs here.
**/
STATIC
VOID
AttachConsoleDevices (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextOutProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      //
      // ConSplitter's own text-out handle carries no device path; skip it.
      //
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath))) {
        continue;
      }

      EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
      EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);
    }

    FreePool (Handles);
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath))) {
        continue;
      }

      EfiBootManagerUpdateConsoleVariable (ConIn, DevicePath, NULL);
    }

    FreePool (Handles);
  }

  //
  // Let ConPlatform install the console-device GUIDs and ConSplitter bind
  // the devices whose paths were just added.
  //
  EfiBootManagerConnectAll ();
}

/**
  Append a firmware volume file to BootOrder as a boot option.

  @param[in]  FileGuid      FFS file to boot, reached through the FV that holds
                            it.
  @param[in]  Description   Boot option description.
  @param[in]  Position      Index in BootOrder to insert at, if the option has
                            to be added.  It has no effect on an option that is
                            already stored - see ArrangeBootOrderFirst.
  @param[out] OptionNumber  On success, the Boot#### number the option is stored
                            under: the one already there if an equal option
                            existed, the newly assigned one if not.  May be NULL.

  @retval EFI_SUCCESS   The option is in BootOrder (or was already).
  @retval other         No firmware volume carries FileGuid, or the option
                        could not be built.
**/
STATIC
EFI_STATUS
RegisterFvBootOption (
  IN CONST EFI_GUID  *FileGuid,
  IN CHAR16          *Description,
  IN UINTN           Position,
  OUT UINT16         *OptionNumber OPTIONAL
  )
{
  EFI_STATUS                         Status;
  UINTN                              HandleCount;
  EFI_HANDLE                         *Handles;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_DEVICE_PATH_PROTOCOL           *FvDp;
  EFI_DEVICE_PATH_PROTOCOL           *FullDp;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  VOID                               *Buffer;
  UINTN                              Size;
  EFI_FV_FILETYPE                    FileType;
  EFI_FV_FILE_ATTRIBUTES             Attributes;
  UINT32                             AuthStatus;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (
          gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv)
          ))
    {
      continue;
    }

    //
    // Only advertise the file in a volume that really carries it, otherwise
    // the boot option points at nothing and BDS fails halfway through.
    //
    Buffer     = NULL;
    Size       = 0;
    FileType   = 0;
    Attributes = 0;
    AuthStatus = 0;
    Status     = Fv->ReadFile (Fv, FileGuid, &Buffer, &Size, &FileType, &Attributes, &AuthStatus);
    if (Buffer != NULL) {
      FreePool (Buffer);
    }

    if (EFI_ERROR (Status)) {
      continue;
    }

    if (EFI_ERROR (
          gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&FvDp)
          ))
    {
      continue;
    }

    EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
    FullDp = AppendDevicePathNode (FvDp, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
    if (FullDp == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = EfiBootManagerInitializeLoadOption (
               &Option,
               LoadOptionNumberUnassigned,
               LoadOptionTypeBoot,
               LOAD_OPTION_ACTIVE,
               Description,
               FullDp,
               NULL,
               0
               );
    if (!EFI_ERROR (Status)) {
      EFI_BOOT_MANAGER_LOAD_OPTION  *Existing;
      UINTN                         ExistingCount;
      INTN                          Found;

      //
      // Add it only if an equal option is not stored yet.  Equality is the
      // library's own - type, attributes, description, device path and optional
      // data - and that is enough here because FV_MAIN carries a FvNameGuid, so
      // the path this function builds contains no RAM address and is identical
      // on every boot.  Before the name GUID the stored path carried the address
      // PrePi decompressed the volume to, the comparison could never match, and
      // a new Boot#### was written every boot; a duplicate showing up again now
      // means something volatile crept back into the path, which is a bug to
      // find rather than a case to tolerate.
      //
      Existing = EfiBootManagerGetLoadOptions (&ExistingCount, LoadOptionTypeBoot);
      Found    = (Existing == NULL) ? -1 :
                 EfiBootManagerFindLoadOption (&Option, Existing, ExistingCount);
      if (Found == -1) {
        Status = EfiBootManagerAddLoadOptionVariable (&Option, Position);
      } else {
        //
        // Already stored: take its number rather than letting the add path
        // assign a second one, and report success so the caller can arrange
        // the order around the entry that is really there.
        //
        Option.OptionNumber = Existing[Found].OptionNumber;
        Status              = EFI_SUCCESS;
      }

      if ((OptionNumber != NULL) && !EFI_ERROR (Status)) {
        *OptionNumber = (UINT16)Option.OptionNumber;
      }

      EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
      EfiBootManagerFreeLoadOption (&Option);
    }

    FreePool (FullDp);
    break;
  }

  FreePool (Handles);
  return Status;
}

/**
  RegisterFvBootOption with caller data attached as OptionalData.

  The "Android on <disk>" options carry the disk id BootAndroid should boot
  from, which RegisterFvBootOption has no way to pass.  Dedup is the boot
  manager's own comparison (type, attributes, description, device path and
  optional data), so a stored option keeps its number as long as nothing
  volatile enters the path.

  @param[in] FileGuid     File in a firmware volume to boot.
  @param[in] Description  Boot option description.
  @param[in] Position     Index in BootOrder to insert at, if the option has
                          to be added.  Ignored when it is already stored.
  @param[in] Data         OptionalData, or NULL.
  @param[in] DataSize     Size of Data in bytes.

  @retval EFI_SUCCESS   Option registered (or already stored).
  @retval EFI_NOT_FOUND No firmware volume carries FileGuid.
**/
STATIC
EFI_STATUS
RegisterFvBootOptionWithData (
  IN CONST EFI_GUID  *FileGuid,
  IN CHAR16          *Description,
  IN UINTN           Position,
  IN CONST VOID      *Data,
  IN UINTN           DataSize
  )
{
  EFI_STATUS                         Status;
  UINTN                              HandleCount;
  EFI_HANDLE                         *Handles;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_DEVICE_PATH_PROTOCOL           *FvDp;
  EFI_DEVICE_PATH_PROTOCOL           *FullDp;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  VOID                               *Buffer;
  UINTN                              Size;
  EFI_FV_FILETYPE                    FileType;
  EFI_FV_FILE_ATTRIBUTES             Attributes;
  UINT32                             AuthStatus;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (
          gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv)
          ))
    {
      continue;
    }

    Buffer     = NULL;
    Size       = 0;
    FileType   = 0;
    Attributes = 0;
    AuthStatus = 0;
    Status     = Fv->ReadFile (Fv, FileGuid, &Buffer, &Size, &FileType, &Attributes, &AuthStatus);
    if (Buffer != NULL) {
      FreePool (Buffer);
    }

    if (EFI_ERROR (Status)) {
      continue;
    }

    if (EFI_ERROR (
          gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&FvDp)
          ))
    {
      continue;
    }

    EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
    FullDp = AppendDevicePathNode (FvDp, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
    if (FullDp == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    Status = EfiBootManagerInitializeLoadOption (
               &Option,
               LoadOptionNumberUnassigned,
               LoadOptionTypeBoot,
               LOAD_OPTION_ACTIVE,
               Description,
               FullDp,
               (VOID *)Data,
               DataSize
               );
    if (!EFI_ERROR (Status)) {
      EFI_BOOT_MANAGER_LOAD_OPTION  *Existing;
      UINTN                         ExistingCount;
      INTN                          Found;

      Existing = EfiBootManagerGetLoadOptions (&ExistingCount, LoadOptionTypeBoot);
      Found    = (Existing == NULL) ? -1 :
                 EfiBootManagerFindLoadOption (&Option, Existing, ExistingCount);
      if (Found == -1) {
        Status = EfiBootManagerAddLoadOptionVariable (&Option, Position);
      } else {
        Status = EFI_SUCCESS;
      }

      EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
      EfiBootManagerFreeLoadOption (&Option);
    }

    FreePool (FullDp);
    break;
  }

  FreePool (Handles);
  return Status;
}

/**
  Name a disk the way the "UEFI OS on <class>" options do.

  A733SmhcDxe builds its controller device path from a VENDOR node in this
  package's GUID namespace followed by the controller's name ("SMHC3" is
  the eMMC, "SMHC0" the microSD slot), so that text is what identifies the
  class here; anything else falls back to the generic name.

  @param[in]  DiskHandle  Handle of the whole disk.
  @param[out] Id          Receives the short class name.
  @param[in]  IdChars     Capacity of Id in characters.
**/
STATIC
VOID
DiskClassId (
  IN  EFI_HANDLE  DiskHandle,
  OUT CHAR16      *Id,
  IN  UINTN       IdChars
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DiskDp;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  CHAR16                    *Text;
  CHAR8                      Name8[32];
  UINTN                      NameLen;
  BOOLEAN                    IsSmhc;

  StrCpyS (Id, IdChars, L"Disk");

  DiskDp = DevicePathFromHandle (DiskHandle);
  if (DiskDp == NULL) {
    return;
  }

  IsSmhc = FALSE;
  Name8[0] = 0;
  for (Node = DiskDp; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType (Node) == HW_VENDOR_DP))
    {
      VENDOR_DEVICE_PATH  *V = (VENDOR_DEVICE_PATH *)Node;

      if (CompareGuid (&V->Guid, &gA733TokenSpaceGuid)) {
        IsSmhc  = TRUE;
        NameLen = DevicePathNodeLength (Node) - sizeof (VENDOR_DEVICE_PATH);
        if (NameLen >= sizeof (Name8)) {
          NameLen = sizeof (Name8) - 1;
        }

        CopyMem (Name8, (UINT8 *)V + sizeof (VENDOR_DEVICE_PATH), NameLen);
        Name8[NameLen] = 0;
      }
    }
  }

  if (IsSmhc) {
    //
    // SMHC3 is the board's eMMC, SMHC0 the microSD slot (see
    // A733SmhcBoardLib); a future controller is still an SMHC disk.
    //
    if (AsciiStrCmp (Name8, "SMHC3") == 0) {
      StrCpyS (Id, IdChars, L"eMMC");
    } else {
      StrCpyS (Id, IdChars, L"SD");
    }

    return;
  }

  Text = ConvertDevicePathToText (DiskDp, FALSE, FALSE);
  if (Text != NULL) {
    if (StrStr (Text, L"NVMe") != NULL) {
      StrCpyS (Id, IdChars, L"NVMe");
    }

    FreePool (Text);
  }
}

/**
  One "Android on <disk>" boot option per disk carrying an Android "boot_a".

  BootAndroid takes the disk id in its OptionalData and boots from the disk
  it names, so a board with Android on the eMMC and on a USB stick offers
  both instead of silently preferring one.  The option is only registered
  when some firmware volume really carries BootAndroid, so the menu never
  offers an entry that cannot run.

  @param[in] StartPosition  BootOrder index the first new option goes in at.

  @return The next free index: StartPosition plus one per option actually
          added.
**/
STATIC
UINTN
RegisterAndroidBootOptions (
  IN UINTN  StartPosition
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
  CHAR8                        Name8[37];
  UINTN                        I;
  CHAR16                       Id[64];
  CHAR16                       Desc[128];
  EFI_GUID                     BootAndroidGuid = {
    0x6F4B1C22, 0x8A7D, 0x4E5B, { 0x9C, 0x01, 0x3D, 0x2E, 0x5F, 0x8A, 0x9B, 0x44 }
  };
  UINTN                        Position;

  Position = StartPosition;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPartitionInfoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Position;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartInfo);
    if (EFI_ERROR (Status) || (PartInfo->Type != PARTITION_TYPE_GPT)) {
      continue;
    }

    ZeroMem (Name8, sizeof (Name8));
    for (I = 0; (I < 36) && (PartInfo->Info.Gpt.PartitionName[I] != L'\0'); I++) {
      Name8[I] = (CHAR8)PartInfo->Info.Gpt.PartitionName[I];
    }

    if (AsciiStrCmp (Name8, "boot_a") != 0) {
      continue;
    }

    //
    // Strip the partition node to reach the whole-disk handle.
    //
    ChildDp  = DevicePathFromHandle (Handles[Index]);
    if (ChildDp == NULL) {
      continue;
    }

    ParentDp = DuplicateDevicePath (ChildDp);
    if (ParentDp == NULL) {
      continue;
    }

    {
      EFI_DEVICE_PATH_PROTOCOL  *P = ParentDp;
      UINTN                     NodeLen;

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

    FreeDp = ParentDp;
    Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &ParentDp, &ParentHandle);
    FreePool (FreeDp);
    if (EFI_ERROR (Status)) {
      continue;
    }

    DiskClassId (ParentHandle, Id, sizeof (Id) / sizeof (Id[0]));
    UnicodeSPrint (Desc, sizeof (Desc), L"Android on %s", Id);

    Status = RegisterFvBootOptionWithData (
               &BootAndroidGuid,
               Desc,
               Position,
               Id,
               (StrLen (Id) + 1) * sizeof (CHAR16)
               );
    if (!EFI_ERROR (Status)) {
      Position++;
    }
    DEBUG ((
      DEBUG_INFO,
      "PlatformBds: %s registered for %s: %r\n",
      Desc,
      Id,
      Status
      ));
  }

  FreePool (Handles);
  return Position;
}

/**
  Whether a volume carries an extlinux configuration BootExtlinux can parse.

  The two paths are the ones U-Boot's distro_bootcmd checks.
**/
STATIC
BOOLEAN
VolumeHasExtlinuxConfig (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;

  Status = Root->Open (Root, &File, L"\\extlinux\\extlinux.conf", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    File->Close (File);
    return TRUE;
  }

  Status = Root->Open (Root, &File, L"\\boot\\extlinux\\extlinux.conf", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    File->Close (File);
    return TRUE;
  }

  return FALSE;
}

/**
  One "Boot extlinux on <disk>" option per volume carrying an extlinux
  configuration.

  The volume's own device path is the option's OptionalData, so BootExtlinux
  boots from the disk the entry names instead of scanning and taking the first
  configuration it finds.  With Android on the eMMC and a distro on the SD
  card, the menu then offers both.

  @param[in] StartPosition  BootOrder index the first new option goes in at.

  @return The next free index: StartPosition plus one per option actually
          added.
**/
STATIC
UINTN
RegisterExtlinuxBootOptions (
  IN UINTN  StartPosition
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  UINTN                            Position;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_DEVICE_PATH_PROTOCOL         *VolumeDp;
  EFI_DEVICE_PATH_PROTOCOL         *Dp;
  EFI_DEVICE_PATH_PROTOCOL         *ParentDp;
  EFI_HANDLE                       DiskHandle;
  CHAR16                           Id[64];
  CHAR16                           Desc[128];

  Position = StartPosition;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Position;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FileSystem);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if (EFI_ERROR (FileSystem->OpenVolume (FileSystem, &Root))) {
      continue;
    }

    if (VolumeHasExtlinuxConfig (Root)) {
      VolumeDp = DevicePathFromHandle (Handles[Index]);
      if (VolumeDp != NULL) {
        Dp = DuplicateDevicePath (VolumeDp);
        if (Dp != NULL) {
          ParentDp = Dp;
          if (!EFI_ERROR (gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &ParentDp, &DiskHandle))) {
            DiskClassId (DiskHandle, Id, sizeof (Id) / sizeof (Id[0]));
          } else {
            StrCpyS (Id, sizeof (Id) / sizeof (Id[0]), L"Disk");
          }

          UnicodeSPrint (Desc, sizeof (Desc), L"Boot extlinux on %s", Id);
          Status = RegisterFvBootOptionWithData (
                     &mBootExtlinuxFileGuid,
                     Desc,
                     Position,
                     VolumeDp,
                     GetDevicePathSize (VolumeDp)
                     );
          if (!EFI_ERROR (Status)) {
            Position++;
          }

          DEBUG ((
            DEBUG_INFO,
            "PlatformBds: %s registered: %r\n",
            Desc,
            Status
            ));
          FreePool (Dp);
        }
      }
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return Position;
}

//
// ---------------------------------------------------------------------------
// EndOfDxe
// ---------------------------------------------------------------------------
//

STATIC
VOID
EFIAPI
EmptyCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
}

/**
  Signal gEfiEndOfDxeEventGroupGuid.

  SecurityStubDxe's Defer3rdPartyImageLoad only allows images from outside a
  firmware volume once its mEndOfDxe flag is set by this event group; until
  then LoadImage returns EFI_ACCESS_DENIED for them and BDS reports the
  image as "deferred but not loaded".
**/
STATIC
VOID
SignalEndOfDxe (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   EndOfDxeEvent;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  EmptyCallback,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &EndOfDxeEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "PlatformBds: EndOfDxe event: %r\n", Status));
    return;
  }

  gBS->SignalEvent (EndOfDxeEvent);
  gBS->CloseEvent (EndOfDxeEvent);
  DEBUG ((DEBUG_INFO, "PlatformBds: EndOfDxe signaled (3rd-party image load enabled)\n"));
}

//
// ---------------------------------------------------------------------------
// FDT: publish the board device tree as the FDT configuration table, so an
// EFI-stub kernel has a hardware description to boot from.  Sources, in
// order: the file named by the FdtOverrideBasePath variable (searched on
// every SimpleFileSystem volume), then the board DTB packed into FvMain.
// FdtOverrideOverlayPath names a .dtbo applied on top of either.  Any
// filesystem failure silently falls back to the built-in DTB; nothing is
// recorded.
// ---------------------------------------------------------------------------
//

//
// The device-tree configuration table the arm64 Linux EFI stub looks for
// (same GUID edk2-a733 installs).
//
STATIC EFI_GUID  mFdtTableGuid = {
  0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 }
};

//
// The board DTB packed into FvMain as a RAW section (see the FDF).  Same
// file GUID as edk2-a733's packaged DTB.
//
STATIC CONST EFI_GUID  mBoardDtbFileGuid = {
  0x4a7330d7, 0xc0de, 0x42a7, { 0x83, 0x30, 0xd1, 0xb7, 0xa7, 0xa7, 0x00, 0x01 }
};

/**
  Read a CHAR16 variable from the Setup-form namespace.

  @param[in]  Name  Variable name.

  @return The value as a NUL-terminated allocated string, or NULL when the
          variable is absent, empty, oversized (> 1024 bytes) or its read
          fails.  The caller frees the result.
**/
STATIC
CHAR16 *
FdtReadPathVariable (
  IN CONST CHAR16  *Name
  )
{
  UINTN   Size;
  UINT8   *Raw;
  CHAR16  *Value;

  Size = 0;
  if (gRT->GetVariable ((CHAR16 *)Name, &gA733SetupFormSetGuid, NULL, &Size, NULL) != EFI_BUFFER_TOO_SMALL) {
    return NULL;
  }

  if ((Size == 0) || (Size > 1024)) {
    return NULL;
  }

  Raw = AllocatePool (Size + sizeof (CHAR16));
  if (Raw == NULL) {
    return NULL;
  }

  if (EFI_ERROR (gRT->GetVariable ((CHAR16 *)Name, &gA733SetupFormSetGuid, NULL, &Size, Raw))) {
    FreePool (Raw);
    return NULL;
  }

  //
  // Terminate it ourselves: the variable may have been written without the
  // NUL the string handling below assumes.
  //
  Raw[Size] = 0;
  Raw[Size + 1] = 0;
  Value = (CHAR16 *)Raw;
  if (Value[0] == 0) {
    FreePool (Raw);
    return NULL;
  }

  return Value;
}

/**
  Load one file from the first SimpleFileSystem volume that has it.

  Any miss or read failure returns FALSE silently - the caller falls back
  to the firmware's built-in DTB.  The loaded buffer must start with a
  valid FDT magic (checked here) and is at most 16 MB.

  @param[in]  Path     Volume-relative path (e.g. L"\\dtb\\board.dtb").
  @param[out] Out      Loaded file.
  @param[out] OutSize  File size in bytes.

  @retval TRUE   The file was loaded and looks like an FDT.
  @retval FALSE  No volume had a readable FDT at Path.
**/
STATIC
BOOLEAN
FdtLoadFileFromVolumes (
  IN  CONST CHAR16  *Path,
  OUT VOID          **Out,
  OUT UINTN         *OutSize
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  VOID                             *Buffer;
  UINTN                            FileSize;

  Handles     = NULL;
  HandleCount = 0;
  Status      = gBS->LocateHandleBuffer (
                       ByProtocol,
                       &gEfiSimpleFileSystemProtocolGuid,
                       NULL,
                       &HandleCount,
                       &Handles
                       );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Root = NULL;
    File = NULL;
    Info = NULL;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || (Fs == NULL)) {
      continue;
    }

    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
      continue;
    }

    if (EFI_ERROR (Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0)) || (File == NULL)) {
      Root->Close (Root);
      continue;
    }

    InfoSize = 0;
    Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    Info = AllocatePool (InfoSize);
    if ((Info == NULL) ||
        EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, (VOID *)Info)))
    {
      if (Info != NULL) {
        FreePool (Info);
      }
      File->Close (File);
      Root->Close (Root);
      continue;
    }
    FileSize = (UINTN)Info->FileSize;
    FreePool (Info);

    if ((FileSize < 40) || (FileSize > SIZE_16MB)) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    Buffer = AllocatePool (FileSize);
    if (Buffer == NULL) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    Status = File->Read (File, &FileSize, Buffer);
    File->Close (File);
    Root->Close (Root);
    if (EFI_ERROR (Status) || (FdtCheckHeader (Buffer) != 0)) {
      FreePool (Buffer);
      continue;
    }

    FreePool (Handles);
    *Out     = Buffer;
    *OutSize = FileSize;
    return TRUE;
  }

  FreePool (Handles);
  return FALSE;
}

/**
  Publish the FDT configuration table.

  @retval EFI_SUCCESS           A tree is published (built-in or override).
  @retval EFI_NOT_FOUND         No built-in DTB in the firmware and no
                                usable override.
  @retval other                 A memory or table-install failure.
**/
STATIC
EFI_STATUS
InstallPlatformFdt (
  VOID
  )
{
  EFI_STATUS            Status;
  VOID                  *Existing;
  VOID                  *Base;
  UINTN                 BaseSize;
  CONST CHAR16          *Source;
  CHAR16                *Path;
  VOID                  *Overlay;
  UINTN                 OverlaySize;
  UINT32                BaseTotal;
  UINTN                 WorkSize;
  EFI_PHYSICAL_ADDRESS  Pages;
  VOID                  *Work;

  //
  // If something already published one, leave it alone.
  //
  Existing = NULL;
  if (!EFI_ERROR (EfiGetSystemConfigurationTable (&mFdtTableGuid, &Existing)) && (Existing != NULL)) {
    return EFI_SUCCESS;
  }

  //
  // Base: the override file when one is named and loads, else the DTB from
  // the firmware.  Any filesystem failure silently falls back.
  //
  Base      = NULL;
  BaseSize  = 0;
  Source    = L"firmware";

  Path = FdtReadPathVariable (L"FdtOverrideBasePath");
  if (Path != NULL) {
    if (FdtLoadFileFromVolumes (Path, &Base, &BaseSize)) {
      //
      // Path stays allocated: Source points into it for the log line at
      // the end.
      //
      Source = Path;
    } else {
      DEBUG ((DEBUG_INFO, "PlatformBds: FDT override %s not usable, using the built-in DTB\n", Path));
      FreePool (Path);
    }
  }

  if (Base == NULL) {
    Status = GetSectionFromAnyFv (&mBoardDtbFileGuid, EFI_SECTION_RAW, 0, &Base, &BaseSize);
    if (EFI_ERROR (Status) || (Base == NULL) || (BaseSize < 40)) {
      DEBUG ((DEBUG_WARN, "PlatformBds: no FDT: no override and no built-in DTB (%r)\n", Status));
      return EFI_NOT_FOUND;
    }
  }

  //
  // The overlay file loads before the working copy is sized, so the copy
  // can carry room for what the overlay will add.
  //
  Overlay     = NULL;
  OverlaySize = 0;
  Path        = FdtReadPathVariable (L"FdtOverrideOverlayPath");
  if (Path != NULL) {
    if (!FdtLoadFileFromVolumes (Path, &Overlay, &OverlaySize)) {
      DEBUG ((DEBUG_INFO, "PlatformBds: FDT overlay %s not usable, skipped\n", Path));
    }
    FreePool (Path);
  }

  //
  // Working copy in runtime pages: the OS reads the tree after
  // ExitBootServices, so it cannot live in reclaimable boot-services
  // memory.  Headroom for the fix-ups and the overlay; FdtOpenInto sets
  // totalsize to the full buffer, which stays valid (slack is legal).
  //
  BaseTotal = SwapBytes32 (*(UINT32 *)((UINT8 *)Base + 4));
  if ((BaseTotal < 40) || (BaseTotal > BaseSize)) {
    DEBUG ((DEBUG_WARN, "PlatformBds: FDT base has a broken totalsize\n"));
    return EFI_INVALID_PARAMETER;
  }

  WorkSize = BaseTotal + SIZE_4KB + ((Overlay != NULL) ? OverlaySize + SIZE_4KB : 0);
  Pages    = 0;
  Status   = gBS->AllocatePages (
                    AllocateAnyPages,
                    EfiRuntimeServicesData,
                    EFI_SIZE_TO_PAGES (WorkSize),
                    &Pages
                    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Work = (VOID *)(UINTN)Pages;
  if (FdtOpenInto (Base, Work, (INT32)WorkSize) != 0) {
    DEBUG ((DEBUG_WARN, "PlatformBds: FDT fdt_open_into failed\n"));
    gBS->FreePages (Pages, EFI_SIZE_TO_PAGES (WorkSize));
    return EFI_UNSUPPORTED;
  }

  //
  // Board fix-ups.  Always: they describe this board's hardware, not a user
  // preference, and a tree published without them describes blocks that are
  // not mounted.
  //
  A733FdtApplyBoardFixups (Work);

  //
  // Overlay last.  A failed overlay costs nothing: the base tree stays
  // whole and is published as-is.
  //
  if (Overlay != NULL) {
    if (FdtOverlayApply (Work, Overlay) == 0) {
      DEBUG ((DEBUG_INFO, "PlatformBds: FDT overlay applied (%u bytes)\n", (UINT32)OverlaySize));
    } else {
      DEBUG ((DEBUG_INFO, "PlatformBds: FDT overlay failed, publishing the base tree\n"));
    }
    FreePool (Overlay);
  }

  Status = gBS->InstallConfigurationTable (&mFdtTableGuid, Work);
  if (EFI_ERROR (Status)) {
    gBS->FreePages (Pages, EFI_SIZE_TO_PAGES (WorkSize));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "PlatformBds: FDT installed from %s (%u bytes, overlay %a)\n",
    Source,
    (UINT32)BaseTotal,
    (Overlay != NULL) ? "yes" : "no"
    ));

  if (Path != NULL) {
    FreePool (Path);
  }
  return EFI_SUCCESS;
}

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_STATUS  PmicStatus;
  UINT8       ChipId;
  UINT8       Ctl2;
  UINT32      Cldo5Mv;

  //
  // EndOfDxe first.  SecurityStubDxe's Defer3rdPartyImageLoad returns
  // EFI_ACCESS_DENIED for every image loaded from outside a firmware
  // volume - an EFI-stub kernel, a distro image, even a Shell.efi on a USB
  // stick - until this event group has been signalled, because its own
  // mEndOfDxe flag would otherwise stay FALSE.  Firmware-volume images are
  // exempt (FileFromFv), which is why the built-in applications always ran
  // and the first Android kernel load came back "Access Denied".  It is
  // signalled here, before devices are connected and boot options run.
  //
  SignalEndOfDxe ();

  //
  // PMIC first, and before the console devices are attached: whether the
  // board's eMMC I/O rail (CLDO5, "vcc-pc") is already at 1.8 V decides
  // whether HS200/HS400 need a rail switch, and the answer has to come from
  // the chip, not the device tree.  A PMIC that does not answer logs why and
  // lets the rest of the boot proceed - the eMMC path that works today must
  // not depend on it.
  //
  PmicStatus = A733PmicInit ();
  //
  // A chip that answered with the expected ID is the only case where the rail
  // figures below mean anything; A733PmicInit has already logged anything
  // else (no answer, or an unexpected ID).
  //
  if (!EFI_ERROR (PmicStatus)) {
    if (!EFI_ERROR (A733PmicGetChipId (&ChipId)) &&
        !EFI_ERROR (A733PmicGetRailMv (A733_PMIC_CLDO5, &Cldo5Mv)) &&
        !EFI_ERROR (A733PmicGetLdoCtl2 (&Ctl2)))
    {
      DEBUG ((
        DEBUG_ERROR,
        "Pmic: AXP8191 id %02x on S_TWI0, CLDO5 %u mV (%a), CTL2 %02x\n",
        ChipId,
        Cldo5Mv,
        A733PmicRailEnabled (A733_PMIC_CLDO5) ? "enabled" : "disabled",
        Ctl2
        ));
    }
  }

  //
  // Clock tree, when the build asks for it.  First thing BDS does, so the
  // lines land before the console and boot-option work starts changing the
  // device set they describe.
  //
  if (PcdGetBool (PcdA733CcuDumpEnable)) {
    A733CcuDumpTree ();
  }

  //
  // Pin state, when the build asks for it.  Same place as the clock tree, so
  // the two describe the same moment before the console and boot-option work
  // starts changing the pins.
  //
  if (PcdGetBool (PcdA733GpioDumpEnable)) {
    A733GpioDumpBanks ();
  }

  //
  // ConnectAll walks the handle list once per dispatch round, and every driver
  // is already dispatched when BDS runs, so a driver that binds to a handle
  // created later in that walk has to wait for the next walk.  The SD chain
  // has several such links - SdDxe creates the SD block handle, PartitionDxe
  // creates the partition handles, Ext4Dxe creates the filesystem handle - and
  // the extlinux option scan can only see the last one.  Walk until the set of
  // filesystems stops growing rather than assuming a depth.
  //
  {
    EFI_HANDLE  *FsHandles;
    UINTN       FsCount;
    UINTN       Previous;
    UINTN       Round;

    Previous = 0;
    for (Round = 0; Round < 8; Round++) {
      EfiBootManagerConnectAll ();

      FsCount = 0;
      if (!EFI_ERROR (
            gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &FsCount,
                   &FsHandles
                   )
            ))
      {
        FreePool (FsHandles);
      }

      if ((Round != 0) && (FsCount == Previous)) {
        break;
      }

      Previous = FsCount;
    }
  }

  //
  // The variable set follows the card: restore it onto a store that has never
  // been written, and arm the ReadyToBoot mirror.  Runs after ConnectAll so
  // the FAT stack exists, and before the boot options are built so a restored
  // BootOrder keeps the order it names.
  //
  A733VarSnapshotInit ();

  AttachConsoleDevices ();

  //
  // Publish the board DTB as the FDT configuration table: the device tree
  // an EFI-stub kernel boots with.  Runs after ConnectAll so the volume
  // scan sees every filesystem the override path could live on.
  //
  InstallPlatformFdt ();

  EfiBootManagerRefreshAllBootOption ();
  //
  // Boot targets, registered in the order they should boot: the OS first
  // (every "Android on <disk>" option, then an extlinux volume if one is
  // present, then Debian), then the tools -
  // fastboot before the Shell when PcdA733FastbootBoot is set (the
  // default), the Shell first when it is not.
  //
  // Position is what makes that order: it is the index in BootOrder an
  // option is inserted at when it is new, so a first boot comes up in
  // exactly this sequence.  An option that is already stored keeps the
  // place it has - which is the point: once the order exists, changing it
  // in the Boot Manager is a user decision and nothing here re-arranges
  // it.  A later firmware adds only what is missing, at the end.
  //
  // Each registration is a no-op unless a firmware volume really carries
  // the application, so the menu never lists something that cannot run.
  //
  {
    UINTN  Position;

    UINTN  ExtlinuxPosition;

    Position          = RegisterAndroidBootOptions (0);
    ExtlinuxPosition  = RegisterExtlinuxBootOptions (Position);
    if (ExtlinuxPosition == Position) {
      //
      // No volume carries a configuration right now; keep the scanning entry
      // so a medium plugged in after boot can still be used.
      //
      Position = RegisterFvBootOption (&mBootExtlinuxFileGuid, L"Boot Extlinux", Position, NULL) == EFI_SUCCESS
                   ? Position + 1 : Position;
    } else {
      Position = ExtlinuxPosition;
    }
    Position = RegisterFvBootOption (&mBootDebianFileGuid, L"Boot Debian", Position, NULL) == EFI_SUCCESS
                 ? Position + 1 : Position;
    Position = RegisterFvBootOption (&mFastbootFileGuid, L"Android Fastboot", Position, NULL) == EFI_SUCCESS
                 ? Position + 1 : Position;
    RegisterFvBootOption (&mShellFileGuid, L"UEFI Shell", Position, NULL);
  }

  //
  // Countdown keys, so the boot-timeout wait the progress bar visualises is
  // also interruptible (ArmPkg PlatformBm's wiring):
  //   - ENTER is the continue key: pressing it ends the wait immediately and
  //     boots the first BootOrder entry.
  //   - ESC is mapped to the Boot Manager menu (PcdBootManagerMenuFile,
  //     UiApp here): pressing it during the wait drops into the menu.
  //
  {
    EFI_STATUS                    KeyStatus;
    EFI_INPUT_KEY                 Enter;
    EFI_INPUT_KEY                 Esc;
    EFI_BOOT_MANAGER_LOAD_OPTION  BootManagerMenu;

    Enter.ScanCode    = SCAN_NULL;
    Enter.UnicodeChar = CHAR_CARRIAGE_RETURN;
    KeyStatus = EfiBootManagerRegisterContinueKeyOption (0, &Enter, NULL);
    if (EFI_ERROR (KeyStatus) && (KeyStatus != EFI_ALREADY_STARTED)) {
      DEBUG ((DEBUG_WARN, "PlatformBds: continue key: %r\n", KeyStatus));
    }

    Esc.ScanCode    = SCAN_ESC;
    Esc.UnicodeChar = CHAR_NULL;
    KeyStatus = EfiBootManagerGetBootManagerMenu (&BootManagerMenu);
    if (!EFI_ERROR (KeyStatus)) {
      KeyStatus = EfiBootManagerAddKeyOptionVariable (
                    NULL,
                    (UINT16)BootManagerMenu.OptionNumber,
                    0,
                    &Esc,
                    NULL
                    );
      if (EFI_ERROR (KeyStatus) && (KeyStatus != EFI_ALREADY_STARTED)) {
        DEBUG ((DEBUG_WARN, "PlatformBds: ESC hotkey: %r\n", KeyStatus));
      }
      EfiBootManagerFreeLoadOption (&BootManagerMenu);
    } else {
      DEBUG ((DEBUG_WARN, "PlatformBds: boot manager menu: %r\n", KeyStatus));
    }
  }

  //
  // Draw the logo last, so it is what remains on screen through the
  // timeout countdown (everything above prints to the serial console, and
  // the logo would cover it on the panel otherwise).
  //
  {
    EFI_STATUS  LogoStatus;

    LogoStatus = BootLogoEnableLogo ();
    if (EFI_ERROR (LogoStatus)) {
      DEBUG ((DEBUG_WARN, "PlatformBds: boot logo: %r\n", LogoStatus));
    }
  }

  //
  // Boot override: honour the RTC GPR[6] boot-mode flag.  The A7A kernel
  // writes it on `reboot bootloader` (0x5F) and the fastboot application
  // writes 0x5E ("continue" -> boot the installed Android) and 0x5D (its
  // copy of reboot-bootloader, meaning the Shell); the vendor U-Boot reads
  // the same slot (RTC_BOOT_INDEX 6).  The flag is consumed here so the
  // next plain reboot goes back to the normal boot order.
  //
  // With no flag this returns and BdsDxe runs the standard countdown,
  // which the logo and progress bar above are what the user sees.
  //
  {
    EFI_BOOT_MANAGER_LOAD_OPTION  *DefaultOptions;
    UINTN                          DefaultOptionCount;
    UINTN                          DefaultIndex;
    STATIC CONST CHAR16           *DefaultDescs[3] = {
      L"Android Fastboot",
      L"Android on ",    // prefix: "Android on <disk-id>" per-disk options
      L"UEFI Shell"
    };
    UINTN      D;
    BOOLEAN    BootOverride = FALSE;
    UINTN      GprBase      = 0x07090000UL + 0x100UL;
    UINT32     BootModeFlag;

    BootModeFlag = MmioRead32 (GprBase + 6 * 4);
    if (BootModeFlag == 0x5F) {
      DEBUG ((DEBUG_INFO, "PlatformBds: RTC boot-mode 0x5F (fastboot)\n"));
      BootOverride = TRUE;
    } else if (BootModeFlag == 0x5E) {
      DEBUG ((DEBUG_INFO, "PlatformBds: RTC boot-mode 0x5E (Android)\n"));
      DefaultDescs[0] = L"Android on ";
      DefaultDescs[1] = L"Android Fastboot";
      BootOverride    = TRUE;
    } else if (BootModeFlag == 0x5D) {
      DEBUG ((DEBUG_INFO, "PlatformBds: RTC boot-mode 0x5D (Shell)\n"));
      DefaultDescs[0] = L"UEFI Shell";
      DefaultDescs[1] = L"Android Fastboot";
      DefaultDescs[2] = L"Android on ";
      BootOverride    = TRUE;
    }

    //
    // Clear the slot either way, so a stale value does not steer a later
    // cold boot.
    //
    if (BootModeFlag != 0) {
      MmioWrite32 (GprBase + 6 * 4, 0);
    }

    if (!BootOverride) {
      return;
    }

    DefaultOptions = EfiBootManagerGetLoadOptions (&DefaultOptionCount, LoadOptionTypeBoot);
    for (D = 0; D < ARRAY_SIZE (DefaultDescs); D++) {
      BOOLEAN  Booted = FALSE;

      for (DefaultIndex = 0; DefaultIndex < DefaultOptionCount; DefaultIndex++) {
        if (DefaultOptions[DefaultIndex].Description == NULL) {
          continue;
        }

        {
          UINTN  MatchLen = StrLen (DefaultDescs[D]);

          //
          // "Android on " is a prefix match (the per-disk options are named
          // "Android on <disk-id>"); the others match exactly.
          //
          if ((StrnCmp (DefaultOptions[DefaultIndex].Description, DefaultDescs[D], MatchLen) == 0) &&
              ((MatchLen == 0) ||
               (StrCmp (DefaultDescs[D], L"Android on ") != 0) ||
               (StrLen (DefaultOptions[DefaultIndex].Description) > MatchLen)))
          {
            DEBUG ((
              DEBUG_INFO,
              "PlatformBds: boot-mode target: %s\n",
              DefaultOptions[DefaultIndex].Description
              ));
            EfiBootManagerBoot (&DefaultOptions[DefaultIndex]);
            DEBUG ((
              DEBUG_INFO,
              "PlatformBds: '%s' returned\n",
              DefaultOptions[DefaultIndex].Description
              ));
            Booted = TRUE;
            break;
          }
        }
      }

      if (!Booted) {
        continue;
      }

      //
      // The fastboot application returns when the user presses `continue`
      // or quits it; fall through to the next default instead of letting
      // BdsDxe re-select fastboot from the top of BootOrder.  Anything
      // later that boots a real OS takes the system over and never
      // returns, so only D == 0 continues.
      //
      if (D == 0) {
        continue;
      }

      break;
    }

    EfiBootManagerFreeLoadOptions (DefaultOptions, DefaultOptionCount);
  }
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  IN UINT16  TimeoutRemain
  )
{
  //
  // edk2-rk3588 style boot progress (as in edk2-a733): during the BDS
  // timeout countdown, BootLogoUpdateProgress draws a full-width bar along
  // the bottom edge of the framebuffer, filling left to right from 0 to
  // 100% under a one-line key hint.  It needs gEdkiiBootLogo2Protocol,
  // which BootGraphicsResourceTableDxe publishes; with no GOP (monitor
  // not yet hot-plugged) it degrades to the serial dot below.
  //
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION  Black;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION  White;
  UINT16                               Timeout;
  EFI_STATUS                           Status;

  Timeout = FixedPcdGet16 (PcdPlatformBootTimeOut);
  if (Timeout == 0) {
    Print (L".");
    return;
  }

  Black.Raw = 0x00000000;
  White.Raw = 0x00FFFFFF;

  Status = BootLogoUpdateProgress (
             White.Pixel,
             Black.Pixel,
             L"ESC: Boot Manager    ENTER: Boot now",
             White.Pixel,
             ((Timeout - TimeoutRemain) * 100) / Timeout,
             0
             );
  if (EFI_ERROR (Status)) {
    Print (L".");
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}