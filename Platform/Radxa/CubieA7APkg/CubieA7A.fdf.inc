## @file
#  FFS packaging rules used by this platform.
#
#  Copyright (c) 2026, edk2-a733 contributors.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Rule.Common.SEC.SELF_RELOC]
  FILE SEC = $(NAMED_GUID) {
    TE  TE Align = Auto         $(INF_OUTPUT)/$(MODULE_NAME).efi
  }

[Rule.Common.DXE_CORE]
  FILE DXE_CORE = $(NAMED_GUID) {
    PE32 PE32                   $(INF_OUTPUT)/$(MODULE_NAME).efi
  }

[Rule.Common.DXE_DRIVER]
  FILE DRIVER = $(NAMED_GUID) {
    DXE_DEPEX DXE_DEPEX Optional $(INF_OUTPUT)/$(MODULE_NAME).depex
    PE32 PE32                   $(INF_OUTPUT)/$(MODULE_NAME).efi
  }
[Rule.Common.UEFI_DRIVER]
  FILE DRIVER = $(NAMED_GUID) {
    DXE_DEPEX DXE_DEPEX Optional $(INF_OUTPUT)/$(MODULE_NAME).depex
    PE32 PE32                   $(INF_OUTPUT)/$(MODULE_NAME).efi
  }

[Rule.Common.UEFI_APPLICATION]
  FILE APPLICATION = $(NAMED_GUID) {
    PE32 PE32                   $(INF_OUTPUT)/$(MODULE_NAME).efi
  }

[Rule.Common.DXE_RUNTIME_DRIVER]
  FILE DRIVER = $(NAMED_GUID) {
    DXE_DEPEX DXE_DEPEX Optional $(INF_OUTPUT)/$(MODULE_NAME).depex
    PE32 PE32                   $(INF_OUTPUT)/$(MODULE_NAME).efi
  }

# ACPI tables: AcpiTables.inf is a USER_DEFINED module whose output is a raw
# table blob, not a PE image.  Pack it as FREEFORM so AcpiPlatformDxe can find
# it by FILE_GUID (7E374E25-...) in the FV at EndOfDxe.
[Rule.Common.USER_DEFINED.ACPITABLE]
  FILE FREEFORM = $(NAMED_GUID) {
    RAW ACPI               |.acpi
    RAW ASL                |.aml
  }
