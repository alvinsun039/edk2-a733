/** @file
  Register map for the Allwinner A733 (sun60iw2p1) USB3.1 GEN2 DRD: the
  DesignWare DWC3 core, its application register block and the SerDes
  subsystem the core's PHY sits behind.

  Manual: 18.4 "USB3.1 GEN2 DRD".  The manual gives the block's address, its
  register classes and its application registers; it points at the DesignWare
  core's own programming guide for the xHCI/global/device register windows,
  which is where the offsets in the second half of this header come from.  Both
  sources are named per item below, because the two are read differently: a
  value from the manual is a documented register, a value from the databook is
  the core's interface, and a value from the board is what this silicon needs.

  What the block is on this board.  One USB3 receptacle (a Type-A jack whose
  SuperSpeed pair is COMB0's lane 0, driven by the DRD) and one USB2 port (U2P0,
  the femtoPHY whose registers are the application block at 0x06B00000).  The
  board's three USB2 Type-A ports and its USB WiFi module hang off one 4-port
  hub, and that hub's upstream port is this controller: its USB2 half is on
  U2P0, its SuperSpeed half on the COMB0 lane.  Both halves have to be up for
  the hub to appear, which is why this header describes COMB0's link registers,
  its PIPE lane map and its PMA register file alongside the core's.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_DWC3_H__
#define A733_DWC3_H__

#include <Uefi.h>

//
// Manual 18.4.4.1 "USB3.1 GEN2 DRD Controller Base Address": the controller is
// one megabyte at 0x06A00000 (the manual's memory map gives the same window,
// "USB3.1 GEN2 DRD 0x06A00000---0x06AFFFFF").  The whole megabyte has to be
// mapped before it is touched: the xHCI registers sit at the bottom, the
// core's global registers at +0xC100, and an unmapped read is a translation
// fault, not a device that stays quiet.
//
#define A733_DWC3_BASE          0x06A00000U
#define A733_DWC3_WINDOW_SIZE   0x00100000U

//
// Manual 18.4.4.2 "Register Class" - the three windows inside that megabyte:
//
//   0x00000 - 0x07FFF  xHCI 1.1 registers (the eXtensible Host Controller
//                      Interface specification, revision 1.1 - so their offsets
//                      are the specification's, not the SoC's)
//   0xC100 - 0xC6FF    the DWC3 core's global registers (DesignWare "Enhanced
//                      SuperSpeed USB3.1 Controller Program Guide", 1.90a)
//   0x100000 - 0x1007FF the application registers (18.4.5 below)
//
// The application window is the same address the USB2.0 PHY's registers have
// been known by in this tree: 0x06A00000 + 0x100000 is 0x06B00000, which
// <A733Usb.h> names A733_USB2_PHY_BASE.  Both names describe one register file;
// the manual files it under the DRD, which is why the offsets below are the
// manual's U2P0_* names.
//
#define A733_DWC3_XHCI_OFFSET     0x00000000U
#define A733_DWC3_GLOBAL_OFFSET   0x0000C100U
#define A733_DWC3_APP_BASE        (A733_DWC3_BASE + 0x00100000U)
#define A733_DWC3_APP_SIZE        0x00000800U

//
// DWC3 global registers.  Not in the manual - manual 18.4.4.2 hands this window
// to the DesignWare program guide - so the offsets and bit positions below are
// the core's interface, taken from the databook's register map as it appears in
// two independent implementations of the same core (U-Boot's
// include/linux/usb/dwc3.h struct offsets, and the vendor kernel's
// drivers/usb/dwc3/core.h), and each one is checked against a read-back at
// run time: GSNPSID identifies the core, GCTL and GUSB2PHYCFG0 are printed with
// the field values decoded, so a wrong offset cannot pass silently.
//
#define A733_DWC3_GCTL             0x0000C110U
#define A733_DWC3_GSNPSID          0x0000C120U
#define A733_DWC3_GUSB2PHYCFG(n)   (0x0000C200U + 4U * (n))
#define A733_DWC3_GUSB3PIPECTL(n)  (0x0000C2C0U + 4U * (n))

//
// GSNPSID: "U3" (0x5533) in the upper half, the core's revision below it.  A
// block whose clock or bus gate is closed reads 0 or 0xffffffff here, which is
// the cheapest test of whether the core answered at all.
//
#define A733_DWC3_GSNPSID_MASK     0xFFFF0000U
//
// Two identities, both valid: 0x5533 is the DWC3 of USB 3.0 era cores, 0x3331
// is the USB 3.1 GEN2 one - which is what this SoC reports (GSNPSID reads
// 0x33313130, i.e. "3110" as ASCII).  Accepting only the older one made the
// driver refuse a core that was sitting there answering perfectly.
//
#define A733_DWC3_GSNPSID_U3       0x55330000U
#define A733_DWC3_GSNPSID_U31      0x33310000U
#define A733_DWC3_GSNPSID_REV_MASK 0x0000FFFFU

//
// GCTL.  Bit assignments from the databook; the composite value to aim for is
// the one a working Linux boot on this board leaves in the register
// (0x30C11005), which is also what the vendor's own EDK2 bring-up targets.
//
//   CORESOFTRESET bit 11  the core's own reset; assert for the length of the
//                         PHY reset dance, then release
//   PRTCAPDIR    13:12    role: 1 = host, 2 = device, 3 = OTG.  The core
//                         arbitrates its own register view from this field, so
//                         a core left in OTG can drop the xHCI window out from
//                         under a driver that binds later
//   PWRDNSCALE   19:16    the core's internal power-down timebase, scaled for
//                         the 24 MHz suspend clock
//   U2RSTECN     16       USB2 reset-early workaround; set in the live Linux
//                         state and in the vendor's bring-up
//   DSBLCLKGTNG  0        disables the core's internal clock gating.  The
//                         vendor's bring-up found the USB2 root port never
//                         completing speed detection with it clear
//
#define A733_DWC3_GCTL_CORESOFTRESET   BIT11
#define A733_DWC3_GCTL_PRTCAPDIR_SHIFT 12U
#define A733_DWC3_GCTL_PRTCAPDIR_MASK  (0x3U << A733_DWC3_GCTL_PRTCAPDIR_SHIFT)
#define A733_DWC3_GCTL_PRTCAPDIR_HOST  1U
#define A733_DWC3_GCTL_U2RSTECN        BIT16
#define A733_DWC3_GCTL_PWRDNSCALE_SHIFT 19U
// PWRDNSCALE is bits 31:19 (13 wide); written out as the mask itself because
// 0x1FFF << 19 does not fit the field's own width.
#define A733_DWC3_GCTL_PWRDNSCALE_MASK 0xFFF80000U
#define A733_DWC3_GCTL_DSBLCLKGTNG     BIT0
//
// PWRDNSCALE for the 24 MHz suspend clock.  The number itself is board data: it
// is what a working Linux boot reads back on this board (GCTL = 0x30C11005, so
// 0x30C00000 >> 19 = 1560), not a value the databook derives for this clock.
//
//
// PWRDNSCALE counts suspend-clock periods in one 16 kHz period, so a 24 MHz
// suspend clock gives 24000000 / 16000 = 1500 - the value the working firmware
// holds (GCTL 0x2EE11004) and the value Linux's dwc3_set_power_down_clk_scale()
// computes from susp_clk.  1560 was this driver's own arithmetic error and is
// visible in its GCTL (0x30C11005): everything else in that word already
// matched, and the 4% error is enough for the core's internal timers never to
// finish waiting on the clock they are measuring.
//
//
// 1560, not 1500: this is the value live Linux holds on this board, and the
// working firmware's own note is explicit that matching Linux is what makes the
// USB2 port complete speed detection - "live Linux GCTL = 0x30c11005".  The
// 1500 variant (0x2ee11004) is what that firmware prints after its own write,
// and the same note records the consequence: the port never finishes the chirp
// and reports speed 0.
//
#define A733_DWC3_PWRDNSCALE_24MHZ     1560U

//
// LLUCTL (Link Layer Utility Control), at 0xD024.  Not in the manual's globals
// class - manual 18.4.4.2 lists 0xC100-0xC6FF - so the offset is taken from
// the vendor kernel's drivers/usb/dwc3/dwc3-sunxi.h (DWC3_LLUCTL = 0xd024,
// applied by dwc3_hw_init() in dwc3-sunxi-plat.c), and it is verified live on
// this board by the working reference firmware: base+0xD024 reads 0x008B8080
// and accepts writes, while base+0xF24 - where a "globals-space + 0xF24"
// reading of the same register lands - reads constant 0 and ignores writes, a
// dead hole.  The reference's own note records that correction explicitly.
//
// The two bits the BSP kernel names are FORCE_GEN1 (bit 10, force Gen1 speed
// on a Gen2 link) and INV_SYNC_HDR (bit 30, Gen2 polarity detection from the
// data block sync header).  Neither is set in the value this board's working
// boot holds (0x008B8080 - bits 7, 15, 16, 17, 19 and 23), so the write below
// reproduces the SoC boot chain's state rather than applying the quirks the
// device tree's "aw,inv-sync-hdr-quirk" (sun60iw2p1.dtsi) would ask for.
//
#define A733_DWC3_LLUCTL              0x0000D024U
#define A733_DWC3_LLUCTL_INV_SYNC_HDR BIT30
#define A733_DWC3_LLUCTL_FORCE_GEN1   BIT10
//
// The word the working reference firmware reads back on a boot that enumerates
// the hub (its "LLUCTL=0x008B8080" line); the value this driver writes so the
// core holds exactly that state.
//
#define A733_DWC3_LLUCTL_BOARD        0x008B8080U

//
// GUSB2PHYCFG(0) - the USB2.0 port's PHY interface.  The board's device tree
// describes the PHY as UTMI (phy_type = "utmi"), which fixes three fields:
//
//   PHYSOFTRST  bit 31   the DWC3 interface reset towards the PHY
//   USBTRDTIM   13:10    turnaround time; 9 for an 8-bit UTMI+ interface
//                        (the databook's USBTRDTIM_UTMI_8_BIT)
//   PHYIF       bit 3    0 = 8-bit UTMI+, 1 = 16-bit
//   ENBLSLPM    bit 8    LPM disable, per the board's DT quirk
//   SUSPHY      bit 6    PHY suspend; cleared here, because a suspended PHY
//                        reports no line state and the port then shows no
//                        device whatever is plugged into the hub
//
#define A733_DWC3_GUSB2PHYCFG_PHYSOFTRST     BIT31
#define A733_DWC3_GUSB2PHYCFG_USBTRDTIM_SHIFT 10U
#define A733_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK (0xFU << A733_DWC3_GUSB2PHYCFG_USBTRDTIM_SHIFT)
#define A733_DWC3_GUSB2PHYCFG_USBTRDTIM_8BIT (9U << A733_DWC3_GUSB2PHYCFG_USBTRDTIM_SHIFT)
#define A733_DWC3_GUSB2PHYCFG_ENBLSLPM       BIT8
#define A733_DWC3_GUSB2PHYCFG_PHYIF          BIT3
#define A733_DWC3_GUSB2PHYCFG_SUSPHY         BIT6
//
// ULPI_UTMI_SEL is the interface *type* and PHYIF the width inside UTMI+: 0 is
// UTMI+ (8-bit with PHYIF=0, 16-bit with PHYIF=1) and 1 is ULPI, where PHYIF
// chooses between 8-bit and 12-bit ULPI.  Printed because the two fields
// together are what says which PHY interface the USB2 port actually runs on,
// which is not something the DT's "phys" phandle can be trusted for once the
// core is up.
//
#define A733_DWC3_GUSB2PHYCFG_ULPI_UTMI_SEL  BIT4

//
// GUSB3PIPECTL(0) - the SuperSpeed PIPE interface.  Both bits are cleared while
// the core is in host mode, and the reset is then pulsed once more after COMB0
// is up (see A733Xhci2CoreInit), which is what lets the PIPE interface come out
// of reset against a running PMA:
//
//   PHYSOFTRST  bit 31   a PIPE reset left asserted is a reset of the whole
//                        PHY interface the core shares between its ports
//   SUSPHY      bit 17   suspend
//   UX_EXIT_PX  bit 27   "not supposed to be used in normal operation"; the
//                        Linux core clears it unconditionally
//   DEPOCHANGE  bit 18   an RX detection debounce the Linux core leaves alone;
//                        this is the bit UX_EXIT_PX used to be defined as here,
//                        which cleared the wrong one and never cleared the
//                        right one
//
#define A733_DWC3_GUSB3PIPECTL_PHYSOFTRST  BIT31
#define A733_DWC3_GUSB3PIPECTL_UX_EXIT_PX  BIT27
#define A733_DWC3_GUSB3PIPECTL_SUSPHY      BIT17

//
// Manual 18.4.4.3 "Application Register List" - the USB2.0 port's own register
// group at 0x06A00000 + 0x100000 (offsets below are relative to that).
//
#define A733_DWC3_U2P0_ISCR     0x0000U
#define A733_DWC3_U2P0_PHYCTL   0x0010U
#define A733_DWC3_U2P0_PHYTST   0x0014U
#define A733_DWC3_U2P0_PHYTUNE  0x0018U
#define A733_DWC3_U2P0_PHYSTS   0x0024U

//
// U2P0_ISCR (manual 18.4.5.1): the SIE-facing VBUS/ID verdicts.  Bits 13:12
// force the VBUS-valid value the controller is handed - 0b11 forces "valid" -
// and bits 11:10 choose which of the PHY's detectors produces it.  The board's
// VBUS pin is not bonded to this PHY's comparator, so the force is what makes
// the port usable as a host; the vendor's bring-up writes the same field.
//
#define A733_DWC3_U2P0_ISCR_VBUS_FORCE_SHIFT 12U
#define A733_DWC3_U2P0_ISCR_VBUS_FORCE_MASK  (0x3U << A733_DWC3_U2P0_ISCR_VBUS_FORCE_SHIFT)
#define A733_DWC3_U2P0_ISCR_VBUS_FORCE_VALID (0x3U << A733_DWC3_U2P0_ISCR_VBUS_FORCE_SHIFT)

//
// U2P0_PHYCTL (manual 18.4.5.2).  The manual's reset value is 0x00022418 and
// bit 3 - SIDDQ - is 1 there, i.e. the PHY comes out of reset powered down.
//
//   SIDDQ        bit 3    1 = PHY disabled.  This is the switch; a PHY left in
//                         SIDDQ hands the core no UTMI clock at all.
//   VBUSVLDEXTSEL bit 4   which source asserts the D+ pull-up; reset value 1
//   VBUSVLDEXT   bit 5    1 = the VBUS signal is taken as valid
//   OTGDISABLE   bit 10   powers down the VBUS comparator; set out of reset,
//                         and this port never drives VBUS itself
//   FSEL         14:12    PHY reference frequency: 0b010 = 24 MHz
//   RETENABLEN   bit 17, TXBITSTUFFEN bit 18, TXBITSTUFFENH bit 19: tune bits
//                         the board's working state has set
//
#define A733_DWC3_U2P0_PHYCTL_SIDDQ        BIT3
#define A733_DWC3_U2P0_PHYCTL_VBUSVLDEXTSEL BIT4
#define A733_DWC3_U2P0_PHYCTL_VBUSVLDEXT   BIT5
#define A733_DWC3_U2P0_PHYCTL_OTGDISABLE   BIT10
#define A733_DWC3_U2P0_PHYCTL_FSEL_SHIFT   12U
#define A733_DWC3_U2P0_PHYCTL_FSEL_MASK    (0x7U << A733_DWC3_U2P0_PHYCTL_FSEL_SHIFT)
//
// Board-verified host-mode words, not the manual's reset values: this is what a
// working Linux boot on this board leaves in U2P0's PHYCTL and PHYTUNE while
// the port is used as a host, and what the A733 host firmware writes before it
// registers its xHCI.  PHYTUNE's documented reset value is 0x143333D4
// (18.4.5.4); the difference is the transmit/detect tuning the vendor's
// "phy_tune_param" carries for this board.
//
#define A733_DWC3_U2P0_PHYCTL_HOST_ON  0x000E2430U  // SIDDQ=0, host tune (board)
#define A733_DWC3_U2P0_PHYTUNE_HOST    0x143338D6U  // vendor phy_tune_param (board)

//
// Manual 18.2.5 "Register List" (SERDES_SUBSYS_APP at 0x06C00000): the SerDes
// subsystem's own registers.  Everything this driver touches inside it is in
// the first 64 KB - the bus gating/reset word at +0x0008, COMB0's link
// registers at +0x1000 and the PIPE lane map at +0x6100 - and the block's AHB
// clock and reset come from the CCU (0x13C0/0x13C4, see <A733Ccu.h>).  The
// PHY's own register file is a separate window and is mapped separately; see
// <A733_COMBPHY0_BASE>.
//
#define A733_SERDES_BASE           0x06C00000U
#define A733_SERDES_WINDOW_SIZE    0x00010000U
#define A733_SERDES_SUBSYS_USB3P1_BGR  0x0008U
#define A733_SERDES_USB3P1_BGR_ACLK_EN       BIT17  // manual 18.2.6.2
#define A733_SERDES_USB3P1_BGR_HCLK_EN       BIT16  // manual 18.2.6.2
#define A733_SERDES_USB3P1_BGR_U2P0_PHY_RSTN BIT4   // manual 18.2.6.2
//
// Bit 21 of the same word, USB3P1_USB2P0_UTMI_CLK_SEL (manual 18.2.6.2), is
// the one field here that is deliberately left at zero: 0 feeds the DWC3's
// UTMI interface from the USB2.0 PHY - which is the port the hub is on - while
// 1 switches it to the CCU's own UTMI clock, the "USB3.1 only DRD independent"
// mode.  Both the manual and the board's working state agree that the USB2
// port needs 0; there is no case in which this driver would set it.
//
#define A733_SERDES_USB3P1_BGR_U2P0_UTMI_CLK_SEL BIT21

//
// The mux in front of COMB0, at +0x0000 - the one register in this block the
// manual's register list does not name (18.2.5 starts at SUBSYS_PCIE_BGR,
// +0x0004, and 18.2.6 has no description of +0x0000).  It is the vendor BSP's
// HSI_GBLC: bit 0, PHY_USE_SEL, chooses what the SerDes pins of COMB0 carry -
// 0 leaves the PHY on PCIe0, 1 hands it to the USB3.1 DRD (vendor
// bsp/drivers/phy/sunxi-synopsys-combophy.c, combo_phy_mode_set()).
//
// It is not optional once the SuperSpeed lane is to be used: with the mux on
// PCIe0 the DWC3's PIPE interface is left without a lane clock, and the core
// wedges at its first port reset rather than reporting an empty port.  The
// other direction of the mux matters as much - with the bit set and the PHY's
// own PMA still down, the PIPE interface is clocked by a PHY that is not
// running, which is the same wedge from the other side.  So the bit is set
// only as part of the COMB0 bring-up below, and is released again if that
// bring-up finds the PMA not ready.
//
#define A733_SERDES_HSI_GBLC           0x0000U
#define A733_SERDES_HSI_GBLC_PHY_USE_SEL BIT0

//
// COMB0's link registers, manual 18.2.6.5-18.2.6.9.  The manual lists them at
// 0x1000.. of the SERDES_SUBSYS_APP window, i.e. 0x06C01000, which is also the
// base the board's device tree gives the combo-phy0 node (reg entry 0,
// 0x06c01000 length 0xa00 - the link status register at +0x900 is the last one
// in it).
//
#define A733_SERDES_COMBO0_LINK_CTRL0  0x1000U
#define A733_SERDES_COMBO0_LINK_CTRL1  0x1004U
#define A733_SERDES_COMBO0_LINK_CTRL2  0x1008U
#define A733_SERDES_COMBO0_LINK_CTRL3  0x100CU
#define A733_SERDES_COMBO0_LINK_STUS0  0x1900U

//
// CTRL0 (manual 18.2.6.5).  RESET_N resets the whole PHY except its APB
// registers; PIPE_LINK_RESET_N_SOFT resets the USB link and its lanes, again
// except the APB registers; DP_LINK_RESET_N_SOFT is the same for a DisplayPort
// link, which this board does not bond.  TYPEC_CONN_DIR must be static while
// RESET_N is de-asserted, and 0 is the normal orientation the jack's SuperSpeed
// pair is wired to (schematic: COMB0-TX0/RX0 -> USB_SSTX0/USB_SSRX0).
//
#define A733_SERDES_COMBO0_CTRL0_RESET_N              BIT0
#define A733_SERDES_COMBO0_CTRL0_PIPE_LINK_RESET_N    BIT4
#define A733_SERDES_COMBO0_CTRL0_DP_LINK_RESET_N      BIT8
#define A733_SERDES_COMBO0_CTRL0_TYPEC_CONN_DIR       BIT12

//
// CTRL1 (manual 18.2.6.6): which reference clock each PMA PLL and the PHY's
// digital side run from, and how that clock is terminated and divided.
//
// All three selects are set to 1, i.e. pma_cmn_refclk_int - the PHY's internal
// oscillator, fed by the 24 MHz DCXO gate this driver opens in the RTC block
// <A733_RTC_CCU_DCXO_SERDES0> - rather than cmn_refclk_<p/m>, the differential
// REF-CLK pins, which this board leaves unconnected (COMB0-REF-CLKP/N appear on
// the SoC side only).  With the internal reference in use the matching fields
// are the ones the manual states for it: REFCLK_TERM_EN stays 0 (termination
// is for the differential input), DIG_DIV stays 0 (divide by 1, the 19.2-27 MHz
// range the manual gives for it), and INT_MODE stays at the reset value.  The
// fields are set individually and the rest of the word is left as found, so a
// boot chain that has already chosen a reference keeps it.
//
#define A733_SERDES_COMBO0_CTRL1_REFCLK_DIG_SEL    BIT0
#define A733_SERDES_COMBO0_CTRL1_REFCLK_TERM_EN    BIT16
#define A733_SERDES_COMBO0_CTRL1_PLL0_REFCLK_SEL   BIT20
#define A733_SERDES_COMBO0_CTRL1_PLL1_REFCLK_SEL   BIT24

//
// CTRL2 (manual 18.2.6.7): IDDQ_EN_SOFT is the PMA's analog power switch -
// "all analog logic powered down" - and IDDQ_EN_SEL decides who drives it: 1
// selects this soft bit, 0 selects the manual's "SYS" source, "controlled by the
// SoC through GPIO in test mode".  Both come out of reset 0, so out of reset the
// PMA's power state is not this register's decision at all, and clearing
// IDDQ_EN_SOFT while SEL stays 0 writes to a switch nothing is listening to.
//
// On this die SEL does not hold what is written to it: a boot-time write of
// 0b10 to this register reads back 0x00000000, so the "select the soft control"
// combination cannot be reached from software at all, and the PMA's analog
// domain is left to the SoC's own source.  That is the state the vendor's
// power-on path leaves (it clears both bits, bsp combo0_usb_pwr_set(false)) and
// the state the working reference firmware runs, so both bits are cleared here
// and nothing else in the word is touched - a full-word write would also clear
// the BSCAN selects and the debug lane field that share it.
//
#define A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SOFT  BIT0
#define A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SEL   BIT1

//
// CTRL3 (manual 18.2.6.8): PLLCLK_EN_LN starts the data-rate clock in the PMA
// for the lane, and POWER_STATE_REQ_LN (9:4, one-hot) asks for a link power
// state - A0 is 0x1 (Tx/Rx active) and is one of the two states the manual
// requires to be requested after reset release.
//
#define A733_SERDES_COMBO0_CTRL3_PLLCLK_EN_LN          BIT0
#define A733_SERDES_COMBO0_CTRL3_POWER_STATE_REQ(State) ((State) << 4)
#define A733_SERDES_COMBO0_CTRL3_POWER_STATE_A0        1U

//
// STUS0 (manual 18.2.6.9): the PMA's own verdicts.  CMN_READY is the one that
// says the common block - reference, PLLs, bias - is up; CLOCK_EN_ACK echoes
// PLLCLK_EN_LN and POWER_STATE_ACK (13:8) the requested power state.  All three
// are read-only, so the read-back after the sequence is the PHY's answer and
// not this driver's own write returned.
//
#define A733_SERDES_COMBO0_STUS0_CMN_READY        BIT0
#define A733_SERDES_COMBO0_STUS0_CLOCK_EN_ACK     BIT4
#define A733_SERDES_COMBO0_STUS0_POWER_STATE_ACK_SHIFT 8U
#define A733_SERDES_COMBO0_STUS0_POWER_STATE_ACK_MASK  (0x3FU << A733_SERDES_COMBO0_STUS0_POWER_STATE_ACK_SHIFT)

//
// The PIPE lane map, manual 18.2.6.25/18.2.6.26 at +0x6100/+0x6104.  Both come
// out of reset 0xF, which the manual defines as reserved - i.e. the DWC3's PIPE
// interface is mapped to no PHY lane at all until these are written.  0 selects
// COMB0's lane 0 (the clock map's own wording: "COMB0 PHY Lane0 or Lane3 PIPE
// CLK", and lane 1..2 for the RX data map), which is the lane wired to the
// jack.
//
#define A733_SERDES_USB3P1_PIPE_CLK_MAP_CTRL  0x6100U
#define A733_SERDES_USB3P1_PIPE_RXD_MAP_CTRL  0x6104U
#define A733_SERDES_USB3P1_PIPE_MAP_MASK      0x0000000FU
#define A733_SERDES_USB3P1_PIPE_MAP_LANE0     0x00000000U

//
// The serdes node's second register window - the device tree's "serdes top
// register part-2" (reg entry 1, 0x06c06000 length 0x2000) - which is a
// different window from the subsystem's own register list at 0x06C00000.  Only
// one register in it concerns the USB3 lane this driver brings up: the raw
// SerDes transmit data-width conversion control at +0x0004, whose enable and
// lane-run/reset bits the vendor's PHY reset sequence ORs in on every reset
// release (bsp/drivers/phy/sunxi-cadence-combophy.c, combo0_usb_phy_reset()).
// The PIPE lane map is NOT here - it is a subsystem register at +0x6100/+0x6104
// (above) - and an earlier version of this driver wrote those map values into
// this window by mistake.
//
#define A733_SERDES_COMBO_REG              0x6000U
#define A733_SERDES_DP_RAWS_TXDW_CONV_CTRL 0x0004U
#define A733_SERDES_DP_RAW_DATA_EN         BIT0
#define A733_SERDES_DP_RAW_LN_EN_MASK      (0xFU << 4)
#define A733_SERDES_DP_RAW_LN_RSTN_MASK    (0xFU << 8)

//
// COMBPHY0's register file, manual memory map "COMBPHY0 0x06C80000---0x06C9FFFF"
// and the board's device tree (combo-phy0's second reg entry, 0x06c80000 length
// 0x20000).  The manual gives the window and nothing else: these are the Cadence
// PMA's own 16-bit registers, so every access here is 16-bit and a register is
// addressed at "base + (offset << 1)" - the offset is the PMA's register number,
// not a byte offset, which is why the address of the last one this driver reads
// (0xC008) is 0x06C98010 and not 0x06C8C008.
//
#define A733_COMBPHY0_BASE         0x06C80000U
#define A733_COMBPHY0_WINDOW_SIZE  0x00020000U

//
// PMA register 0xC008 bits 11:8: the vendor's PLL re-lock hook.  When the
// PMA's common block does not report ready after reset release, the vendor BSP
// clears these four bits, waits, and polls CMN_READY again
// (bsp/drivers/phy/sunxi-cadence-combophy.c, combo0_usb_phy_reset()).  The
// manual does not describe this register - the whole COMBPHY0 window is the
// PMA's - so the field is the vendor's, used here as the same last-resort
// retry and reported as such if it is what makes the PMA come up.
//
#define A733_COMBPHY0_PMA_PLL_RELOCK    0xC008U
#define A733_COMBPHY0_PMA_PLL_RELOCK_MASK (0xFU << 8)

//
// RTC domain (manual memory map: RTC 0x07090000---0x07090FFF).  The vendor
// kernel's clock driver (ccu-sun60iw2-rtc.c) puts the 24 MHz reference gate for
// COMB0's PLLs at +0x16C, bit 4 - the manual's RTC chapter does not list it, so
// this offset is the vendor's, and the block is one the non-secure side may
// touch (Linux's own RTC driver runs there).  It is opened by the COMB0
// bring-up below, because that PHY's PLLs are fed from it: with the gate closed
// pma_cmn_refclk_int has no source and the PMA never reports ready.
//
#define A733_RTC_CCU_BASE      0x07090000U
#define A733_RTC_CCU_WINDOW_SIZE 0x00001000U
#define A733_RTC_CCU_DCXO_SERDES   0x016CU
#define A733_RTC_CCU_DCXO_SERDES0  BIT4

//
// xHCI register offsets this driver reads for its log, from the xHCI
// specification (revision 1.1, chapter 5): the capability registers at the
// bottom of the window and the operational ones at CAPLENGTH.
//
// Why CAPLENGTH matters even though the manual's register-class table
// (18.4.4.2) puts the whole xHCI window at 0x00000-0x07FFF: the capability
// registers are at offset 0 of that window and the operational register set
// starts CAPLENGTH bytes in, so the port registers are at
// "base + CAPLENGTH + 0x400 + 0x10 * (n - 1)" - spec 5.4.8 (PORTSC: "one
// register per port, 10h apart, starting at 400h of the operational register
// space"), not at a bare 0x400.  MdeModulePkg's XhciDxe does the same
// (XhciReg.h's XHC_PORTSC_OFFSET 0x0400 is added to Xhc->CapLength by
// XhcReadOpReg), so the two agree with the manual's class table.
//
#define A733_XHCI_CAPLENGTH     0x0000U
#define A733_XHCI_HCSPARAMS1    0x0004U
#define A733_XHCI_HCCPARAMS1    0x0010U
#define A733_XHCI_USBCMD        0x0000U
#define A733_XHCI_USBSTS        0x0004U
#define A733_XHCI_PORTSC        0x0400U  // first port register set, inside the
                                         // operational set; 0x10 per port
#define A733_XHCI_PORTSC_STRIDE 0x0010U
#define A733_XHCI_USBSTS_CNR    BIT11    // controller not ready
#define A733_XHCI_USBSTS_HCH    BIT0     // host controller halted
#define A733_XHCI_USBCMD_RS     BIT0     // run/stop
#define A733_XHCI_USBCMD_HCRST  BIT1     // host controller reset
//
// HCSPARAMS1 (xHCI 5.3.6) packs three unrelated counts into one word: bits 7:0
// are MaxSlots, bits 18:16 MaxIntrs and bits 31:24 MaxPorts.  This core reports
// 0x02000140, which is 64 slots and 2 ports - reading bits 7:0 as the port
// count made the bring-up log walk six ports the core does not implement, whose
// PORTSC reads back whatever the bus leaves there, and report a connect on one
// of them.  The port count this controller actually has is the one XhciDxe
// derives from the same word.
//
#define A733_XHCI_HCSPARAMS1_MAXSLOTS_MASK   0x000000FFU
#define A733_XHCI_HCSPARAMS1_MAXPORTS_MASK   0xFF000000U
#define A733_XHCI_HCSPARAMS1_MAXPORTS_SHIFT  24U
#define A733_XHCI_PORTSC_CCS    BIT0     // current connect status
#define A733_XHCI_PORTSC_PED    BIT1     // port enabled
#define A733_XHCI_PORTSC_PR     BIT4     // port reset - write 1 resets the port
#define A733_XHCI_PORTSC_PLS_SHIFT  5U   // port link state, 4 bits
#define A733_XHCI_PORTSC_PLS_MASK   0xFU
#define A733_XHCI_PORTSC_PP     BIT9     // port power
#define A733_XHCI_PORTSC_SPEED_SHIFT 10U // port speed, 4 bits (3 = high speed)
#define A733_XHCI_PORTSC_SPEED_MASK  0xFU
#define A733_XHCI_PORTSC_LWS    BIT16    // link state write strobe
#define A733_XHCI_PORTSC_CSC    BIT17    // connect status change (write 1 clears)
#define A733_XHCI_PORTSC_PEC    BIT18    // port enabled change         "  "
#define A733_XHCI_PORTSC_WRC    BIT19    // warm port reset change      "  "
#define A733_XHCI_PORTSC_OCC    BIT20    // over-current change         "  "
#define A733_XHCI_PORTSC_PRC    BIT21    // port reset change           "  "
#define A733_XHCI_PORTSC_PLC    BIT22    // port link state change      "  "
#define A733_XHCI_PORTSC_CEC    BIT23    // port config error change    "  "
#define A733_XHCI_PORTSC_WPR    BIT31    // warm port reset - write 1 resets the port
//
// The bits of PORTSC that may not be written back as read.  The change bits
// are RW1C, PED is RW1CS (writing 1 disables the port), PR and WPR are RW1S
// (writing 1 resets the port).  Only PP is ever set, so a read-modify-write of
// PP masks this whole set out of the value it writes; a plain write-back of the
// read value would clear every pending change and disable a live port.
//
#define A733_XHCI_PORTSC_NO_WRITE_MASK \
  (A733_XHCI_PORTSC_CSC | A733_XHCI_PORTSC_PEC | A733_XHCI_PORTSC_WRC | \
   A733_XHCI_PORTSC_OCC | A733_XHCI_PORTSC_PRC | A733_XHCI_PORTSC_PLC | \
   A733_XHCI_PORTSC_CEC | A733_XHCI_PORTSC_PED | A733_XHCI_PORTSC_PR | \
   A733_XHCI_PORTSC_WPR)

//
// Extended capabilities (spec 1.1 chapter 7).  HCCPARAMS1's xECP field is a
// dword offset from the register base, and each capability's Next field is a
// dword offset to the one after it, so the list is walked in bytes as
// "offset + 4 * next".
//
// The Supported Protocol capability is the only place that says which root
// port number belongs to USB2 and which to USB3: its major/minor revision, its
// port offset and count, and (when PSIC is non-zero) its protocol speed ID
// list, whose PSIV values are what PORTSC's speed field reports.  That is why
// the port log prints these entries rather than assuming "port 1 is USB2".
//
#define A733_XHCI_HCCPARAMS1_PPC         BIT3   // port power control
#define A733_XHCI_HCCPARAMS1_XECP_SHIFT  16U
#define A733_XHCI_HCCPARAMS1_XECP_MASK   0xFFFF0000U
//
// The xHCI window is 0x8000 bytes wide on this SoC (manual 18.4.4.2), so an
// extended-capability pointer outside it is a misread and not an offset.
//
#define A733_XHCI_WINDOW_SIZE            0x00008000U
#define A733_XHCI_EXT_CAP_ID(v)          ((v) & 0xFFU)
#define A733_XHCI_EXT_CAP_NEXT(v)        (((v) >> 8) & 0xFFU)
#define A733_XHCI_EXT_CAP_ID_SUPPORTED_PROTOCOL  0x02U
#define A733_XHCI_SUPPROT_MAJREV_SHIFT   24U      // DW0, 8 bits
#define A733_XHCI_SUPPROT_MINPORT_SHIFT  16U      // DW0, 8 bits, 1-based
#define A733_XHCI_SUPPROT_MINREV_MASK    0x000000FFU  // DW1 bits 7:0
#define A733_XHCI_SUPPROT_PSIC_SHIFT     8U       // DW1 bits 15:8
#define A733_XHCI_SUPPROT_PSIC_MASK      0x0000FF00U
#define A733_XHCI_SUPPROT_NAME_SHIFT     16U      // DW1 bits 31:16, two UTF-16 chars
#define A733_XHCI_SUPPROT_PORTOFF_MASK   0x000000FFU  // DW2 bits 7:0, 1-based
#define A733_XHCI_SUPPROT_PORTCOUNT_SHIFT 8U      // DW2 bits 15:8
#define A733_XHCI_SUPPROT_PORTCOUNT_MASK 0x0000FF00U
//
// One protocol speed ID dword per PSIC entry, at "capability + 12 + 4 * i".
//
#define A733_XHCI_PSI_PSIV_MASK          0x0000000FU
#define A733_XHCI_PSI_PSIE_SHIFT         4U
#define A733_XHCI_PSI_PLT                BIT6
#define A733_XHCI_PSI_PFD                BIT7
//
// How many extended capabilities the walk follows before it stops.  A sane
// list is under ten entries; the cap is what keeps a corrupted Next pointer
// from turning one log line into an unbounded MMIO scan.
//
#define A733_XHCI_LOG_MAX_EXT_CAPS       16U
//
// How many Supported Protocol entries the port report keeps.  This core has two
// (one USB2, one USB3).
//
#define A733_XHCI_LOG_MAX_PROTOCOLS      4U
//
// How many ports the bring-up log reads back.  A DWC3 has one or two; the cap
// exists so a core that reports a nonsensical count cannot turn one log line
// into 255 MMIO reads.
//
#define A733_XHCI_LOG_MAX_PORTS  8U

//
// USBSTS.CNR is the specification's own gate on the operational registers
// (xHCI 1.2 4.2.2 and 5.3.2): it is set while the controller is not ready, and
// software must wait for it to read back 0 before writing any operational or
// runtime register.  XhciDxe asserts on the flag rather than waiting for it, so
// the wait belongs to whoever owns the reset that set it - here the DWC3 core
// soft reset.  100 ms in 100 us steps is far longer than a DWC3 core takes to
// finish its post-reset initialisation, so a timeout is a finding, not a tune.
//
#define A733_XHCI_CNR_DELAY_US  100U
#define A733_XHCI_CNR_TRIES     1000U

#endif