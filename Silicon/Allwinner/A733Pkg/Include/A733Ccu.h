/** @file
  CCU (clock control unit) register map for the Allwinner A733 (sun60iw2p1).

  Every layout below comes from the A733 user manual's CCU chapter; each
  comment names the register the layout was checked against on hardware.
  The CCU is an 8 KB window and the SoC does not decode address bit 13, so
  any offset past +0x1FFF aliases back into the window.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_CCU_H__
#define A733_CCU_H__

#include <Uefi.h>

//
// Manual: CCU chapter, CCU base and window size.
//
#define A733_CCU_WINDOW_SIZE  0x2000U

//
// Manual: CCU chapter, module CLK_REG.  bit31 gates the module clock, 4:0 hold
// FACTOR_M (M = FACTOR_M + 1), and the parent selector sits just under the
// gate: bits 26:24 (3 wide) for most modules, bits 25:24 (2 wide) for GIC.
// Verified against CPU_PERI (0x0568) and SMHC2 (0x0D20).  Both the field width
// and the value -> parent table are properties of the module, so both live in
// A733CcuLib.c beside the module's own row.
//
// The parent table is NOT the same for every module - the vendor register
// header gives each module its own mapping (SMHC0's 001 is PERI0_400M where
// SMHC2's is PERI0_800M).  A733_CCU_SRC below therefore names parents by
// frequency class, not by the value a register holds: a caller passes the
// class it wants, and A733CcuSetModuleClock finds it in that module's table or
// refuses the request.
//
// The manual states the module clock is "Clock Source / M / N": the divider is
// a pair of 5-bit factors and both are stored as value + 1.
//
//   M = FACTOR_M + 1   (FACTOR_M, bits 4:0,  M is 1..32)
//   N = FACTOR_N + 1   (FACTOR_N, bits 12:8, N is 1..32)
//
// A factor field of 0 therefore means a divider of 1, which is how the hardware
// reads it.  A733CcuSetModuleClockDivider writes both fields in one
// read-modify-write; A733CcuSetModuleClock is the M-only wrapper and leaves N
// at 0 (divider 1) rather than at whatever residue was there before.
//
#define A733_CCU_CLK_GATING         BIT31
#define A733_CCU_CLK_SRC_SHIFT      24U
// The common selector shape (3 bits at 26:24).  Each module's own width is in
// its A733CcuLib.c row, because GIC's selector is only 2 bits.
#define A733_CCU_CLK_SRC_WIDTH      3U
#define A733_CCU_CLK_SRC_MASK       (0x7U << A733_CCU_CLK_SRC_SHIFT)
#define A733_CCU_CLK_FACTOR_M_SHIFT 0U
#define A733_CCU_CLK_FACTOR_M_WIDTH 5U
#define A733_CCU_CLK_FACTOR_M_MASK  0x1FU
#define A733_CCU_CLK_FACTOR_N_SHIFT 8U
#define A733_CCU_CLK_FACTOR_N_WIDTH 5U
#define A733_CCU_CLK_FACTOR_N_MASK  (0x1FU << A733_CCU_CLK_FACTOR_N_SHIFT)
// The smallest and largest divider either 5-bit factor field encodes, i.e.
// field value + 1.
#define A733_CCU_CLK_FACTOR_MIN     1U
#define A733_CCU_CLK_FACTOR_MAX     (A733_CCU_CLK_FACTOR_M_MASK + 1U)

//
// Manual: CCU chapter, module BGR_REG (bus gating / reset).  Sub-block n
// owns bit n (clock gate: 0 off / 1 on) and bit 16 + n (reset: 0 asserted /
// 1 released).  Verified against SMHC2 (0x0D2C), and against the A733
// register table, which spells the same polarity out per sub-block.
// The library only ever drives sub-block 0, the module's own bus clock.
//
#define A733_CCU_BGR_SUB_CLK(Sub)  (1U << (Sub))
#define A733_CCU_BGR_SUB_RST(Sub)  (1U << (16U + (Sub)))
#define A733_CCU_BGR_SUB_COUNT     16U

// Sub-block 0 carries the module's own bus clock and reset.
#define A733_CCU_BGR_MAIN_SUB  0U

//
// The clock parents a module's selector may name, identified by frequency class
// rather than by a register value.  Which selector value yields a class is the
// module's own business - see the per-module tables in A733CcuLib.c.
//
typedef enum {
  A733_CCU_SRC_HOSC = 0,
  A733_CCU_SRC_SYS_CLK24M,
  A733_CCU_SRC_CLK32K,
  A733_CCU_SRC_PERI0_800M,
  A733_CCU_SRC_PERI0_600M,
  A733_CCU_SRC_PERI0_480M,
  A733_CCU_SRC_PERI0_400M,
  A733_CCU_SRC_PERI0_300M,
  A733_CCU_SRC_PERI0_200M,
  A733_CCU_SRC_PERI0_160M,
  A733_CCU_SRC_PERI0_150M,
  A733_CCU_SRC_PERI1_800M,
  A733_CCU_SRC_PERI1_600M,
  A733_CCU_SRC_PERI1_480M,
  A733_CCU_SRC_PERI1_400M,
  A733_CCU_SRC_PERI1_300M,
  A733_CCU_SRC_PERI1_200M,
  A733_CCU_SRC_PERI1_160M,
  A733_CCU_SRC_DEPLL3X,
  A733_CCU_SRC_DEPLL4X,
  A733_CCU_SRC_VIDEO0PLL4X,
  A733_CCU_SRC_VIDEO1PLL4X,
  A733_CCU_SRC_VIDEO2PLL4X,
  A733_CCU_SRC_VIDEO3PLL4X,
  A733_CCU_SRC_PERI0PLL2X,
  A733_CCU_SRC_NPUPLL,
  A733_CCU_SRC_GPUPLL,
  A733_CCU_SRC_CCIPLL,
  A733_CCU_SRC_VE0PLL,
  A733_CCU_SRC_VE1PLL,
  A733_CCU_SRC_MAX
} A733_CCU_SRC;

//
// Rates for the source classes that are a fixed tap.  A733_CCU_SRC_HOSC_HZ is
// the 24 MHz oscillator the CCU can switch a module onto: it is NOT the 26 MHz
// DCXO that feeds the PLLs (A733_CCU_DCXO_HZ), and the two must not be
// conflated.  The PERI0/PERI1 rates are the manual's nominal derivatives of
// PERI0PLL2X/PERI1PLL2X; their field positions could not be reconciled against
// a measured clock on this silicon, so they are named constants rather than
// something the library decodes - see A733CcuGetPllHz.  The PLL-derived
// classes (DEPLL*, VIDEO*PLL, PERI0PLL2X, NPUPLL, GPUPLL, CCIPLL, VE*PLL) have
// no rate this port could verify; the library reports those as unknown and
// invents nothing.  PERI0PLL2X is PERI0's 2X output, which the manual derives
// from the PERI0 fields as f*N/(M1*P0): it is not assumed to be 1.2 GHz, and
// its absolute rate is as unverified as the PLLs'.
//
#define A733_CCU_SRC_HOSC_HZ        24000000U
#define A733_CCU_SRC_SYS_CLK24M_HZ  24000000U
#define A733_CCU_SRC_CLK32K_HZ      32768U
#define A733_CCU_SRC_PERI0_800M_HZ  800000000U
#define A733_CCU_SRC_PERI0_600M_HZ  600000000U
#define A733_CCU_SRC_PERI0_480M_HZ  480000000U
#define A733_CCU_SRC_PERI0_400M_HZ  400000000U
#define A733_CCU_SRC_PERI0_300M_HZ  300000000U
#define A733_CCU_SRC_PERI0_200M_HZ  200000000U
#define A733_CCU_SRC_PERI0_160M_HZ  160000000U
#define A733_CCU_SRC_PERI0_150M_HZ  150000000U
#define A733_CCU_SRC_PERI1_800M_HZ  800000000U
#define A733_CCU_SRC_PERI1_600M_HZ  600000000U
#define A733_CCU_SRC_PERI1_480M_HZ  480000000U
#define A733_CCU_SRC_PERI1_400M_HZ  400000000U
#define A733_CCU_SRC_PERI1_300M_HZ  300000000U
#define A733_CCU_SRC_PERI1_200M_HZ  200000000U
#define A733_CCU_SRC_PERI1_160M_HZ  160000000U

// DCXO feeding every PLL (verified: 26 MHz).
#define A733_CCU_DCXO_HZ  26000000U

//
// Manual: CCU chapter, module register table.  Offsets are relative to the
// CCU base; a module without a register of that kind has none ("-" in the
// manual's table).  UART0/UART1 take their clock from APB_UART (0x0538),
// which is shared with the rest of the APB peripherals.
//
#define A733_CCU_GIC_CLK        0x0560U
#define A733_CCU_DMA0_BGR       0x0704U
#define A733_CCU_UART0_BGR      0x0E00U
#define A733_CCU_UART1_BGR      0x0E04U
#define A733_CCU_APB_UART_CLK   0x0538U
#define A733_CCU_SMHC0_CLK      0x0D00U
#define A733_CCU_SMHC0_BGR      0x0D0CU
#define A733_CCU_SMHC1_CLK      0x0D10U
#define A733_CCU_SMHC1_BGR      0x0D1CU
#define A733_CCU_SMHC2_CLK      0x0D20U
#define A733_CCU_SMHC2_BGR      0x0D2CU
#define A733_CCU_SMHC3_CLK      0x0D30U
#define A733_CCU_SMHC3_BGR      0x0D3CU
#define A733_CCU_NAND0_CLK      0x0C80U
#define A733_CCU_NAND0_BGR      0x0C8CU
#define A733_CCU_SPI0_CLK       0x0F00U
#define A733_CCU_SPI0_BGR       0x0F04U
#define A733_CCU_USB0_CLK       0x1300U
#define A733_CCU_USB0_BGR       0x1304U
#define A733_CCU_USB1_CLK       0x1308U
#define A733_CCU_USB1_BGR       0x130CU
// The 24 MHz reference both USB2.0 PHYs run their PLL from.  Its gate is bit 31
// and is set out of reset (the register reads 0x8000_0000), which is why a
// driver that only has to keep it on does not have to enable anything else.
#define A733_CCU_USB01_REF_CLK  0x1340U
//
// The rest of the USB clock block, the group the USB3.1 GEN2 DRD (the DWC3 core
// at 0x06A00000) runs on.  A733Xhci2Dxe is the one driver in this package that
// programs it; the fields below are the manual's, register by register:
//
//   0x1348 USB2_U2_REF    the USB2.0 PHY's own reference clock (manual
//                         4.1.6.199): gate bit 31, source 26:24 (000 SYS_CLK24M,
//                         001 DCXO).  The PHY's FSEL field expects a 24 MHz
//                         reference, so the source is SYS_CLK24M.
//   0x1350 USB2_SUSPEND    (4.1.6.200): gate bit 31, source bit 24 (0 RTC_32K,
//                         1 SYS_CLK24M), FACTOR_M 4:0 (M = value + 1).
//   0x1354 USB2_MF        the microframe clock (4.1.6.201): gate, source 26:24
//                         (000 SYS_CLK24M, 001 PERI0_300M, 010 DCXO), FACTOR_M.
//   0x135C USB2_BGR       (4.1.6.202): bit 16 is the block reset, 1 = released.
//   0x1360 USB2_U3_UTMI   (4.1.6.203): gate, source (SYS_CLK24M / PERI0_300M /
//                         DCXO), FACTOR_M.  A733Xhci2Dxe only opens its gate and
//                         leaves the rate the boot chain set: with
//                         USB3P1_USB2P0_UTMI_CLK_SEL = 0 the core's USB2 port
//                         takes its UTMI clock from the femtoPHY, so this one
//                         clocks the USB3 side.
//   0x1364 USB2_U2_PIPE   (4.1.6.204): the USB2 half's PIPE clock.  Named for
//                         completeness; the DWC3 takes its USB2 port's clock
//                         from the femtoPHY (see USB3P1_USB2P0_UTMI_CLK_SEL in
//                         A733Dwc3.h), so this gate stays as the boot chain
//                         leaves it - closed - which is what the A733 host
//                         firmware's own USB clock state has too.
//   0x13C0 SERDES_PHY_CFG (4.1.6.208): gate bit 31, source 26:24 (000
//                         SYS_CLK24M, 001 PERI0_600M), FACTOR_M 4:0.  The
//                         SerDes subsystem's register clock: with it off the
//                         DWC3's AHB slave reads back zero and drops writes.
//   0x13C4 SERDES_BGR     (4.1.6.209): bit 16 is the block reset, 1 = released.
//
#define A733_CCU_USB2_U2_REF_CLK   0x1348U
#define A733_CCU_USB2_SUSPEND      0x1350U
//
// The USB2 bus clock (mf) and the USB2-side UTMI clock, as the working firmware
// leaves them: 0x81000104 - gate on, source select set, divider landing on
// 60 MHz.  The gate alone leaves 24 MHz, which that firmware's A/B records as
// the reason its port never completed speed detection.
//
#define A733_CCU_USB2_MF_60MHZ       0x81000104U
#define A733_CCU_USB2_U3_UTMI_60MHZ  0x81000104U

#define A733_CCU_USB2_MF           0x1354U
#define A733_CCU_RST_USB_2         0x135CU
#define A733_CCU_USB2_U3_UTMI_CLK  0x1360U
#define A733_CCU_USB2_U2_PIPE_CLK  0x1364U
#define A733_CCU_SERDES_PHY_CFG    0x13C0U
#define A733_CCU_RST_BUS_SERDES    0x13C4U
//
// The fields those six words share.  Where a register deviates (SUSPEND's source
// is one bit at 24, not the 26:24 field) the comment above says so.
//
#define A733_CCU_USB2_CLK_SRC_SHIFT  24U
#define A733_CCU_USB2_CLK_SRC_MASK   (0x7U << A733_CCU_USB2_CLK_SRC_SHIFT)
#define A733_CCU_USB2_CLK_SRC_SYS_CLK24M  0U
#define A733_CCU_USB2_CLK_SRC_PERI0_300M  1U
#define A733_CCU_USB2_CLK_FACTOR_M_MASK   0x1FU
#define A733_CCU_USB2_SUSPEND_SRC_SYS_CLK24M  BIT24
#define A733_CCU_USB2_RST_BIT        BIT16  // 0 asserted, 1 released
#define A733_CCU_SERDES_PHY_CFG_SRC_PERI0_600M  (1U << A733_CCU_USB2_CLK_SRC_SHIFT)
#define A733_CCU_SERDES_RST_BIT      BIT16  // 0 asserted, 1 released
#define A733_CCU_GMAC0_CLK      0x1410U
#define A733_CCU_GMAC0_BGR      0x141CU
#define A733_CCU_DE0_CLK        0x0A00U
#define A733_CCU_DE0_BGR        0x0A04U
#define A733_CCU_TCONLCD0_CLK   0x1500U
#define A733_CCU_TCONLCD0_BGR   0x1504U
#define A733_CCU_DSI0_CLK       0x1580U
#define A733_CCU_DSI0_BGR       0x1584U
//
// HDMI's parent selector (bits 26:24) lives in HDMI_PRE_CLK_REG (0x1684), the
// CLK_REG below; the HDMI BGR (0x168C) only gates and resets the block and
// carries no selector of its own.  TCONLCD0 (0x1500) and GPU0 (0x0B20) are the
// same shape, with the selector in their own CLK_REG rather than in the BGR.
//
#define A733_CCU_HDMI_CLK       0x1684U
#define A733_CCU_HDMI_BGR       0x168CU
#define A733_CCU_GPU0_CLK       0x0B20U
#define A733_CCU_NPU_CLK        0x0B00U
#define A733_CCU_NPU_BGR        0x0B04U
#define A733_CCU_VE_DEC_CLK     0x0A88U
#define A733_CCU_VE_ENC0_CLK    0x0A80U
#define A733_CCU_VE_BGR         0x0A8CU  // shared by VE_DEC and VE_ENC0

//
// Manual: CCU chapter, 4.1.6.194 "USB0 Clock Register" and 4.1.6.195 "USB0 Bus
// Gating Reset Register" (USB1 is 4.1.6.196/4.1.6.197 at +0x1308/+0x130C and
// has the same layout; the reference clock both PHYs share is 4.1.6.198 at
// +0x1340).  The USB host blocks break the module model the rest of this header
// follows: one BGR word carries two gates (OHCI in bit 0, EHCI in bit 4) and
// two block resets (OHCI in bit 16, EHCI in bit 20), and the PHY's reset sits in
// the CLK word rather than in the BGR.  A733CcuEnableModule's sub-block 0 is
// therefore the OHCI half only; a USB host driver has to open the EHCI half and
// release the PHY itself, which is why the bits are named here rather than
// hidden behind the generic module model.
//
// USB0 is the one window with a third function in it: the MUSB core at
// 0x04100000 (the OTG port, and the port the FES/flashing path uses) has its
// own gate and reset in the same BGR word.  USB1's BGR stops at bit 20 and has
// those two bits reserved, so a host driver that only ever learns this block
// from USB1 will not know they exist - hence the names.
//
#define A733_CCU_USB_CLK_MODULE_EN   BIT31  // USBn_CLKEN, the host block's clock
#define A733_CCU_USB_CLK_PHY_RSTN    BIT30  // USBn_PHY_RSTN: 0 asserted, 1 released
//
// USB0_CLK bits 25:24: the manual's OHCI 12 MHz source select - where the OHCI
// half's 12 MHz comes from, in four states the manual names.  It sits in this CLK
// word and not in the BGR, next to the two fields above, so one word decides the
// whole port's clocks.  Nothing here programs it: this package publishes no OHCI
// driver, so the field is left exactly as the boot chain leaves it, and the
// comment exists so that nobody sets it blind.
//
#define A733_CCU_USB_CLK_OHCI12M_SHIFT  24U
#define A733_CCU_USB_CLK_OHCI12M_MASK   (0x3U << A733_CCU_USB_CLK_OHCI12M_SHIFT)
#define A733_CCU_USB_CLK_OHCI12M_48M_DIV4  0U   // 12M divided from the 48 MHz
#define A733_CCU_USB_CLK_OHCI12M_SYS_CLK24M 1U  // from SYS_CLK24M
#define A733_CCU_USB_CLK_OHCI12M_RTC_32K    2U  // from RTC_32K
#define A733_CCU_USB_CLK_OHCI12M_RC16M      3U  // from RC16M
#define A733_CCU_USB_BGR_DEVICE_RST  BIT24  // USB0_DEVICE_RST: 0 asserted, 1 released
#define A733_CCU_USB_BGR_EHCI_RST    BIT20  // USBn_EHCI_RST: 0 asserted, 1 released
#define A733_CCU_USB_BGR_OHCI_RST    BIT16  // USBn_OHCI_RST: 0 asserted, 1 released
#define A733_CCU_USB_BGR_DEVICE_GATE BIT8   // USB0_DEVICE_GATING: 0 masked, 1 pass
#define A733_CCU_USB_BGR_EHCI_GATE   BIT4   // USBn_EHCI_GATING: 0 masked, 1 pass
#define A733_CCU_USB_BGR_OHCI_GATE   BIT0   // USBn_OHCI_GATING: 0 masked, 1 pass
#define A733_CCU_USB_REF_CLK_GATE    BIT31  // USB0_USB1_REF_CLK gating (0x1340)
//
// The reference clock's source, manual 4.1.6.198 bits 26:24: 000 takes the
// PHY PLL's reference from SYS_CLK24M, 001 from the DCXO (the crystal input
// itself, i.e. not the divided system clock).  Only those two values are
// documented; the rest of the field reads as "undefined" here rather than being
// given a name nobody has verified.
//
#define A733_CCU_USB_REF_CLK_SEL_SHIFT  24U
#define A733_CCU_USB_REF_CLK_SEL_MASK   (0x7U << A733_CCU_USB_REF_CLK_SEL_SHIFT)
#define A733_CCU_USB_REF_CLK_SEL_SYS_CLK24M  0U
#define A733_CCU_USB_REF_CLK_SEL_DCXO        1U

//
// The rest of what the A733 USB subsystem needs from the CCU, none of which is
// in the manual's USB0/USB1 clock chapter - the manual describes the two
// per-controller clock/gating words and the shared reference clock and stops
// there, while a port that actually runs also needs the bus masters, the
// PHY's own clock gate and the PLL output that feeds the dividers.  Offsets and
// values are the ones the A733 host firmware's USB clock init writes (its
// SunxiClockInitUsb, A733ClockLib), every one of them a read-modify-write so
// the boot chain's own bits survive.
//
//   0x05C0  keyed AHB master gate; the USB system's gate is bit 9.  The low
//           three bits are part of the key and the write clears them, because
//           OR-ing the key on switches on six unrelated bus masters.
//   0x05A4  CLK_MSI_LITE2 + its AHB/MBUS resets: without them the EHCI's
//           USBSTS never echoes USBCMD, i.e. no host completion ever reports.
//   0x1A00  CLK_RES_DCAP_24M, bit 3: the 24 MHz mclk of the USB2 PHY MMIO
//           window at 0x06B00000.  A PHY register block with no mclk reads as
//           zeros and drops every write.
//   0x00A0  pll-peri0 output gates (bits 27:25).  The boot chain leaves only
//           OUTPUT2 open, and a divider whose source output is gated falls back
//           to SYS_CLK24M without saying so.
//
#define A733_CCU_AHB_GATE_REG          0x05C0U
#define A733_CCU_AHB_GATE_USB_SYS      BIT9
#define A733_CCU_AHB_GATE_STRAY        0x00000007U   // key bits, not gates
#define A733_CCU_MSI_LITE2_REG         0x05A4U
#define A733_CCU_MSI_LITE2_ON          0x00030001U   // gate + AHB/MBUS resets
#define A733_CCU_RES_DCAP_REG          0x1A00U
#define A733_CCU_RES_DCAP_USB_PHY_GATE BIT3
#define A733_CCU_PLL_PERI0_REG         0x00A0U
#define A733_CCU_PLL_PERI0_OUT_GATES   (BIT25 | BIT26 | BIT27)

//
// USB0_BGR as the host firmware leaves it after its clock init: OHCI and EHCI
// gates (bits 0 and 4) with their resets released (bits 16 and 20).  It is the
// whole of what makes the host half of the port a clocked block, and the host
// half is what owns the PHY window at 0x04101800 - so these bits are the
// precondition for programming that window, whatever role the port then runs
// in.  The device half's own gate and reset (bits 8 and 24) are separate and
// stay under the device driver's control.
//
#define A733_CCU_USB_BGR_HOST_HALF     0x00110011U

//
// Manual: CCU chapter, module register table.  The order of the enum matches
// the module table in A733CcuLib.c.
//
typedef enum {
  A733_CCU_MODULE_GIC = 0,
  A733_CCU_MODULE_DMA0,
  A733_CCU_MODULE_UART0,
  A733_CCU_MODULE_UART1,
  A733_CCU_MODULE_SMHC0,
  A733_CCU_MODULE_SMHC1,
  A733_CCU_MODULE_SMHC2,
  A733_CCU_MODULE_SMHC3,
  A733_CCU_MODULE_NAND0,
  A733_CCU_MODULE_SPI0,
  A733_CCU_MODULE_USB0,
  A733_CCU_MODULE_USB1,
  A733_CCU_MODULE_GMAC0,
  A733_CCU_MODULE_DE0,
  A733_CCU_MODULE_TCONLCD0,
  A733_CCU_MODULE_DSI0,
  A733_CCU_MODULE_HDMI,
  A733_CCU_MODULE_GPU0,
  A733_CCU_MODULE_NPU,
  A733_CCU_MODULE_VE_DEC,
  A733_CCU_MODULE_VE_ENC0,
  A733_CCU_MODULE_MAX
} A733_CCU_MODULE;

//
// Manual: CCU chapter, PLL register table.
//
#define A733_CCU_PLL_DDR_REG     0x0020U
#define A733_CCU_PLL_PERI0_REG   0x00A0U
#define A733_CCU_PLL_PERI1_REG   0x00C0U
#define A733_CCU_PLL_GPU0_REG    0x00E0U
#define A733_CCU_PLL_VIDEO0_REG  0x0120U
#define A733_CCU_PLL_VIDEO1_REG  0x0140U
#define A733_CCU_PLL_VIDEO2_REG  0x0160U

//
// Manual: CCU chapter, PLL_PERI0/PERI1 control field names.  The manual
// gives the formulas by field name (PERI0PLL2X = f*N/(M1*P0),
// PERI0_800M = f*N/(M1*P1), PERI0_480M = f*N/(M1*P2), PERI0_600M = 2X/2)
// but the excerpt we hold does not tie P0/P1/P2/M1/N to bit positions, and
// no PERI0 output could be reconciled against a measured clock.  These three
// fields are what A733CcuDumpTree decodes; the library never presents the
// result as measured.
//
#define A733_CCU_PLL_P0_SHIFT  20U
#define A733_CCU_PLL_P0_WIDTH  3U
#define A733_CCU_PLL_M1_SHIFT  16U
#define A733_CCU_PLL_M1_WIDTH  4U
#define A733_CCU_PLL_N_SHIFT   8U
#define A733_CCU_PLL_N_WIDTH   8U

//
// CPU PLL block base (a separate block from the CPU_SUBSYS registers at
// 0x08000000): CPU_PLL_BACK +0x0000, CPU_PLL_L +0x1000, CPU_PLL_B +0x2000,
// CPU_PLL_DSU +0x3000.
//
#define A733_CPU_PLL_BACK_REG  0x0000U
#define A733_CPU_PLL_L_REG     0x1000U
#define A733_CPU_PLL_B_REG     0x2000U
#define A733_CPU_PLL_DSU_REG   0x3000U

//
// Manual: CCU and CPU PLL chapters, PLL register table.  The order matches
// the PLL tables in A733CcuLib.c.
//
typedef enum {
  A733_CCU_PLL_DDR = 0,       // CCU   + 0x0020
  A733_CCU_PLL_PERI0,         // CCU   + 0x00A0
  A733_CCU_PLL_PERI1,         // CCU   + 0x00C0
  A733_CCU_PLL_GPU0,          // CCU   + 0x00E0
  A733_CCU_PLL_VIDEO0,        // CCU   + 0x0120
  A733_CCU_PLL_VIDEO1,        // CCU   + 0x0140
  A733_CCU_PLL_VIDEO2,        // CCU   + 0x0160
  A733_CCU_PLL_CPU_BACK,      // CPU PLL block + 0x0000
  A733_CCU_PLL_CPU_L,         // CPU PLL block + 0x1000
  A733_CCU_PLL_CPU_B,         // CPU PLL block + 0x2000
  A733_CCU_PLL_CPU_DSU,       // CPU PLL block + 0x3000
  A733_CCU_PLL_MAX
} A733_CCU_PLL;

//
// Manual: CPU PLL chapter, CPU_[BACK|L|B|DSU]_PLL_CTRL.
// P and M0 are value + 1, N is the raw field for L/B/DSU.
//
#define A733_CPU_PLL_EN           BIT31
#define A733_CPU_PLL_LDO_EN       BIT30
#define A733_CPU_PLL_LOCK_EN      BIT29
#define A733_CPU_PLL_LOCK         BIT28
#define A733_CPU_PLL_OUTPUT_GATE  BIT27
#define A733_CPU_PLL_UPDATE       BIT26
#define A733_CPU_PLL_P_SHIFT      16U
#define A733_CPU_PLL_P_WIDTH      4U
#define A733_CPU_PLL_N_SHIFT      8U
#define A733_CPU_PLL_N_WIDTH      8U
#define A733_CPU_PLL_M0_SHIFT     20U
#define A733_CPU_PLL_M0_WIDTH     2U

//
// Manual: CPU PLL chapter, the clock select register that every CTRL
// register has at +0x1C (CPU_L_CLK at 0x0887101C, CPU_DSU_CLK at
// 0x0887301C).  17:16 is P as a power of two (00 = 1, 01 = 2, 10 = 4),
// the other three dividers are value + 1.
//
#define A733_CPU_CLK_SEL_OFFSET  0x001CU
#define A733_CPU_CLK_SEL_SHIFT   24U
#define A733_CPU_CLK_SEL_WIDTH   3U
#define A733_CPU_CLK_P_SHIFT     16U
#define A733_CPU_CLK_P_WIDTH     2U
#define A733_CPU_CLK_N_SHIFT     8U
#define A733_CPU_CLK_N_WIDTH     2U
#define A733_CPU_CLK_M1_SHIFT    2U
#define A733_CPU_CLK_M1_WIDTH    2U
#define A733_CPU_CLK_M_SHIFT     0U
#define A733_CPU_CLK_M_WIDTH     2U

//
// Only CPU_DSU_PLL is a verified encoding of the select field: CPU_L_CLK
// reads 0x03010000 and CPU_DSU_CLK reads 0x03010105, both naming the DSU
// PLL, and 988 MHz / P(2) is the 494 MHz the port measures.  The remaining
// encodings are not documented in the excerpts we hold, so the library
// refuses to name them instead of guessing.
//
#define A733_CPU_CLK_SEL_DSU_PLL  3U

#endif