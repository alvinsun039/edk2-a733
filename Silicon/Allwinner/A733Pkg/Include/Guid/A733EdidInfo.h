/** @file
  Shared sink-EDID information between A733HdmiDxe (producer) and
  A733SetupDxe (consumer, builds the filtered Display Mode list).

  A733HdmiDxe publishes this as an EFI configuration table after a
  successful EDID probe; A733SetupDxe reads it to decide which preset
  modes the Setup page offers.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __A733_EDID_INFO_H__
#define __A733_EDID_INFO_H__

#include <Library/A733HdmiModeLib.h>

#define A733_EDID_INFO_GUID \
  { 0x4a7330e0, 0xed1d, 0x4b6a, { 0x9c, 0x41, 0xa2, 0xb8, 0xd0, 0xed, 0x11, 0x52 } }

//
// Bit i of SupportedMask corresponds to preset i of the firmware mode
// table (DISPLAY_MODE_640_480_60 .. DISPLAY_MODE_4096_2160_60).
//
//
// Max detailed timing descriptors we carry for the Setup list.
//
#define A733_EDID_MAX_DTD  4

typedef struct {
  BOOLEAN  Valid;          // EDID read and parsed
  UINT32   SupportedMask;  // presets the sink advertises
  UINT32   NativeHActive;  // preferred timing
  UINT32   NativeVActive;
  UINT32   NativePixelClockHz;
  UINTN    DtdCount;       // detailed timings below
  A733_HDMI_MODE  Dtd[A733_EDID_MAX_DTD];
} A733_EDID_INFO;

extern EFI_GUID  gA733EdidInfoGuid;

#endif // __A733_EDID_INFO_H__
