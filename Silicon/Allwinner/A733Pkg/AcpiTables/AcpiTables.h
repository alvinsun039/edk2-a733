/** @file
 *
 *  A733 defines for constructing ACPI tables
 *
 *  Modelled after edk2-rockchip's Silicon/Rockchip/RK3588/Include/
 *  AcpiTables.h (ACPI_HEADER + OEM identity + MADT init helpers), trimmed
 *  to what the A733 tables need (no PCIe/IORT/MCFG config-space entries:
 *  the A733 root port has no ECAM window).
 *
 *  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
 *  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef __ACPITABLES_H__
#define __ACPITABLES_H__

#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/MemoryMappedConfigurationSpaceAccessTable.h>

#define EFI_ACPI_OEM_ID  {'A','7','3','3',' ',' '}

#define EFI_ACPI_OEM_TABLE_ID  SIGNATURE_64 ('A','7','3','3',' ',' ',' ',' ')

#define EFI_ACPI_OEM_REVISION      0x00000000
#define EFI_ACPI_CREATOR_ID        SIGNATURE_32 ('E','D','K','2')
#define EFI_ACPI_CREATOR_REVISION  0x00000000

// A macro to initialise the common header part of EFI ACPI tables as defined by
// EFI_ACPI_DESCRIPTION_HEADER structure.
#define ACPI_HEADER(Signature, Type, Revision)  {                 \
    Signature,                      /* UINT32  Signature */       \
    sizeof (Type),                  /* UINT32  Length */          \
    Revision,                       /* UINT8   Revision */        \
    0,                              /* UINT8   Checksum */        \
    EFI_ACPI_OEM_ID,                /* UINT8   OemId[6] */        \
    EFI_ACPI_OEM_TABLE_ID,          /* UINT64  OemTableId */      \
    EFI_ACPI_OEM_REVISION,          /* UINT32  OemRevision */     \
    EFI_ACPI_CREATOR_ID,            /* UINT32  CreatorId */       \
    EFI_ACPI_CREATOR_REVISION       /* UINT32  CreatorRevision */ \
  }

#define EFI_ACPI_6_0_GIC_REDISTRIBUTOR_INIT(RedisRegionAddr, RedisDiscLength) \
  {                                                                           \
    EFI_ACPI_6_0_GICR,                                                        \
    sizeof (EFI_ACPI_6_0_GICR_STRUCTURE),                                     \
    0,                                                                        \
    RedisRegionAddr,                                                          \
    RedisDiscLength                                                           \
  }

//
// MCFG keeps only the table header: the A733 PCIe root port is a
// DesignWare controller without an ECAM window, so there is no
// memory-mapped configuration space to describe.  The file exists as a
// skeleton (v1, no ConfigSpace entries).
//
#pragma pack(push, 1)
typedef struct {
  EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER    Header;
} A733_MCFG_TABLE;
#pragma pack(pop)

#endif // __ACPITABLES_H__
