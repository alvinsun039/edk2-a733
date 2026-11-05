/** @file
  Display clock / reset sequences for the Allwinner A733 (sun60iw2).

  The HDMI pipeline needs a handful of CCU operations that the generic module
  model in A733CcuLib does not describe: the keyed AHB/MBUS master gates, the
  DE0 PLL and its bus gates, the vo1/dpss/tcon-tv crossing and the HDMI TV
  output divider.  They live here - one function per pipeline stage that the
  display clock bring-up performs - instead of being poked from the driver.

  Register offsets are relative to the CCU base; raw register access goes
  through A733CcuLib (A733CcuRead / A733CcuWrite).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_DISPLAY_CLOCK_LIB_H_
#define A733_DISPLAY_CLOCK_LIB_H_

#include <Uefi/UefiBaseType.h>

//
// Keyed AHB / MBUS master-gate registers (relative to the CCU base) and the
// display-engine bits inside them.  Bit numbers come from the BSP
// ccu-sun60iw2 master-gate definitions.
//
#define A733_CCU_AHB_MAT_GATE_REG    0x05C0U
#define A733_CCU_MBUS0_MAT_GATE_REG  0x05E0U
#define A733_CCU_MBUS1_MAT_GATE_REG  0x05E4U

#define A733_AHB_GATE_DE_SYS   BIT5    // de-ahb-gate
#define A733_MBUS0_GATE_DESYS  BIT11   // desys-mbus-gate (display engine)

/**
  Set master-gate bits on the keyed AHB / MBUS matrices.

  The keyed base state is established by a previous boot stage; this only
  adds the caller's bits and clears the key's stray low bits (see the
  implementation for why the stray bits matter).

  @param[in] AhbGateBits    Bits to set in the AHB gate register (0x05C0).
  @param[in] Mbus0GateBits  Bits to set in MBUS gate 0 (0x05E0).
  @param[in] Mbus1GateBits  Bits to set in MBUS gate 1 (0x05E4); 0 = leave.
**/
VOID
EFIAPI
A733DisplayClockSetAhbMbusGates (
  IN UINT32  AhbGateBits,
  IN UINT32  Mbus0GateBits,
  IN UINT32  Mbus1GateBits
  );

/**
  Clock the display engine: PLL_DE -> 600 MHz CLK_DE0, bus gate/reset and
  the DE_SYS reset release.
**/
VOID
EFIAPI
A733DisplayClockInitDe0 (
  VOID
  );

/**
  Clock the HDMI output: vo1 / dpss_top1 / tcon-tv gates, the HDMI module
  gate and its resets, and the HDMI SFR clock.
**/
VOID
EFIAPI
A733DisplayClockInitHdmi (
  VOID
  );

/**
  Program the HDMI TV output clock (0x1684) for a target rate.

  @param[in] SourceHz  Parent clock rate (pll-video0-4x = 594 MHz).
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
  );

#endif // A733_DISPLAY_CLOCK_LIB_H_
