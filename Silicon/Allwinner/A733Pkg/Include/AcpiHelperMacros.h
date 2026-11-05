/** @file
  Helper macros for ACPI table construction — Allwinner A733.

  Subset of the (newer-edk2) MdeModulePkg AcpiHelperMacros.h, vendored here
  because this tree's edk2 does not ship that header yet.  Keep the macro
  names and expansion identical so the .aslc sources stay copy-compatible
  with the rk3588 AcpiTables template (edk2-rockchip).

  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#pragma once

//
// Macros for the Generic Address Space
//
#define ACPI_NULL_GAS  { EFI_ACPI_5_0_SYSTEM_MEMORY,  0, 0, EFI_ACPI_5_0_UNDEFINED, 0L }
#define ACPI_GAS8(Address)   { EFI_ACPI_5_0_SYSTEM_MEMORY,  8, 0, EFI_ACPI_5_0_BYTE,      Address }
#define ACPI_GAS16(Address)  { EFI_ACPI_5_0_SYSTEM_MEMORY, 16, 0, EFI_ACPI_5_0_WORD,      Address }
#define ACPI_GAS32(Address)  { EFI_ACPI_5_0_SYSTEM_MEMORY, 32, 0, EFI_ACPI_5_0_DWORD,     Address }
#define ACPI_GAS64(Address)  { EFI_ACPI_5_0_SYSTEM_MEMORY, 64, 0, EFI_ACPI_5_0_QWORD,     Address }
#define ACPI_GASN(Address)   { EFI_ACPI_5_0_SYSTEM_MEMORY,  0, 0, EFI_ACPI_5_0_DWORD,     Address }

//
// Macros for the Multiple APIC Description Table (MADT)
//
#define EFI_ACPI_6_0_GIC_DISTRIBUTOR_INIT(GicDistHwId, GicDistBase, GicDistVector, GicVersion) \
  { \
    EFI_ACPI_6_0_GICD, sizeof (EFI_ACPI_6_0_GIC_DISTRIBUTOR_STRUCTURE), EFI_ACPI_RESERVED_WORD, \
    GicDistHwId, GicDistBase, GicDistVector, GicVersion, \
    {EFI_ACPI_RESERVED_BYTE, EFI_ACPI_RESERVED_BYTE, EFI_ACPI_RESERVED_BYTE} \
  }

// Note the parking protocol is configured by UEFI if required
#define EFI_ACPI_6_0_GICC_STRUCTURE_INIT(GicId, AcpiCpuUid, Mpidr, Flags, PmuIrq,    \
                                         GicBase, GicVBase, GicHBase, GsivId, GicRBase, Efficiency)       \
  {                                                                                  \
    EFI_ACPI_6_0_GIC, sizeof (EFI_ACPI_6_0_GIC_STRUCTURE), EFI_ACPI_RESERVED_WORD,   \
    GicId, AcpiCpuUid, Flags, 0, PmuIrq, 0, GicBase, GicVBase, GicHBase,             \
    GsivId, GicRBase, Mpidr, Efficiency,                                             \
    {EFI_ACPI_RESERVED_BYTE, EFI_ACPI_RESERVED_BYTE, EFI_ACPI_RESERVED_BYTE}         \
  }
