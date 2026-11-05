/** @file
  Display clock tree bring-up for the Allwinner A733 (sun60iw2).

  Direct CCU programming: there is no clock framework in EDK2, so every
  gate / reset / PLL involved in the HDMI path is handled here, in the
  same low-level style as A733UsbHostDxe does for USB.

  Reset semantics (verified against the BSP ccu_reset.c): writing a
  reset bit = RELEASE (deassert). The same convention appears in the
  live register snapshot research/sun60iw2-ccu-phy-usb-snapshot.txt
  (0x168C reads 0x00070001 on a running system).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "A733HdmiDxe.h"

#include <Library/A733CcuLib.h>
#include <Library/A733DisplayClockLib.h>
#include <Library/A733PowerLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

/**
  Bring up every clock the HDMI path needs.

  Pipeline: DE3.0 -> TCON3 (tcon-tv) -> DWC-HDMI + SNPS PHY, with the
  TCON-TV/HDMI crossing clocked from vo1 (tcon-top1) and the pixel clock
  sourced from the top-PHY PLL (not CCU), matching the A7A DTS
  hdmi_clock_source = 0.

  @retval EFI_SUCCESS   All gates/resets/power domains are up.
  @retval other         (never fails hard: the debug console is already
                         up by the time this runs, and a hung display
                         must not brick the boot).
**/
EFI_STATUS
A733DisplayClockInit (VOID)
{
  EFI_STATUS  St;

  DEBUG ((DEBUG_INFO, "A733Clk: display clock bring-up start\n"));

  //
  // Keyed AHB / MBUS master gates.  These are key-protected and the A7A
  // boot chain already left them at their keyed base state, so we only
  // add the display-engine gate bits (RMW in A733DisplayClockLib) rather than
  // writing a full golden value over the base state.
  //
  A733DisplayClockSetAhbMbusGates (
    A733_AHB_GATE_DE_SYS,
    A733_MBUS0_GATE_DESYS,
    0x00000805
    );
  DEBUG ((
    DEBUG_VERBOSE,
    "A733Clk: keyed gates AHB=0x%08x MBUS0=0x%08x MBUS1=0x%08x\n",
    A733CcuRead (A733_CCU_AHB_MAT_GATE_REG),
    A733CcuRead (A733_CCU_MBUS0_MAT_GATE_REG),
    A733CcuRead (A733_CCU_MBUS1_MAT_GATE_REG)
    ));

  //
  // Power domains: DE_SYS runs the display engine, VO1 runs
  // hdmi0 + tcon3 + vo1. VO (id 9) is the LCD side, not needed here.
  //
  A733PowerDomainOn (A733_POWER_DE_SYS);
  A733PowerDomainOn (A733_POWER_VO1);

  //
  // DE clock (PLL_DE -> CLK_DE0 600MHz -> bus gate/reset, DE_SYS reset).
  //
  A733DisplayClockInitDe0 ();
  DEBUG ((DEBUG_INFO, "A733Clk: DE0 clock up (600MHz), PLL=%08x CLK=%08x\n",
          A733CcuRead (CCU_PLL_DE_REG),
          A733CcuRead (CCU_DE0_CLK_REG)));

  //
  // VO1 / TCON side (dpss_top1, video_out1, tcontv0 bus) and the HDMI
  // module gate + resets + SFR clock.  In A733DisplayClockLib::A733DisplayClockInitHdmi.
  //
  A733DisplayClockInitHdmi ();
  DEBUG ((DEBUG_VERBOSE, "A733Clk: vo1/dpss/tcon gates: top=%08x vo=%08x tcon=%08x\n",
          A733CcuRead (CCU_DPSS_TOP1_REG),
          A733CcuRead (CCU_VIDEO_OUT1_REG),
          A733CcuRead (CCU_TCONTV0_REG)));

  //
  // CLK_HDMI (HDMI_TV_CFG 0x1684): the DWC core clock at the pixel rate,
  // divided down from pll-video0-4x.  The source PLL is preset by the
  // boot chain (CCU PLL_VIDEO0, 0x0120); A733DisplayClockSetHdmiTvRate derives
  // the N field from (source, target) so the value tracks the chosen VIC
  // instead of being a hard-coded 148.5 MHz register image.
  //
  St = A733DisplayClockSetHdmiTvRate (
         A733_PLL_VIDEO0_4X_HZ,               // 594 MHz, U-Boot preset (mux 0)
         A733HdmiActiveMode ()->PixelClockHz
         );
  if (EFI_ERROR (St)) {
    DEBUG ((DEBUG_ERROR,
            "A733Clk: hdmi-tv rate programming failed (%r)\n", St));
  }
  DEBUG ((DEBUG_VERBOSE, "A733Clk: hdmi gate=%08x tvcfg=%08x sfr=%08x\n",
          A733CcuRead (CCU_HDMI_REG),
          A733CcuRead (CCU_HDMI_TV_CFG_REG),
          A733CcuRead (CCU_HDMI_SFR_REG)));

  //
  // tcon-top forwarding: pixel clock from the top-PHY PLL (not the CCU),
  // and the TCON-TV / HDMI internal path gates on.  The BSP writes the
  // TVE clock-source select to tcon_top[0] (vo0) while the HDMI clk gate
  // path uses tcon_top[sel>2]=vo1; write BOTH instances so the TVE dclk
  // source is set regardless of which top the silicon actually routes.
  //
  //
  // Gate/source-select bits are additive enables; OR them in so a
  // future owner of the other tcon-top fields (EDP path on vo0,
  // etc.) is not clobbered by a whole-word write.
  //
  MmioOr32 (TCON_TOP_VO0_BASE + TCON_TOP_TV_SETUP, TV0_TV_CLK_SRC_TVE);
  MmioOr32 (TCON_TOP_VO0_BASE + TCON_TOP_CLK_GATE,
           TV0_CLK_GATE | TV0_HDMI_GATE);
  MmioOr32 (TCON_TOP_VO1_BASE + TCON_TOP_TV_SETUP, TV0_TV_CLK_SRC_TVE);
  MmioOr32 (TCON_TOP_VO1_BASE + TCON_TOP_CLK_GATE,
           TV0_CLK_GATE | TV0_HDMI_GATE);
  DEBUG ((DEBUG_VERBOSE, "A733Clk: tcon-top vo0=%08x vo1=%08x\n",
          MmioRead32 (TCON_TOP_VO0_BASE + TCON_TOP_CLK_GATE),
          MmioRead32 (TCON_TOP_VO1_BASE + TCON_TOP_CLK_GATE)));

  DEBUG ((DEBUG_INFO, "A733Clk: display clocks up\n"));
  return EFI_SUCCESS;
}