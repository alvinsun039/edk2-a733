/** @file
  A733HdmiEdid - read the sink EDID over the DWC-HDMI DDC I2CM master
  and derive the display's native mode from the first detailed timing
  descriptor.

  The DDC I2CM block (offset 0x7E00) is the standard DesignWare HDMI
  DDC master; the byte-wise polled sequence mirrors U-Boot's dw_hdmi.c
  (hdmi_read_edid).  The block must be driven after the HDMI core clock
  is up (A733HdmiHwInit) - the DDC pins sit on the PHY, so a dead core
  means a dead DDC.

  We read block 0 only - the DTD in bytes 0x36..0x47 carries the
  preferred/native timing, which is all the GOP needs for a single
  native mode.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include "A733HdmiDxe.h"

//
// EDID-derived native mode, cached once the HDMI core is up (DDC needs
// the core clock).  Populated by A733HdmiProbeEdid(); consumed by
// A733HdmiActiveMode() when the Setup preset is NATIVE.
//
A733_HDMI_MODE  mEdidNativeMode;
BOOLEAN         mEdidNativeValid;

//
// EDID block 0 layout (VESA E-EDID 1.4).
//
#define EDID_BLOCK_SIZE          128
#define EDID_HEADER_SIZE         8
#define EDID_DTD1_OFFSET         0x36   // preferred detailed timing descriptor
#define EDID_DTD_SIZE            18

#define EDID_DTD_PIXEL_CLOCK_LO  0
#define EDID_DTD_PIXEL_CLOCK_HI  1
#define EDID_DTD_HA_LO           2
#define EDID_DTD_HBLANK_LO       3
#define EDID_DTD_HA_HBLANK_HI    4
#define EDID_DTD_VA_LO           5
#define EDID_DTD_VBLANK_LO       6
#define EDID_DTD_VA_VBLANK_HI    7
#define EDID_DTD_HSOFF_LO        8
#define EDID_DTD_HSWIDTH_LO      9
#define EDID_DTD_VSOFF_LO        10
#define EDID_DTD_VSWIDTH_LO      11
#define EDID_DTD_HS_VS_HI        12

//
// EDID EEPROM sits at I2C address 0x50 on the DDC bus.
//
#define DDC_SLAVE_ADDR            0x50


//
// Top-PHY PLL reference (24 MHz DCXO, sun60i phy_top).  The A7A pixel
// clock is Fout = 24 MHz * (N+1) / (P0+1); the anchor 1080p60 uses
// N=98 / P0=15 -> 148.5 MHz.  Derive N/P0 for an arbitrary EDID pixel
// clock.
//
#define EDID_PLL_REF_HZ       24000000u
#define EDID_PLL_N_MAX        254      // 8-bit field: N+1 <= 255
#define EDID_PLL_P0_MAX       63       // 6-bit field

//
// A "null" DTD (all zeros) means no timing in this slot.
//
STATIC
BOOLEAN
IsNullDtd (
  IN CONST UINT8  *Dtd
  )
{
  UINTN  Index;

  for (Index = 0; Index < EDID_DTD_SIZE; Index++) {
    if (Dtd[Index] != 0) {
      return FALSE;
    }
  }

  return TRUE;
}

/**
  Pick the PLL N/P0 divider pair for a target pixel clock.

  @param[in]  PixelClockHz  Desired pixel clock.
  @param[out] N             PLL multiplier field.
  @param[out] P0            PLL pre-divider field.

  @retval EFI_SUCCESS       A representable pair found.
  @retval EFI_UNSUPPORTED   Clock out of the PLL's range.
**/
STATIC
EFI_STATUS
EdidPllFactors (
  IN  UINT32  PixelClockHz,
  OUT UINT32  *N,
  OUT UINT32  *P0
  )
{
  UINT32  P0cand;
  UINT32  Ncand;

  for (P0cand = 0; P0cand <= EDID_PLL_P0_MAX; P0cand++) {
    //
    // Fout = ref * (N+1) / (P0+1)  ->  N+1 = Fout * (P0+1) / ref.
    //
    UINT64  Ratio = (UINT64)PixelClockHz * (P0cand + 1);

    if (Ratio % EDID_PLL_REF_HZ != 0) {
      continue;   // exact division only (integer-N PLL)
    }

    Ncand = (UINT32)(Ratio / EDID_PLL_REF_HZ) - 1;
    if (Ncand <= EDID_PLL_N_MAX) {
      *N  = Ncand;
      *P0 = P0cand;
      return EFI_SUCCESS;
    }
  }

  return EFI_UNSUPPORTED;
}

/**
  Poll the DDC I2CM done bit.

  @param[in] TimeoutMs  Budget in milliseconds.

  @retval TRUE   Operation completed.
  @retval FALSE  Timeout.
**/
STATIC
BOOLEAN
DdcWaitDone (
  IN UINTN  TimeoutMs
  )
{
  UINTN  Tries;

  for (Tries = 0; Tries < TimeoutMs * 10; Tries++) {
    UINT8  Stat;

    Stat = MmioRead8 (HDMI_BASE + IH_I2CM_STAT0_REG);
    if ((Stat & 0x02) != 0) {          // I2CMASTERDONE
      MmioWrite8 (HDMI_BASE + IH_I2CM_STAT0_REG, Stat);
      return TRUE;
    }
    if ((Stat & 0x01) != 0) {          // I2CMASTERERROR (NAK / bus error)
      MmioWrite8 (HDMI_BASE + IH_I2CM_STAT0_REG, Stat);
      DEBUG ((DEBUG_WARN, "A733Hdmi: DDC master error (IH=%02x)\n", Stat));
      return FALSE;
    }
    MicroSecondDelay (100);
  }

  return FALSE;
}

/**
  Soft-reset the DDC I2CM master (clears any stuck state).
**/
STATIC
VOID
DdcReset (VOID)
{
  MmioWrite8 (HDMI_BASE + DDC_I2CM_SOFTRSTZ_REG, 0);   // assert reset
}

/**
  Read EDID block 0 from the DDC I2CM master.

  @param[out] Buf  Buffer for EDID_BLOCK_SIZE bytes.

  @retval EFI_SUCCESS       Block read.
  @retval EFI_DEVICE_ERROR  DDC timeout / no sink.
  @retval EFI_NO_MEDIA      EDID header mismatch (no display / no EDID).
**/
STATIC
EFI_STATUS
DdcReadEdidBlock (
  OUT UINT8  *Buf
  )
{
  UINTN  Try;
  UINTN  N;
  UINT8  Header[EDID_HEADER_SIZE];

  //
  // Un-mute the I2CM status interrupt (IH_MUTE_I2CM_STAT0, 0x0185).
  // A733HdmiHwInit masks all interrupt groups by writing 0xFF to the
  // IH_MUTE_* registers; with the I2CM group muted, IH_I2CM_STAT0 stays
  // 0x00 even when a DDC transfer completes - every read then times
  // out.  This is the BSP offset (dw_mc.h: IH_MUTE_I2CM_STAT0 = 0x185),
  // NOT the CEC group.
  //
  //
  // Un-mute only the I2CM-STAT0 DONE bit (bit0) we poll below;
  // the other group bits (abort/nack/arb-lost) stay masked so a
  // stray DDC fault cannot raise the GIC line.
  //
  MmioAndThenOr8 (HDMI_BASE + 0x0185, (UINT8)~BIT0, 0);

  //
  // 100 kHz standard-mode clock.  The DWC controller derives SCL from
  // the programmed high/low cycle counters; without them the DDC master
  // never clocks the bus and every read times out.  Values from the
  // sunxi dw-hdmi reference (sunxi_dw_hdmi_of_to_plat).
  //
  MmioWrite8 (HDMI_BASE + DDC_I2CM_SS_SCL_HCNT_1_REG, (UINT8)(DDC_SCL_HCNT >> 8));
  MmioWrite8 (HDMI_BASE + DDC_I2CM_SS_SCL_HCNT_0_REG, (UINT8)(DDC_SCL_HCNT & 0xFF));
  MmioWrite8 (HDMI_BASE + DDC_I2CM_SS_SCL_LCNT_1_REG, (UINT8)(DDC_SCL_LCNT >> 8));
  MmioWrite8 (HDMI_BASE + DDC_I2CM_SS_SCL_LCNT_0_REG, (UINT8)(DDC_SCL_LCNT & 0xFF));
  MmioWrite8 (HDMI_BASE + DDC_I2CM_DIV_REG, DDC_I2CM_DIV_STD_MODE);

  MmioWrite8 (HDMI_BASE + DDC_I2CM_SLAVE_REG, DDC_SLAVE_ADDR);
  MmioWrite8 (HDMI_BASE + DDC_I2CM_SEGADDR_REG, DDC_I2CM_SEGADDR_DDC);
  MmioWrite8 (HDMI_BASE + DDC_I2CM_SEGPTR_REG, 0);

  for (Try = 0; Try < 5; Try++) {
    for (N = 0; N < EDID_BLOCK_SIZE; N++) {
      MmioWrite8 (HDMI_BASE + DDC_I2CM_ADDRESS_REG, (UINT8)N);
      MmioWrite8 (HDMI_BASE + DDC_I2CM_OPERATION_REG, DDC_I2CM_OP_RD);

      if (!DdcWaitDone (10)) {
        UINT8  Stat;

        Stat = MmioRead8 (HDMI_BASE + IH_I2CM_STAT0_REG);
        DEBUG ((
          DEBUG_ERROR,
          "A733Hdmi: DDC byte %u timeout (IH=%02x, try %u)\n",
          N, Stat, Try
          ));
        DdcReset ();
        break;
      }

      Buf[N] = MmioRead8 (HDMI_BASE + DDC_I2CM_DATAI_REG);
    }

    if (N == EDID_BLOCK_SIZE) {
      break;
    }
  }

  if (Try >= 5) {
    DEBUG ((DEBUG_WARN, "A733Hdmi: EDID DDC read timed out (no sink?)\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Validate the EDID header so a live-but-blank DDC (unpowered sink,
  // floating bus) is not mistaken for a real EDID.
  //
  CopyMem (Header, Buf, EDID_HEADER_SIZE);
  if (!CompareMem (Header, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", EDID_HEADER_SIZE)) {
    return EFI_SUCCESS;
  }

  {
    UINTN  D;

    DEBUG ((DEBUG_ERROR, "A733Hdmi: EDID header invalid, 128 bytes:\n"));
    for (D = 0; D < EDID_BLOCK_SIZE; D += 16) {
      DEBUG ((
        DEBUG_ERROR,
        "  %02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
        Buf[D+0], Buf[D+1], Buf[D+2], Buf[D+3],
        Buf[D+4], Buf[D+5], Buf[D+6], Buf[D+7],
        Buf[D+8], Buf[D+9], Buf[D+10], Buf[D+11],
        Buf[D+12], Buf[D+13], Buf[D+14], Buf[D+15]
        ));
    }
  }
  return EFI_NO_MEDIA;
}

/**
  Parse the first non-null detailed timing descriptor (DTD) of EDID
  block 0 and fill an A733_HDMI_MODE with it.

  The 18-byte DTD layout (all little-endian fields):
    +0..1  pixel clock in 10 kHz units
    +2     HActive[7:0]
    +3     HBlank[7:0]
    +4     HActive[11:8] | HBlank[11:8] << 4
    +5     VActive[7:0]
    +6     VBlank[7:0]
    +7     VActive[11:8] | VBlank[11:8] << 4
    +8     HSyncOffset[7:0]
    +9     HSyncWidth[7:0]
    +10    VSyncOffset[7:0]
    +11    VSyncWidth[7:0]
    +12    bit7 VSyncPositive, bit6 HSyncPositive
    +13..14 HSize (ignored)
    +15..16 VSize (ignored)

  @param[in]  Edid  EDID block 0.
  @param[out] Mode  Filled native mode (Vic 0 = non-CEA).

  @retval EFI_SUCCESS       DTD found and parsed.
  @retval EFI_NOT_FOUND     No usable DTD.
**/
STATIC
EFI_STATUS
ParseEdidNativeMode (
  IN  CONST UINT8   *Edid,
  OUT A733_HDMI_MODE *Mode
  )
{
  CONST UINT8  *Dtd;
  UINTN        Index;
  UINT16       HActive;
  UINT16       HBlank;
  UINT16       VActive;
  UINT16       VBlank;
  UINT32       PixelClockHz;

  for (Index = 0; Index < 4; Index++) {
    Dtd = Edid + EDID_DTD1_OFFSET + Index * EDID_DTD_SIZE;

    if (IsNullDtd (Dtd)) {
      continue;
    }

    PixelClockHz = (UINT32)(Dtd[EDID_DTD_PIXEL_CLOCK_LO] |
                            (Dtd[EDID_DTD_PIXEL_CLOCK_HI] << 8)) * 10000u;

    HActive = (UINT16)(Dtd[EDID_DTD_HA_LO] |
                       ((Dtd[EDID_DTD_HA_HBLANK_HI] & 0xF0) << 4));
    HBlank  = (UINT16)(Dtd[EDID_DTD_HBLANK_LO] |
                       ((Dtd[EDID_DTD_HA_HBLANK_HI] & 0x0F) << 8));
    VActive = (UINT16)(Dtd[EDID_DTD_VA_LO] |
                       ((Dtd[EDID_DTD_VA_VBLANK_HI] & 0xF0) << 4));
    VBlank  = (UINT16)(Dtd[EDID_DTD_VBLANK_LO] |
                       ((Dtd[EDID_DTD_VA_VBLANK_HI] & 0x0F) << 8));

    //
    // Guard against absurd timings (corrupt EDID); require a sane pixel
    // clock (10..600 MHz) and active area.
    //
    if ((PixelClockHz < 10000000u) || (PixelClockHz > 600000000u) ||
        (HActive < 320) || (HActive > 7680) ||
        (VActive < 200) || (VActive > 4320) ||
        (HBlank == 0) || (VBlank == 0))
    {
      continue;
    }

    ZeroMem (Mode, sizeof (*Mode));
    Mode->Vic           = 0;   // non-CEA timing from the sink
    Mode->PixelClockHz  = PixelClockHz;
    Mode->HActive       = HActive;
    Mode->HTotal        = (UINT16)(HActive + HBlank);
    Mode->HSyncStart    = (UINT16)(HActive +
                                   Dtd[EDID_DTD_HSOFF_LO] +
                                   ((Dtd[EDID_DTD_HS_VS_HI] & 0xC0) << 2));
    Mode->HSyncEnd      = (UINT16)(Mode->HSyncStart +
                                   Dtd[EDID_DTD_HSWIDTH_LO] +
                                   ((Dtd[EDID_DTD_HS_VS_HI] & 0x30) << 4));
    Mode->VActive       = VActive;
    Mode->VTotal        = (UINT16)(VActive + VBlank);
    Mode->VSyncStart    = (UINT16)(VActive +
                                   Dtd[EDID_DTD_VSOFF_LO] +
                                   ((Dtd[EDID_DTD_HS_VS_HI] & 0x0C) << 2));
    Mode->VSyncEnd      = (UINT16)(Mode->VSyncStart +
                                   Dtd[EDID_DTD_VSWIDTH_LO] +
                                   ((Dtd[EDID_DTD_HS_VS_HI] & 0x03) << 4));
    Mode->HSyncPositive = (Dtd[EDID_DTD_HS_VS_HI] & 0x40) != 0;
    Mode->VSyncPositive = (Dtd[EDID_DTD_HS_VS_HI] & 0x80) != 0;

    //
    // Derive the PLL factors for this pixel clock.  A clock the PLL
    // cannot represent exactly is rejected (the FC timings are fine but
    // the output would drift) - fall through to the next DTD.
    //
    if (EFI_ERROR (EdidPllFactors (PixelClockHz, &Mode->PhyPllN, &Mode->PhyPllP0))) {
      DEBUG ((
        DEBUG_WARN,
        "A733Hdmi: EDID clock %u Hz not representable by PLL - skipping DTD\n",
        PixelClockHz
        ));
      continue;
    }

    DEBUG ((
      DEBUG_INFO,
      "A733Hdmi: EDID native %ux%u @ %u Hz (PLL N=%u P0=%u)\n",
      Mode->HActive,
      Mode->VActive,
      Mode->PixelClockHz,
      Mode->PhyPllN,
      Mode->PhyPllP0
      ));

    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

/**
  Read the sink EDID (block 0) through the DDC I2CM master and fill the
  native mode from the first detailed timing descriptor.

  @param[out] Mode  Filled with the EDID native timing on success.

  @retval EFI_SUCCESS       Native mode derived from the EDID.
  @retval EFI_NOT_FOUND     No EDID / no usable DTD (caller falls back).
  @retval others            DDC failure.
**/
EFI_STATUS
A733HdmiReadEdidNativeMode (
  OUT A733_HDMI_MODE  *Mode
  )
{
  EFI_STATUS  Status;
  UINT8       Edid[EDID_BLOCK_SIZE];

  if (Mode == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = DdcReadEdidBlock (Edid);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return ParseEdidNativeMode (Edid, Mode);
}

/**
  Probe the sink EDID (call with the HDMI core clock running, i.e. after
  A733HdmiHwInit) and cache the native mode.

  The DDC bus is dead before the core is clocked, so reading here - and
  only here - is what makes the I2CM master actually respond.  Failure
  (no display / no DTD) leaves mEdidNativeValid FALSE and the active
  mode stays on the preset/anchor.

  @retval EFI_SUCCESS       EDID read and native mode cached.
  @retval EFI_NOT_FOUND     No EDID or no usable DTD.
  @retval others            DDC failure.
**/
EFI_STATUS
A733HdmiProbeEdid (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mEdidNativeValid) {
    return EFI_SUCCESS;   // already probed
  }

  Status = A733HdmiReadEdidNativeMode (&mEdidNativeMode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mEdidNativeValid = TRUE;
  DEBUG ((
    DEBUG_INFO,
    "A733Hdmi: native mode from EDID %ux%u @ %u Hz\n",
    mEdidNativeMode.HActive,
    mEdidNativeMode.VActive,
    mEdidNativeMode.PixelClockHz
    ));

  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Full-mode parsing: which of the firmware's preset modes the sink
// advertises, so the Setup "Display Mode" list can show only supported
// resolutions (no more out-of-range black screens).
// ---------------------------------------------------------------------------

//
// EDID block 0 layout.
//
#define EDID_BT_OFFSET        0x23   // established timings, 3 bytes
#define EDID_ST_OFFSET        0x26   // standard timings, 8 x 2 bytes
#define EDID_ST_COUNT         8
#define EDID_DTD_COUNT        4
#define EDID_EXT_COUNT_OFFSET 0x7E

//
// Standard-timing byte pair: byte0 = HActive/8 - 31, byte1 = aspect+rate.
// byte1 == 0x01 or 0x00 means "unused".
//
#define ST_HA_BYTE(St)  ((St)[0])
#define ST_AR_BYTE(St)  ((St)[1])
#define ST_USED(St)     (((St)[0] != 0x01) && ((St)[0] != 0x00))
#define ST_HACTIVE(St)  (((UINT16)((St)[0]) + 31u) * 8u)
#define ST_RATE(St)     ((St)[1] & 0x3F)

//
// Refresh rate from the standard-timing rate field (adjusts in 60 Hz
// base: field value 0..255, actual ~= field*8+60 for the 60 Hz family,
// but the common ones are 60/75/85; match on the field value directly).
//
STATIC
UINT16
StRefresh (
  IN UINT8  RateField
  )
{
  //
  // VESA standard-timing refresh field (byte1[5:0]).
  //
  switch (RateField) {
    case 0x01: return 60;
    case 0x02: return 75;
    case 0x03: return 85;
    case 0x04: return 30;
    case 0x05: return 50;
    case 0x06: return 90;
    case 0x07: return 100;
    default:   return 60;
  }
}

//
// VESA standard-timing aspect ratio (byte1[7:6]) -> vertical lines from
// the horizontal pixel count.
//
STATIC
UINT32
StVActive (
  IN UINT32  HActive,
  IN UINT8   AspectField
  )
{
  switch (AspectField) {
    case 0x00: return HActive * 10 / 16;   // 16:10
    case 0x01: return HActive * 3 / 4;     // 4:3
    case 0x02: return HActive * 4 / 5;     // 5:4
    default:   return HActive * 9 / 16;    // 16:9
  }
}

//
// EDID-861 extension (block 1): tag 0x02, DTDs start at offset 4.
//
#define CEA_TAG_VIDEO      0x02
#define CEA_DTD_OFFSET     0x04

/**
  Parse EDID block 0 into the supported-preset bitmap: the 4 DTDs, the
  8 standard timings and the established timings each contribute.

  @param[in]  Edid   EDID block 0.
  @param[out] Bitmap Bit i set when preset i is advertised.
**/
STATIC
VOID
ParseBlock0Modes (
  IN  CONST UINT8  *Edid,
  OUT UINT32       *Bitmap
  )
{
  const A733_HDMI_MODE  *Modes;
  UINT32                Count;
  UINT32                i;
  UINT32                Index;
  UINT16                HActive;
  UINT16                VActive;
  UINT16                HBlank;
  UINT16                VBlank;
  UINT32                Refresh;

  Modes = A733HdmiGetPresetTable (&Count);
  (VOID)Modes;

  //
  // Detailed timing descriptors.
  //
  for (Index = 0; Index < EDID_DTD_COUNT; Index++) {
    CONST UINT8  *Dtd = Edid + EDID_DTD1_OFFSET + Index * EDID_DTD_SIZE;

    if (IsNullDtd (Dtd)) {
      continue;
    }

    HActive = (UINT16)(Dtd[EDID_DTD_HA_LO] |
                       ((Dtd[EDID_DTD_HA_HBLANK_HI] & 0xF0) << 4));
    HBlank  = (UINT16)(Dtd[EDID_DTD_HBLANK_LO] |
                       ((Dtd[EDID_DTD_HA_HBLANK_HI] & 0x0F) << 8));
    VActive = (UINT16)(Dtd[EDID_DTD_VA_LO] |
                       ((Dtd[EDID_DTD_VA_VBLANK_HI] & 0xF0) << 4));
    VBlank  = (UINT16)(Dtd[EDID_DTD_VBLANK_LO] |
                       ((Dtd[EDID_DTD_VA_VBLANK_HI] & 0x0F) << 8));

    if ((HActive == 0) || (VActive == 0)) {
      continue;
    }

    Refresh = (UINT32)((Dtd[EDID_DTD_PIXEL_CLOCK_LO] |
                        (Dtd[EDID_DTD_PIXEL_CLOCK_HI] << 8)) * 10000u) /
              ((HActive + HBlank) * (VActive + VBlank));

    DEBUG ((
      DEBUG_INFO,
      "A733Hdmi: EDID DTD%u %ux%u @ ~%u Hz\n",
      Index, HActive, VActive, Refresh
      ));

    for (i = 0; i < Count; i++) {
      if ((Modes[i].HActive == HActive) && (Modes[i].VActive == VActive) &&
          (Refresh != 0) &&
          (Modes[i].PixelClockHz / ((UINT32)Modes[i].HTotal * Modes[i].VTotal) >= Refresh - 1) &&
          (Modes[i].PixelClockHz / ((UINT32)Modes[i].HTotal * Modes[i].VTotal) <= Refresh + 1))
      {
        *Bitmap |= (UINT32)(1u << i);
        break;
      }
    }
  }

  //
  // Standard timings.
  //
  for (Index = 0; Index < EDID_ST_COUNT; Index++) {
    CONST UINT8  *St = Edid + EDID_ST_OFFSET + Index * 2;
    UINT32       VActive;

    if (!ST_USED (St)) {
      continue;
    }

    HActive = ST_HACTIVE (St);
    VActive = StVActive (HActive, (St[1] >> 6) & 0x03);
    Refresh = StRefresh (ST_RATE (St));

    DEBUG ((
      DEBUG_INFO,
      "A733Hdmi: EDID ST%u %ux%u @ %u Hz\n",
      Index, HActive, VActive, Refresh
      ));

    for (i = 0; i < Count; i++) {
      if ((Modes[i].HActive == HActive) && (Modes[i].VActive == VActive)) {
        UINT32  PresetHz = Modes[i].PixelClockHz /
                           ((UINT32)Modes[i].HTotal * Modes[i].VTotal);

        if (((PresetHz >= Refresh - 1) && (PresetHz <= Refresh + 1)) ||
            (Refresh == 0))
        {
          *Bitmap |= (UINT32)(1u << i);
          break;
        }
      }
    }
  }

  //
  // Established timings: bit map of common modes; bit 0=720x400@70 ...
  // Only the ones we can drive are worth setting here (covered by DTD/ST
  // in practice), so no explicit handling - the DTD/ST passes above
  // already picked up anything the sink advertises twice.
  //
}

/**
  Parse the CEA-861 extension block (EDID block 1) and add every Video
  Data Block VIC that maps to a preset mode.

  @param[in]  Edid1  EDID block 1.
  @param[out] Bitmap Bit i set when preset i is advertised.
**/
STATIC
VOID
ParseCeaModes (
  IN  CONST UINT8  *Edid1,
  OUT UINT32       *Bitmap
  )
{
  const A733_HDMI_MODE  *Modes;
  UINT32                Count;
  UINT32                i;
  UINT8                 DtdCount;
  UINTN                 Offset;
  UINT8                 Tag;

  if ((Edid1[0] != 0x02) ||            // CEA extension tag
      (Edid1[1] & 0x80) == 0)          // requires 1st data block present
  {
    return;
  }

  Modes  = A733HdmiGetPresetTable (&Count);
  DtdCount = Edid1[2] & 0x0F;
  Offset = CEA_DTD_OFFSET + (UINTN)DtdCount * 18;

  while (Offset < 127) {
    Tag = Edid1[Offset];

    if (Tag == 0x00) {                 // end of data blocks
      break;
    }

    if ((Tag & 0x0F) == CEA_TAG_VIDEO) {
      UINTN  Length = (Tag >> 5) & 0x07;
      UINTN  N;

      for (N = 1; N <= Length; N++) {
        UINT8  Vic = Edid1[Offset + N] & 0x7F;

        for (i = 0; i < Count; i++) {
          if ((Modes[i].Vic != 0) && (Modes[i].Vic == Vic)) {
            *Bitmap |= (UINT32)(1u << i);
            break;
          }
        }
      }

      Offset += 1 + Length;
    } else {
      Offset += 1 + ((Tag >> 5) & 0x07);
    }
  }
}

/**
  Fill the supported-preset bitmap from the sink EDID (blocks 0 and 1).

  @param[out] Bitmap  Bit i set when preset mode i is supported.

  @retval EFI_SUCCESS       EDID read and parsed.
  @retval EFI_DEVICE_ERROR  DDC failure.
  @retval EFI_NO_MEDIA      No valid EDID header.
**/
EFI_STATUS
A733HdmiGetSupportedPresets (
  OUT UINT32  *Bitmap
  )
{
  EFI_STATUS  Status;
  UINT8       Edid0[EDID_BLOCK_SIZE];
  UINT8       Edid1[EDID_BLOCK_SIZE];
  UINT8       Header[EDID_HEADER_SIZE];

  if (Bitmap == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Bitmap = 0;

  Status = DdcReadEdidBlock (Edid0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (Header, Edid0, EDID_HEADER_SIZE);
  if (CompareMem (Header, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", EDID_HEADER_SIZE)) {
    return EFI_NO_MEDIA;
  }

  ParseBlock0Modes (Edid0, Bitmap);

  //
  // Extension block 1 (CEA-861) when the sink advertises one.
  //
  if (Edid0[EDID_EXT_COUNT_OFFSET] > 0) {
    if (!EFI_ERROR (DdcReadEdidBlock (Edid1))) {
      ParseCeaModes (Edid1, Bitmap);
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "A733Hdmi: EDID supported-preset bitmap = 0x%08x\n",
    *Bitmap
    ));

  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// EDID DTD mode list: every detailed timing descriptor, including ones
// that do not match a firmware preset (e.g. 1920x1080@30 on a 1080P30
// display).  The Setup page offers these as extra entries above the
// preset-derived list; DISPLAY_MODE_EDID_BASE + i selects them.
// ---------------------------------------------------------------------------

/**
  Return the EDID's detailed timing descriptors as modes.

  The first DTD (the preferred/native timing) is guaranteed to fill
  Modes[0] when the sink has any EDID at all; subsequent DTDs fill the
  rest until the 4-slot table is exhausted.  Modes that the firmware
  cannot drive (no PLL factors) are skipped.

  @param[in]  MaxModes  Capacity of Modes.
  @param[out] Modes     Array of A733_HDMI_MODE, filled in DTD order.
  @param[out] Count     Number of modes returned.

  @retval EFI_SUCCESS       DTDs parsed (Count may be 0).
  @retval EFI_NO_MEDIA      No valid EDID.
**/
EFI_STATUS
A733HdmiGetEdidModes (
  IN  UINTN           MaxModes,
  OUT A733_HDMI_MODE  *Modes,
  OUT UINTN           *Count
  )
{
  EFI_STATUS  Status;
  UINT8       Edid[EDID_BLOCK_SIZE];
  UINTN       Index;
  UINTN       Out;

  if ((Modes == NULL) || (Count == NULL) || (MaxModes == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Count = 0;

  Status = DdcReadEdidBlock (Edid);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CompareMem (Edid, "\x00\xFF\xFF\xFF\xFF\xFF\xFF\x00", EDID_HEADER_SIZE)) {
    return EFI_NO_MEDIA;
  }

  //
  // Full EDID dump for bring-up verification (verbose builds).
  //
  {
    UINTN  D;

    DEBUG ((DEBUG_INFO, "A733Hdmi: full EDID block 0 (128 bytes):\n"));
    for (D = 0; D < EDID_BLOCK_SIZE; D += 16) {
      DEBUG ((
        DEBUG_INFO,
        "  %02x %02x %02x %02x %02x %02x %02x %02x "
        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
        Edid[D+0], Edid[D+1], Edid[D+2], Edid[D+3],
        Edid[D+4], Edid[D+5], Edid[D+6], Edid[D+7],
        Edid[D+8], Edid[D+9], Edid[D+10], Edid[D+11],
        Edid[D+12], Edid[D+13], Edid[D+14], Edid[D+15]
        ));
    }
  }

  Out = 0;

  for (Index = 0; (Index < EDID_DTD_COUNT) && (Out < MaxModes); Index++) {
    CONST UINT8  *Dtd = Edid + EDID_DTD1_OFFSET + Index * EDID_DTD_SIZE;
    UINT16       HActive;
    UINT16       HBlank;
    UINT16       VActive;
    UINT16       VBlank;
    UINT32       PixelClockHz;

    if (IsNullDtd (Dtd)) {
      continue;
    }

    PixelClockHz = (UINT32)(Dtd[EDID_DTD_PIXEL_CLOCK_LO] |
                            (Dtd[EDID_DTD_PIXEL_CLOCK_HI] << 8)) * 10000u;
    HActive = (UINT16)(Dtd[EDID_DTD_HA_LO] |
                       ((Dtd[EDID_DTD_HA_HBLANK_HI] & 0xF0) << 4));
    HBlank  = (UINT16)(Dtd[EDID_DTD_HBLANK_LO] |
                       ((Dtd[EDID_DTD_HA_HBLANK_HI] & 0x0F) << 8));
    VActive = (UINT16)(Dtd[EDID_DTD_VA_LO] |
                       ((Dtd[EDID_DTD_VA_VBLANK_HI] & 0xF0) << 4));
    VBlank  = (UINT16)(Dtd[EDID_DTD_VBLANK_LO] |
                       ((Dtd[EDID_DTD_VA_VBLANK_HI] & 0x0F) << 8));

    if ((PixelClockHz < 10000000u) || (PixelClockHz > 600000000u) ||
        (HActive < 320) || (HActive > 7680) ||
        (VActive < 200) || (VActive > 4320) ||
        (HBlank == 0) || (VBlank == 0))
    {
      continue;
    }

    ZeroMem (&Modes[Out], sizeof (Modes[Out]));
    Modes[Out].Vic          = 0;
    Modes[Out].PixelClockHz = PixelClockHz;
    Modes[Out].HActive      = HActive;
    Modes[Out].HTotal       = (UINT16)(HActive + HBlank);
    Modes[Out].HSyncStart   = (UINT16)(HActive +
                                       Dtd[EDID_DTD_HSOFF_LO] +
                                       ((Dtd[EDID_DTD_HS_VS_HI] & 0xC0) << 2));
    Modes[Out].HSyncEnd     = (UINT16)(Modes[Out].HSyncStart +
                                       Dtd[EDID_DTD_HSWIDTH_LO] +
                                       ((Dtd[EDID_DTD_HS_VS_HI] & 0x30) << 4));
    Modes[Out].VActive      = VActive;
    Modes[Out].VTotal       = (UINT16)(VActive + VBlank);
    Modes[Out].VSyncStart   = (UINT16)(VActive +
                                       Dtd[EDID_DTD_VSOFF_LO] +
                                       ((Dtd[EDID_DTD_HS_VS_HI] & 0x0C) << 2));
    Modes[Out].VSyncEnd     = (UINT16)(Modes[Out].VSyncStart +
                                       Dtd[EDID_DTD_VSWIDTH_LO] +
                                       ((Dtd[EDID_DTD_HS_VS_HI] & 0x03) << 4));
    Modes[Out].HSyncPositive = (Dtd[EDID_DTD_HS_VS_HI] & 0x40) != 0;
    Modes[Out].VSyncPositive = (Dtd[EDID_DTD_HS_VS_HI] & 0x80) != 0;

    if (EFI_ERROR (EdidPllFactors (PixelClockHz, &Modes[Out].PhyPllN, &Modes[Out].PhyPllP0))) {
      continue;   // not drivable
    }

    Out++;
  }

  *Count = Out;
  return EFI_SUCCESS;
}
