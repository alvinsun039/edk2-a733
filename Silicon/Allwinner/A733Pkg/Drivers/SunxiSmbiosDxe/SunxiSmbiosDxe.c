/** @file
  SunxiSmbiosDxe — publish SMBIOS records for the Allwinner A733
  (shared driver; board identity comes from PCDs, so the same binary
  serves the Orange Pi 4 Pro and the Radxa Cubie A7A). The EDK2 UiApp /
  FrontPage queries EFI_SMBIOS_PROTOCOL for system, CPU and memory info;
  populating these records makes Setup look like a real BIOS instead of
  falling back to the "Wonder Computer Model 1000Z" placeholder strings.

  Records published:
    Type 0  BIOS Information
    Type 1  System Information
    Type 2  Baseboard
    Type 3  Chassis
    Type 4  Processor (A733, 8 cores, octa-core ARMv8.2-A)
    Type 7  Cache (L1/L2 placeholder)
    Type 16 Physical Memory Array (single 6 GB array)
    Type 17 Memory Device          (single LPDDR4X stick)
    Type 19 Memory Array Mapped Address
    Type 32 System Boot Information

  Copyright (c) 2026, Orange Pi 4 Pro EDK2 port.
  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
*/

#include <PiDxe.h>
#include <IndustryStandard/SmBios.h>
#include <Protocol/Smbios.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

/**
  Entry point: look up the SMBIOS protocol and publish the A733 records.
**/

//
// Helper: append one SMBIOS record. Each record is a fixed structure
// followed by a sequence of NUL-terminated strings then a trailing
// double-NUL. We pack everything into a temporary buffer and call
// Smbios->Add(). String indexes are 1-based (1 = first string).
//
STATIC
EFI_STATUS
AddRecord (
  IN EFI_SMBIOS_PROTOCOL  *Smbios,
  IN VOID                 *Header,
  IN UINTN                 HeaderLength,
  IN CONST CHAR8         **Strings,
  IN UINTN                 StringCount,
  OUT EFI_SMBIOS_HANDLE   *Handle  OPTIONAL
  )
{
  EFI_STATUS                Status;
  EFI_SMBIOS_TABLE_HEADER  *Record;
  UINTN                     TotalLen;
  UINTN                     Index;
  UINTN                     Off;
  EFI_SMBIOS_HANDLE         LocalHandle;

  TotalLen = HeaderLength;
  for (Index = 0; Index < StringCount; Index++) {
    TotalLen += AsciiStrSize (Strings[Index]);
  }

  TotalLen += 1;                              // final NUL when no strings
  if (StringCount == 0) {
    TotalLen += 1;                            // need two NULs in that case
  }

  Record = AllocateZeroPool (TotalLen);
  if (Record == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Record, Header, HeaderLength);
  Off = HeaderLength;
  for (Index = 0; Index < StringCount; Index++) {
    UINTN  Sz = AsciiStrSize (Strings[Index]);
    CopyMem ((UINT8 *)Record + Off, Strings[Index], Sz);
    Off += Sz;
  }

  // trailing NUL terminator(s) already zeroed by AllocateZeroPool.
  LocalHandle = SMBIOS_HANDLE_PI_RESERVED;
  Status      = Smbios->Add (Smbios, NULL, &LocalHandle, Record);
  if (Handle != NULL) {
    *Handle = LocalHandle;
  }

  FreePool (Record);
  return Status;
}

EFI_STATUS
EFIAPI
SunxiSmbiosEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_SMBIOS_PROTOCOL  *Smbios;
  EFI_SMBIOS_HANDLE    MemArrayHandle;
  EFI_SMBIOS_HANDLE    MemDeviceHandle;
  CHAR8                FirmwareVersionAscii[64];
  CHAR16              *FirmwareVersionUcs2;
  CHAR8                FirmwareVendorAscii[64];
  CHAR16              *FirmwareVendorUcs2;
  CHAR8                SmbiosManufacturerAscii[64];
  CHAR8                SmbiosSystemProductNameAscii[64];
  CHAR8                SmbiosBaseboardProductNameAscii[64];
  CHAR8                SmbiosChassisManufacturerAscii[64];
  UINT32               MemorySizeMb;
  UINT8                MemoryType;
  UINT16               MemorySpeed;

  MemorySizeMb = (UINT32)(PcdGet64 (PcdSystemMemorySize) / 0x100000);
  MemoryType   = PcdGet8  (PcdSmbiosMemoryType);
  MemorySpeed  = PcdGet16 (PcdSmbiosMemorySpeed);

  Status = gBS->LocateProtocol (
                  &gEfiSmbiosProtocolGuid,
                  NULL,
                  (VOID **)&Smbios
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiSmbiosDxe: SMBIOS protocol missing - %r\n", Status));
    return Status;
  }

  //
  // Convert the UCS-2 PCD strings to ASCII for Type 0/1.
  //
  FirmwareVendorUcs2  = (CHAR16 *)PcdGetPtr (PcdFirmwareVendor);
  FirmwareVersionUcs2 = (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString);
  if ((FirmwareVendorUcs2 == NULL) || (*FirmwareVendorUcs2 == 0)) {
    FirmwareVendorUcs2 = L"Allwinner A733 EDK2 Port";
  }

  if ((FirmwareVersionUcs2 == NULL) || (*FirmwareVersionUcs2 == 0)) {
    FirmwareVersionUcs2 = L"v0.1";
  }

  UnicodeStrToAsciiStrS (FirmwareVendorUcs2,  FirmwareVendorAscii,  sizeof (FirmwareVendorAscii));
  UnicodeStrToAsciiStrS (FirmwareVersionUcs2, FirmwareVersionAscii, sizeof (FirmwareVersionAscii));

  //
  // Board identity strings come from PCDs so the same binary names whatever
  // A733 board the DSC targets. An empty string suppresses the record's
  // string slot (SMBIOS consumers treat it as NULL).
  //
  AsciiStrCpyS (SmbiosManufacturerAscii,        sizeof (SmbiosManufacturerAscii),        (CHAR8 *)PcdGetPtr (PcdSmbiosManufacturer));
  AsciiStrCpyS (SmbiosSystemProductNameAscii,   sizeof (SmbiosSystemProductNameAscii),   (CHAR8 *)PcdGetPtr (PcdSmbiosSystemProductName));
  AsciiStrCpyS (SmbiosBaseboardProductNameAscii, sizeof (SmbiosBaseboardProductNameAscii), (CHAR8 *)PcdGetPtr (PcdSmbiosBaseboardProductName));
  AsciiStrCpyS (SmbiosChassisManufacturerAscii, sizeof (SmbiosChassisManufacturerAscii), (CHAR8 *)PcdGetPtr (PcdSmbiosChassisManufacturer));

  //
  // Type 0 - BIOS Information
  //
  {
    SMBIOS_TABLE_TYPE0  Type0;
    CONST CHAR8        *Strs[3];

    ZeroMem (&Type0, sizeof (Type0));
    Type0.Hdr.Type     = EFI_SMBIOS_TYPE_BIOS_INFORMATION;
    Type0.Hdr.Length   = sizeof (Type0);
    Type0.Vendor       = 1;
    Type0.BiosVersion  = 2;
    Type0.BiosReleaseDate = 3;
    //
    // (value + 1) * 64 KB: the FD is 1.5 MB, so 23.
    //
    Type0.BiosSize        = 23;
    *(UINT64 *)&Type0.BiosCharacteristics = 0;
    Type0.BiosCharacteristics.BiosIsUpgradable      = 1;
    Type0.BiosCharacteristics.BootFromCdIsSupported = 1;
    Type0.BiosCharacteristics.SelectableBootIsSupported = 1;
    Type0.BIOSCharacteristicsExtensionBytes[0] = 0;
    Type0.BIOSCharacteristicsExtensionBytes[1] =
      BIT0 |   // ACPI supported
      BIT2 |   // UEFI Spec supported
      BIT3;    // SMBIOS table is virtual machine? (no) - leave 0
    Type0.SystemBiosMajorRelease         = 0;
    Type0.SystemBiosMinorRelease         = 2;
    Type0.EmbeddedControllerFirmwareMajorRelease = 0xFF;
    Type0.EmbeddedControllerFirmwareMinorRelease = 0xFF;

    Strs[0] = FirmwareVendorAscii;
    Strs[1] = FirmwareVersionAscii;
    Strs[2] = (CONST CHAR8 *)__DATE__;        // build date as release date

    AddRecord (Smbios, &Type0, sizeof (Type0), Strs, 3, NULL);
  }

  //
  // Type 1 - System Information
  //
  {
    SMBIOS_TABLE_TYPE1  Type1;
    CONST CHAR8        *Strs[5];

    ZeroMem (&Type1, sizeof (Type1));
    Type1.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;
    Type1.Hdr.Length = sizeof (Type1);
    Type1.Manufacturer = 1;
    Type1.ProductName  = 2;
    Type1.Version      = 3;
    Type1.SerialNumber = 4;
    Type1.SKUNumber    = 5;
    Type1.Family       = 5;                   // reuse SKU string
    Type1.WakeUpType   = SystemWakeupTypePowerSwitch;

    Strs[0] = SmbiosManufacturerAscii;
    Strs[1] = SmbiosSystemProductNameAscii;
    Strs[2] = "v1.0";
    Strs[3] = "0000000000000000";
    Strs[4] = "Allwinner A733";

    AddRecord (Smbios, &Type1, sizeof (Type1), Strs, 5, NULL);
  }

  //
  // Type 2 - Baseboard
  //
  {
    SMBIOS_TABLE_TYPE2  Type2;
    CONST CHAR8        *Strs[4];

    ZeroMem (&Type2, sizeof (Type2));
    Type2.Hdr.Type   = EFI_SMBIOS_TYPE_BASEBOARD_INFORMATION;
    Type2.Hdr.Length = sizeof (Type2);
    Type2.Manufacturer = 1;
    Type2.ProductName  = 2;
    Type2.Version      = 3;
    Type2.SerialNumber = 4;
    Type2.FeatureFlag.Motherboard = 1;
    Type2.BoardType    = BaseBoardTypeMotherBoard;

    Strs[0] = SmbiosManufacturerAscii;
    Strs[1] = SmbiosBaseboardProductNameAscii;
    Strs[2] = "v1.0";
    Strs[3] = "0000000000000000";

    AddRecord (Smbios, &Type2, sizeof (Type2), Strs, 4, NULL);
  }

  //
  // Type 3 - Chassis
  //
  {
    SMBIOS_TABLE_TYPE3  Type3;
    CONST CHAR8        *Strs[3];

    ZeroMem (&Type3, sizeof (Type3));
    Type3.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;
    Type3.Hdr.Length = sizeof (Type3);
    Type3.Manufacturer = 1;
    Type3.Type         = MiscChassisTypeDeskTop;
    Type3.Version      = 2;
    Type3.SerialNumber = 3;
    Type3.BootupState      = ChassisStateSafe;
    Type3.PowerSupplyState = ChassisStateSafe;
    Type3.ThermalState     = ChassisStateSafe;
    Type3.SecurityStatus   = ChassisSecurityStatusNone;

    Strs[0] = SmbiosChassisManufacturerAscii;
    Strs[1] = "v1.0";
    Strs[2] = "0000000000000000";

    AddRecord (Smbios, &Type3, sizeof (Type3), Strs, 3, NULL);
  }

  //
  // Type 4 - Processor
  // The Allwinner A733 is an octa-core ARMv8.2-A: 6x Cortex-A55 + 2x
  // Cortex-A76 in a big.LITTLE arrangement - board.dts cpu-map, the MADT
  // MPIDRs and the PPTT clusters all agree.  One physical socket, 8 cores
  // and 8 threads.  The speeds are measured ceilings: A76 2.002 GHz, A55
  // 1.794 GHz, and Type 4 reports the big cluster.
  //
  {
    SMBIOS_TABLE_TYPE4  Type4;
    CONST CHAR8        *Strs[5];

    ZeroMem (&Type4, sizeof (Type4));
    Type4.Hdr.Type   = EFI_SMBIOS_TYPE_PROCESSOR_INFORMATION;
    Type4.Hdr.Length = sizeof (Type4);
    Type4.Socket            = 1;
    Type4.ProcessorType     = CentralProcessor;
    Type4.ProcessorFamily   = ProcessorFamilyIndicatorFamily2;
    Type4.ProcessorManufacturer = 2;
    // PROCESSOR_ID_DATA layout differs per arch; leaving zeros is OK.
    Type4.ProcessorVersion  = 3;
    Type4.Voltage.ProcessorVoltageCapability3_3V = 0;
    Type4.Voltage.ProcessorVoltageCapability5V   = 0;
    Type4.ExternalClock     = 24;                 // sys24M reference
    Type4.MaxSpeed          = 2002;
    Type4.CurrentSpeed      = 2002;
    // Status: bit6 = socket populated, bits[2:0] = 001 (CPU enabled).
    Type4.Status            = 0x41;
    Type4.ProcessorUpgrade  = ProcessorUpgradeUnknown;
    Type4.SerialNumber      = 4;
    Type4.AssetTag          = 5;
    Type4.PartNumber        = 5;
    Type4.CoreCount         = 8;
    Type4.EnabledCoreCount  = 8;
    Type4.ThreadCount       = 8;
    Type4.ProcessorCharacteristics = (UINT16)(
        BIT2 |   // 64-bit capable
        BIT3 |   // multi-core
        BIT5 |   // execute protection
        BIT6     // enhanced virtualization
        );
    Type4.ProcessorFamily2  = ProcessorFamilyARMv8;

    Strs[0] = "CPU0";
    Strs[1] = "Allwinner";
    Strs[2] = "Allwinner A733 (2x A76 + 6x A55)";
    Strs[3] = "0000000000000000";
    Strs[4] = "A733";

    AddRecord (Smbios, &Type4, sizeof (Type4), Strs, 5, NULL);
  }

  //
  // Type 16 - Physical Memory Array (single soldered bank)
  //
  {
    SMBIOS_TABLE_TYPE16  Type16;

    ZeroMem (&Type16, sizeof (Type16));
    Type16.Hdr.Type   = EFI_SMBIOS_TYPE_PHYSICAL_MEMORY_ARRAY;
    Type16.Hdr.Length = sizeof (Type16);
    Type16.Location  = MemoryArrayLocationSystemBoard;
    Type16.Use       = MemoryArrayUseSystemMemory;
    Type16.MemoryErrorCorrection = MemoryErrorCorrectionNone;
    Type16.MaximumCapacity       = MemorySizeMb * 1024;     // KB
    Type16.MemoryErrorInformationHandle = 0xFFFE;
    Type16.NumberOfMemoryDevices = 1;

    AddRecord (Smbios, &Type16, sizeof (Type16), NULL, 0, &MemArrayHandle);
  }

  //
  // Type 17 - Memory Device (soldered LPDDR, type/speed from PCDs)
  //
  {
    SMBIOS_TABLE_TYPE17  Type17;
    CONST CHAR8         *Strs[5];

    ZeroMem (&Type17, sizeof (Type17));
    Type17.Hdr.Type   = EFI_SMBIOS_TYPE_MEMORY_DEVICE;
    Type17.Hdr.Length = sizeof (Type17);
    Type17.MemoryArrayHandle = MemArrayHandle;
    Type17.MemoryErrorInformationHandle = 0xFFFE;
    Type17.TotalWidth = 32;
    Type17.DataWidth  = 32;
    Type17.Size       = (UINT16)MemorySizeMb;       // MB
    Type17.FormFactor = MemoryFormFactorChip;
    Type17.DeviceLocator = 1;
    Type17.BankLocator   = 2;
    Type17.MemoryType    = MemoryType;
    Type17.Speed         = MemorySpeed;
    Type17.ConfiguredMemoryClockSpeed = MemorySpeed;
    Type17.Manufacturer  = 3;
    Type17.SerialNumber  = 4;
    Type17.AssetTag      = 5;
    Type17.PartNumber    = 5;

    Strs[0] = "DRAM0";
    Strs[1] = "BANK0";
    Strs[2] = "Unknown";
    Strs[3] = "0000000000000000";
    Strs[4] = "LPDDR";

    AddRecord (Smbios, &Type17, sizeof (Type17), Strs, 5, &MemDeviceHandle);
  }

  //
  // Type 19 - Memory Array Mapped Address
  // PcdSystemMemoryBase = 0x40000000, Size = 0x200000000 (8 GB).
  //
  {
    SMBIOS_TABLE_TYPE19  Type19;
    UINT64               Base;
    UINT64               Size;

    Base = PcdGet64 (PcdSystemMemoryBase);
    Size = PcdGet64 (PcdSystemMemorySize);

    ZeroMem (&Type19, sizeof (Type19));
    Type19.Hdr.Type   = EFI_SMBIOS_TYPE_MEMORY_ARRAY_MAPPED_ADDRESS;
    Type19.Hdr.Length = sizeof (Type19);
    Type19.StartingAddress = 0xFFFFFFFF;        // use Extended fields
    Type19.EndingAddress   = 0xFFFFFFFF;
    Type19.MemoryArrayHandle = MemArrayHandle;
    Type19.PartitionWidth    = 1;
    Type19.ExtendedStartingAddress = Base;
    Type19.ExtendedEndingAddress   = Base + Size - 1;

    AddRecord (Smbios, &Type19, sizeof (Type19), NULL, 0, NULL);
  }

  //
  // Type 32 - System Boot Information (no errors detected)
  //
  {
    SMBIOS_TABLE_TYPE32  Type32;

    ZeroMem (&Type32, sizeof (Type32));
    Type32.Hdr.Type   = EFI_SMBIOS_TYPE_SYSTEM_BOOT_INFORMATION;
    Type32.Hdr.Length = sizeof (Type32);
    Type32.BootStatus = BootInformationStatusNoError;

    AddRecord (Smbios, &Type32, sizeof (Type32), NULL, 0, NULL);
  }

  DEBUG ((DEBUG_INFO, "SunxiSmbiosDxe: published Type 0/1/2/3/4/16/17/19/32\n"));
  return EFI_SUCCESS;
}
