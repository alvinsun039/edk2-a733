/** @file
  Display clock / reset sequences for the Allwinner A733 (sun60iw2).

  Every register offset and value here was taken from the working A733
  display bring-up (edk2-a733, SunxiDisplayClock.c / A733ClockLib.c) so the
  behaviour is unchanged; what moved is only where the code lives.

  Reset semantics (verified against the BSP ccu_reset.c): writing a reset
  bit = RELEASE (deassert).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/A733DisplayClockLib.h>

#include <Library/A733CcuLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

//
// CCU register offsets (relative to the CCU base in A733CcuLib).
//
// Keyed AHB / MBUS master gates.  The keyed base state is established by the
// previous boot stage; drivers turn on only their own gate bits, never by
// OR-ing a bare key on (the low key bits are real gate bits and would switch
// on six extra bus masters).
//
#define CCU_AHB_GATE_REG      0x05C0
#define CCU_MBUS_GATE0_REG    0x05E0
#define CCU_MBUS_GATE1_REG    0x05E4

//
// DE0 / display engine.
//
#define CCU_PLL_DE_REG        0x02E0
#define PLL_DE_LOCK           BIT28
#define CCU_DE0_CLK_REG       0x0A00
// gate(31) + mux=0 (pll-de-3x) + M field[4:0]=0 (no divider).  The DE must
// run >= ~2x dclk (>= 297 MHz for 1080p60); a stale M=1 divided the 3x tap by
// two (225 MHz), which is below the scanout requirement.
#define CCU_DE0_CLK_E1        0x80000000
#define CCU_BUS_DE0_REG       0x0A04
#define CCU_BUS_DE0_E1        0x00010001   // gate bit0 + reset bit16 released
#define CCU_DE_SYS_RST_REG    0x0A74
#define RST_BUS_DE_SYS        BIT16

//
// HDMI block and its vo1 crossing.
//
#define CCU_DPSS_TOP1_REG     0x16CC
#define CCU_DPSS_TOP1_E1      0x00010001
#define CCU_VIDEO_OUT1_REG    0x16EC
#define CCU_VIDEO_OUT1_E1     0x00010001
#define CCU_TCONTV0_REG       0x1604
#define CCU_TCONTV0_E1        0x00010001
#define CCU_HDMI_REG          0x168C
#define CCU_HDMI_E1           0x00070001   // gate + MAIN/SUB/HDCP resets released
#define CCU_HDMI_SFR_REG      0x1690
#define CCU_HDMI_SFR_E1       0x80000000   // mux sys24M + gate

//
// HDMI TV output clock (CLK_HDMI_TV, 0x1684):
//   M divider shift 0 width 5, N divider shift 8 width 5, mux shift 24
//   width 3 (0 = pll-video0-4x), gate BIT31.  Divider fields are one-based
//   (field = divider - 1); rate = parent / (M+1) / (N+1).  N holds the whole
//   divider here (M = 0), so field N = parent/target - 1.
//
#define CCU_HDMI_TV_CFG_REG  0x1684
#define HDMI_TV_N_SHIFT      8
#define HDMI_TV_N_WIDTH      5
#define HDMI_TV_MUX_SHIFT    24
#define HDMI_TV_MUX_PLL_VIDEO0_4X  0
#define HDMI_TV_GATE         BIT31
#define HDMI_TV_MAX_DIV      (1U << HDMI_TV_N_WIDTH)   // field max 31 -> div 32

//
// Stray bits of the keyed AHB/MBUS master-gate registers.  A plain
// read-modify-write that ORs the key constant on lights these up (they are
// the low bits of the key) and switches on six bus masters the vendor leaves
// gated - the ethernet-after-EDK2 bug.  When a keyed base state already
// exists, a driver only needs to OR its own gate bits and then clear these
// strays.
//
#define CCU_AHB_GATE_STRAY   0x00000007U
#define CCU_MBUS0_GATE_STRAY 0x01005000U

/**
  Set master-gate bits on the keyed AHB / MBUS matrices.

  Preserves every existing gate, sets only the caller's bits and clears the
  stray bits the key's low field would otherwise light up.  Requires the
  keyed base state to already be present in the register (a previous boot
  stage, or another driver, established it) - this function does not add the
  key itself.

  @param[in] AhbGateBits    Bits to set in the AHB gate register.
  @param[in] Mbus0GateBits  Bits to set in MBUS gate 0.
  @param[in] Mbus1GateBits  Bits to set in MBUS gate 1; 0 leaves it alone.
**/
VOID
EFIAPI
A733DisplayClockSetAhbMbusGates (
  IN UINT32  AhbGateBits,
  IN UINT32  Mbus0GateBits,
  IN UINT32  Mbus1GateBits
  )
{
  UINT32  V;

  V  = A733CcuRead (CCU_AHB_GATE_REG);
  V |= AhbGateBits;
  V &= ~CCU_AHB_GATE_STRAY;
  A733CcuWrite (CCU_AHB_GATE_REG, V);

  V  = A733CcuRead (CCU_MBUS_GATE0_REG);
  V |= Mbus0GateBits;
  V &= ~CCU_MBUS0_GATE_STRAY;
  A733CcuWrite (CCU_MBUS_GATE0_REG, V);

  if (Mbus1GateBits != 0) {
    A733CcuSetBits (CCU_MBUS_GATE1_REG, Mbus1GateBits);
  }
}

/**
  CCU for DE0: PLL_DE -> CLK_DE0 (600 MHz), bus gate/reset, DE_SYS reset.
**/
VOID
EFIAPI
A733DisplayClockInitDe0 (
  VOID
  )
{
  UINTN  Tries;

  //
  // PLL_DE: bring LDO + enable up first, wait for lock, then open the
  // pll-de-3x output.  An all-at-once write of the final value does not lock.
  //
  A733CcuWrite (CCU_PLL_DE_REG, 0xEC000000);
  MicroSecondDelay (50);
  A733CcuWrite (CCU_PLL_DE_REG, 0xEC134A00);   // N=75 -> 1800MHz, 3x div 3

  for (Tries = 0; Tries < 100; Tries++) {
    if ((A733CcuRead (CCU_PLL_DE_REG) & PLL_DE_LOCK) != 0) {
      break;
    }
    MicroSecondDelay (100);
  }
  if (Tries >= 100) {
    DEBUG ((DEBUG_ERROR, "A733DisplayClock: PLL_DE did not lock\n"));
  }

  A733CcuWrite (CCU_DE0_CLK_REG, CCU_DE0_CLK_E1);
  A733CcuSetBits (CCU_BUS_DE0_REG, CCU_BUS_DE0_E1);
  A733CcuSetBits (CCU_DE_SYS_RST_REG, RST_BUS_DE_SYS);
}

/**
  CCU for the HDMI block and its vo1 crossing.  DE/VIDEO_OUT master-gate bits
  are assumed already set by whoever owns the DE0 clock path.
**/
VOID
EFIAPI
A733DisplayClockInitHdmi (
  VOID
  )
{
  A733CcuSetBits (CCU_DPSS_TOP1_REG, CCU_DPSS_TOP1_E1);
  A733CcuSetBits (CCU_VIDEO_OUT1_REG, CCU_VIDEO_OUT1_E1);
  A733CcuSetBits (CCU_TCONTV0_REG, CCU_TCONTV0_E1);
  A733CcuSetBits (CCU_HDMI_REG, CCU_HDMI_E1);
  A733CcuWrite (CCU_HDMI_SFR_REG, CCU_HDMI_SFR_E1);
}

/**
  Program the HDMI TV output clock (0x1684) for a target rate.

  The parent is selected by the caller via SourceHz (mux 0 = pll-video0-4x on
  this SoC); the whole divider goes in N with M left at 1 (field 0).

  @param[in] SourceHz  Parent clock rate (e.g. pll-video0-4x = 594 MHz).
  @param[in] TargetHz  Desired output rate (e.g. 148.5 MHz pixel clock).

  @retval EFI_SUCCESS      Register programmed.
  @retval EFI_UNSUPPORTED  SourceHz is not an integer multiple of TargetHz,
                           or the divider does not fit the N field.
**/
EFI_STATUS
EFIAPI
A733DisplayClockSetHdmiTvRate (
  IN UINT32  SourceHz,
  IN UINT32  TargetHz
  )
{
  UINT32  Div;
  UINT32  Reg;

  if ((SourceHz == 0) || (TargetHz == 0) || (SourceHz % TargetHz != 0)) {
    DEBUG ((DEBUG_ERROR,
            "A733DisplayClock: hdmi-tv rate %u from %u not an integer divider\n",
            TargetHz, SourceHz));
    return EFI_UNSUPPORTED;
  }

  Div = SourceHz / TargetHz;
  if (Div > HDMI_TV_MAX_DIV) {
    DEBUG ((DEBUG_ERROR,
            "A733DisplayClock: hdmi-tv divider %u exceeds %u-bit N field\n",
            Div, (UINT32)HDMI_TV_N_WIDTH));
    return EFI_UNSUPPORTED;
  }

  Reg = HDMI_TV_GATE
      | ((UINT32)HDMI_TV_MUX_PLL_VIDEO0_4X << HDMI_TV_MUX_SHIFT)
      | ((Div - 1) << HDMI_TV_N_SHIFT);
  A733CcuWrite (CCU_HDMI_TV_CFG_REG, Reg);

  return EFI_SUCCESS;
}
