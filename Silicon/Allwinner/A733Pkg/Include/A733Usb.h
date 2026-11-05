/** @file
  USB 2.0 host port register map for the Allwinner A733 (sun60iw2p1).

  Manual: memory map, "HIGH-SPEED INTERFACE": USB0 is
  0x04100000---0x041FFFFF and USB1 is 0x04200000---0x042FFFFF, one megabyte
  each.  A window holds four 0x400 blocks, in this order:

    USBn + 0x0000  the USB2.0 host controller's EHCI registers (manual 18.6;
                   EHCI 1.0 layout, so their offsets are the specification's,
                   not the SoC's)
    USBn + 0x0400  its OHCI companion (manual 18.6; OHCI 1.0a layout)
    USBn + 0x0800  the PHY's HCI-side registers, described below

  0x0420000 and 0x0420400 are the board device tree's ehci1/ohci1 nodes, and
  0x0420800 is exactly the window it gives the usbphy node as "pmu1"
  (phy@4100400: reg-names phy_ctrl/pmu0/pmu1 = 0x4100400/0x4101800/0x4200800).
  The arithmetic is the same one port down - EHCI0 at 0x04101000 + 0x800 is
  that node's "pmu0" - so the two descriptions agree about where this block is.

  Provenance.  The CCU side of the bring-up is transcribed from the manual and
  needs no more than that (see <A733Ccu.h>).  The PHY side is not in the manual
  at all: chapter 18.6, "USB2.0 HOST", is a two-page overview with a block
  diagram and no register list, and no other chapter documents the 0x800 block
  of a USBn window.  The offsets and bits below therefore come from two places,
  and each one says which:

  - the generic Allwinner USB PHY programming (mainline's "pmu" map: AHB/INCR
    configuration in the block's first word, SIDDQ in the word at +0x10), which
    is the sequence every sunxi SoC with this PHY needs;
  - live register state of a working Linux boot on this board, for the three
    values the generic sequence does not cover: the AHB burst bits this SoC
    adds, the PHY tune word, and the UTMI control word.

  Values from the second source are marked "board-verified" below and name what
  is observed when they are missing, so the next reader can re-derive them
  instead of trusting them.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_USB_H__
#define A733_USB_H__

#include <Uefi.h>

//
// Manual: memory map, USB1 window, and the offset of each 0x400 block inside it.
//
#define A733_USB1_BASE        0x04200000U
#define A733_USB1_EHCI_BASE   (A733_USB1_BASE + 0x0000U)
#define A733_USB1_OHCI_BASE   (A733_USB1_BASE + 0x0400U)
#define A733_USB1_HCI_BASE    (A733_USB1_BASE + 0x0800U)
#define A733_USB_BLOCK_SIZE   0x0400U

//
// The window the SEC page tables must cover before any of the three blocks is
// touched: 4 KB holds all of them, and the manual's megabyte is not needed.
//
#define A733_USB1_MMIO_BASE   A733_USB1_BASE
#define A733_USB1_MMIO_SIZE   0x1000U

//
// USB0, the OTG port next door (manual memory map: USB0 is 0x04100000---
// 0x041FFFFF).  It holds one block more than USB1 does, and everything is one
// 0x1000 further in because the MUSB core occupies the first block: device tree
// sun60i-a733.dtsi puts the MUSB core at 0x04100000 (usb@4100000,
// "allwinner,sun55i-a733-musb"), EHCI0 at 0x04101000 and OHCI0 at 0x04101400,
// and the PHY's HCI window at 0x04101800 - which is that tree's usbphy "pmu0",
// the same relationship USB1's 0x0420800 has to its "pmu1".  The survey reads
// EHCI0 and the MUSB core and writes neither.
//
#define A733_USB0_BASE        0x04100000U
#define A733_USB0_EHCI_BASE   (A733_USB0_BASE + 0x1000U)
#define A733_USB0_OHCI_BASE   (A733_USB0_BASE + 0x1400U)
#define A733_USB0_HCI_BASE    (A733_USB0_BASE + 0x1800U)

//
// MUSB core registers, byte-wide, at the base of the USB0 window (Mentor's
// core register map, the one every "musb" driver uses; the device tree node
// above names the core, the map is the core's own).  Only three are read,
// because between them they answer whether a hub behind the OTG port is
// visible: FADDR says whether the core has been addressed at all, POWER says
// whether it is in host mode, and DEVCTL says whether it sees a session and
// what its VBUS comparator makes of the port's 5 V.
//
// The map itself - and the fact that this instantiation does not put the
// register file where the classic MUSB core does - lives in <A733Musb.h>,
// beside the device-mode driver that programs the same core.  It is included
// rather than repeated because the two readers must agree: the survey here
// and the driver there address one register file.
//
#include <A733Musb.h>

//
// EHCI 1.0 register offsets (specification chapter 2), the ones the controller
// identification below reads.  The operational registers that follow them are
// reached at CAPLENGTH + their offset, which is why CAPLENGTH is read first.
//
#define A733_EHCI_CAPLENGTH_REG    0x00U
#define A733_EHCI_HCSPARAMS_REG    0x04U
#define A733_EHCI_HCCPARAMS_REG    0x08U
#define A733_EHCI_USBCMD_REG       0x00U
#define A733_EHCI_USBSTS_REG       0x04U
#define A733_EHCI_PORTSC_REG       0x44U
//
// PORTSC's current-connect bit (specification 2.3.9, bit 0), its port-owner bit
// (bit 13) and its line-state field (bits 11:10): together they are what says
// whether the PHY sees anything on the port, and - when the owner bit is set -
// whether what it sees belongs to the OHCI companion instead of the EHCI.
//
#define A733_EHCI_PORTSC_CCS          BIT0
#define A733_EHCI_PORTSC_LINE_SHIFT   10U
#define A733_EHCI_PORTSC_OWNER        BIT13

//
// The rest of PORTSC that the port claim needs: port enable (bit 2) and port
// reset (bit 8) say whether a port is in the middle of something a write must
// not disturb, and bits 1, 3 and 5 are the specification's write-1-to-clear
// change bits - a read-modify-write has to leave them zero or it acknowledges a
// status change nobody has read.
//
#define A733_EHCI_PORTSC_PE           BIT2
#define A733_EHCI_PORTSC_PR           BIT8
#define A733_EHCI_PORTSC_CHANGE_MASK  0x0000002AU

//
// CONFIGFLAG (specification 2.3.7; operational register offset 0x40) and its
// Configured Flag.  While CF is clear, every port belongs to the companion
// controller, which is the state a controller nobody has configured yet is in,
// and is why PORTSC.PortOwner reads 1 out of reset.  Setting CF routes the
// ports to the EHCI controller; software's own initialisation sequence does
// exactly that as its last step, and MdeModulePkg's EhcInitHC does too.
//
#define A733_EHCI_CONFIGFLAG_REG      0x40U
#define A733_EHCI_CONFIGFLAG_ROUTE    BIT0

//
// HCSPARAMS fields: root port count (bits 3:0) and companion controller count
// (bits 11:8).  The latter is non-zero on this SoC - the OHCI companion is what
// PORTSC.PortOwner has to point at - and it is worth printing next to the port
// state so a claim that does nothing can be told from a claim that was never
// needed.
//
#define A733_EHCI_HCSPARAMS_NPORTS_MASK  0x0000000FU
#define A733_EHCI_HCSPARAMS_NPCC_SHIFT   8U
#define A733_EHCI_HCSPARAMS_NPCC_MASK    0x0000000FU

//
// PHY-side (HCI) registers, offsets inside A733_USB1_HCI_BASE.
//
// The A733 host firmware addresses the same words from the EHCI block's base, at
// +0x800 and up (its HCI_USB_CTRL_REG = 0x800, HCI_CTRL_3_REG = 0x808,
// HCI_PHY_CTRL_REG = 0x810, HCI_PHY_TUNE_REG = 0x818, HCI_UTMI_CTRL_REG = 0x81C,
// HCI_UTMI_STAT_REG = 0x824, all relative to 0x04101000).  0x04101000 + 0x800 is
// 0x04101800 and 0x04101000 + 0x824 is 0x04101824, so the two maps are one map
// and the offsets below are theirs minus 0x800 - the register numbers in the two
// drivers can be compared directly, and every value taken from theirs (the
// passby word, the tune word, the UTMI control word) lands in the same place.
//
// +0x00 is the AHB master / PHY interface word.  The four generic bits are
// mainline's SUNXI_AHB_* set: maximum-length INCR8 bursts, INCR4 bursts,
// INCRx alignment and the ULPI bypass that selects the on-chip PHY instead of
// an external one.  The two upper bits are what this SoC's own HCI code adds to
// that set; the manual says nothing about them, so they are kept because the
// only firmware known to have this port working sets them, and an AHB master
// configuration is not something to guess at twice.
//
#define A733_USB_HCI_AHB_CTRL       0x00U
#define A733_USB_HCI_AHB_INCR8      BIT10
#define A733_USB_HCI_AHB_INCR4      BIT9
#define A733_USB_HCI_AHB_INCRX      BIT8
#define A733_USB_HCI_ULPI_BYPASS    BIT0
#define A733_USB_HCI_AHB_SUN60IW2   (BIT15 | BIT11)  // this SoC's HCI code adds these
//
// The word the A733 host firmware writes into that register when it enables
// USB0's PHY (its __usb_passby(), HCI0 branch): bits 15 and 11 (this SoC's two
// additions above), INCR4 (9), INCRx align (8) and ULPI bypass (0) - 0x00008A01.
// Bit 10, INCR8, is in the generic mainline AHB set and NOT in this word: the
// only firmware known to bring this port up drives maximum-length bursts in
// units of sixteen, not eight.  The bypass bit is the one that matters for the
// data path - with it clear the PHY's UTMI port is not joined to the controller
// and no packet moves in either direction.
//
// A733MusbDeviceDxe writes this word while PHY0 is held in reset, and then ORs
// INCR8 into it later, in the device-mode passby step - which is exactly what
// the reference does (its host half writes this mask, its device half adds
// INCR8), so the port ends up with 0x00008B01.  An earlier version of this
// driver wrote only the four lower bits and recorded that the reference word
// "moved the port into a state where the host never saw a connect"; that
// measurement was taken on a build that closed the host half and re-pulsed
// PHY_RSTN before the device half ran, so it tested that sequence and not these
// two bits.
//
// INCR8 was measured on this board and changed nothing (3.14 MB/s either way),
// so the passby word keeps the reference's bits.
#define A733_USB_HCI_PASSBY_VALUE   \
  (A733_USB_HCI_AHB_SUN60IW2 | A733_USB_HCI_AHB_INCR4 | \
   A733_USB_HCI_AHB_INCRX | A733_USB_HCI_ULPI_BYPASS)

//
// +0x08 holds the controller's third control word; bit 9 forces the port into
// suspend, which keeps the PHY's line receiver parked and hides whatever is
// plugged in.  It has to be clear for a host port.
//
#define A733_USB_HCI_CTRL3          0x08U
#define A733_USB_HCI_CTRL3_FORCE_SUSPEND  BIT9

//
// +0x10 is the PHY control word; bit 3 is SIDDQ, "PHY powered down", and is 1
// out of reset.  Clearing it is what the generic Allwinner sequence does on
// every SoC with this PHY, and it is the single write without which the
// controller has no UTMI clock and no port state at all.
//
#define A733_USB_HCI_PHY_CTRL       0x10U
#define A733_USB_HCI_PHY_SIDDQ      BIT3

//
// +0x18 is the PHY tune word and +0x1C a UTMI control word.  The manual
// documents neither: both values are what a working Linux boot on this board
// leaves in those registers (the tune word is the vendor kernel's
// "aw,phy_tune_param"), recorded from that kernel's register state.  Without the
// tune word the high-speed signal levels are wrong, which shows up as an
// enumeration that starts and then fails a data phase rather than as silence.
//
#define A733_USB_HCI_PHY_TUNE       0x18U
#define A733_USB_HCI_PHY_TUNE_VALUE 0x063338C6U
#define A733_USB_HCI_UTMI_CTRL      0x1CU
#define A733_USB_HCI_UTMI_CTRL_VALUE 0x00000053U
// Read-only companion of the UTMI control word: it is logged, never written,
// because a zero here means the PHY interface never came up.
#define A733_USB_HCI_UTMI_STAT      0x24U

//
// PCK600 power domain "USB2" (id 8 in the SoC's power-domain description: the
// domains are 0x1000 apart from the block base, a write of 0x8 to +0x0 switches
// the domain on and +0x8 holds its status in the low nibble).  Manual: memory
// map gives the PPU 0x07060000---0x07078FFF.  The per-domain register layout
// itself is not in the manual; it comes from the SoC's power-domain
// description, and the driver only ever switches a domain on, never off.
//
#define A733_PPU_BASE            0x07060000U
#define A733_PPU_USB2_DOMAIN     8U
#define A733_PPU_DOMAIN_STRIDE   0x1000U
#define A733_PPU_PWR_OFFSET      0x0000U
#define A733_PPU_STATUS_OFFSET   0x0008U
#define A733_PPU_COMMAND_ON      0x8U
#define A733_PPU_STATUS_ON       0x8U
#define A733_PPU_STATUS_MASK     0xFU
#define A733_PPU_WINDOW_SIZE     0x00020000U  // covers the whole documented block
//
// The PCK600 block's bus clock is CLK_R_PPU in the always-on R_CCU: until that
// gate is open no PCK600 register answers, and a domain switch written through
// a gated block is silently dropped.  Manual 4.2.5.17 gives S_PPU_BGR_REG's
// default as 1, so the gate is already open out of reset - the driver opens it
// once more and reads it back anyway, because a step-0 line that does not show
// the gate cannot tell a powered-down domain from a gated one.  That read is
// only harmless while the window is mapped, which is why A733PlatformLib maps
// this page: an unmapped window is a page fault, not a device that stays quiet.
//
#define A733_R_CCU_BASE          0x07010000U
#define A733_R_CCU_SIZE          0x400U    // block window (device tree r_ccu reg)
#define A733_R_CCU_R_PPU_GATE    0x01ACU
#define A733_R_CCU_R_PPU_GATE_BIT BIT0

//
// The USB2 PHY itself: 0x06B00000, the "aw-phy" the manual's block diagram
// shows shared by USB0's device and host halves, laid out as four per-port
// groups of 0x20 bytes plus a common group at 0.  Port 0 is USB0's.
//
//   +0x00        common ISCR   - line state, VBUS verdicts, and the
//                                FORCE_VBUS_VALID field at bits 13:12 that
//                                tells the SIE the rail is good.
//   +0x10+0x20n  port n PHY_CTRL  - SIDDQ at bit 3 (1 = analog powered down)
//                                plus the PHY's tuning fields.
//   +0x18+0x20n  port n PHYTUNE   - the same tune word the HCI window carries.
//
// The values are the A733 host firmware's (phy-sunxi-plat.c's phy_u2_set, as
// its USB driver applies them to USB0 before the port is handed over): SIDDQ
// released with the host tune bits, the tune word, and VBUS forced valid.  A
// PHY whose port is still in SIDDQ has no UTMI clock to give, however correct
// the rest of the port's programming is - which is why this block is part of
// powering USB0's PHY and not a detail of the host path.
//
// The window is a page of MMIO like any other on this platform and is mapped by
// A733PlatformLib (its A733_USB2_PHY_MAP_* entry, base and size from here), so
// touching it is a register access and not a page fault.
//
#define A733_USB2_PHY_BASE        0x06B00000U
#define A733_USB2_PHY_WINDOW_SIZE 0x800U
#define A733_USB2_PHY_ISCR         0x0000U
#define A733_USB2_PHY_ISCR_FORCE_VBUS_VALID (0x3U << 12)
#define A733_USB2_PHY_CTRL(n)      (0x0010U + 0x20U * (n))
#define A733_USB2_PHY_TUNE(n)      (0x0018U + 0x20U * (n))
#define A733_USB2_PHY_PORT_USB0    0U
#define A733_USB2_PHY_CTRL_SIDDQ   BIT3
#define A733_USB2_PHY_CTRL_HOST_ON 0x000E2430U  // SIDDQ=0 + host tune (live)
#define A733_USB2_PHY_TUNE_HOST    0x143338D6U  // the vendor's phy_tune_param

//
// SYSCFG (0x03000000 - the same 4 KB window as the SRAMC word, see
// A733Musb.h): the 200-ohm resistance calibration for the USB2 PHYs, per port.
//
//   +0x160  CAL_EN (bit 0, 1 = manual trim), PHY_o_RES200_SEL(n) at bit 4+n,
//           and PHY_o_RES200_TRIM_SEL at bit 10.
//   +0x164  PHY_o_RES200_TRIM(n), one byte per port: 0xC8 is the manual value,
//           an all-zero byte hands the trim to the calibration engine.
//
// Enabling calibration for port n is therefore "clear CAL_EN, set RES200_SEL(n),
// clear that port's TRIM byte", which is what the A733 host firmware's
// usb_phyx_res_cal() does for USB0 and USB1.  A wrong termination trim degrades
// the high-speed eye and shows up as transaction errors, not as silence.
//
#define A733_SYSCFG_BASE              0x03000000U
#define A733_SYSCFG_RESCAL_CTRL_REG   0x0160U
#define A733_SYSCFG_RES0_CTRL_REG     0x0164U
// +0x168 carries the same trim bytes for the second resistor bank; the A733 host
// firmware additionally clears bits 15:8 of it while it powers the PHYs up (the
// vendor phy driver's phy_rescal_set_v2), so the calibration loop drives that
// byte too instead of the manual's default.
#define A733_SYSCFG_RES1_CTRL_REG     0x0168U
#define A733_SYSCFG_CAL_EN            BIT0
#define A733_SYSCFG_RES200_SEL(n)     (BIT4 << (n))
#define A733_SYSCFG_RES200_TRIM_MASK(n) ((UINT32)0xFFU << (8U * (n)))
#define A733_SYSCFG_RES200_TRIM_SEL   BIT10

#endif