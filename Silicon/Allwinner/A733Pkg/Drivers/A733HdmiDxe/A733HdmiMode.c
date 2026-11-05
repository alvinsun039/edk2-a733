/** @file
  A733 HDMI video-mode table.

  One entry per Display-page preset (index = DISPLAY_MODE_* value from
  VarStoreData.h, so the Setup oneof maps straight into this array),
  carrying the CEA-861 / VESA DMT timing the DE / TCON / DWC / CCU-0x1684
  stages derive their registers from.  Sync polarity and pixel clock come
  from the standards; the top-PHY PLL anchor (PhyPllN / PhyPllP0) is
  computed as Fout = 24 MHz * (n+1) / (p0+1) and matches the measured
  1080p60 anchor (98 / 15 -> 148.5 MHz).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <VarStoreData.h>

#include <A733SetupFormSetGuid.h>

#include "A733HdmiDxe.h"
#include <Guid/A733EdidInfo.h>
#include <Library/UefiLib.h>

//
// Editor-mode entry mirroring the DISPLAY_MODE structure from
// A733Display.h (the Setup page CUSTOM parameters).
//
typedef struct {
  UINT32  Vic;
  UINT32  OscFreq;
  UINT32  HActive;
  UINT32  HFrontPorch;
  UINT32  HSync;
  UINT32  HBackPorch;
  UINT32  HSyncActive;
  UINT32  VActive;
  UINT32  VFrontPorch;
  UINT32  VSync;
  UINT32  VBackPorch;
  UINT32  VSyncActive;
  UINT32  DenActive;
  UINT32  ClkActive;
} DISPLAY_MODE_EDITOR;


const A733_HDMI_MODE *
A733HdmiGetMode (
  UINT32  Vic
  )
{
  UINTN  i;
  UINT32 Count;

  Count = 0;
  A733HdmiGetPresetTable (&Count);

  for (i = 0; i < Count; i++) {
    if ((mHdmiModes[i].Vic != 0) && (mHdmiModes[i].Vic == Vic)) {
      return &mHdmiModes[i];
    }
  }
  return NULL;
}

/**
  Convert the DISPLAY_MODE structure used by the Setup page (see
  A733Display.h) into an A733_HDMI_MODE with the PLL factors computed
  from the editor pixel clock.

  @param[in]  EditorData  Pointer to the PcdDisplayModeCustom data.
  @param[out] Mode        Filled-in A733 mode.

  @retval EFI_SUCCESS       Mode computed.
  @retval EFI_UNSUPPORTED   Pixel clock not representable by the PLL.
**/
EFI_STATUS
A733HdmiModeFromEditor (
  IN  CONST VOID      *EditorData,
  OUT A733_HDMI_MODE  *Mode
  )
{
  CONST DISPLAY_MODE_EDITOR  *Editor;
  UINT32                     PixelKhz;
  UINT32                     P0;
  BOOLEAN                    Found;

  Editor = (CONST DISPLAY_MODE_EDITOR *)EditorData;

  //
  // PLL: Fout = 24 MHz * (n+1) / (p0+1).  Search p0 for an exact
  // integer n within the 8-bit field; the CUSTOM editor allows
  // 25175-600000 kHz.
  //
  PixelKhz = Editor->OscFreq;
  Found    = FALSE;
  for (P0 = 0; P0 < 128; P0++) {
    UINT64  N1;

    N1 = ((UINT64)PixelKhz * (P0 + 1)) / 24000;   // (n+1) = f / 24 MHz
    if (N1 < 1 || N1 > 256) {
      continue;
    }
    if ((N1 * 24000) == ((UINT64)PixelKhz * (P0 + 1))) {
      Mode->PhyPllN  = (UINT32)N1 - 1;
      Mode->PhyPllP0 = P0;
      Found          = TRUE;
      break;
    }
  }
  if (!Found) {
    return EFI_UNSUPPORTED;
  }

  Mode->Vic           = Editor->Vic;
  Mode->PixelClockHz  = (UINT64)PixelKhz * 1000;
  Mode->HActive       = (UINT16)Editor->HActive;
  Mode->HTotal        = (UINT16)(Editor->HActive +
                                 Editor->HFrontPorch +
                                 Editor->HSync +
                                 Editor->HBackPorch);
  Mode->HSyncStart    = (UINT16)(Editor->HActive + Editor->HFrontPorch);
  Mode->HSyncEnd      = (UINT16)(Mode->HSyncStart + Editor->HSync);
  Mode->VActive       = (UINT16)Editor->VActive;
  Mode->VTotal        = (UINT16)(Editor->VActive +
                                 Editor->VFrontPorch +
                                 Editor->VSync +
                                 Editor->VBackPorch);
  Mode->VSyncStart    = (UINT16)(Editor->VActive + Editor->VFrontPorch);
  Mode->VSyncEnd      = (UINT16)(Mode->VSyncStart + Editor->VSync);
  Mode->HSyncPositive = (Editor->HSyncActive == 1);
  Mode->VSyncPositive = (Editor->VSyncActive == 1);

  return EFI_SUCCESS;
}

/**
  Return the mode selected by the Setup "Display Mode" page through the
  PcdDisplayModePreset DynamicHII PCD:
    - DISPLAY_MODE_NATIVE (0x80000000): the anchor mode (1080p60) - the
      driver does not parse EDID yet, so "native" means the measured
      default.
    - DISPLAY_MODE_CUSTOM (0x80000001): timing from PcdDisplayModeCustom.
    - 0..25: the preset table above.

  Falls back to the anchor mode when the selection is unavailable.
  DISPLAY_MODE_NATIVE resolves from the sink EDID, but the DDC bus only
  works after the HDMI core is clocked (A733HdmiHwInit) - so the EDID
  read happens there and the result is cached in mEdidNativeMode.  Until
  then (i.e. at driver entry, before HwInit) NATIVE means the 1080p60
  anchor.
**/
extern A733_HDMI_MODE  mEdidNativeMode;    // set by A733HdmiProbeEdid()
extern BOOLEAN         mEdidNativeValid;

const A733_HDMI_MODE *
A733HdmiActiveMode (
  VOID
  )
{
  STATIC A733_HDMI_MODE  CustomMode;
  UINT32                 Preset;
  UINTN                  Size;

  Size   = sizeof (Preset);
  Preset = DISPLAY_MODE_1920_1080_60;
  if (!EFI_ERROR (gRT->GetVariable (
                        L"DisplayModePreset",
                        &gA733SetupFormSetGuid,
                        NULL,
                        &Size,
                        &Preset
                        )))
  {
    if (Preset == DISPLAY_MODE_CUSTOM) {
      if (!EFI_ERROR (A733HdmiModeFromEditor (
                        PcdGetPtr (PcdDisplayModeCustom),
                        &CustomMode
                        )))
      {
        return &CustomMode;
      }
      return A733HdmiGetMode (HDMI_VIC_ANCHOR);
    }

    if (Preset == DISPLAY_MODE_NATIVE) {
      //
      // Follow the sink: pick the best mode its EDID advertises, capped
      // at 1080p (A733HdmiSelectBestMode falls back to the anchor when no
      // EDID is available yet).
      //
      return A733HdmiSelectBestMode ();
    }

    if ((Preset >= DISPLAY_MODE_EDID_BASE) &&
        (Preset < DISPLAY_MODE_EDID_BASE + A733_EDID_MAX_DTD))
    {
      //
      // EDID detailed-timing presets are also bypassed for now; force
      // 1080p60 and let the code below resolve from the preset table.
      //
      Preset = DISPLAY_MODE_1920_1080_60;
    }

    {
      UINT32  Count = 0;

      A733HdmiGetPresetTable (&Count);
      if ((Preset != DISPLAY_MODE_640_480_60) && (Preset < Count)) {
        return &mHdmiModes[Preset];
      }
      //
      // Preset 0 (640x480) or an out-of-range value is a stale/defective
      // varstore entry - never let it drive the panel.  Fall back to the
      // 1080p60 anchor instead of showing a useless 640x480.
      //
      return A733HdmiGetMode (HDMI_VIC_ANCHOR);
    }
  }

  return A733HdmiSelectBestMode ();
}

/**
  Consider one candidate for A733HdmiSelectBestMode.

  A candidate counts only when it is drivable (the mode table carries PLL
  factors for it) and its geometry fits the 1080p cap.  It wins when it
  covers more pixels than the incumbent, or the same pixels at a higher
  refresh.

  @param[in]     Candidate    Mode to consider.
  @param[in,out] Best         Current winner; replaced on a better score.
  @param[in,out] BestPixels   Pixel count of *Best.
  @param[in,out] BestRefresh  Refresh of *Best, Hz.
**/
STATIC
VOID
ConsiderMode (
  IN     CONST A733_HDMI_MODE  *Candidate,
  IN OUT CONST A733_HDMI_MODE  **Best,
  IN OUT UINTN                 *BestPixels,
  IN OUT UINT32                *BestRefresh
  )
{
  UINTN   Pixels;
  UINT32  Refresh;

  if ((Candidate->PhyPllN == 0) || (Candidate->PhyPllP0 == 0)) {
    return;
  }

  //
  // Cap: never larger than the anchor geometry, even when the sink
  // advertises more.  HActive/VActive are what the DE and the framebuffer
  // are sized for.
  //
  if ((Candidate->HActive > HDMI_HACTIVE) || (Candidate->VActive > HDMI_VACTIVE)) {
    return;
  }

  Pixels   = (UINTN)Candidate->HActive * Candidate->VActive;
  Refresh  = Candidate->PixelClockHz / ((UINT32)Candidate->HTotal * Candidate->VTotal);

  if ((Pixels > *BestPixels) ||
      ((Pixels == *BestPixels) && (Refresh > *BestRefresh)))
  {
    *Best        = Candidate;
    *BestPixels  = Pixels;
    *BestRefresh = Refresh;
  }
}

/**
  Best mode the sink advertises, capped at 1080p.

  Candidates are the sink's preferred (native) detailed timing and every
  preset timing its EDID advertises.  The winner is the one with the most
  pixels, ties broken by refresh - except that the 1080p60 anchor is kept
  whenever no candidate covers strictly more pixels than it.  That rule
  keeps the working 1080p60 output on sinks whose EDID lists their native
  at the same size (e.g. a 1080p30 box whose preferred timing is 30 Hz but
  whose range descriptor accepts 60 Hz), while a smaller panel (say a
  720p display) gets its own best mode instead of a mode it cannot show.

  Falls back to the anchor when no EDID has been read yet (driver entry,
  before the DDC master is clocked) or when the sink advertises nothing
  usable.

  @return  The mode to drive.
**/
CONST A733_HDMI_MODE *
A733HdmiSelectBestMode (
  VOID
  )
{
  CONST A733_HDMI_MODE  *Best;
  UINT32                 Mask;
  UINT32                 Count;
  UINT32                 i;
  UINTN                  BestPixels;
  UINT32                 BestRefresh;

  Best        = A733HdmiGetMode (HDMI_VIC_ANCHOR);
  BestPixels  = (UINTN)Best->HActive * Best->VActive;
  BestRefresh = Best->PixelClockHz / ((UINT32)Best->HTotal * Best->VTotal);

  if (!mEdidNativeValid) {
    return Best;
  }

  if (EFI_ERROR (A733HdmiGetSupportedPresets (&Mask))) {
    Mask = 0;
  }

  ConsiderMode (&mEdidNativeMode, &Best, &BestPixels, &BestRefresh);

  A733HdmiGetPresetTable (&Count);
  for (i = 0; i < Count; i++) {
    if ((Mask & (1u << i)) != 0) {
      ConsiderMode (&mHdmiModes[i], &Best, &BestPixels, &BestRefresh);
    }
  }

  DEBUG ((DEBUG_INFO,
          "A733Hdmi: best sink mode %ux%u@%u Hz (native %ux%u@%u, mask 0x%08x)%a\n",
          Best->HActive, Best->VActive, BestRefresh,
          mEdidNativeMode.HActive, mEdidNativeMode.VActive,
          mEdidNativeMode.PixelClockHz / 1000000,
          Mask,
          (Best == A733HdmiGetMode (HDMI_VIC_ANCHOR)) ? ", anchor kept" : ""));

  return Best;
}