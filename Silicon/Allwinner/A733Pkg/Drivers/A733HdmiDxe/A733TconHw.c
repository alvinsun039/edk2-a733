/** @file
  TCON3 (tcon-tv) bring-up, timing driven by the active A733_HDMI_MODE.

  TCON3 is the timing generator between the DE3.0 video pipe and the
  DWC HDMI controller.  The BASIC1-5 registers are derived from the
  CEA-861 timing fields of the active mode (the anchor mode is 1080p60:
  pixel clock 148.5 MHz, H 1920/2008/2052/2200, V 1080/1084/1089/1125,
  positive H/V sync).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "A733HdmiDxe.h"

#include <Library/DebugLib.h>
#include <Library/IoLib.h>

#define TCON3_BASE_ABS  TCON3_BASE

/**
  Enable the TCON3 module itself (global control, data path tristate off).
**/
STATIC
VOID
TconModuleInit (VOID)
{
  //
  // Global control: bit31 tcon_en is the module master enable (BSP
  // tcon_tv_init: tcon_gctl.bits.tcon_en = 1).  Bit0 alone (io_map_sel)
  // does not gate the scanout; writing 1 there left the TCON collecting
  // DE data into its FIFO but never emitting pixels to the DWC.
  //
  MmioWrite32 (TCON3_BASE_ABS + TCON3_TV_CTL_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_GINT0_REG,  0);
  //
  // A733 user manual TV_GCTL_REG: bit0 UNSTRICT_TIMING_MODE_SEL.
  // 0 = strict timing mode (TCON follows the DE's sync exactly), 1 =
  // unstrict.  Strict mode is the right choice for a locked DE/TCON
  // pair, but if the DE pixel clock is not yet in lock the strict FIFO
  // gating starves the TCON (black + TV_FIFO_UNDER_FLOW).  Run unstrict
  // so DE data can always drain.  Bit31 is reserved on A733.
  //
  MmioWrite32 (TCON3_BASE_ABS + TCON3_GCTL_REG,   0x00000001);

  //
  // Data/IO polarity and tristate defaults.
  //
  MmioWrite32 (TCON3_BASE_ABS + TCON3_DATA_IO_POL0_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_DATA_IO_POL1_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_DATA_IO_TRI0_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_DATA_IO_TRI1_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_PIXEL_DEPTH_REG,  0);
}

/**
  Program the timing registers from the active video mode.

  Register semantics (sun60iw2 tcon-tv, progressive):
    BASIC1 = vt*2                (whole frame, doubled)
    BASIC2 = yo(v_active-1) | xo(h_active-1)<<16
    BASIC3 = hbp | ht(h_total-1)<<16      hbp = hsync_w + hback - 1
    BASIC4 = vbp | vt*2<<16               vbp = vsync_w + vback - 1
    BASIC5 = vspw | hspw<<16              vspw = vsync_w - 1, hspw = hsync_w - 1
**/
STATIC
VOID
TconTimingInit (VOID)
{
  const A733_HDMI_MODE  *Mode;
  UINT32                 HAct;
  UINT32                 HTotal;
  UINT32                 HSyncW;
  UINT32                 HBack;
  UINT32                 VAct;
  UINT32                 VTotal;
  UINT32                 VSyncW;
  UINT32                 VBack;

  Mode    = A733HdmiActiveMode ();
  HAct    = Mode->HActive;
  HTotal  = Mode->HTotal;
  HSyncW  = Mode->HSyncEnd - Mode->HSyncStart;
  HBack   = HTotal - Mode->HSyncEnd;
  VAct    = Mode->VActive;
  VTotal  = Mode->VTotal;
  VSyncW  = Mode->VSyncEnd - Mode->VSyncStart;
  VBack   = VTotal - Mode->VSyncEnd;

  MmioWrite32 (TCON3_BASE_ABS + TCON3_BASIC1_REG, VTotal * 2);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_BASIC2_REG,
               ((HAct - 1) << 16) | (VAct - 1));
  MmioWrite32 (TCON3_BASE_ABS + TCON3_BASIC3_REG,
               ((HTotal - 1) << 16) | (HSyncW + HBack - 1));
  MmioWrite32 (TCON3_BASE_ABS + TCON3_BASIC4_REG,
               ((VTotal * 2) << 16) | (VSyncW + VBack - 1));
  MmioWrite32 (TCON3_BASE_ABS + TCON3_BASIC5_REG,
               ((HSyncW - 1) << 16) | (VSyncW - 1));

  //
  // Sync polarities from the active mode (io0_inv = vsync, io1_inv =
  // hsync; both 1 = positive, which 1080p60 uses).  Then the fill
  // window (vactive_space = 0 => fill disabled).
  //
  MmioWrite32 (TCON3_BASE_ABS + TCON3_IO_POL_REG,
               (Mode->VSyncPositive ? IO0_INV : 0) |
               (Mode->HSyncPositive ? IO1_INV : 0) |
               IO2_INV);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_IO_TRI_REG, 0x0FFFFFFF);

  MmioWrite32 (TCON3_BASE_ABS + TCON3_FILL_START0_REG, 0x00466000);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_FILL_END0_REG,   0x00465000);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_FILL_DATA0_REG,  0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_FILL_CTL_REG,    0);

  DEBUG ((DEBUG_INFO,
          "A733Tcon: timing %ux%u@%uHz htotal=%u vtotal=%u\n",
          HAct, VAct,
          Mode->PixelClockHz / (VTotal * HTotal),
          HTotal, VTotal));
}

/**
  Select the DE as TCON3 input, program an identity CEU (colour engine
  unit, effectively a passthrough) and enable the TV output.
**/
STATIC
VOID
TconSourceAndEnable (VOID)
{
  //
  // CEU diagonal = 0x100 (identity for non-linear RGB).
  // All off-diagonal coefficients and offsets are zero.
  //
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_00_10_REG, 0x00000100);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_01_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_02_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_03_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_10_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_11_10_REG, 0x00000100);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_12_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_13_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_20_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_21_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_22_10_REG, 0x00000100);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_23_10_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_0L_11_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_1L_11_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_2L_11_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_3L_11_REG, 0);
  MmioWrite32 (TCON3_BASE_ABS + TCON3_CEU_CTL_REG,  0);

  //
  // TV control word: src_sel = 0 (DE input, the normal path; BSP
  // tcon_tv_src_select(LCD_SRC_DE)).  start delay 31, module enable
  // bit31.
  //
  MmioWrite32 (TCON3_BASE_ABS + TCON3_TV_CTL_REG,
               TV_EN | (0 << TV_SRC_SEL_SHIFT) | (31 << TV_START_DELAY_SHIFT));
}

/**
  Bring TCON3 up for 1080p60. Called after the DE3.0 and before the
  HDMI controller, mirroring the kernel order.
**/
VOID
A733TconInit (VOID)
{
  DEBUG ((DEBUG_INFO, "A733Tcon: TCON3 init (mode-driven timing)\n"));

  TconModuleInit ();
  TconTimingInit  ();
  TconSourceAndEnable ();

  DEBUG ((DEBUG_VERBOSE, "A733Tcon: TV_CTL=%08x BASIC3=%08x IO_POL=%08x\n",
          MmioRead32 (TCON3_BASE_ABS + TCON3_TV_CTL_REG),
          MmioRead32 (TCON3_BASE_ABS + TCON3_BASIC3_REG),
          MmioRead32 (TCON3_BASE_ABS + TCON3_IO_POL_REG)));
}