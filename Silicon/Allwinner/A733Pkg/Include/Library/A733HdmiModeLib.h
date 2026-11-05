/** @file
  Shared HDMI mode types and API between A733HdmiDxe (mode table owner)
  and A733SetupDxe (EDID-filtered Display Mode list).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __A733_HDMI_MODE_LIB_H__
#define __A733_HDMI_MODE_LIB_H__

#include <Uefi.h>

//
// Active video mode.  DE / TCON / DWC timing and the CCU 0x1684 divider
// are derived from the selected entry; the top-PHY PLL (n/p0) factors are
// measured for 1080p60 and derived per-EDID for sink timings.  Modes with
// all-zero PLL factors are not drivable.
//
typedef struct {
  UINT32   Vic;            // CEA-861 VIC
  UINT32   PixelClockHz;   // pixel clock
  UINT16   HActive;        // active line pixels
  UINT16   HTotal;         // whole line incl. blanking
  UINT16   HSyncStart;     // first sync pixel
  UINT16   HSyncEnd;       // last sync pixel
  UINT16   VActive;        // active lines
  UINT16   VTotal;         // whole frame incl. blanking
  UINT16   VSyncStart;     // first sync line
  UINT16   VSyncEnd;       // last sync line
  BOOLEAN  HSyncPositive;
  BOOLEAN  VSyncPositive;
  UINT32   PhyPllN;        // top-PHY PLL n field
  UINT32   PhyPllP0;       // top-PHY PLL p0 field
} A733_HDMI_MODE;

//
// Preset mode table (Setup "Display Mode" presets, index == DISPLAY_MODE_*
// value).  Defined in A733HdmiModeLib; exported so the HDMI driver can
// iterate it without owning the data.
//
extern CONST A733_HDMI_MODE  mHdmiModes[];

const A733_HDMI_MODE *
A733HdmiGetPresetTable (
  OUT UINT32  *Count
  );

#endif // __A733_HDMI_MODE_LIB_H__
