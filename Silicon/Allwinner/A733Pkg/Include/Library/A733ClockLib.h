/** @file
  A733 Clock / Reset Controller library.

  Converges the scattered per-driver CCU register pokes (module gates,
  resets, PLLs and the keyed AHB/MBUS master-gate registers) into this
  SoC library so every register bit definition and "Linux golden" value
  lives in one place instead of being re-derived by each driver.

  Register offsets are relative to the main CCU (SUNXI_CCU_BASE =
  0x02002000) unless a function says otherwise.

  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_CLOCK_LIB_H_
#define A733_CLOCK_LIB_H_

#include <Uefi/UefiBaseType.h>

//
// SoC CCU bases (kept here so callers do not each re-derive them).
//
#define A733_CCU_BASE    0x02002000UL
#define A733_R_CCU_BASE  0x07010000UL

//
// Keyed AHB / MBUS master-gate registers (0x05C0 / 0x05E0 / 0x05E4) and the
// SoC-fixed master gate bits drivers pass to SunxiClockSetAhbMbusGates.
// Bit numbers come from the BSP ccu-sun60iw2 master-gate definitions.
//
#define A733_CCU_AHB_MAT_GATE_REG   0x05C0
#define A733_CCU_MBUS0_MAT_GATE_REG 0x05E0
#define A733_CCU_MBUS1_MAT_GATE_REG 0x05E4

#define A733_AHB_GATE_DE_SYS    BIT5     // de-ahb-gate
#define A733_AHB_GATE_SERDES    BIT8     // serdes-ahb-gate
#define A733_AHB_GATE_USB_SYS   BIT9     // usb-sys-ahb-gate
#define A733_MBUS0_GATE_DESYS   BIT11    // desys-mbus-gate (display engine)
#define A733_MBUS0_GATE_SERDES  BIT28    // serdes-mbus-gate

/**
  Raw 32-bit access to a CCU register (read / write / set / clear bits).

  @param RegOffset  Offset from A733_CCU_BASE.

  @return  SunxiCcuRead returns the register value; the writers return the
           value written for convenience.
**/
UINT32
EFIAPI
SunxiCcuRead (
  IN UINT32  RegOffset
  );

VOID
EFIAPI
SunxiCcuWrite (
  IN UINT32  RegOffset,
  IN UINT32  Value
  );

VOID
EFIAPI
SunxiCcuSetBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  );

VOID
EFIAPI
SunxiCcuClearBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  );

//
// Keyed AHB / MBUS master-gate registers (0x05C0 / 0x05E0 / 0x05E4).
//
// These registers only accept a write when the accompanying "key" bits are
// already present in the register value.  A previous boot stage (U-Boot on
// this board) establishes that keyed base state; drivers then only turn on
// the master-gate bits they own.  The register has no separate "add one bit"
// semantics - the write is the whole register - so SunxiClockSetAhbMbusGates
// does a read-modify-write that preserves every existing gate, sets only the
// caller's bits, and clears the low-key stray bits that an earlier driver
// proved switch on six extra bus masters (the ethernet-after-EDK2 bug).
//
VOID
EFIAPI
SunxiClockSetAhbMbusGates (
  IN UINT32  AhbGateBits,    // bits to set in 0x05C0
  IN UINT32  Mbus0GateBits,  // bits to set in 0x05E0
  IN UINT32  Mbus1GateBits   // bits to set in 0x05E4
  );

//
// Per-module CCU bring-up sequences, each replicating exactly what the
// corresponding driver used to poke by hand (values unchanged, so a driver
// can move to the library without changing behaviour).
//

/**
  CCU for DE0: PLL_DE -> 600 MHz CLK_DE0, bus gate/reset and DE_SYS reset.
**/
VOID
EFIAPI
SunxiClockInitDe0 (
  VOID
  );

/**
  CCU for the HDMI block: CLK_HDMI gate + MAIN/SUB/HDCP resets, SFR clock,
  vo1/dpss/tcon-tv gates and the tcon-top vo1 forwarding gates.
**/
VOID
EFIAPI
SunxiClockInitHdmi (
  VOID
  );

/**
  CCU for GMAC0: module gate/reset, PHY reference clock (25 MHz) and the
  PTP reference clock gate.
**/
VOID
EFIAPI
SunxiClockInitGmac0 (
  VOID
  );

/**
  CCU for SPI0: bus gate/reset and the SPI0 clock.
**/
VOID
EFIAPI
SunxiClockInitSpi0 (
  VOID
  );

/**
  CCU for the USB (EHCI0/1 + xHCI2/DWC3) subsystem: keyed AHB gate,
  msi_lite2 / res_dcap gates, per-controller module gates + resets and
  the serdes clock feeding the USB3 combo PHY.
**/
VOID
EFIAPI
SunxiClockInitUsb (
  VOID
  );

/**
  Assert or release the PHY reset of one USB controller.

  @param UsbIndex  0 = usb0 (EHCI0), 1 = usb1 (EHCI1).
  @param Assert    TRUE holds the PHY in reset, FALSE releases it.
**/
VOID
EFIAPI
SunxiClockSetUsbPhyReset (
  IN UINTN    UsbIndex,
  IN BOOLEAN  Assert
  );

/**
  Program the HDMI TV output clock (0x1684) for a target rate.

  @param SourceHz  Parent clock rate (e.g. pll-video0-4x = 594 MHz).
  @param TargetHz  Desired output rate (e.g. 148.5 MHz pixel clock).

  @retval EFI_SUCCESS       Register programmed.
  @retval EFI_UNSUPPORTED   SourceHz is not an integer multiple of
                            TargetHz, or the divider exceeds the N field.
**/
EFI_STATUS
EFIAPI
SunxiClockSetHdmiTvRate (
  IN UINT32  SourceHz,
  IN UINT32  TargetHz
  );

#endif // A733_CLOCK_LIB_H_
