/** @file
  A733 Clock / Reset Controller library implementation.

  Every register offset and value here was lifted verbatim from the
  per-driver CCU pokes it replaces (SunxiGmacDxe, SunxiSpiDxe,
  A7AFvbDxe, SunxiDisplayClock.c) so a driver can move to this
  library without changing behaviour.  The keyed AHB/MBUS master-gate
  registers (0x05C0/0x05E0/0x05E4) are only ever modified to set a
  caller's own gate bits (SunxiClockSetAhbMbusGates) - never by OR-ing a
  bare key on, which is what accidentally switched on six bus masters.

  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/A733ClockLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

//
// CCU register offsets (relative to A733_CCU_BASE = 0x02002000).
//
// Keyed AHB / MBUS master gates.  The keyed base state is established by
// the previous boot stage; drivers turn on only their own gate bits via
// SunxiClockSetAhbMbusGates(), never by OR-ing a bare key on (the low key
// bits are real gate bits and would switch on six extra bus masters).
//
#define CCU_AHB_GATE_REG      0x05C0
#define CCU_MBUS_GATE0_REG    0x05E0
#define CCU_MBUS_GATE1_REG    0x05E4

//
// DE0 / display engine (SunxiDisplayClock.c).
//
#define CCU_PLL_DE_REG        0x02E0
#define PLL_DE_LOCK           BIT28
#define CCU_DE0_CLK_REG       0x0A00
// gate(31) + mux=0 (pll-de-3x) + M field[4:0]=0 (no divider).  The DE
// must run >= ~2x dclk (>= 297 MHz for 1080p60); a stale M=1 divided
// the 3x tap by two (225 MHz) which is below scanout requirement.
#define CCU_DE0_CLK_E1        0x80000000
#define CCU_BUS_DE0_REG       0x0A04
#define CCU_BUS_DE0_E1        0x00010001   // gate bit0 + reset bit16 released
#define CCU_DE_SYS_RST_REG    0x0A74
#define RST_BUS_DE_SYS        BIT16

//
// HDMI block (SunxiDisplayClock.c).
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
// GMAC0 (SunxiGmacDxe.c).
//
#define CCU_GMAC_PTP_CLK_REG  0x1400
#define GMAC_PTP_CLK_GATE     BIT31
#define CCU_GMAC0_PHY_CLK_REG 0x1410
#define GMAC0_PHY_CLK_LIVE    (BIT31 | 6)  // 25 MHz, pll-peri0-150m M=150/25
#define CCU_GMAC0_BGR_REG     0x141C
#define GMAC0_BGR_LIVE        (BIT0 | BIT16 | BIT17)  // gate + RST + AXI_RST

//
// SPI0 (SunxiSpiDxe.c / A7AFvbDxe.c).
//
#define CCU_SPI0_CLK_REG      0x0F00
#define CCU_SPI0_CLK_LIVE     0x81000005   // module clock gate + mux/div
#define CCU_SPI0_BGR_REG      0x0F04
#define CCU_SPI0_BUS_GATE     BIT0
#define CCU_SPI0_BUS_RST      BIT16         // 1 = deasserted

//
// USB (SunxiUsbDxe.c).  Offsets confirmed against BSP ccu-sun60iw2.c
// clock and reset tables.  Field bits are RMW: only the gates/resets this
// driver owns are forced, the rest of each register is left as the boot
// chain configured it.
//   0x1300/0x1308: usb0/usb1 parent gate BIT31 + RST_USB_x_PHY_RSTN BIT30
//   0x1304/0x130C: usb0/1 ohci gate BIT0 rst BIT16, ehci gate BIT4 rst BIT20
//   0x1340 usb_ref / 0x1348 usb2_u2_ref: gate BIT31, mux [26:24] (0=sys24M)
//   0x1350 usb2_suspend: gate BIT31, mux bit24 (1=sys24M), M [4:0] = 0
//   0x1354 usb2_mf:      gate BIT31, mux [26:24] (0=sys24M), M [4:0] = 0
//
#define CCU_MSI_LITE2_REG     0x05A4
#define CCU_RES_DCAP_REG      0x1A00
#define CCU_RES_DCAP_USB_PHY_GATE  BIT3
#define CCU_USB_CLK_REG       0x1300
#define CCU_USB0_GATE_RST_REG 0x1304
#define CCU_USB1_CLK_REG      0x1308
#define CCU_USB1_GATE_RST_REG 0x130C
#define CCU_USB_REF_REG       0x1340
#define CCU_USB2_U2_REF_REG   0x1348
#define CCU_USB2_SUSPEND_REG  0x1350
#define CCU_USB2_MF_REG       0x1354
#define CCU_USB2_U3_UTMI_REG  0x1360
#define CCU_USB2_RST_REG      0x135C
#define CCU_USB2_RST_BIT      BIT16

#define CCU_USB_CLK_ON_BITS       (BIT31 | BIT30)  // gate + PHY_RSTN released
#define CCU_USB_GATE_RST_ON_BITS  0x00110011       // ohci+ehci gates + resets
#define CCU_USB_MUX_MASK          (0x7U << 24)
#define CCU_USB2_SUSPEND_MUX_BIT  BIT24
#define CCU_USB2_M_MASK           0x1FU

//
// pll-peri0 (0x00A0): the 2X -> 600M -> 300M output chain feeding
// usb2-mf's mux is gated by the PLL output gates.  Earlier boot stages
// leave only OUTPUT2 (BIT25) on; with OUTPUT0 (BIT27) off the 300M source
// does not exist and the CCU's glitchless switch silently reverts any
// CLK_SRC_SEL=1 write back to sys24M (observed on hardware: usb2-mf stuck
// at 0x81000004 - gate and M taken, mux dropped).
//
#define CCU_PLL_PERI0_REG         0x00A0
#define CCU_PLL_PERI0_OUT_GATES   (BIT25 | BIT26 | BIT27)

//
// Serdes clock for the USB3 (combo0) / PCIe (combo1) PHY subsystem.
//   CCU_SERDES_PHY_CFG_CLK: reg 0x13C0, M [4:0], mux [24], gate BIT31.
//   RST_BUS_SERDES:          reg 0x13C4, BIT16 (1 = released)
//
#define CCU_SERDES_PHY_CFG_REG  0x13C0
#define CCU_SERDES_RST_REG      0x13C4
#define CCU_SERDES_RST_BIT      BIT16
#define CCU_SERDES_PHY_CFG_GATE BIT31
#define CCU_SERDES_PHY_CFG_M_MASK  0x1Fu
#define CCU_SERDES_PHY_CFG_MUX_MASK BIT24
#define CCU_SERDES_PHY_CFG_MUX_PERI600  BIT24   // mux 1 = pll-peri0 600M
#define CCU_SERDES_PHY_CFG_M_DIV6  0x05u        // 600 / (5+1) = 100 MHz

//
// HDMI TV output clock (SunxiDisplayClock.c).
//   CLK_HDMI_TV (0x1684): SUNXI_CCU_MP_WITH_MUX_GATE_NO_INDEX
//     M divider shift 0 width 5, N divider shift 8 width 5,
//     mux shift 24 width 3 (0 = pll-video0-4x), gate BIT31.
//   Divider fields are one-based (field = divider - 1); rate =
//   parent / (M+1) / (N+1).  N holds the whole divider here (M = 0),
//   so field N = parent/target - 1.
//
#define CCU_HDMI_TV_CFG_REG  0x1684
#define HDMI_TV_N_SHIFT      8
#define HDMI_TV_N_WIDTH      5
#define HDMI_TV_M_SHIFT      0
#define HDMI_TV_MUX_SHIFT    24
#define HDMI_TV_MUX_PLL_VIDEO0_4X  0
#define HDMI_TV_GATE         BIT31
#define HDMI_TV_MAX_DIV      (1U << HDMI_TV_N_WIDTH)   // field max 31 -> div 32

/**
  Raw 32-bit CCU register access.
**/
UINT32
EFIAPI
SunxiCcuRead (
  IN UINT32  RegOffset
  )
{
  return MmioRead32 (A733_CCU_BASE + RegOffset);
}

VOID
EFIAPI
SunxiCcuWrite (
  IN UINT32  RegOffset,
  IN UINT32  Value
  )
{
  MmioWrite32 (A733_CCU_BASE + RegOffset, Value);
}

VOID
EFIAPI
SunxiCcuSetBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  )
{
  MmioOr32 (A733_CCU_BASE + RegOffset, Bits);
}

VOID
EFIAPI
SunxiCcuClearBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  )
{
  MmioAnd32 (A733_CCU_BASE + RegOffset, ~Bits);
}

//
// Whole-word write with read-back verification.  Used for clock registers
// whose value must be exact (mux + divider + gate in one word): a plain
// read-modify-write chain can silently lose a field when the selected
// clock source is momentarily absent, and only a full re-write recovers.
//
STATIC
VOID
SunxiCcuWriteVerify (
  IN UINT32  RegOffset,
  IN UINT32  Value
  )
{
  UINTN  Try;

  for (Try = 0; Try < 8; Try++) {
    MmioWrite32 (A733_CCU_BASE + RegOffset, Value);
    if (MmioRead32 (A733_CCU_BASE + RegOffset) == Value) {
      return;
    }

    MicroSecondDelay (100);
  }

  DEBUG ((
    DEBUG_ERROR,
    "A733ClockLib: CCU 0x%08x stuck at 0x%08x (want 0x%08x)\n",
    A733_CCU_BASE + RegOffset,
    MmioRead32 (A733_CCU_BASE + RegOffset),
    Value
    ));
}

//
// Stray bits of the keyed AHB/MBUS master-gate registers.  A plain
// read-modify-write that ORs the key constant on lights these up (they are
// the low bits of the key) and switches on six bus masters the vendor
// leaves gated - the ethernet-after-EDK2 bug (see SunxiPcieDxe).  When a
// keyed base state already exists (U-Boot establishes it), a driver only
// needs to OR its own gate bits and then clear these strays.
//
#define CCU_AHB_GATE_STRAY   0x00000007U
#define CCU_MBUS0_GATE_STRAY 0x01005000U

/**
  Set master-gate bits on the keyed AHB / MBUS matrices.

  Preserves every existing gate, sets only the caller's bits and clears
  the stray bits the key's low field would otherwise light up.  Requires
  the keyed base state to already be present in the register (a previous
  boot stage, or another driver, established it) - this function does not
  add the key itself.
**/
VOID
EFIAPI
SunxiClockSetAhbMbusGates (
  IN UINT32  AhbGateBits,
  IN UINT32  Mbus0GateBits,
  IN UINT32  Mbus1GateBits
  )
{
  UINT32  V;

  V  = SunxiCcuRead (CCU_AHB_GATE_REG);
  V |= AhbGateBits;
  V &= ~CCU_AHB_GATE_STRAY;
  SunxiCcuWrite (CCU_AHB_GATE_REG, V);

  V  = SunxiCcuRead (CCU_MBUS_GATE0_REG);
  V |= Mbus0GateBits;
  V &= ~CCU_MBUS0_GATE_STRAY;
  SunxiCcuWrite (CCU_MBUS_GATE0_REG, V);

  if (Mbus1GateBits != 0) {
    SunxiCcuSetBits (CCU_MBUS_GATE1_REG, Mbus1GateBits);
  }
}

/**
  CCU for DE0: PLL_DE -> CLK_DE0 (600 MHz), bus gate/reset, DE_SYS reset.
**/
VOID
EFIAPI
SunxiClockInitDe0 (
  VOID
  )
{
  UINTN  Tries;

  //
  // PLL_DE: bring LDO + enable up first, wait for lock, then open the
  // pll-de-3x output.  Mirrors DeClockInit() (an all-at-once write of the
  // final value does not lock).
  //
  SunxiCcuWrite (CCU_PLL_DE_REG, 0xEC000000);
  MicroSecondDelay (50);
  SunxiCcuWrite (CCU_PLL_DE_REG, 0xEC134A00);   // N=75 -> 1800MHz, 3x div 3

  for (Tries = 0; Tries < 100; Tries++) {
    if ((SunxiCcuRead (CCU_PLL_DE_REG) & PLL_DE_LOCK) != 0) {
      break;
    }
    MicroSecondDelay (100);
  }
  if (Tries >= 100) {
    DEBUG ((DEBUG_ERROR, "A733Clock: PLL_DE did not lock\n"));
  }

  SunxiCcuWrite (CCU_DE0_CLK_REG, CCU_DE0_CLK_E1);
  SunxiCcuSetBits (CCU_BUS_DE0_REG, CCU_BUS_DE0_E1);
  SunxiCcuSetBits (CCU_DE_SYS_RST_REG, RST_BUS_DE_SYS);
}

/**
  CCU for the HDMI block and its vo1 crossing.  DE/VIDEO_OUT master-gate
  bits are assumed already set by whoever owns the DE0 clock path.
**/
VOID
EFIAPI
SunxiClockInitHdmi (
  VOID
  )
{
  SunxiCcuSetBits (CCU_DPSS_TOP1_REG, CCU_DPSS_TOP1_E1);
  SunxiCcuSetBits (CCU_VIDEO_OUT1_REG, CCU_VIDEO_OUT1_E1);
  SunxiCcuSetBits (CCU_TCONTV0_REG, CCU_TCONTV0_E1);
  SunxiCcuSetBits (CCU_HDMI_REG, CCU_HDMI_E1);
  SunxiCcuWrite (CCU_HDMI_SFR_REG, CCU_HDMI_SFR_E1);
}

/**
  CCU for GMAC0: module gate/reset, PHY reference clock, PTP clock.
**/
VOID
EFIAPI
SunxiClockInitGmac0 (
  VOID
  )
{
  //
  // The GMAC0 PHY reference clock is pll-peri0-150m (600M / 4).  Open the
  // pll-peri0 output gates explicitly so the PHY clock exists regardless of
  // whether the USB init ran first - the gates are otherwise only opened by
  // the USB2 clock setup.
  //
  SunxiCcuSetBits (CCU_PLL_PERI0_REG, CCU_PLL_PERI0_OUT_GATES);

  SunxiCcuSetBits (CCU_GMAC0_BGR_REG, GMAC0_BGR_LIVE);
  //
  // RMW: gate + M divider only, so the mux field and any boot-
  // chain residue in 0x1410 survive (USB init opens the same
  // pll-peri0 output gates).
  //
  SunxiCcuClearBits (CCU_GMAC0_PHY_CLK_REG, 0x1Fu);
  SunxiCcuSetBits   (CCU_GMAC0_PHY_CLK_REG, GMAC0_PHY_CLK_LIVE);
  SunxiCcuSetBits (CCU_GMAC_PTP_CLK_REG, GMAC_PTP_CLK_GATE);
}

/**
  CCU for SPI0: module clock, then bus gate and reset release.
**/
VOID
EFIAPI
SunxiClockInitSpi0 (
  VOID
  )
{
  SunxiCcuWrite (CCU_SPI0_CLK_REG, CCU_SPI0_CLK_LIVE);
  SunxiCcuSetBits (CCU_SPI0_BGR_REG, CCU_SPI0_BUS_GATE);
  SunxiCcuSetBits (CCU_SPI0_BGR_REG, CCU_SPI0_BUS_RST);
}

/**
  CCU for the USB (EHCI0/1 + xHCI2/DWC3) subsystem.

  Mirrors what SunxiUsbDxe used to poke by hand: keyed AHB gate, the
  msi_lite2 / res_dcap gates the EHCI and USB2-PHY paths need, the
  per-controller module gates + resets, and the serdes clock that feeds
  the USB3 combo PHY.  Every field is set by RMW so bits the boot chain
  owns are preserved.
**/
VOID
EFIAPI
SunxiClockInitUsb (
  VOID
  )
{
  //
  // Keyed AHB master gate: only USB's own gate bit is added, the keyed
  // base state established by the boot chain is preserved.
  //
  SunxiClockSetAhbMbusGates (A733_AHB_GATE_USB_SYS, 0, 0);

  //
  // CLK_MSI_LITE2 gate + AHB/MBU resets: the EHCI IRQ path is gated
  // without these (USBSTS.PSS/ASS never echo USBCMD.PSE/ASE).
  //
  SunxiCcuSetBits (CCU_MSI_LITE2_REG, BIT0 | BIT16 | BIT17);

  //
  // CLK_RES_DCAP_24M: the mclk for the USB2 PHY MMIO @ 0x06B00000.
  //
  SunxiCcuSetBits (CCU_RES_DCAP_REG, CCU_RES_DCAP_USB_PHY_GATE);
  MicroSecondDelay (10);

  //
  // USB0/USB1 module clocks: parent gate + PHY_RSTN released, then the
  // per-controller ohci/ehci gates and their resets.
  //
  SunxiCcuSetBits (CCU_USB_CLK_REG, CCU_USB_CLK_ON_BITS);
  SunxiCcuSetBits (CCU_USB0_GATE_RST_REG, CCU_USB_GATE_RST_ON_BITS);
  SunxiCcuSetBits (CCU_USB1_CLK_REG, CCU_USB_CLK_ON_BITS);
  SunxiCcuSetBits (CCU_USB1_GATE_RST_REG, CCU_USB_GATE_RST_ON_BITS);

  //
  // pll-peri0 output gates: open OUTPUT0/1/2 so the 600M/300M derived
  // outputs exist (see the note at the register defines above).
  //
  SunxiCcuSetBits (CCU_PLL_PERI0_REG, CCU_PLL_PERI0_OUT_GATES);

  //
  // USB ref clock: gate + mux field forced to sys24M (mux 0).
  //
  SunxiCcuSetBits (CCU_USB_REF_REG, BIT31);
  SunxiCcuClearBits (CCU_USB_REF_REG, CCU_USB_MUX_MASK);

  //
  // USB2 (xHCI2/DWC3): u2_ref gate, suspend clock (mux bit24 = sys24M,
  // M = 0) and the USB2 reset — all matching the live vendor stack.
  //
  SunxiCcuSetBits (CCU_USB2_U2_REF_REG, BIT31);
  SunxiCcuSetBits (CCU_USB2_SUSPEND_REG, BIT31 | CCU_USB2_SUSPEND_MUX_BIT);
  SunxiCcuClearBits (CCU_USB2_SUSPEND_REG, CCU_USB2_M_MASK);
  //
  // USB2 bus/MF clock.  Live Linux clk_summary on the U-Boot+Linux card
  // (2026-09-09): usb2-mf = 24000000, mux 0 (sys24M) - the 60 MHz mux-1
  // path is NOT what Linux runs, and the hub enumerates fine at 24 MHz
  // there.  The mux-1 write is also rejected by the CCU glitchless
  // switch (readback keeps mux 0 while M sticks), so gate-only (M=0,
  // mux=0) is both the Linux configuration and the only value the
  // hardware accepts cleanly.  PWRDNSCALE is matched to Linux in
  // SunxiUsbDxe (1560, not 1500).
  //
  SunxiCcuWriteVerify (CCU_USB2_MF_REG, BIT31);
  //
  // usb2-u3-utmi (0x1360): gate ON, same 60 MHz config as the bus clock.
  // Without a gated UTMI clock the PHY interface never acknowledges the
  // core reset (USBSTS.CNR sticks after HCRST).
  //
  SunxiCcuWriteVerify (CCU_USB2_U3_UTMI_REG, BIT31);

  SunxiCcuSetBits (CCU_USB2_RST_REG, CCU_USB2_RST_BIT);

  MicroSecondDelay (50);

  //
  // Serdes clock for the USB3 combo PHY: parent pll-peri0 600 MHz
  // (mux 1), M = 5 (divide by 6) for 100 MHz, then gate; release the
  // serdes reset after the clock is up.
  //
  SunxiCcuClearBits (CCU_SERDES_PHY_CFG_REG,
                     CCU_SERDES_PHY_CFG_M_MASK | CCU_SERDES_PHY_CFG_MUX_MASK);
  SunxiCcuSetBits (CCU_SERDES_PHY_CFG_REG,
                   CCU_SERDES_PHY_CFG_MUX_PERI600 | CCU_SERDES_PHY_CFG_M_DIV6);
  SunxiCcuSetBits (CCU_SERDES_PHY_CFG_REG, CCU_SERDES_PHY_CFG_GATE);
  MicroSecondDelay (10);

  SunxiCcuSetBits (CCU_SERDES_RST_REG, CCU_SERDES_RST_BIT);
  MicroSecondDelay (50);
}

/**
  Assert or release the PHY reset of one USB controller.

  The PHY reset (RST_USB_x_PHY_RSTN, bit30 of the controller's module
  clock register) must bracket the HCI SIDDQ/passby programming:
  asserted before, released after.

  @param UsbIndex  0 = usb0 (EHCI0), 1 = usb1 (EHCI1).
  @param Assert    TRUE holds the PHY in reset, FALSE releases it.
**/
VOID
EFIAPI
SunxiClockSetUsbPhyReset (
  IN UINTN    UsbIndex,
  IN BOOLEAN  Assert
  )
{
  UINT32  Reg;

  Reg = (UsbIndex == 0) ? CCU_USB_CLK_REG : CCU_USB1_CLK_REG;

  //
  // The module clock gate (BIT31) stays on either way; only PHY_RSTN
  // (BIT30) is driven: 1 = released, 0 = held in reset.
  //
  if (Assert) {
    SunxiCcuClearBits (Reg, BIT30);
  } else {
    SunxiCcuSetBits (Reg, CCU_USB_CLK_ON_BITS);
  }
}

/**
  Program the HDMI TV output clock (0x1684) for a target rate.

  The register is SUNXI_CCU_MP_WITH_MUX_GATE_NO_INDEX: M divider at
  bits [4:0], N divider at bits [12:8] (both one-based fields), mux at
  bits [26:24], gate at bit 31.  The parent is selected by the caller
  via SourceHz (mux 0 = pll-video0-4x on this SoC); the whole divider
  goes in N with M left at 1 (field 0).

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
  )
{
  UINT32  Div;
  UINT32  Reg;

  if ((SourceHz == 0) || (TargetHz == 0) || (SourceHz % TargetHz != 0)) {
    DEBUG ((DEBUG_ERROR,
            "A733Clock: hdmi-tv rate %u from %u not an integer divider\n",
            TargetHz, SourceHz));
    return EFI_UNSUPPORTED;
  }

  Div = SourceHz / TargetHz;
  if (Div > HDMI_TV_MAX_DIV) {
    DEBUG ((DEBUG_ERROR,
            "A733Clock: hdmi-tv divider %u exceeds %u-bit N field\n",
            Div, (UINT32)HDMI_TV_N_WIDTH));
    return EFI_UNSUPPORTED;
  }

  Reg = HDMI_TV_GATE
      | ((UINT32)HDMI_TV_MUX_PLL_VIDEO0_4X << HDMI_TV_MUX_SHIFT)
      | ((Div - 1) << HDMI_TV_N_SHIFT);
  SunxiCcuWrite (CCU_HDMI_TV_CFG_REG, Reg);

  return EFI_SUCCESS;
}
