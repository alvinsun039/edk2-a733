/** @file
  DWC HDMI 2.0 controller + SNPS PHY bring-up.

  Implements the DW-HDMI register sequence the BSP kernel drives:
  frame composer timing, video path bypass config, minimal AVI/VSI
  infoframes, clock gating, the SNPS PHY I2CM programming for the 8bpc
  MPLL and drive values, and the top-PHY PLL that supplies the pixel
  clock (hdmi_clock_source = 0, as on the Cubie A7A).

  The frame-composer and PHY register images here are the measured
  1080p60 values: this stage is only entered for a mode that carries a
  PHY anchor (top-PHY PLL n/p0) in the A733_HDMI_MODE table, which
  A733HdmiDxeEntryPoint enforces.  Geometry upstream of this stage
  (DE / TCON / CCU 0x1684) is mode-driven and may describe a different
  mode; the anchor guard keeps this 1080p-only block consistent.

  The DW core registers are BYTE-addressed (writeb); read-modify-write
  must therefore always be 8-bit.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "A733HdmiDxe.h"

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define HDMI_BASE_ABS  HDMI_BASE

STATIC
VOID
HdmiWrite8 (
  IN UINT32  Offset,
  IN UINT8   Value
  )
{
  MmioWrite8 ((UINTN)(HDMI_BASE_ABS + Offset), Value);
}

STATIC
UINT8
HdmiRead8 (
  IN UINT32  Offset
  )
{
  return MmioRead8 ((UINTN)(HDMI_BASE_ABS + Offset));
}

STATIC
VOID
HdmiAndThenOr8 (
  IN UINT32  Offset,
  IN UINT8   And,
  IN UINT8   Or
  )
{
  HdmiWrite8 (Offset, (UINT8)((HdmiRead8 (Offset) & And) | Or));
}

/**
  Write one register of the SNPS PHY through the DW I2CM master.
**/
STATIC
VOID
SnpsPhyWrite (
  IN UINT8   Reg,
  IN UINT16  Value
  )
{
  UINTN  Tries;

  HdmiWrite8 (PHY_I2CM_ADDR_REG, Reg);
  HdmiWrite8 (PHY_I2CM_DATAO_1_REG, (UINT8)(Value >> 8));
  HdmiWrite8 (PHY_I2CM_DATAO_0_REG, (UINT8)(Value & 0xFF));
  HdmiWrite8 (PHY_I2CM_OPERATION_REG, 0x10);   // write

  for (Tries = 0; Tries < 100; Tries++) {
    if ((HdmiRead8 (IH_I2CMPHY_STAT0_REG) & BIT1) != 0) {
      HdmiWrite8 (IH_I2CMPHY_STAT0_REG, BIT1);  // clear done
      break;
    }
    MicroSecondDelay (10);
  }
  if (Tries >= 100) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: PHY I2CM write 0x%02x timed out\n", Reg));
  }
}

/**
  Frame composer: active area, blanking and sync timing fields derived
  from the active video mode; the fixed control fields (preambles,
  control-duration) keep the measured 1080p values.
**/
STATIC
VOID
FcTimingInit (VOID)
{
  const A733_HDMI_MODE  *Mode;
  UINT32                 HActive;
  UINT32                 HBlank;
  UINT32                 VActive;
  UINT32                 VBlank;
  UINT32                 HSyncDelay;
  UINT32                 HSyncWidth;
  UINT32                 VSyncDelay;
  UINT32                 VSyncWidth;
  UINT8                  InvidConf;

  Mode       = A733HdmiActiveMode ();
  HActive    = Mode->HActive;
  HBlank     = Mode->HTotal - Mode->HActive;
  VActive    = Mode->VActive;
  VBlank     = Mode->VTotal - Mode->VActive;
  HSyncDelay = Mode->HSyncStart - Mode->HActive;
  HSyncWidth = Mode->HSyncEnd - Mode->HSyncStart;
  VSyncDelay = Mode->VSyncStart - Mode->VActive;
  VSyncWidth = Mode->VSyncEnd - Mode->VSyncStart;

  //
  // INVIDCONF: DE active-high (bit3), sync polarities (bit4 vsync,
  // bit5 hsync), bit6 set (HDMI mode, same base as the measured 0x78).
  //
  InvidConf = 0x08 | BIT6;
  if (Mode->VSyncPositive) {
    InvidConf |= BIT4;
  }
  if (Mode->HSyncPositive) {
    InvidConf |= BIT5;
  }
  HdmiWrite8 (FC_INVIDCONF_REG, InvidConf);

  HdmiWrite8 (FC_INHACTIVE0_REG, (UINT8)(HActive & 0xFF));
  HdmiWrite8 (FC_INHACTIVE1_REG, (UINT8)(HActive >> 8));
  HdmiWrite8 (FC_INHBLANK0_REG,  (UINT8)(HBlank & 0xFF));
  HdmiWrite8 (FC_INHBLANK1_REG,  (UINT8)(HBlank >> 8));
  HdmiWrite8 (FC_INVACTIVE0_REG, (UINT8)(VActive & 0xFF));
  HdmiWrite8 (FC_INVACTIVE1_REG, (UINT8)(VActive >> 8));
  HdmiWrite8 (FC_INVBLANK_REG,   (UINT8)(VBlank & 0xFF));
  HdmiWrite8 (FC_HSYNCINDELAY0_REG, (UINT8)(HSyncDelay & 0xFF));
  HdmiWrite8 (FC_HSYNCINDELAY1_REG, (UINT8)(HSyncDelay >> 8));
  HdmiWrite8 (FC_HSYNCINWIDTH0_REG, (UINT8)(HSyncWidth & 0xFF));
  HdmiWrite8 (FC_HSYNCINWIDTH1_REG, (UINT8)(HSyncWidth >> 8));
  HdmiWrite8 (FC_VSYNCINDELAY_REG,  (UINT8)(VSyncDelay & 0xFF));
  HdmiWrite8 (FC_VSYNCINWIDTH_REG,  (UINT8)(VSyncWidth & 0xFF));
  HdmiWrite8 (FC_CTRLDUR_REG, 0x0C);
  HdmiWrite8 (FC_EXCTRLDUR_REG, 0x20);
  HdmiWrite8 (FC_EXCTRLSPAC_REG, 0x01);
  HdmiWrite8 (FC_CH0PREAM_REG, 0x0B);
  HdmiWrite8 (FC_CH1PREAM_REG, 0x16);
  HdmiWrite8 (FC_CH2PREAM_REG, 0x21);
  HdmiWrite8 (FC_PR_CTRL_REG, 0x10);   // incoming pixel repetition = 1
}

/**
  Video pipeline: RGB888 passthrough (no CSC, no pixel packing).
**/
STATIC
VOID
VideoPathInit (VOID)
{
  HdmiWrite8 (VP_PR_CD_REG, 0x00);     // PR=1, color depth 8bpc
  HdmiWrite8 (VP_STUFF_REG, 0x27);
  HdmiWrite8 (VP_REMAP_REG, 0x00);
  HdmiWrite8 (VP_CONF_REG, 0x47);      // bypass, output RGB
  HdmiWrite8 (TX_INVID0_REG, 0x01);    // RGB888 mapping, external DE
  HdmiWrite8 (TX_INSTUFFING_REG, 0x07);
  HdmiWrite8 (0x0202, 0x00);
  HdmiWrite8 (0x0203, 0x00);
  HdmiWrite8 (0x0204, 0x00);
  HdmiWrite8 (0x0205, 0x00);
  HdmiWrite8 (0x0206, 0x00);
  HdmiWrite8 (0x0207, 0x00);
}

/**
  Send the minimum VSI + AVI infoframes (VIC 16, RGB, BT.709, 16:9).
**/
STATIC
VOID
InfoframeInit (VOID)
{
  //
  // VSI: HDMI vendor OUI 00-0C-03.
  //
  HdmiWrite8 (FC_DATAUTO0_REG, 0x00);
  HdmiWrite8 (0x1029, 0x03);
  HdmiWrite8 (0x1030, 0x0C);
  HdmiWrite8 (0x1031, 0x00);
  HdmiWrite8 (0x1032, 0x00);
  HdmiWrite8 (0x1033, 0x00);
  HdmiWrite8 (0x1034, 0x00);
  HdmiWrite8 (0x1035, 0x00);
  //
  // VSI auto-write: only flip VSD_AUTO, preserve every other DATAUTO0
  // field (the 0x00 write above already cleared them at init).
  //
  HdmiAndThenOr8 (FC_DATAUTO0_REG, (UINT8)~BIT3, BIT3);
  HdmiWrite8 (FC_PACKET_TX_EN_REG, 0x10);

  //
  // AVI: RGB, active-format-present, BT.709, 16:9, full range, VIC from
  // the active mode.
  //
  HdmiWrite8 (FC_AVICONF0_REG, 0x40);
  HdmiWrite8 (FC_AVICONF1_REG, 0xA8);
  HdmiWrite8 (FC_AVICONF2_REG, 0x08);
  HdmiWrite8 (FC_AVIVID_REG,   (UINT8)A733HdmiActiveMode ()->Vic);

  //
  // GCP default phase 0, no GMD.
  //
  HdmiAndThenOr8 (FC_GCP_REG, (UINT8)~BIT2, 0);
  HdmiWrite8 (0x1101, 0x00);
  HdmiWrite8 (FC_DATAUTO1_REG, 0x01);
  HdmiWrite8 (FC_DATAUTO2_REG, 0x11);
}

/**
  Top-PHY PLL: the A7A supplies the HDMI pixel clock from this PLL
  (24MHz DCXO -> 148.5MHz). Mirrors phy_top.c top_phy_config() for
  hdmi_clock_source=0.

  Register fields (0x05600020, sun60i bit layout):
    [31] pll_en  [30] pll_ldo_en  [29] lock_enable  [27] pll_output_gate
    [16:22] pll_p0  [8:15] pll_n  [6:7] pll_unlock_model  [5] pll_lock_model

  The BSP sets the bits *step by step* (output gate is opened only AFTER
  the PLL reports locked). An all-at-once write of the final value
  pre-opens the gate and the PLL never locks — verified on hardware
  (readback 0xEB0F6200, lock bit never set). So we mirror the BSP order.
**/
STATIC
VOID
TopPhyPllInit (VOID)
{
  UINT32  PllReg;
  UINTN   Tries;

  //
  // PHY top power rail: reset/power-domain deasserts (0x05600000).
  //
  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0000, 0x00000017);

  //
  // 24MHz DCXO -> pixel clock: Fout = 24 * (pll_n+1) / (pll_p0+1).  The
  // factors come from the active mode (measured anchor: 98/15 -> 148.5).
  // Step 1: clock divider fields only — en/ldo/output stay off.
  //
  PllReg  = (A733HdmiActiveMode ()->PhyPllN << 8) |
            (A733HdmiActiveMode ()->PhyPllP0 << 16);
  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0020, PllReg);

  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0028, TOP_PHY_LDO_E1);
  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_002C, TOP_PHY_PAT0_E1);
  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0030, TOP_PHY_PAT1_E1);

  //
  // Step 2: power the PLL up (en + ldo).
  //
  PllReg |= BIT31 | BIT30;
  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0020, PllReg);

  //
  // Step 3: level-shifter gate + output clock select = PHY PLL.
  // REG_0024 bit layout: [7] pll_level_shifter_gate, [8] hdmi_outclk_sel,
  // [9] pll_unlock_irq_en. Only bit7 must be set; outclk_sel stays 0
  // (PHY PLL is the clock source, not the CCU).
  //
  MmioOr32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0024, BIT7);

  //
  // Step 4: arm the lock detector, then poll for lock.
  //
  PllReg |= BIT29;
  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0020, PllReg);

  for (Tries = 0; Tries < 200; Tries++) {
    if ((MmioRead32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0040) & BIT0) != 0) {
      break;
    }
    MicroSecondDelay (100);
  }
  if (Tries >= 200) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: top-PHY PLL did not lock\n"));
  }

  //
  // Step 5: open the output gate only after lock.
  //
  PllReg |= BIT27;
  MmioWrite32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0020, PllReg);

  DEBUG ((DEBUG_INFO, "A733Hdmi: top-PHY PLL %08x\n",
          MmioRead32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0020)));
}

/**
  SNPS PHY: reset, I2CM config, MPLL + drive values, power up.

  PHY_CONF0 (0x3000, byte) bit layout from the BSP dw_phy.h:
    [0] SELDIPIF  [1] SELDATAENPOL  [2] ENHPDRXSENSE
    [3] TXPWRON   [4] PDDQ          [5] SVSRET
    [6] SPARES_1  [7] SPARES_2
  dw_phy_set_power(): PDDQ = !state, TXPWRON = state.
**/
STATIC
VOID
SnpsPhyInit (VOID)
{
  UINTN  Tries;

  //
  // Assert PHY reset, power down (PDDQ=1, TXPWRON=0), toggle SVSRET.
  //
  HdmiWrite8 (MC_PHYRSTZ_REG, 0x01);
  HdmiAndThenOr8 (PHY_CONF0_REG, (UINT8)~(BIT4 | BIT3), BIT4); // PDDQ=1, TXPWRON=0
  HdmiAndThenOr8 (PHY_CONF0_REG, (UINT8)~BIT5, 0);
  MicroSecondDelay (5);
  HdmiAndThenOr8 (PHY_CONF0_REG, (UINT8)~BIT5, BIT5);   // SVSRET=1
  HdmiWrite8 (MC_PHYRSTZ_REG, 0x00);   // deassert

  HdmiWrite8 (JTAG_PHY_CONFIG_REG, 0x10);
  HdmiWrite8 (PHY_I2CM_SLAVE_REG, 0x69);

  //
  // MPLL: only the 148.5 MHz values are measured on this platform (the
  // TMDS clock actually comes from the external top-PHY PLL, so the DWC
  // serial PLL is on the bypass path); drive values follow the DTS
  // snps_phy bands by pixel clock (25-185.625 / 185.625-340 / 340-600
  // MHz).  Reference: CubieA7A DTS hdmi0 snps_phy property.
  //
  SnpsPhyWrite (SNPS_PHY_MPLL_0, SNPS_MPLL_E1);
  SnpsPhyWrite (SNPS_PHY_MPLL_1, SNPS_MPLL_E2);
  SnpsPhyWrite (SNPS_PHY_MPLL_2, SNPS_MPLL_E3);
  {
    const A733_HDMI_MODE  *Mode = A733HdmiActiveMode ();
    UINT32                 PixelKhz = (UINT32)(Mode->PixelClockHz / 1000);
    UINT32                 Drv0;
    UINT32                 Drv1;
    UINT32                 Drv2;

    if (PixelKhz <= 185625) {
      Drv0 = 0x0007; Drv1 = 0x8160; Drv2 = 0x8188;
    } else if (PixelKhz <= 340000) {
      Drv0 = 0x0004; Drv1 = 0x8040; Drv2 = 0x8E85;
    } else {
      Drv0 = 0x0000; Drv1 = 0x80c0; Drv2 = 0x82F6;
    }

    SnpsPhyWrite (SNPS_PHY_DRV_0, Drv0);
    SnpsPhyWrite (SNPS_PHY_DRV_1, Drv1);
    SnpsPhyWrite (SNPS_PHY_DRV_2, Drv2);
  }

  //
  // Power the TX up: PDDQ=0, TXPWRON=1. Then poll lock (STAT0 bit0).
  //
  HdmiAndThenOr8 (PHY_CONF0_REG, (UINT8)~BIT3, BIT3); // TXPWRON=1
  HdmiAndThenOr8 (PHY_CONF0_REG, (UINT8)~BIT4, 0);    // PDDQ=0

  for (Tries = 0; Tries < 100; Tries++) {
    if ((HdmiRead8 (PHY_STAT0_REG) & BIT0) != 0) {
      break;
    }
    MicroSecondDelay (100);
  }

  DEBUG ((DEBUG_INFO, "A733Hdmi: PHY_CONF0=%02x STAT0=%02x %a\n",
          HdmiRead8 (PHY_CONF0_REG),
          HdmiRead8 (PHY_STAT0_REG),
          (Tries < 100) ? "LOCKED" : "no-lock"));
}

/**
  Enable the DW-HDMI controller end to end.
**/
VOID
A733HdmiHwInit (VOID)
{
  //
  // Board-level HDMI resistor select (from the DTS hdmi_resistor_select).
  //
  MmioOr32  (SYSCFG_BASE + SYSCFG_HDMI_R1_REG, BIT12);
  MmioAnd32 (SYSCFG_BASE + SYSCFG_HDMI_R2_REG, ~0xFF000000);

  DEBUG ((DEBUG_INFO, "A733Hdmi: DWC controller start\n"));

  //
  // Mute video + mask all interrupts until configured.
  //
  HdmiWrite8 (FC_GCP_REG, 0x02);
  HdmiWrite8 (IH_MUTE_REG, 0x03);

  FcTimingInit ();
  VideoPathInit ();

  //
  // Infoframes, then unmute and clock everything (pixel/tmds/prep/csc),
  // leaving only the HDCP clock gated (never used).
  //
  InfoframeInit ();
  HdmiWrite8 (0x0180, 0xFF);
  HdmiWrite8 (0x0181, 0xFF);
  HdmiWrite8 (0x0182, 0xFF);
  HdmiWrite8 (0x0183, 0xFF);
  HdmiWrite8 (0x0185, 0xFF);
  HdmiWrite8 (0x0186, 0xFF);
  HdmiWrite8 (0x0188, 0xFF);
  HdmiWrite8 (0x0189, 0xFF);
  HdmiWrite8 (IH_MUTE_REG, 0x00);

  HdmiWrite8 (MC_SWRSTZREQ_REG, 0x07);  // assert core/tmds/fe resets
  HdmiWrite8 (MC_SWRSTZREQ_REG, 0x00);  // release

  //
  // The core reset above torn down the interrupt unit too - re-assert
  // the per-group mute masks so the IH_MUTE=0x00 at L412 stays the only
  // unmuting (dwc_hdmi_ih_mute order; see L400-407).
  //
  HdmiWrite8 (0x0180, 0xFF);
  HdmiWrite8 (0x0181, 0xFF);
  HdmiWrite8 (0x0182, 0xFF);
  HdmiWrite8 (0x0183, 0xFF);
  HdmiWrite8 (0x0185, 0xFF);
  HdmiWrite8 (0x0186, 0xFF);
  HdmiWrite8 (0x0188, 0xFF);
  HdmiWrite8 (0x0189, 0xFF);

  HdmiWrite8 (MC_FLOWCTRL_REG, 0x00);

  //
  // The MC_SWRSTZREQ assertion above reset the frame composer; the FC
  // timing written before it is gone. Re-issue the full video-path
  // configuration (the BSP does the same: dw_avp_config after the
  // module reset, and FC_INVIDCONF is re-written last to trigger the
  // frame-composer iteration that pushes the timing to the output).
  //
  FcTimingInit ();
  VideoPathInit ();

  //
  // Pixel clock enable (MC_CLKDIS low=enable). HDCP stays gated.
  //
  HdmiWrite8 (MC_CLKDIS_REG, 0x00);
  HdmiWrite8 (MC_CLKDIS_REG, 0x40);     // HDCP only

  FcTimingInit ();                       // re-assert after reset

  //
  // Clock the PHY and start TMDS.
  //
  TopPhyPllInit ();
  SnpsPhyInit ();

  //
  // Un-mute video, finally.
  //
  HdmiAndThenOr8 (FC_GCP_REG, (UINT8)~(BIT0 | BIT1), 0x01);
  DEBUG ((DEBUG_INFO, "A733Hdmi: output enabled (1080p60, RGB)\n"));
}

/**
  Register-level diagnosis of the whole HDMI pipeline.

  Reads back one key register from each stage (CCU -> DE -> TCON ->
  DWC -> PHY) so a missing link in the chain shows up as a wrong value
  on the UART instantly, instead of a silent black screen.
**/
VOID
A733HdmiDiag (VOID)
{
  DEBUG ((DEBUG_INFO, "--- A733Hdmi register diagnosis ---\n"));

  //
  // CCU: gates/resets for the HDMI + DE0 path. Expected: 168C=00070001,
  // 16CC=00010001, 1604=00010001, 0A04=00010001, 0A00 gate bit31.
  //
  DEBUG ((DEBUG_INFO, "CCU 168C=%08x 16CC=%08x 1604=%08x 0A00=%08x 0A04=%08x\n",
          MmioRead32 (CCU_BASE + CCU_HDMI_REG),
          MmioRead32 (CCU_BASE + CCU_DPSS_TOP1_REG),
          MmioRead32 (CCU_BASE + CCU_TCONTV0_REG),
          MmioRead32 (CCU_BASE + CCU_DE0_CLK_REG),
          MmioRead32 (CCU_BASE + CCU_BUS_DE0_REG)));

  //
  // tcon-top vo1: gate + clock select for TCON_TV0 (the A7A DTS wires
  // tcon3's top to vo1 @0x5510000).  Per the A733 user manual VO1 regs:
  //   0x0000 TV_CLK_EDP_I2S1_SRC bit1 TV0_EDP_HDMI_CK_SEL: 0 = TCON_TV0
  //          clock from HDMI PHY (required for HDMI), 1 = from EDP.
  //          bit3 TCON_TV0_HDMIPHY_CCU_CK_SEL: 0 = from top-PHY PLL.
  //          => setup = 0x00000000 (clock follows the HDMI PHY PLL).
  //   0x0020 VO1_MODULE_GATING: bit20 TV0_GATE + bit28 TV0_HDMI_GATE.
  //
  MmioWrite32 (TCON_TOP_VO1_BASE + TCON_TOP_TV_SETUP, 0x00000000);
  MmioWrite32 (TCON_TOP_VO1_BASE + TCON_TOP_CLK_GATE,
               TV0_CLK_GATE | TV0_HDMI_GATE);
  DEBUG ((DEBUG_INFO, "tcon-top vo1: setup=%08x gate=%08x\n",
          MmioRead32 (TCON_TOP_VO1_BASE + TCON_TOP_TV_SETUP),
          MmioRead32 (TCON_TOP_VO1_BASE + TCON_TOP_CLK_GATE)));

  //
  // DE3.0: output path routing. Expected DE2TCON disp0->tcon3 mux=4
  // (bits[1:0]), vch0 layer0 at 0x101000.
  //
  DEBUG ((DEBUG_INFO, "DE 8010=%08x 8028=%08x 8100=%08x 8104=%08x 8108=%08x 101000=%08x\n",
          MmioRead32 (DE_BASE + DE_TOP_DE2TCON_MUX_REG),
          MmioRead32 (DE_BASE + DE_TOP_VCH2CORE_MUX_REG),
          MmioRead32 (DE_BASE + DE_TOP_RTMX_GLB_CTL_REG),
          MmioRead32 (DE_BASE + DE_TOP_RTMX_GLB_STS_REG),
          MmioRead32 (DE_BASE + DE_TOP_RTMX_OUT_SIZE_REG),
          MmioRead32 (DE_BASE + DE_VCH0_OVL_BASE + OVL_L0_CTL_REG)));

  //
  // TCON3: TV ctl (src_sel+delay+en), basic3 timing, io_pol.
  //
  DEBUG ((DEBUG_INFO, "TCON3 090=%08x 094=%08x 0A0=%08x 088=%08x\n",
          MmioRead32 (TCON3_BASE + TCON3_TV_CTL_REG),
          MmioRead32 (TCON3_BASE + TCON3_BASIC1_REG),
          MmioRead32 (TCON3_BASE + TCON3_BASIC3_REG),
          MmioRead32 (TCON3_BASE + TCON3_IO_POL_REG)));

  //
  // TCON3 debug: fifo_under_flow(bit30) set means the DE never delivered
  // pixels to the TCON; current_line(bits[11:0]) advancing means the TCON
  // scanout is live.  Read three times to see motion.
  //
  DEBUG ((DEBUG_INFO, "TCON3 DBG=%08x DBG2=%08x DBG3=%08x (gctl=%08x)\n",
          MmioRead32 (TCON3_BASE + TCON3_DEBUGC_REG),
          MmioRead32 (TCON3_BASE + TCON3_DEBUGC_REG),
          MmioRead32 (TCON3_BASE + TCON3_DEBUGC_REG),
          MmioRead32 (TCON3_BASE + TCON3_GCTL_REG)));

  //
  // DWC: MC clk/flow control, FC config, TX and PHY status.
  //
  DEBUG ((DEBUG_INFO, "DWC CLKDIS=%02x FLOWCTRL=%02x TXVID0=%02x\n",
          HdmiRead8 (MC_CLKDIS_REG),
          HdmiRead8 (MC_FLOWCTRL_REG),
          HdmiRead8 (TX_INVID0_REG)));
  DEBUG ((DEBUG_INFO, "DWC LOCKON=%02x LOCKON2=%02x (PCLK b6 TCLK b5)\n",
          HdmiRead8 (0x4006),
          HdmiRead8 (0x4009)));
  DEBUG ((DEBUG_INFO, "CCU PLL_VIDEO0(0120)=%08x HDMI_TV_CFG(1684)=%08x\n",
          MmioRead32 (CCU_BASE + 0x0120),
          MmioRead32 (CCU_BASE + 0x1684)));
  DEBUG ((DEBUG_INFO, "DWC INVIDCONF=%02x HACT0=%02x VACT0=%02x HSYNCINW=%02x\n",
          HdmiRead8 (FC_INVIDCONF_REG),
          HdmiRead8 (FC_INHACTIVE0_REG),
          HdmiRead8 (FC_INVACTIVE0_REG),
          HdmiRead8 (FC_HSYNCINWIDTH0_REG)));

  //
  // Infoframes and output.
  //
  DEBUG ((DEBUG_INFO, "DWC AVI1=%02x TXEN=%03x GCP=%02x\n",
          HdmiRead8 (FC_AVICONF1_REG),
          HdmiRead8 (FC_PACKET_TX_EN_REG),
          HdmiRead8 (FC_GCP_REG)));

  //
  // Top-PHY PLL + SNPS PHY.
  //
  DEBUG ((DEBUG_INFO, "top-PHY 020=%08x 024=%08x 040=%08x\n",
          MmioRead32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0020),
          MmioRead32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0024),
          MmioRead32 (HDMI_TOP_PHY_BASE + TOP_PHY_REG_0040)));

  DEBUG ((DEBUG_INFO, "SNPS CONF0=%02x STAT0=%02x\n",
          HdmiRead8 (PHY_CONF0_REG),
          HdmiRead8 (PHY_STAT0_REG)));

  //
  // Frame-composer activity: if the FC is generating frames, at least
  // one of these interrupts will have latched (NULL 0x01 / NVBI 0x08
  // / HBR 0x20 etc.). All-zero means no video is being pushed in.
  //
  HdmiWrite8 (0x0100, 0x7F);       // clear FC_STAT0 latches (W1C)
  HdmiWrite8 (0x0101, 0x7F);       // clear FC_STAT1 latches (W1C)
  MicroSecondDelay (1000);
  DEBUG ((DEBUG_INFO, "DWC FC_STAT0=%02x FC_STAT1=%02x (frame EC4504=%02x)\n",
          HdmiRead8 (0x0100),
          HdmiRead8 (0x0101),
          HdmiRead8 (0x4504)));

  //
  // TCON3 output tristate: non-zero there floats DE/VS/HS/data, so the
  // DWC never sees a frame boundary (FC_STAT0 stays 0) -> monitor locks
  // blanking only ("signal, black").  Any set bit = output stage dead.
  //
  DEBUG ((DEBUG_INFO, "T3 08C tri=%08x 330=%08x 334=%08x 338=%08x 33C=%08x\n",
          MmioRead32 (TCON3_BASE + TCON3_IO_TRI_REG),
          MmioRead32 (TCON3_BASE + 0x330),
          MmioRead32 (TCON3_BASE + 0x334),
          MmioRead32 (TCON3_BASE + 0x338),
          MmioRead32 (TCON3_BASE + 0x33C)));

  //
  // (bring-up FC frame probe removed 2026-09-08: the IH_MUTE unmask +
  // 2x200ms sampling only proved the FC clock domain produces frames,
  // which the working panel already proves; HdmiDiag can re-check it on
  // demand.)
  //

  //
  // TCON input select: DE (src_ctl=0, TV_CTL src_sel=0).  The bring-up-era
  // colourbar demo (src_sel=1 + 2s delay + FC frame probe) is removed now
  // that the DE->TCON->DWC chain is proven on the panel - boot goes
  // straight to the DE picture.  The manual colourbar still lives in the
  // HdmiDiag Shell application for whenever the pixel path is in doubt.
  //
  MmioWrite32 (TCON3_BASE + TCON3_SRC_CTL_REG, 0);
  MmioWrite32 (TCON3_BASE + TCON3_TV_CTL_REG,
               TV_EN | (0 << TV_SRC_SEL_SHIFT) | (31 << TV_START_DELAY_SHIFT));

  //
  // DE352 is AHB_MODE: registers are written live over the AHB bus, no
  // shadow/DBUFF/RCQ commit is involved.  The 0x100008 "DBUFF" pulse is
  // only meaningful when a prior owner left the DE in double-buffer mode
  // (e.g. the OPi GOP repointing a BSP-configured mixer).  On a from-cold
  // AHB bring-up it is not part of the commit path.
  //
  DEBUG ((DEBUG_INFO, "TCON DE input restored (src_ctl=%08x)\n",
          MmioRead32 (TCON3_BASE + TCON3_SRC_CTL_REG)));
  DEBUG ((DEBUG_INFO, "DE->TCON: TCON DBG=%08x (bit30=underflow) DE RTMX_STS=%08x\n",
          MmioRead32 (TCON3_BASE + TCON3_DEBUGC_REG),
          MmioRead32 (DE_BASE + DE_TOP_RTMX_GLB_STS_REG)));

  //
  // RCQ liveness re-check: the RCQ commit ran earlier in
  // A733DeRcqCommit; read layer0 again now to see if it held.  A value
  // still 0xFF008013 means the RCQ shadow was applied and the problem
  // is the mixer output path; 0 means the shadow never landed.
  //
  DEBUG ((DEBUG_INFO, "RCQ recheck: layer0=%08x fcolor=%08x\n",
          MmioRead32 (DE_BASE + DE_VCH0_OVL_BASE + OVL_L0_CTL_REG),
          MmioRead32 (DE_BASE + DE_VCH0_OVL_BASE + OVL_FCOLOR0_REG)));
  DEBUG ((DEBUG_INFO, "RCQ recheck: bld_en=%08x bld_fcolor=%08x bld_outctl=%08x\n",
          MmioRead32 (DE_BASE + DE_DISP0_BLD_BASE + BLD_EN_REG),
          MmioRead32 (DE_BASE + DE_DISP0_BLD_BASE + BLD_PIPE0_FCOLOR_REG),
          MmioRead32 (DE_BASE + DE_DISP0_BLD_BASE + BLD_OUT_CTL_REG)));

  DEBUG ((DEBUG_INFO, "--- end register diagnosis ---\n"));
}
