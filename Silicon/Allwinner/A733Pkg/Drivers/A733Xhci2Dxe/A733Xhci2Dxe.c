/** @file
  USB3.1 GEN2 DRD (DesignWare DWC3) host bring-up for the Allwinner A733
  (sun60iw2p1), on the port the board's USB2 hub hangs off.

  This driver is the SoC-side glue only.  It opens the core's clocks and resets,
  takes the USB2.0 port's PHY out of power-down, runs the DWC3 core's own reset
  and puts it in host mode, and then publishes the controller as a
  non-discoverable PCI device so that MdeModulePkg's stock XhciDxe binds to it.
  UsbBusDxe, UsbKbDxe and UsbMassStorageDxe sit above that and are not this
  driver's business.  Nothing here implements USB.

  What the port is.  The A733's USB3.1 GEN2 DRD (manual 18.4) is one xHCI 1.1
  controller behind a DWC3 core, with two ports: a SuperSpeed lane through the
  SerDes subsystem's COMB0 PHY, and one USB2.0 port through its own femtoPHY
  ("U2P0", whose register block is the controller's application window at
  0x06B00000).  The board's three USB2 Type-A ports plus the on-board USB WiFi
  module hang off one 4-port hub, and that hub's upstream port is this
  controller.  The hub's USB2 half is on U2P0 and its SuperSpeed half on
  COMB0's lane 0 (the schematic's USB_SSTX0/USB_SSRX0, to the USB3 Type-A
  receptacle).  Which of the two the hub actually enumerates over is not
  something to assume: the port report at the end of this file says what each
  root port sees, and a boot that reports an empty SuperSpeed port while U2P0
  is powered and detecting is a boot where the hub is not terminating on the
  USB2 lane.  Both PHYs are therefore brought up here, and the report decides.
  The two EHCI controllers next door are separate silicon with their own root
  ports; the board's three Type-A ports are not theirs, which is why both read
  back ccs=0 on an empty root port.

  Order, and why it is that order:

  1. PCK600's USB2 power domain on, if it is not already.  The femtoPHY's
     registers are in it and a gated domain reads back as zero, so nothing
     written to U2P0 before this survives.

  2. CCU: the USB subsystem's AHB master gate, CLK_MSI_LITE2 with its resets,
     CLK_RES_DCAP_24M, the pll-peri0 output gates, and then the DRD's own clock
     group (manual 4.1.6.199-4.1.6.204): the USB2 PHY's 24 MHz reference, the
     suspend clock, the microframe clock, the UTMI clock, and USB2's block reset
     released.  Offsets and fields are <A733Ccu.h>'s.

  3. CCU: the SerDes subsystem's register clock (manual 4.1.6.208, 100 MHz from
     PERI0_600M/6) and its block reset (4.1.6.209).  Without these the DWC3's
     AHB slave is bus-gated, and a gated slave does not report itself: GCTL
     reads zero and every write is dropped.

  4. The 24 MHz reference gate for COMB0's PLLs in the RTC block (0x07090000 +
     0x16C bit 4).  Vendor-kernel offset, not the manual's; see <A733Dwc3.h>.

  5. SerDes subsystem: the DWC3's AXI and AHB clocks (manual 18.2.6.2 bits 17
     and 16) and the USB2.0 PHY's reset released (bit 4).  Bit 21 of that word -
     which UTMI clock the core's USB2 side is fed from - is deliberately left
     clear: 0 takes it from the femtoPHY, which is the port the hub is on.

  6. The board, before any port detector runs: the hub's own 3.3 V rail
     (PMIC DCDC1, net VCC_3V3_USB20HUB - the hub has no enable pin and no
     GPIO reset, so the rail is the whole of its power control) is read, and
     switched on if its bit reads clear; then the R_PIO pin that switches the
     5 V of the Type-A ports behind the hub.  Both are logged with their
     read-back.  Nothing here waits for the hub: the report at the end says
     whether it terminated.

  7. U2P0: SIDDQ cleared with the board's host-mode tune word, the tune word,
     and the SIE's VBUS verdict forced valid.  A PHY still in SIDDQ hands the
     core no UTMI clock, and then the core's own reset never completes.

  8. COMB0, the SuperSpeed lane's PHY: the mux that hands it to the USB3.1 DRD,
     the PIPE lane map, the PMA out of IDDQ and onto its internal 24 MHz
     reference, its programmed lane/PLL state, the PHY and link reset release,
     the lane's data-rate clock, the A0 power state, and the poll for the PMA's
     own CMN_READY verdict.  A PHY that does not answer is logged and stepped
     over rather than hung on.  The function's header has the order and why.
     This comes after U2P0 because that is the order the working firmware on
     this board brings the two up in, and because the poll at the end of it is
     the one step here that a half-awake PHY block fails.

  9. The DWC3 core: read GSNPSID, then the reset dance the core requires -
     CORESOFTRESET asserted, the USB2 and USB3 PHY interface resets pulsed, and
     the core released last - and then PRTCAPDIR, GUSB3PIPECTL0, GUSB2PHYCFG0
     and the rest of GCTL, programmed for host mode on an 8-bit UTMI interface.
     This is U-Boot's dwc3_core_init() order with this board's own values, and
     the SuperSpeed interface's reset is released once, with COMB0 already
     running behind it.

  10. Publish the controller without touching USBCMD.  The host-controller
      reset (USBCMD.HCRST) and the run/stop switch belong to the stock
      XhciDxe when it binds, exactly as in the working firmware
      (edk2-a733 X17/X23): the SERDES UTMI clock select is clear the whole
      time, so its HCRST runs on the femtoPHY clock and completes, and the
      controller it starts is the one whose USB2 port can see the wire.
      Doing HCRST here with the CCU clock selected and handing a running
      core to XhciDxe was measured to make the driver's own HCRST time out
      and lose the xHCI register view.

  What this driver also does is read the capability registers back after the
  core reset, so the log says whether the core came out of reset in a state a
  host driver can bind to - a CAPLENGTH of zero is what a core that is still
  arbitrating (or still bus-gated) reports, and XhciDxe asserts on it.

  Everything is logged at DEBUG_INFO, one line per step, each with the read-back
  of the word it wrote: the CCU group, the SerDes word, the COMB0 mux and PIPE
  lane map, the PMA's programmed state and its own CMN_READY verdict, the U2P0
  words, GCTL / GUSB2PHYCFG0 / GUSB3PIPECTL0 with their fields decoded and the
  PHY interface each port is on, the xHCI capability registers, the Supported
  Protocol capabilities (which say which root port is USB2 and which is USB3,
  with their revisions, port ranges and speed IDs), and then one block per root
  port: raw PORTSC with CCS, PED, PLS, PP, speed, CSC and PRC, taken after the
  port has been powered and given the specification's 20 ms.  A boot whose log
  stops early is one where the step before the last line faulted or hung; a
  boot that finishes says whether the controller came up, whether COMB0's PMA
  answered, and whether either port - and so whether the hub - reports a
  connect, and at what speed.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733GpioLib.h>
#include <Library/A733PmicLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <A733Ccu.h>
#include <A733Dwc3.h>
#include <A733Rpio.h>
#include <A733Usb.h>

//
// Time allowed for the femtoPHY to come out of SIDDQ and for its PLL to lock.
// The manual gives no figure; 1 ms is the same settle A733UsbHostDxe spends on
// the USB2.0 host port's PHY next door.
//
#define A733_XHCI2_PHY_SETTLE_US  1000U

//
// How long the core and its PHY interface resets are held.  This is the
// databook's own figure ("may need at least 50 ms, wait up to 100 ms"), and it
// is the value U-Boot's dwc3_core_soft_reset() holds on both sides of the
// PHY reset pulse.
//
#define A733_XHCI2_RESET_HOLD_US  100000U

//
// Time allowed for the USB2 power domain to report itself on.
//
#define A733_XHCI2_PPU_TRIES    100U
#define A733_XHCI2_PPU_POLL_US  100U

//
// How long after the last CCU gate before the serdes registers are touched:
// the SerDes subsystem's reset was just released, and its register file is not
// readable while it is settling.
//
#define A733_XHCI2_SERDES_SETTLE_US  50U

//
// COMB0's PMA bring-up: how long each step is held, and how long the PHY is
// given to answer.  The two holds are the vendor BSP's own figures for this PHY
// (combo0_usb_phy_reset(): 1 ms with RESET_N asserted, 1 ms again after it is
// released, and 1 ms after PLLCLK_EN before the power state is requested).  The
// polls are bounded so that a PHY which never answers costs a few milliseconds
// and a log line rather than the boot.
//
#define A733_XHCI2_COMBO0_RESET_HOLD_US  1000U
#define A733_XHCI2_COMBO0_SETTLE_US      1000U
#define A733_XHCI2_COMBO0_ACK_TRIES      1000U
#define A733_XHCI2_COMBO0_ACK_POLL_US    1U
#define A733_XHCI2_COMBO0_READY_TRIES    1000U
#define A733_XHCI2_COMBO0_READY_POLL_US  5U
#define A733_XHCI2_COMBO0_RELOCK_DELAY_US 10000U


//
// How long after the hub's 3.3 V rail is switched on before the hub can be
// expected to terminate the upstream link: the rail's own ramp (device tree
// regulator-enable-ramp-delay, 1 ms), the hub's 12 MHz crystal start, and its
// internal power-on reset.  USB 2.0 allows a hub 100 ms for bPwrOn2PwrGood
// (7.1.7.3); 20 ms is the same order as the port power allowance above and
// covers this part with margin.
//
#define A733_XHCI2_HUB_POWER_SETTLE_US  20000U

//
// One Supported Protocol capability, as the port report needs it: the root
// port range it covers (1-based, inclusive) and the USB generation it names.
//
typedef struct {
  UINT32  FirstPort;
  UINT32  LastPort;
  UINT32  Major;
  UINT32  Minor;
  UINT32  Psic;
} A733_XHCI2_PROTOCOL;

/**
  Address a register inside the CCU window.

  @param[in] Offset  Register offset inside the CCU window.

  @return The MMIO address of the register.
**/
STATIC
UINTN
Xhci2CcuReg (
  IN UINT32  Offset
  )
{
  return (UINTN)PcdGet64 (PcdA733CcuBase) + Offset;
}

/**
  Address a register inside the DWC3's application window (U2P0).

  @param[in] Offset  Register offset inside that window.

  @return The MMIO address of the register.
**/
STATIC
UINTN
Xhci2AppReg (
  IN UINT32  Offset
  )
{
  return (UINTN)A733_DWC3_APP_BASE + Offset;
}

/**
  Address a register inside the SerDes subsystem's register window.

  @param[in] Offset  Register offset inside that window, as the manual's
                     register list numbers it.

  @return The MMIO address of the register.
**/
STATIC
UINTN
Xhci2SerdesReg (
  IN UINT32  Offset
  )
{
  return (UINTN)A733_SERDES_BASE + Offset;
}

/**
  Address a register of COMB0's PMA register file.

  The PMA's registers are 16 bits wide and are numbered, not byte-addressed:
  register n lives at "base + (n << 1)".  Every access through this helper is
  therefore a 16-bit MMIO access, and passing a byte offset instead of a
  register number reads the neighbouring register.

  @param[in] Register  PMA register number.

  @return The MMIO address of the register.
**/
STATIC
UINTN
Xhci2CombphyReg (
  IN UINT16  Register
  )
{
  return (UINTN)A733_COMBPHY0_BASE + ((UINTN)Register << 1);
}

/**
  Switch the PCK600 USB2 power domain on if it is not already on.

  The femtoPHY's registers are in this domain: with it off they read back as
  zero, and every U2P0 write below would be discarded.  Nothing is switched
  off - the power domain outlives this firmware, and the OS's own power-domain
  driver takes its reference count from there.

  A733UsbHostDxe and A733MusbDeviceDxe switch the same domain on for their own
  ports, so in the usual dispatch order this is a read and no write at all.

  @retval EFI_SUCCESS  The domain reports itself powered on.
  @retval EFI_TIMEOUT  It did not, and the PHY registers cannot be expected to
                       hold what is written to them.
**/
STATIC
EFI_STATUS
A733Xhci2PowerDomainOn (
  VOID
  )
{
  UINTN   Base;
  UINTN   Tries;
  UINT32  Status;

  Base   = (UINTN)A733_PPU_BASE + (UINTN)A733_PPU_USB2_DOMAIN * A733_PPU_DOMAIN_STRIDE;
  Status = MmioRead32 (Base + A733_PPU_STATUS_OFFSET);
  if ((Status & A733_PPU_STATUS_MASK) == A733_PPU_STATUS_ON) {
    DEBUG ((DEBUG_INFO, "A733Xhci2: USB2 power domain already on (status 0x%08x)\n", Status));
    return EFI_SUCCESS;
  }

  MmioWrite32 (Base + A733_PPU_PWR_OFFSET, A733_PPU_COMMAND_ON);

  for (Tries = 0; Tries < A733_XHCI2_PPU_TRIES; Tries++) {
    MicroSecondDelay (A733_XHCI2_PPU_POLL_US);
    Status = MmioRead32 (Base + A733_PPU_STATUS_OFFSET);
    if ((Status & A733_PPU_STATUS_MASK) == A733_PPU_STATUS_ON) {
      DEBUG ((DEBUG_INFO, "A733Xhci2: USB2 power domain switched on (status 0x%08x)\n", Status));
      return EFI_SUCCESS;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "A733Xhci2: USB2 power domain did not come up (status 0x%08x); the PHY will not answer\n",
    Status
    ));
  return EFI_TIMEOUT;
}

/**
  Give the DRD its clocks and take its blocks out of reset.

  The first four writes are the USB subsystem's bus and PHY clocks, the same set
  A733MusbDeviceDxe opens for the OTG port's half of the subsystem (see its
  MusbSubsysClocksOn, which documents why each is needed): the keyed AHB master
  gate, CLK_MSI_LITE2 with its AHB/MBUS resets, CLK_RES_DCAP_24M - the mclk of
  the USB2 PHY window - and the pll-peri0 output gates, without which a divider
  whose source output is closed falls back to SYS_CLK24M and says nothing.

  Then the DRD's own group (manual 4.1.6.199-4.1.6.204).  Each word is a
  read-modify-write that writes the fields this port owns and leaves the rest of
  the boot chain's bits alone, and each is read back after the write.

  The microframe and UTMI clocks keep the source and divider the boot chain
  leaves (SYS_CLK24M, M = 1); only their gates are opened.  That is the
  configuration the board's working firmware runs them at.  The USB2 PIPE clock
  is not touched at all: this port's UTMI clock comes from the femtoPHY (see
  USB3P1_USB2P0_UTMI_CLK_SEL in <A733Dwc3.h>), and the host firmware leaves
  that gate closed.
**/
STATIC
VOID
A733Xhci2CcuOn (
  VOID
  )
{
  UINT32  Value;

  //
  // Keyed AHB master gate: USB's gate is bit 9 and the low three bits are the
  // key, not gates.  OR-ing the key lights six unrelated bus masters, so they
  // are cleared after the OR - which is why this is a read-modify-write.
  //
  Value  = MmioRead32 (Xhci2CcuReg (A733_CCU_AHB_GATE_REG));
  Value |= A733_CCU_AHB_GATE_USB_SYS;
  Value &= ~(UINT32)A733_CCU_AHB_GATE_STRAY;
  MmioWrite32 (Xhci2CcuReg (A733_CCU_AHB_GATE_REG), Value);

  MmioOr32 (Xhci2CcuReg (A733_CCU_MSI_LITE2_REG), A733_CCU_MSI_LITE2_ON);
  MmioOr32 (Xhci2CcuReg (A733_CCU_RES_DCAP_REG), A733_CCU_RES_DCAP_USB_PHY_GATE);
  MmioOr32 (Xhci2CcuReg (A733_CCU_PLL_PERI0_REG), A733_CCU_PLL_PERI0_OUT_GATES);
  MicroSecondDelay (10);

  //
  // USB2_U2_REF: the femtoPHY's 24 MHz reference.  The PHY's FSEL field reads
  // 24 MHz out of reset (manual 18.4.5.2: FSEL 0b010), so the source is
  // SYS_CLK24M and not the 26 MHz DCXO.
  //
  MmioAnd32 (Xhci2CcuReg (A733_CCU_USB2_U2_REF_CLK), ~(UINT32)A733_CCU_USB2_CLK_SRC_MASK);
  MmioOr32 (Xhci2CcuReg (A733_CCU_USB2_U2_REF_CLK), A733_CCU_CLK_GATING);

  //
  // USB2_SUSPEND: 24 MHz from SYS_CLK24M, M = 1.  This is also the clock the
  // core's PWRDNSCALE field is scaled against.
  //
  MmioAnd32 (Xhci2CcuReg (A733_CCU_USB2_SUSPEND), ~(UINT32)A733_CCU_USB2_CLK_FACTOR_M_MASK);
  MmioOr32 (
    Xhci2CcuReg (A733_CCU_USB2_SUSPEND),
    A733_CCU_CLK_GATING | A733_CCU_USB2_SUSPEND_SRC_SYS_CLK24M
    );

  //
  // USB2_MF and USB2_U3_UTMI: gate only, source and divider as found.
  //
  //
  // 0x81000104, not the gate alone.  These two clocks feed the USB2 side of the
  // core - mf is its 60 MHz bus clock and utmi its PHY interface clock - and the
  // gate with no divider leaves them at 24 MHz, which is the configuration the
  // working firmware's own A/B records as "the port never completes speed
  // detection after reset" (PORTSC stuck, ISCR chirping J/K, hub never seen).
  // 0x81000104 is the value that A/B settled on: gate, source and the divider
  // that lands on 60 MHz.
  //
  //
  // Gate only: 24 MHz from sys24M, which is the rate Linux runs these on
  // (usb2-mf = 24000000, mux 0) and the rate the hub enumerates at.  The 60 MHz
  // mux-1 path is not what this board uses, and the CCU's glitchless switch
  // rejects that mux anyway (the divider sticks, the mux does not).
  //
  MmioOr32 (Xhci2CcuReg (A733_CCU_USB2_MF), A733_CCU_CLK_GATING);
  MmioOr32 (Xhci2CcuReg (A733_CCU_USB2_U3_UTMI_CLK), A733_CCU_CLK_GATING);
  MicroSecondDelay (10);

  //
  // USB2's block reset, released last: a block that leaves reset with no clock
  // drives nothing.
  //
  MmioOr32 (Xhci2CcuReg (A733_CCU_RST_USB_2), A733_CCU_USB2_RST_BIT);
  MicroSecondDelay (10);

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: CCU AHB=0x%08x MSI_LITE2=0x%08x RES_DCAP=0x%08x PERI0=0x%08x\n",
    MmioRead32 (Xhci2CcuReg (A733_CCU_AHB_GATE_REG)),
    MmioRead32 (Xhci2CcuReg (A733_CCU_MSI_LITE2_REG)),
    MmioRead32 (Xhci2CcuReg (A733_CCU_RES_DCAP_REG)),
    MmioRead32 (Xhci2CcuReg (A733_CCU_PLL_PERI0_REG))
    ));
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: CCU u2_ref=0x%08x (gate=%u src=%u) suspend=0x%08x (gate=%u) "
    "mf=0x%08x (gate=%u) utmi=0x%08x (gate=%u) pipe=0x%08x RST_USB2=0x%08x (released=%u)\n",
    MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_U2_REF_CLK)),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_U2_REF_CLK)) & A733_CCU_CLK_GATING) != 0,
    (UINT32)((MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_U2_REF_CLK)) & A733_CCU_USB2_CLK_SRC_MASK)
             >> A733_CCU_USB2_CLK_SRC_SHIFT),
    MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_SUSPEND)),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_SUSPEND)) & A733_CCU_CLK_GATING) != 0,
    MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_MF)),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_MF)) & A733_CCU_CLK_GATING) != 0,
    MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_U3_UTMI_CLK)),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_U3_UTMI_CLK)) & A733_CCU_CLK_GATING) != 0,
    MmioRead32 (Xhci2CcuReg (A733_CCU_USB2_U2_PIPE_CLK)),
    MmioRead32 (Xhci2CcuReg (A733_CCU_RST_USB_2)),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_RST_USB_2)) & A733_CCU_USB2_RST_BIT) != 0
    ));
}

/**
  Give the SerDes subsystem its register clock and reset, and open the 24 MHz
  reference gate its PLLs run from.

  The clock is manual 4.1.6.208: PERI0_600M through M = 6 is the 100 MHz the
  subsystem's registers are clocked at, and the block reset (4.1.6.209) is
  released after the clock is up.  Both are read back.

  The DCXO gate is the vendor kernel's (RTC + 0x16C bit 4, see <A733Dwc3.h>):
  24 MHz to COMB0's PLLs.  It is opened here, before any serdes register is
  touched, because COMB0's bring-up below selects that internal reference for
  both PMA PLLs and the PHY's digital side; with the gate closed pma_cmn_refclk_int
  has no source and the PMA never reports ready.  Opening a gate a working boot
  chain has already opened costs nothing, and the read-back in the line below is
  what says whether it was open or this step opened it.
**/
STATIC
VOID
A733Xhci2SerdesOn (
  VOID
  )
{
  UINT32  Value;

  //
  // SERDES_PHY_CFG: source PERI0_600M, M = 6 (FACTOR_M = 5), then the gate.
  // Configured before the gate so the subsystem never runs at an intermediate
  // rate.
  //
  MmioAnd32 (
    Xhci2CcuReg (A733_CCU_SERDES_PHY_CFG),
    ~(UINT32)(A733_CCU_USB2_CLK_SRC_MASK | A733_CCU_USB2_CLK_FACTOR_M_MASK)
    );
  MmioOr32 (
    Xhci2CcuReg (A733_CCU_SERDES_PHY_CFG),
    A733_CCU_SERDES_PHY_CFG_SRC_PERI0_600M | 5U
    );
  MmioOr32 (Xhci2CcuReg (A733_CCU_SERDES_PHY_CFG), A733_CCU_CLK_GATING);
  MicroSecondDelay (10);

  MmioOr32 (Xhci2CcuReg (A733_CCU_RST_BUS_SERDES), A733_CCU_SERDES_RST_BIT);
  MicroSecondDelay (A733_XHCI2_SERDES_SETTLE_US);

  Value = MmioRead32 ((UINTN)A733_RTC_CCU_BASE + A733_RTC_CCU_DCXO_SERDES);
  MmioOr32 ((UINTN)A733_RTC_CCU_BASE + A733_RTC_CCU_DCXO_SERDES, A733_RTC_CCU_DCXO_SERDES0);

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: CCU serdes_phy_cfg=0x%08x (gate=%u mux=%u M=%u) RST_BUS_SERDES=0x%08x "
    "(released=%u) dcxo_serdes was 0x%08x now 0x%08x (comb0 gate=%u)\n",
    MmioRead32 (Xhci2CcuReg (A733_CCU_SERDES_PHY_CFG)),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_SERDES_PHY_CFG)) & A733_CCU_CLK_GATING) != 0,
    (UINT32)((MmioRead32 (Xhci2CcuReg (A733_CCU_SERDES_PHY_CFG)) & A733_CCU_USB2_CLK_SRC_MASK)
             >> A733_CCU_USB2_CLK_SRC_SHIFT),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_SERDES_PHY_CFG)) & A733_CCU_USB2_CLK_FACTOR_M_MASK) + 1U,
    MmioRead32 (Xhci2CcuReg (A733_CCU_RST_BUS_SERDES)),
    (MmioRead32 (Xhci2CcuReg (A733_CCU_RST_BUS_SERDES)) & A733_CCU_SERDES_RST_BIT) != 0,
    Value,
    MmioRead32 ((UINTN)A733_RTC_CCU_BASE + A733_RTC_CCU_DCXO_SERDES),
    (MmioRead32 ((UINTN)A733_RTC_CCU_BASE + A733_RTC_CCU_DCXO_SERDES) &
     A733_RTC_CCU_DCXO_SERDES0) != 0
    ));

  //
  // The SerDes subsystem's own gate word (manual 18.2.6.2): AXI and AHB clocks
  // to the DWC3, and - in the BSP style the working firmware's CCU init uses -
  // bit 21 set: the core's USB2-side UTMI clock taken from the CCU while the
  // combo PHY is brought up.  That select is handed back to the femtoPHY after
  // COMB0 is up and before the DWC3 reset dance (the X17 single-variable
  // change), and the U2P0 PHY reset is released inside A733Xhci2Combo0On, as
  // in the BSP combo_usb2_clk_set sequence.
  //
  Value  = MmioRead32 ((UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR);
  Value |= A733_SERDES_USB3P1_BGR_ACLK_EN |
           A733_SERDES_USB3P1_BGR_HCLK_EN |
           A733_SERDES_USB3P1_BGR_U2P0_UTMI_CLK_SEL;
  MmioWrite32 ((UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR, Value);
  MicroSecondDelay (10);

  Value = MmioRead32 ((UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR);
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: SERDES+0x0008=0x%08x (aclk=%u hclk=%u u2p0_phy_rstn=%u utmi_clk_sel=%u)\n",
    Value,
    (Value & A733_SERDES_USB3P1_BGR_ACLK_EN) != 0,
    (Value & A733_SERDES_USB3P1_BGR_HCLK_EN) != 0,
    (Value & A733_SERDES_USB3P1_BGR_U2P0_PHY_RSTN) != 0,
    (Value & A733_SERDES_USB3P1_BGR_U2P0_UTMI_CLK_SEL) != 0
    ));
}

//
// One write into COMB0's PMA register file: the register number (the PMA's
// numbering, see <Xhci2CombphyReg>) and the 16-bit value.
//
typedef struct {
  UINT16  Register;
  UINT16  Value;
} A733_XHCI2_COMBO0_WRITE;

//
//
// The PMA's programmed state for this board's SuperSpeed lane, in the order the
// PHY wants it written.  Where these come from, and what they are not:
//
//   Nothing here is the manual's.  Manual 18.2.5 gives COMBPHY0's window and no
//   register list at all - the file inside it is the Cadence PMA's, and its
//   register meanings are the PMA's datasheet, not this SoC's.  The values are
//   the ones the vendor's own PHY driver programs for a normal-orientation
//   Type-C/USB3 link on this SoC (bsp/drivers/phy/sunxi-cadence-combophy.c,
//   combo0_configure_usb_dp(), the udp_supported path: the A7A is a TYPEC_DP=1
//   board, and that path is the state its working firmware runs the PHY in,
//   with ssc_en = 0).  They are the same values the tree's working reference
//   firmware writes, register for register and in the same order.
//
//   The lane groups are the PMA's own numbering: 0x40xx and 0x41xx are TX lane
//   0, 0x42xx/0x43xx TX lane 1, 0x80xx/0x81xx RX lane 0, 0x82xx/0x83xx RX lane
//   1, 0x44xx..0x47xx and 0x84xx..0x87xx lanes 2 and 3, 0xC0xx the PMA's own
//   control block, and 0x00xx/0x01xx the common block and the two PLLs.
//
//   Lanes 2 and 3 are here even though this board bonds neither: the vendor
//   configures all four lanes of the PMA on every udp_supported bring-up, and
//   an earlier version of this driver dropped them on the reasoning that an
//   unconnected lane needs no configuration.  That version never got
//   CMN_READY - the common block's own ready verdict - while the full table
//   does, so the lanes' analog blocks are part of what the common block waits
//   for and the omission is not a saving.  Nothing is written twice for its own
//   sake either: the writes a later one supersedes (0x4100, 0x8200, 0xE005) are
//   kept because the order is the vendor's, and collapsing it would assume the
//   PMA has no sequencing requirements.
//
//   The table is cut into five here only because the vendor's sequence writes
//   two of COMB0's own link registers in the middle of it (LINK_CTRL1 after the
//   first group, LINK_CTRL0 after the second); the five calls below run in the
//   vendor's order and nothing else happens between them.
//
//   What fails without the block as a whole: the PMA never reports CMN_READY,
//   the PIPE interface has no lane clock, the DWC3's USBSTS.CNR never clears,
//   and the xHC's ports stay in RxDetect with no connect.
//
//   The two 0xE005/0xE006 pairs are worth naming: the value written first
//   (0x0003/0x1124) is also where the vendor's DP-only path stops, and the one
//   written after the lanes are configured (0x000B/0x2224) is the pair the USB3
//   path ends on, so it is the last word the PMA gets about which protocol the
//   lane is carrying.  Both are kept, in order.
//
STATIC CONST A733_XHCI2_COMBO0_WRITE  mCombo0LaneLinkPma[] = {
  { 0xE005, 0x0003 },
  { 0xE006, 0x1124 },
  { 0x4100, 0x00FF },
  { 0x4300, 0x00FF },
  { 0x4102, 0x04AE },
  { 0x4302, 0x04AE },
  { 0x4103, 0x04AE },
  { 0x4303, 0x04AE },
  { 0x8000, 0x0000 },
  { 0x8200, 0x091D },
  { 0x8002, 0x0000 },
  { 0x8202, 0x0900 },
  { 0x8003, 0x0000 },
  { 0x8203, 0x0000 },
  { 0x8006, 0x0000 },
  { 0x8206, 0x010F },
  { 0x40EA, 0x00FF },
  { 0x42EA, 0x00FF },
  { 0x8108, 0x0000 },
  { 0x8308, 0x0009 },
  { 0x8110, 0x0000 },
  { 0x8310, 0x0009 },
  { 0x8118, 0x0000 },
  { 0x8318, 0x000D },
  { 0x4040, 0x2A84 },
  { 0x4240, 0x2A84 },
  { 0x40C6, 0x00A3 },
  { 0x42C6, 0x00A3 },
  { 0x4050, 0x0000 },
  { 0x4250, 0x0000 },
  { 0x404C, 0x001C },
  { 0x424C, 0x001C },
};

//
// The two PLLs' dividers and charge-pump settings (0x00xx/0x01xx) and the
// remaining per-lane fields for lanes 0 and 1, whose power state, reference and
// TX amplitude the group above had just set.
//
STATIC CONST A733_XHCI2_COMBO0_WRITE  mCombo0PllPma[] = {
  { 0x0022, 0x001A },
  { 0x002A, 0x0034 },
  { 0x002C, 0x00DA },
  { 0x0032, 0x0034 },
  { 0x0034, 0x00DA },
  { 0x0064, 0x0082 },
  { 0x0065, 0x0082 },
  { 0x0074, 0x001A },
  { 0x0104, 0x0020 },
  { 0x0105, 0x0007 },
  { 0x010C, 0x0020 },
  { 0x010D, 0x0007 },
  { 0x0114, 0x030C },
  { 0x0115, 0x0007 },
  { 0x0124, 0x0007 },
  { 0x0125, 0x0003 },
  { 0x0126, 0x000F },
  { 0x0128, 0x0132 },
  { 0x8044, 0x001A },
  { 0x8244, 0x001A },
  { 0x8444, 0x001A },
  { 0x8644, 0x001A },
  { 0x8045, 0x0082 },
  { 0x8245, 0x0082 },
  { 0x8445, 0x0082 },
  { 0x8645, 0x0082 },
  { 0x804C, 0x001A },
  { 0x824C, 0x001A },
  { 0x844C, 0x001A },
  { 0x864C, 0x001A },
  { 0x804D, 0x0082 },
  { 0x824D, 0x0082 },
  { 0x844D, 0x0082 },
  { 0x864D, 0x0082 },
  { 0x4123, 0x0A28 },
  { 0x4323, 0x0A28 },
  { 0x4523, 0x0A28 },
  { 0x4723, 0x0A28 },
  { 0x01A1, 0x8600 },
  { 0x01C1, 0x0701 },
  { 0x44E5, 0x0012 },
  { 0x46E5, 0x0012 },
  { 0x40E6, 0x0000 },
  { 0x42E6, 0x0000 },
  { 0x40E7, 0x0001 },
  { 0x42E7, 0x0001 },
  { 0x40E5, 0x0012 },
  { 0x42E5, 0x0012 },
  { 0x40E5, 0x0041 },
  { 0x42E5, 0x0041 },
  { 0x44E6, 0x0001 },
  { 0x46E6, 0x0001 },
  { 0x44E7, 0x0001 },
  { 0x46E7, 0x0001 },
  { 0x44E5, 0x0019 },
  { 0x46E5, 0x0019 },
};

//
// The common block's rate fields and the per-lane timings that are shared by
// both lanes; the vendor's sequence sets TYPEC_CONN_DIR right after them.
//
STATIC CONST A733_XHCI2_COMBO0_WRITE  mCombo0CommonRatePma[] = {
  { 0x0094, 0x0004 },
  { 0x00D4, 0x0004 },
  { 0x01A4, 0x0509 },
  { 0x01C4, 0x0509 },
  { 0x01A5, 0x0F00 },
  { 0x01C5, 0x0F00 },
  { 0x01A6, 0x0F08 },
  { 0x01C6, 0x0F08 },
  { 0x0090, 0x0180 },
  { 0x00D0, 0x019F },
  { 0x0091, 0x9D8A },
  { 0x00D1, 0x6276 },
  { 0x0092, 0x0002 },
  { 0x00D2, 0x0002 },
  { 0x0093, 0x0102 },
  { 0x00D3, 0x0116 },
  { 0x01A0, 0x0002 },
  { 0x01C0, 0x0002 },
  { 0x0098, 0x0001 },
  { 0x00D8, 0x0002 },
  { 0x0099, 0x045F },
  { 0x00D9, 0x04C4 },
  { 0x009A, 0x006B },
  { 0x00DA, 0x006A },
  { 0x009B, 0x0004 },
  { 0x00DB, 0x0004 },
  { 0x0084, 0x0104 },
  { 0x00C4, 0x0104 },
  { 0x0085, 0x0005 },
  { 0x00C5, 0x0005 },
  { 0x0086, 0x0337 },
  { 0x00C6, 0x0337 },
  { 0x0088, 0x0335 },
  { 0x00C8, 0x0335 },
  { 0x0082, 0x0003 },
  { 0x00C2, 0x0003 },
  { 0x009C, 0x00CF },
  { 0x00DC, 0x00CF },
  { 0x009E, 0x00CE },
  { 0x00DE, 0x00CE },
  { 0x009F, 0x0005 },
  { 0x00DF, 0x0005 },
};

//
// The PMA's own control block (0xC0xx) - the transmit and receive clock and
// data-width fields it keeps for the whole PHY - followed by the rate-dependent
// TX/RX tuning for lanes 0 and 1.
//
STATIC CONST A733_XHCI2_COMBO0_WRITE  mCombo0LaneRatePma[] = {
  { 0xC010, 0x5100 },
  { 0xC011, 0x0100 },
  { 0xC018, 0x0A0A },
  { 0xC01A, 0x1000 },
  { 0xC01B, 0x0010 },
  { 0x0041, 0x8200 },
  { 0x0047, 0x8200 },
  { 0x4700, 0x00FF },
  { 0x4701, 0x04AF },
  { 0x4702, 0x04AE },
  { 0x4703, 0x04AE },
  { 0x8400, 0x091D },
  { 0x8401, 0x091D },
  { 0x8402, 0x0900 },
  { 0x8403, 0x0000 },
  { 0x4640, 0x2A84 },
  { 0x464D, 0x0011 },
  { 0x8490, 0x000C },
  { 0x8508, 0x0009 },
  { 0x8549, 0x0C02 },
  { 0x8577, 0x06F6 },
  { 0x8578, 0x4606 },
  { 0x44EB, 0x0006 },
  { 0x8571, 0x0519 },
  { 0x8572, 0x0519 },
  { 0x85E8, 0x1002 },
  { 0x85E5, 0x0B98 },
  { 0x85E2, 0x0C01 },
  { 0x85E3, 0x0000 },
  { 0x85F5, 0x0000 },
  { 0x85F4, 0x0311 },
  { 0x85FF, 0x0000 },
  { 0x87FF, 0x0000 },
  { 0x8480, 0x010A },
  { 0x8482, 0x0003 },
  { 0x46EA, 0x00FF },
  { 0x44EA, 0x00FF },
  { 0x4100, 0x02FF },
  { 0x4101, 0x06AF },
  { 0x4102, 0x06AE },
  { 0x4103, 0x06AE },
  { 0x8200, 0x0D1D },
  { 0x8201, 0x0D1D },
  { 0x8202, 0x0D00 },
  { 0x8203, 0x0500 },
  { 0x4040, 0x2A82 },
  { 0x404D, 0x0014 },
  { 0x8290, 0x0013 },
  { 0x8308, 0x0000 },
  { 0x8349, 0x0C02 },
  { 0x8377, 0x0330 },
  { 0x8378, 0x0300 },
  { 0x42EB, 0x0003 },
  { 0x8371, 0x0019 },
  { 0x8372, 0x0019 },
  { 0x83E8, 0x1004 },
  { 0x83E5, 0x00F9 },
  { 0x83E2, 0x0C01 },
  { 0x83E3, 0x0002 },
  { 0x83F5, 0x0000 },
  { 0x83F4, 0x0031 },
  { 0x83FF, 0x0001 },
  { 0x81FF, 0x0002 },
  { 0x8280, 0x018C },
  { 0x8282, 0x0003 },
  { 0x40EA, 0x000F },
  { 0x42EA, 0x00F0 },
};

//
// The two lanes this board does not bond - their per-lane TX/RX fields, the
// lane-pair power states and the last protocol word.  See the note above on why
// lanes 2 and 3 are programmed anyway.
//
STATIC CONST A733_XHCI2_COMBO0_WRITE  mCombo0DpLanePma[] = {
  { 0xE005, 0x000B },
  { 0xE006, 0x2224 },
  { 0x4500, 0x00FB },
  { 0x4700, 0x00FB },
  { 0x4502, 0x04AA },
  { 0x4702, 0x04AA },
  { 0x4503, 0x04AA },
  { 0x4703, 0x04AA },
  { 0x8400, 0x0000 },
  { 0x8600, 0x0000 },
  { 0x8402, 0x0000 },
  { 0x8602, 0x0000 },
  { 0x8403, 0x0000 },
  { 0x8603, 0x0000 },
  { 0x8406, 0x0000 },
  { 0x8606, 0x0000 },
  { 0x44EA, 0x000F },
  { 0x46EA, 0x000F },
  { 0x8508, 0x0000 },
  { 0x8708, 0x0000 },
  { 0x8510, 0x0000 },
  { 0x8710, 0x0000 },
  { 0x8518, 0x0000 },
  { 0x8718, 0x0000 },
  { 0x4440, 0x08A4 },
  { 0x4640, 0x08A4 },
  { 0x44C6, 0x0003 },
  { 0x46C6, 0x0003 },
  { 0x4450, 0x002A },
  { 0x4650, 0x002A },
  { 0x444C, 0x0000 },
  { 0x464C, 0x0000 },
  { 0x0103, 0x007F },
  { 0x010B, 0x007F },
};

/**
  Write one segment of the PMA sequence above.

  Every access is 16-bit and the register number is the PMA's own, not a byte
  offset (see <Xhci2CombphyReg>).

  @param[in] Writes  The segment, in the order it is to be written.
  @param[in] Count   How many entries it has.
**/
STATIC
VOID
Xhci2PmaWrite (
  IN CONST A733_XHCI2_COMBO0_WRITE  *Writes,
  IN UINTN                          Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; Index++) {
    MmioWrite16 (Xhci2CombphyReg (Writes[Index].Register), Writes[Index].Value);
  }
}

/**
  Bring COMB0 - the SerDes combo PHY's USB3 lane - up, and say whether its PMA
  came ready.

  Until this runs, the SuperSpeed half of the DRD has no PHY: the core's PIPE
  interface is mapped to no lane (the PIPE lane map comes out of reset at the
  manual's reserved value, 18.2.6.25/26), and the mux in front of COMB0 still
  hands the PHY to PCIe0.  A port in that state powers up, runs its receiver
  detector, and reports nothing - which is what "ccs=0 pls=5" on the SuperSpeed
  root port means.

  The order below is the one the hardware needs, and each step is here because
  skipping it leaves the next one with nothing to work on:

  1. The mux (+0x0000, vendor BSP) and the PIPE lane map (+0x6100/+0x6104,
     manual 18.2.6.25/26).  Mux first: it is what connects the PHY's lane to the
     DWC3 at all, and the map is what tells the DWC3 which lane that is.

  2. The PMA out of IDDQ and onto its internal 24 MHz reference (CTRL2 and
     CTRL1, manual 18.2.6.7/18.2.6.6).  A PMA in IDDQ has all analog logic
     powered down; a PMA whose reference select still points at the differential
     REF-CLK pins has no clock to lock to, because this board does not bond
     them.  The gate for that reference (RTC + 0x16C bit 4) is opened by the
     caller, before this.

  3. The PMA's programmed state for the lane (the table above).

  4. The reset release the PHY wants (CTRL0's RESET_N, manual 18.2.6.5) with the
     DP raw-SerDes data-width restore the vendor issues on the way out, then the
     link's own PIPE reset, the DP link reset and the lane's data-rate clock
     (CTRL0's PIPE_LINK_RESET_N_SOFT and DP_LINK_RESET_N_SOFT, CTRL3's
     PLLCLK_EN_LN, manual 18.2.6.5/6.8), and then the link power state A0
     (CTRL3's POWER_STATE_REQ_LN - the manual requires A0 or A2 to be requested
     out of reset, and A0 is Tx/Rx active).

  5. The poll for STUS0's CMN_READY (manual 18.2.6.9).  This is the PHY's own
     verdict on steps 1-4, and the only one that matters: everything above is a
     request, CMN_READY is the answer.  If it does not come within the budget,
     the vendor's last resort is applied once - PMA register 0xC008 bits 11:8
     cleared, which re-locks the PLL - and polled again.

  Nothing here is fatal to the boot.  A PHY that never reports ready is logged
  and the caller carries on to register the controller; the mux stays on COMB0
  either way, because that is what the vendor leaves behind and because the
  core's PIPE interface needs a lane selected to finish its own reset.  A boot
  whose PMA is not ready is then a boot whose SuperSpeed port reports empty and
  whose log says so.

  @retval EFI_SUCCESS  STUS0 reports CMN_READY.
  @retval EFI_TIMEOUT  It did not, either before or after the re-lock retry.
**/
STATIC
EFI_STATUS
A733Xhci2Combo0On (
  VOID
  )
{
  UINT32   Value;
  UINT32   Stus0;
  UINT32   Tries;
  BOOLEAN  Relocked;
  BOOLEAN  Ready;

  //
  // BSP combo_usb2_clk_set(true): release the USB2.0 PHY reset
  // (SUBSYS+0x0008 bit 4) first, while the USB2 UTMI clock select is still on
  // the CCU source - the order SunxiUsbCombo0Init in the working firmware uses.
  //
  MmioOr32 (
    (UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR,
    A733_SERDES_USB3P1_BGR_U2P0_PHY_RSTN
    );

  //
  // 1. Mux COMB0 to the USB3.1 DRD, and map the DWC3's PIPE interface to the
  // PHY's lane 0.  Both registers come out of reset in the "nothing connected"
  // state: the mux to PCIe0, and the lane map to the manual's reserved 0xF.
  //
  MmioOr32 (Xhci2SerdesReg (A733_SERDES_HSI_GBLC), A733_SERDES_HSI_GBLC_PHY_USE_SEL);

  Value  = MmioRead32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_CLK_MAP_CTRL));
  Value &= ~(UINT32)A733_SERDES_USB3P1_PIPE_MAP_MASK;
  Value |= A733_SERDES_USB3P1_PIPE_MAP_LANE0;
  MmioWrite32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_CLK_MAP_CTRL), Value);

  Value  = MmioRead32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_RXD_MAP_CTRL));
  Value &= ~(UINT32)A733_SERDES_USB3P1_PIPE_MAP_MASK;
  Value |= A733_SERDES_USB3P1_PIPE_MAP_LANE0;
  MmioWrite32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_RXD_MAP_CTRL), Value);

  //
  // 2. Analog power (CTRL2) and the reference clock the PLLs run from (CTRL1).
  // CTRL2 is only ever cleared: the vendor's power-on step clears both IDDQ bits
  // and touches nothing else in the word, and on this die IDDQ_EN_SEL does not
  // hold what is written to it anyway (a write of 0b10 reads back 0, see
  // <A733Dwc3.h>).  CTRL1's three selects stay a field-at-a-time OR so a boot
  // chain that already chose something else keeps the rest of the word, and the
  // read-back below prints the whole word, which is how an inherited or reserved
  // field in it would show up.
  //
  MmioAnd32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL2),
    ~(UINT32)(A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SEL | A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SOFT)
    );

  MmioOr32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL1),
    A733_SERDES_COMBO0_CTRL1_PLL0_REFCLK_SEL |
    A733_SERDES_COMBO0_CTRL1_PLL1_REFCLK_SEL |
    A733_SERDES_COMBO0_CTRL1_REFCLK_DIG_SEL
    );

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: combo0 mux, power and reference: hsi_gblc=0x%08x (phy_use_sel=%u) "
    "pipe_clk_map=0x%08x pipe_rxd_map=0x%08x link_ctrl2=0x%08x (iddq_sel=%u iddq=%u) "
    "link_ctrl1=0x%08x (pll0_int=%u pll1_int=%u dig_int=%u)\n",
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_HSI_GBLC)),
    (MmioRead32 (Xhci2SerdesReg (A733_SERDES_HSI_GBLC)) & A733_SERDES_HSI_GBLC_PHY_USE_SEL) != 0,
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_CLK_MAP_CTRL)),
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_RXD_MAP_CTRL)),
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL2)),
    (MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL2)) &
     A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SEL) != 0,
    (MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL2)) &
     A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SOFT) != 0,
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL1)),
    (MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL1)) &
     A733_SERDES_COMBO0_CTRL1_PLL0_REFCLK_SEL) != 0,
    (MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL1)) &
     A733_SERDES_COMBO0_CTRL1_PLL1_REFCLK_SEL) != 0,
    (MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL1)) &
     A733_SERDES_COMBO0_CTRL1_REFCLK_DIG_SEL) != 0
    ));

  //
  // 3. The lane's programmed state, in the vendor's order and cut in five at the
  // two points where its sequence writes one of COMB0's own link registers.  A
  // helper keeps the five calls to one shape; the segments themselves are the
  // arrays above, and their order is the sequence.
  //
  Xhci2PmaWrite (mCombo0LaneLinkPma, ARRAY_SIZE (mCombo0LaneLinkPma));

  //
  // The vendor's param_config writes LINK_CTRL1 whole here, as one word, rather
  // than OR-ing the three selects again: this is the point at which the PLL
  // reference selects are declared settled for the lane table that follows.
  //
  MmioWrite32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL1),
    A733_SERDES_COMBO0_CTRL1_PLL1_REFCLK_SEL |
    A733_SERDES_COMBO0_CTRL1_PLL0_REFCLK_SEL |
    A733_SERDES_COMBO0_CTRL1_REFCLK_DIG_SEL
    );

  Xhci2PmaWrite (mCombo0PllPma, ARRAY_SIZE (mCombo0PllPma));
  Xhci2PmaWrite (mCombo0CommonRatePma, ARRAY_SIZE (mCombo0CommonRatePma));

  //
  // Normal orientation, declared again at the vendor's point in the sequence and
  // before the lane-rate block: TYPEC_CONN_DIR has to be static while RESET_N is
  // released, and the reset below is what releases it.
  //
  MmioAnd32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0),
    ~(UINT32)A733_SERDES_COMBO0_CTRL0_TYPEC_CONN_DIR
    );

  Xhci2PmaWrite (mCombo0LaneRatePma, ARRAY_SIZE (mCombo0LaneRatePma));
  Xhci2PmaWrite (mCombo0DpLanePma, ARRAY_SIZE (mCombo0DpLanePma));

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: combo0 pma writes=%u+%u+%u+%u+%u done: 0x4100=0x%04x "
    "0x8200=0x%04x 0xE006=0x%04x link_ctrl1=0x%08x link_ctrl0=0x%08x\n",
    (UINT32)ARRAY_SIZE (mCombo0LaneLinkPma),
    (UINT32)ARRAY_SIZE (mCombo0PllPma),
    (UINT32)ARRAY_SIZE (mCombo0CommonRatePma),
    (UINT32)ARRAY_SIZE (mCombo0LaneRatePma),
    (UINT32)ARRAY_SIZE (mCombo0DpLanePma),
    MmioRead16 (Xhci2CombphyReg (0x4100)),
    MmioRead16 (Xhci2CombphyReg (0x8200)),
    MmioRead16 (Xhci2CombphyReg (0xE006)),
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL1)),
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0))
    ));

  //
  // 4. RESET_N low, high, and the read-back that says the PHY took it.
  //
  MmioAnd32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0),
    ~(UINT32)A733_SERDES_COMBO0_CTRL0_RESET_N
    );
  MicroSecondDelay (A733_XHCI2_COMBO0_RESET_HOLD_US);
  MmioOr32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0), A733_SERDES_COMBO0_CTRL0_RESET_N);

  //
  // The vendor's reset release restores the DP raw-SerDes TX data-width
  // conversion at the part-2 window's +0x0004 on the way out: data enable, the
  // four lanes' enables and their reset releases, OR-ed into whatever the
  // register holds.  It is part of the same reset release, not of a DP link -
  // the vendor issues it unconditionally, on USB-only boards too - so it is
  // issued here too, and the addressed window is the serdes node's second one
  // (0x06C06000), not the subsystem's register list.
  //
  Value  = MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO_REG) + A733_SERDES_DP_RAWS_TXDW_CONV_CTRL);
  Value |= A733_SERDES_DP_RAW_DATA_EN | A733_SERDES_DP_RAW_LN_EN_MASK |
           A733_SERDES_DP_RAW_LN_RSTN_MASK;
  MmioWrite32 (
    Xhci2SerdesReg (A733_SERDES_COMBO_REG) + A733_SERDES_DP_RAWS_TXDW_CONV_CTRL,
    Value
    );

  for (Tries = 0; Tries < A733_XHCI2_COMBO0_ACK_TRIES; Tries++) {
    if ((MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0)) &
         A733_SERDES_COMBO0_CTRL0_RESET_N) != 0)
    {
      break;
    }

    MicroSecondDelay (A733_XHCI2_COMBO0_ACK_POLL_US);
  }

  MicroSecondDelay (A733_XHCI2_COMBO0_SETTLE_US);

  //
  // The link out of reset, its data-rate clock on, and A0 requested.  PIPE link
  // reset is the lane's own reset and stays released; the power state is a
  // one-hot request, so A0 is OR-ed in rather than replacing the field.  The
  // vendor's release then raises CTRL0's DP_LINK_RESET_N_SOFT as well (manual
  // 18.2.6.5: it releases the reset of the DisplayPort link and its lanes).  No
  // DP link is bonded on this board, but the vendor raises it unconditionally in
  // this sequence and the state the PHY is left in is part of what its common
  // block reports ready on, so it is raised here too rather than reasoned away.
  //
  MmioOr32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0),
    A733_SERDES_COMBO0_CTRL0_PIPE_LINK_RESET_N
    );
  MmioOr32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL3), A733_SERDES_COMBO0_CTRL3_PLLCLK_EN_LN);

  for (Tries = 0; Tries < A733_XHCI2_COMBO0_ACK_TRIES; Tries++) {
    if ((MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL3)) &
         A733_SERDES_COMBO0_CTRL3_PLLCLK_EN_LN) != 0)
    {
      break;
    }

    MicroSecondDelay (A733_XHCI2_COMBO0_ACK_POLL_US);
  }

  MmioOr32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL3),
    A733_SERDES_COMBO0_CTRL3_POWER_STATE_REQ (A733_SERDES_COMBO0_CTRL3_POWER_STATE_A0)
    );
  MmioOr32 (
    Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0),
    A733_SERDES_COMBO0_CTRL0_DP_LINK_RESET_N
    );

  //
  // 5. The PMA's answer, and one re-lock attempt if it is not ready yet.
  //
  Relocked = FALSE;
  Ready    = (MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_STUS0)) &
              A733_SERDES_COMBO0_STUS0_CMN_READY) != 0;
  if (!Ready) {
    MicroSecondDelay (A733_XHCI2_COMBO0_RELOCK_DELAY_US);

    Value  = MmioRead16 (Xhci2CombphyReg (A733_COMBPHY0_PMA_PLL_RELOCK));
    Value &= ~(UINT32)A733_COMBPHY0_PMA_PLL_RELOCK_MASK;
    MmioWrite16 (Xhci2CombphyReg (A733_COMBPHY0_PMA_PLL_RELOCK), (UINT16)Value);

    for (Tries = 0; Tries < A733_XHCI2_COMBO0_READY_TRIES; Tries++) {
      if ((MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_STUS0)) &
           A733_SERDES_COMBO0_STUS0_CMN_READY) != 0)
      {
        break;
      }

      MicroSecondDelay (A733_XHCI2_COMBO0_READY_POLL_US);
    }

    Relocked = TRUE;
    Ready    = (MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_STUS0)) &
                A733_SERDES_COMBO0_STUS0_CMN_READY) != 0;
  }

  Stus0 = MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_STUS0));
  if (!Ready) {
    //
    // The mux is left where it is.  Releasing it on failure was this driver's
    // own idea - the reasoning being that a PIPE interface clocked by a PMA that
    // is not running is worse than none - but a mux pointing back at PCIe0 is
    // exactly the state in which the DRD's PIPE interface has no lane clock at
    // all, and the core then holds USBSTS.CNR set instead of coming up far
    // enough to report its ports.  The vendor and the working reference firmware
    // both leave COMB0 selected whatever the PMA says, so this does too: the
    // port report below still reads, and the log line says why it is empty.
    //
    DEBUG ((
      DEBUG_ERROR,
      "A733Xhci2: combo0 PMA not ready (stus0=0x%08x cmn_ready=0 clock_en_ack=%u "
      "power_state_ack=%u, re-lock %a); mux left on COMB0 (hsi_gblc=0x%08x), so the "
      "SuperSpeed lane has no working PHY and its port can only report empty - "
      "registering the controller anyway\n",
      Stus0,
      (Stus0 & A733_SERDES_COMBO0_STUS0_CLOCK_EN_ACK) != 0,
      (UINT32)((Stus0 & A733_SERDES_COMBO0_STUS0_POWER_STATE_ACK_MASK) >>
               A733_SERDES_COMBO0_STUS0_POWER_STATE_ACK_SHIFT),
      Relocked ? "attempted" : "not needed",
      MmioRead32 (Xhci2SerdesReg (A733_SERDES_HSI_GBLC))
      ));
    return EFI_TIMEOUT;
  }

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: combo0 PMA ready (stus0=0x%08x cmn_ready=1 clock_en_ack=%u "
    "power_state_ack=%u, re-lock %a) link_ctrl0=0x%08x link_ctrl3=0x%08x\n",
    Stus0,
    (Stus0 & A733_SERDES_COMBO0_STUS0_CLOCK_EN_ACK) != 0,
    (UINT32)((Stus0 & A733_SERDES_COMBO0_STUS0_POWER_STATE_ACK_MASK) >>
             A733_SERDES_COMBO0_STUS0_POWER_STATE_ACK_SHIFT),
    Relocked ? "attempted" : "not needed",
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL0)),
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_COMBO0_LINK_CTRL3))
    ));

  return EFI_SUCCESS;
}

/**
  Power the USB2.0 port's femtoPHY up and force its VBUS verdict valid.

  Manual 18.4.5: the application window's U2P0 group is the PHY's own register
  file.  SIDDQ (PHYCTL bit 3) is set out of reset and is the analog power switch;
  with it set the core gets no UTMI clock and its own reset never completes,
  which is a hang rather than an error - hence the read-back line.

  The two written words other than SIDDQ are board data, not the manual's reset
  values (<A733Dwc3.h> names both): the host-mode PHYCTL with the vendor's tune
  bits, the vendor's PHY tune word, and ISCR's force-VBUS-valid field, which is
  what makes a port with no VBUS pin of its own usable as a host.

  A733MusbDeviceDxe writes the same group for the OTG port, with the same
  enable-and-tune values, so whichever of the two runs first the other finds a
  powered PHY.
**/
/**
  Program the SYSCFG resistance-calibration block the way the only firmware
  whose own session ever enumerated this board's hub did (edk2-a733 X16/X23:
  SunxiUsbResCal(0)/(1) + the v2 phy_rescal_set_v2 writes).

  The block is shared by every on-chip USB2 PHY: CAL_EN selects a manual trim,
  one RES200_SEL bit per port hands its 200-ohm trim to the calibration loop,
  and the trim bytes at +0x164 and +0x168 must be clear for the loop to drive
  them.  This is the 6e4882d/d138410 state, verified across 20/20 cold boots
  with the hub and keyboard at 480 Mbit/s; leaving the block at its reset
  value keeps the femtoPHY's analog calibration from converging.  Every write
  is read-modify-write, so a USB host or the OTG driver that runs this same
  sequence before or after us reaches the same state.
**/
STATIC
VOID
A733Xhci2ResCal (
  VOID
  )
{
  UINT32  Value;

  Value  = MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RESCAL_CTRL_REG);
  Value &= ~(UINT32)A733_SYSCFG_CAL_EN;
  Value |= A733_SYSCFG_RES200_SEL (0) | A733_SYSCFG_RES200_SEL (1);
  MmioWrite32 (A733_SYSCFG_BASE + A733_SYSCFG_RESCAL_CTRL_REG, Value);

  Value  = MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RES0_CTRL_REG);
  Value &= ~(UINT32)(A733_SYSCFG_RES200_TRIM_MASK (0) | A733_SYSCFG_RES200_TRIM_MASK (1));
  MmioWrite32 (A733_SYSCFG_BASE + A733_SYSCFG_RES0_CTRL_REG, Value);

  MmioOr32 (A733_SYSCFG_BASE + A733_SYSCFG_RESCAL_CTRL_REG, A733_SYSCFG_RES200_TRIM_SEL);
  MmioAnd32 (A733_SYSCFG_BASE + A733_SYSCFG_RES1_CTRL_REG, ~(UINT32)(0xFFU << 8));

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: SYSCFG rescal +0x160=0x%08x (cal_en=%u res200_sel=0x%x) "
    "+0x164=0x%08x +0x168=0x%08x\n",
    MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RESCAL_CTRL_REG),
    (MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RESCAL_CTRL_REG) & A733_SYSCFG_CAL_EN) != 0,
    (UINT32)((MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RESCAL_CTRL_REG) >> 4) & 0xFU),
    MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RES0_CTRL_REG),
    MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RES1_CTRL_REG)
    ));
}

STATIC
VOID
A733Xhci2PhyUp (
  VOID
  )
{
  UINT32  Iscr;

  A733Xhci2ResCal ();

  MmioWrite32 (Xhci2AppReg (A733_DWC3_U2P0_PHYCTL), A733_DWC3_U2P0_PHYCTL_HOST_ON);
  MmioWrite32 (Xhci2AppReg (A733_DWC3_U2P0_PHYTUNE), A733_DWC3_U2P0_PHYTUNE_HOST);

  //
  // VBUS is not forced here.  The working configuration on this board reports
  // ISCR = 0x02000000 - the PHY's own VBUS comparator, zero force fields - and
  // forcing it valid was this driver's addition; a PHY told that VBUS is present
  // when its comparator says otherwise is a PHY that can report a line state but
  // withhold the connect the controller waits for.  The line state itself is
  // already correct without the force: ISCR bit 25 reads 1 (J) here, the same as
  // it does in the working firmware, which is the board's hub pulling D+ up.
  //
  //
  // ISCR FORCE_VBUS_VALID (bits 13:12) is required, and this is measured, not
  // assumed: the PHY's own VBUS comparator does not see the rail on this board
  // (VBUS_STATUS bit 24 stays 0 even with the 5 V switch high), and without the
  // force the xHCI USB2 port reset never completes speed detection - the hub
  // stays at "device is of 0 speed" and SET_ADDRESS fails.  Forcing it makes
  // VBUS_STATUS read 1 and is what the configuration that enumerated this board's
  // hub on 2026-09-09 does.
  //
  Iscr  = MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_ISCR));
  Iscr &= ~(UINT32)A733_DWC3_U2P0_ISCR_VBUS_FORCE_MASK;
  Iscr |= A733_DWC3_U2P0_ISCR_VBUS_FORCE_VALID;
  MmioWrite32 (Xhci2AppReg (A733_DWC3_U2P0_ISCR), Iscr);

  MicroSecondDelay (A733_XHCI2_PHY_SETTLE_US);

  //
  // The femtoPHY's analog calibration words (CAL area, app offsets 0xD0..0xDC):
  // reset defaults (0x00000040/0x00188000/0x01200000) mean the calibration the
  // SIDDQ edge is meant to trigger never ran, which is the historical cause of
  // chirp falling back to full speed.  Real values vary per boot.
  //
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: U2P0 CAL after SIDDQ edge: 0xD0=0x%08x 0xD4=0x%08x 0xD8=0x%08x 0xDC=0x%08x\n",
    MmioRead32 (Xhci2AppReg (0xD0)),
    MmioRead32 (Xhci2AppReg (0xD4)),
    MmioRead32 (Xhci2AppReg (0xD8)),
    MmioRead32 (Xhci2AppReg (0xDC))
    ));

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: U2P0 phyctl=0x%08x (siddq=%u fsel=%u) phy tune=0x%08x iscr=0x%08x "
    "(vbus_force=%u) physts=0x%08x\n",
    MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_PHYCTL)),
    (MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_PHYCTL)) & A733_DWC3_U2P0_PHYCTL_SIDDQ) != 0,
    (UINT32)((MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_PHYCTL)) & A733_DWC3_U2P0_PHYCTL_FSEL_MASK)
             >> A733_DWC3_U2P0_PHYCTL_FSEL_SHIFT),
    MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_PHYTUNE)),
    MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_ISCR)),
    (UINT32)((MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_ISCR)) & A733_DWC3_U2P0_ISCR_VBUS_FORCE_MASK)
             >> A733_DWC3_U2P0_ISCR_VBUS_FORCE_SHIFT),
    MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_PHYSTS))
    ));
}

/**
  Reset the DWC3 core and put it in host mode.

  The reset is the one the core's databook prescribes and U-Boot implements:
  the core into reset first, then the two PHY interfaces, both held, both
  released, and the core released last - a core released before its PHY has a
  clock does not converge.  The 100 ms holds are the databook's.

  Then the three global words, in the order the core takes them:

  - GCTL, first write: host mode (PRTCAPDIR) alone.  The role has to be settled
    before anything else is programmed, because the core arbitrates its register
    view from it.
  - GUSB3PIPECTL0: PHYSOFTRST and SUSPHY cleared and UX_EXIT_PX cleared, so the
    SuperSpeed side of the PHY interface is running rather than suspended or
    held in reset.  COMB0 is brought up before this function runs, so this is
    the only reset release that reaches it.
  - GUSB2PHYCFG0: 8-bit UTMI with the matching turnaround time, no LPM, no PHY
    suspend - the port the hub hangs off.
  - GCTL, last write: the 24 MHz power-down scale, DSBLCLKGTNG and U2RSTECN.
    The composite this produces is the word a working Linux boot on this board
    reads back (0x30C11005), which is the check the log line makes.

  @retval EFI_SUCCESS       The core answered and is in host mode.
  @retval EFI_DEVICE_ERROR  GSNPSID does not identify a DWC3 core: the block is
                            gated, unclocked, or not there.
**/
STATIC
EFI_STATUS
A733Xhci2CoreInit (
  VOID
  )
{
  UINT32  SnpsId;
  UINT32  Value;

  SnpsId = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GSNPSID);
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: GSNPSID(+0x%04x)=0x%08x (u3=%u revision=0x%04x)\n",
    A733_DWC3_GSNPSID,
    SnpsId,
    (UINT32)(((SnpsId & A733_DWC3_GSNPSID_MASK) == A733_DWC3_GSNPSID_U3) ||
             ((SnpsId & A733_DWC3_GSNPSID_MASK) == A733_DWC3_GSNPSID_U31)),
    SnpsId & A733_DWC3_GSNPSID_REV_MASK
    ));
  if (((SnpsId & A733_DWC3_GSNPSID_MASK) != A733_DWC3_GSNPSID_U3) &&
      ((SnpsId & A733_DWC3_GSNPSID_MASK) != A733_DWC3_GSNPSID_U31))
  {
    DEBUG ((
      DEBUG_ERROR,
      "A733Xhci2: 0x%08x does not identify a DWC3 core; the block is ungated at "
      "best, not clocked at worst - not bringing the controller up\n",
      (UINT32)A733_DWC3_BASE
      ));
    return EFI_DEVICE_ERROR;
  }

  MmioOr32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL, A733_DWC3_GCTL_CORESOFTRESET);
  MmioOr32 (
    (UINTN)A733_DWC3_BASE + A733_DWC3_GUSB2PHYCFG (0),
    A733_DWC3_GUSB2PHYCFG_PHYSOFTRST
    );
  MmioOr32 (
    (UINTN)A733_DWC3_BASE + A733_DWC3_GUSB3PIPECTL (0),
    A733_DWC3_GUSB3PIPECTL_PHYSOFTRST
    );
  MicroSecondDelay (A733_XHCI2_RESET_HOLD_US);

  MmioAnd32 (
    (UINTN)A733_DWC3_BASE + A733_DWC3_GUSB2PHYCFG (0),
    ~(UINT32)A733_DWC3_GUSB2PHYCFG_PHYSOFTRST
    );
  MmioAnd32 (
    (UINTN)A733_DWC3_BASE + A733_DWC3_GUSB3PIPECTL (0),
    ~(UINT32)A733_DWC3_GUSB3PIPECTL_PHYSOFTRST
    );
  MicroSecondDelay (A733_XHCI2_RESET_HOLD_US);

  MmioAnd32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL, ~(UINT32)A733_DWC3_GCTL_CORESOFTRESET);
  MicroSecondDelay (A733_XHCI2_PHY_SETTLE_US);

  //
  // GCTL: host mode first, and the rest of the word after the PHY interface is
  // configured.  The role has to be settled before anything else is programmed,
  // because the core arbitrates which register view it presents from it; the
  // power-down scale and the clock-gating bit are then written last, once the
  // PIPE and UTMI interfaces have been told what they are.
  //
  Value  = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL);
  Value &= ~(UINT32)A733_DWC3_GCTL_PRTCAPDIR_MASK;
  Value |= A733_DWC3_GCTL_PRTCAPDIR_HOST << A733_DWC3_GCTL_PRTCAPDIR_SHIFT;
  MmioWrite32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL, Value);

  //
  // GUSB3PIPECTL0: out of suspend and out of reset, with UX_EXIT_PX cleared as
  // the Linux core clears it.  This is the only place the SuperSpeed interface's
  // reset is written after COMB0 is up: a second assertion after GCTL has been
  // programmed re-resets the PIPE interface the core is by then waiting on, and
  // USBSTS.CNR is exactly what does not come down while it is asserted.
  //
  MmioAnd32 (
    (UINTN)A733_DWC3_BASE + A733_DWC3_GUSB3PIPECTL (0),
    ~(UINT32)(A733_DWC3_GUSB3PIPECTL_SUSPHY | A733_DWC3_GUSB3PIPECTL_UX_EXIT_PX)
    );

  //
  // GUSB2PHYCFG0: 8-bit UTMI with its turnaround time, no LPM, no suspend.
  //
  Value  = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB2PHYCFG (0));
  Value &= ~(UINT32)(A733_DWC3_GUSB2PHYCFG_PHYIF |
                     A733_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK |
                     A733_DWC3_GUSB2PHYCFG_ENBLSLPM |
                     A733_DWC3_GUSB2PHYCFG_SUSPHY);
  Value |= A733_DWC3_GUSB2PHYCFG_USBTRDTIM_8BIT;
  MmioWrite32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB2PHYCFG (0), Value);

  //
  // GCTL again: the 24 MHz power-down scale, internal clock gating off and the
  // USB2 reset-early workaround, which is already set out of reset and is left
  // set.  Every other bit keeps its value.
  //
  Value  = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL);
  Value &= ~(UINT32)A733_DWC3_GCTL_PWRDNSCALE_MASK;
  Value |= A733_DWC3_PWRDNSCALE_24MHZ << A733_DWC3_GCTL_PWRDNSCALE_SHIFT;
  Value |= A733_DWC3_GCTL_DSBLCLKGTNG | A733_DWC3_GCTL_U2RSTECN;
  MmioWrite32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL, Value);

  Value = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL);
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: GCTL=0x%08x DSTS=0x%08x (rstack=%u) GUCTL=0x%08x GUCTL2=0x%08x "
    "GUSB2PHYCFG0=0x%08x (susphy=%u phyif=%u) GUSB3PIPECTL0=0x%08x (susphy=%u)\n",
    Value,
    MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C118U),
    (UINT32)((MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C118U) & BIT17) != 0),
    MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C130U),
    MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C12CU),
    MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C200U),
    (UINT32)((MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C200U) & BIT3) != 0),
    (UINT32)((MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C200U) >> 24) & 0x3U),
    MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C2C0U),
    (UINT32)((MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C2C0U) & BIT5) != 0)
    ));

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: GCTL=0x%08x (coresoftreset=%u prtcapdir=%u pwrdnscale=%u dsblclkgtng=%u "
    "u2rstecn=%u)\n",
    Value,
    (Value & A733_DWC3_GCTL_CORESOFTRESET) != 0,
    (UINT32)((Value & A733_DWC3_GCTL_PRTCAPDIR_MASK) >> A733_DWC3_GCTL_PRTCAPDIR_SHIFT),
    (UINT32)((Value & A733_DWC3_GCTL_PWRDNSCALE_MASK) >> A733_DWC3_GCTL_PWRDNSCALE_SHIFT),
    (Value & A733_DWC3_GCTL_DSBLCLKGTNG) != 0,
    (Value & A733_DWC3_GCTL_U2RSTECN) != 0
    ));
  Value = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB2PHYCFG (0));
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: GUSB2PHYCFG0=0x%08x (physoftrst=%u phyif=%u ulpi_utmi_sel=%u usbtrdtim=%u "
    "enblslpm=%u susphy=%u)\n",
    Value,
    (Value & A733_DWC3_GUSB2PHYCFG_PHYSOFTRST) != 0,
    (UINT32)((Value & A733_DWC3_GUSB2PHYCFG_PHYIF) != 0),
    (UINT32)((Value & A733_DWC3_GUSB2PHYCFG_ULPI_UTMI_SEL) != 0),
    (UINT32)((Value & A733_DWC3_GUSB2PHYCFG_USBTRDTIM_MASK) >> A733_DWC3_GUSB2PHYCFG_USBTRDTIM_SHIFT),
    (Value & A733_DWC3_GUSB2PHYCFG_ENBLSLPM) != 0,
    (Value & A733_DWC3_GUSB2PHYCFG_SUSPHY) != 0
    ));
  //
  // The same two fields, read as the interface they select: this is the line
  // that says which PHY interface the USB2 port is actually on.
  //
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: USB2 port PHY interface: %a\n",
    ((Value & A733_DWC3_GUSB2PHYCFG_ULPI_UTMI_SEL) != 0) ?
    (((Value & A733_DWC3_GUSB2PHYCFG_PHYIF) != 0) ? "ULPI 12-bit" : "ULPI 8-bit") :
    (((Value & A733_DWC3_GUSB2PHYCFG_PHYIF) != 0) ? "UTMI+ 16-bit" : "UTMI+ 8-bit")
    ));
  Value = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB3PIPECTL (0));
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: GUSB3PIPECTL0=0x%08x (physoftrst=%u susphy=%u)\n",
    Value,
    (Value & A733_DWC3_GUSB3PIPECTL_PHYSOFTRST) != 0,
    (Value & A733_DWC3_GUSB3PIPECTL_SUSPHY) != 0
    ));
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: USB3 port PHY interface: PIPE (COMB0 serdes), susphy=%u physoftrst=%u\n",
    (Value & A733_DWC3_GUSB3PIPECTL_SUSPHY) != 0,
    (Value & A733_DWC3_GUSB3PIPECTL_PHYSOFTRST) != 0
    ));

  //
  // The SuperSpeed interface's reset and its end state, read back with the lane
  // map it is pointing at.  There is no second pulse after this: the reset was
  // released above, COMB0 is up before the core left reset, and re-asserting it
  // here would put the core back in front of a PIPE interface it has already
  // told software it is waiting on.
  //
  Value = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB3PIPECTL (0));
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: GUSB3PIPECTL0 end state 0x%08x (physoftrst=%u susphy=%u) "
    "pipe_clk_map=0x%08x pipe_rxd_map=0x%08x\n",
    Value,
    (Value & A733_DWC3_GUSB3PIPECTL_PHYSOFTRST) != 0,
    (Value & A733_DWC3_GUSB3PIPECTL_SUSPHY) != 0,
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_CLK_MAP_CTRL)),
    MmioRead32 (Xhci2SerdesReg (A733_SERDES_USB3P1_PIPE_RXD_MAP_CTRL))
    ));

  //
  // LLUCTL: reproduce the word the working reference firmware's boot holds on
  // the DWC3 core (0x008B8080 - see <A733Dwc3.h> for the offset's provenance
  // and which bits it sets).  That firmware never writes the register; the
  // state comes from the SoC's own boot chain, and its read-back line records
  // exactly this value on a boot that enumerates the hub.  A full-word write
  // is used rather than an OR so the core holds precisely the working state
  // whatever it inherited, and the line below says whether the write changed
  // anything.  The register does not survive the core's own reset (the BSP
  // re-applies its bits after every core resume), which is why the write
  // happens here, after the CORESOFTRESET cycle above; XhciDxe's later
  // host-controller reset only touches the xHCI block and leaves this global
  // register alone.
  //
  {
    UINT32  LlUctl;

    LlUctl = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_LLUCTL);
    MmioWrite32 ((UINTN)A733_DWC3_BASE + A733_DWC3_LLUCTL, A733_DWC3_LLUCTL_BOARD);
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: LLUCTL(+0x%04x) was 0x%08x now 0x%08x (inv_sync_hdr=%u force_gen1=%u)\n",
      A733_DWC3_LLUCTL,
      LlUctl,
      MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_LLUCTL),
      (UINT32)((MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_LLUCTL) &
                A733_DWC3_LLUCTL_INV_SYNC_HDR) != 0),
      (UINT32)((MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_LLUCTL) &
                A733_DWC3_LLUCTL_FORCE_GEN1) != 0)
      ));
  }

  return EFI_SUCCESS;
}

//
// Whether the hub's own 3.3 V rail (PMIC DCDC1) read enabled when this driver
// looked at it.  Kept so the port report's "nothing connected" line can say
// which of the two cases it is - an unpowered hub or a powered hub that is not
// terminating - without reading the PMIC again from there.
//
STATIC BOOLEAN  mHubRailEnabled = FALSE;

/**
  Bring the hub's own supply up, and read the PMIC back.

  Where the hub's power actually comes from (schematic page 14, "USB3.0/2.0/HUB",
  U6 FE1_1S - not inferred):

  - VD33 (pin 20) is net VCC_3V3_USB20HUB behind FB3 (120R 2A), i.e. the PMIC's
    DCDC1 3.3 V through R58 (0R 1%), with C149 (22 uF) as its bulk.  That is the
    hub's supply, and there is no load switch and no enable pin in that path.
  - VDD5 (pin 19) is the 5 V input, on a net of its own with C150/C152 (100 nF);
    it reaches VCC5V0_SYS only through L7 (120R 3A), which is not fitted.  The
    board therefore runs the hub from 3.3 V alone, and nothing here needs 5 V on
    the hub to bring its upstream port up.
  - XRSTJ (pin 16) is R64 (100K 1%) to that same 3.3 V rail with C158 not
    fitted: the hub leaves reset as soon as the rail is there.  No GPIO and no
    SoC reset drives it, which is why nothing here resets the hub.
  - BUSJ (pin 18) is R59 (100K 1%) to the rail, i.e. self-powered; VBUSM
    (pin 17) is R60 (10K 1%) from the rail with C155 (100 nF) to ground, so the
    hub always reads upstream VBUS as present.  PWRJ, LED1 and LED2 are marked
    not connected.

  So the only part of the hub's power that software can be wrong about is
  whether DCDC1 is up, and that is a PMIC register: enable bit 0 of
  DCDC_POWER_ON_OFF_CTL1 (0x10), voltage in DC1OUT_VOL (0x12), from the
  vendor's pmu_axp8191_ctrl_tbl row "dcdc1".  The board's device tree agrees
  with the schematic that this is the hub's rail: reg_dcdc1 is
  regulator-always-on / regulator-boot-on and nothing else claims it.

  The rail is checked on every boot and written only when its bit reads clear,
  so this is idempotent and the line below always says what the chip held.  A
  rail that will not read or will not come up is logged and stepped over: the
  USB2 PHY and the controller are worth bringing up either way, and a log that
  says the rail is down is exactly what an empty port needs beside it.
**/
/**
  Bring the DWC3's USB2 PHY rails up: ELDO2 (3.3 V) and ELDO1 (1.8 V).

  Schematic page 4 names them: VCC33-USB-2 is ELDO2 and VCC33-18-USB-2 is ELDO1,
  and both feed the U2P0 PHY that carries this controller's USB2 port.  Neither
  rail is regulator-boot-on in the board's device tree, so without this the PHY
  has no analog power: its port state machine still reports RxDetect, its
  detector still runs, and it sees nothing - which is exactly what this board
  showed with ccs=0 on every port while the core itself was healthy.

  Logged with read-backs rather than assumes: a rail that the PMIC declines to
  enable reads enabled=0 here.
**/
STATIC
VOID
A733Xhci2PhyRailOn (
  VOID
  )
{
  EFI_STATUS     Status;
  A733_PMIC_LDO  Rail;

  Status = A733PmicInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "A733Xhci2: no PMIC access for the U2P0 rails (%r); the USB2 PHY may be "
      "unpowered and its port will report empty\n",
      Status
      ));
    return;
  }

  ZeroMem (&Rail, sizeof (Rail));
  Status = A733PmicEnableEldo (TRUE, 3300, &Rail);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Xhci2: ELDO2 (VCC33-USB-2) not enabled: %r\n", Status));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: ELDO2 (VCC33-USB-2, U2P0 3.3 V): enabled=%u %u mV "
      "CTL3[0x22]=0x%02x VOL[0x3b]=0x%02x\n",
      Rail.Enabled,
      Rail.MilliVolt,
      Rail.LdoCtl,
      Rail.Vol
      ));
  }

  ZeroMem (&Rail, sizeof (Rail));
  Status = A733PmicEnableEldo (FALSE, 1800, &Rail);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Xhci2: ELDO1 (VCC33-18-USB-2) not enabled: %r\n", Status));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: ELDO1 (VCC33-18-USB-2, U2P0 1.8 V): enabled=%u %u mV "
      "CTL3[0x22]=0x%02x VOL[0x3a]=0x%02x\n",
      Rail.Enabled,
      Rail.MilliVolt,
      Rail.LdoCtl,
      Rail.Vol
      ));
  }
}

STATIC
VOID
A733Xhci2HubRailOn (
  VOID
  )
{
  EFI_STATUS       Status;
  A733_PMIC_DCDC1  Before;
  A733_PMIC_DCDC1  After;

  Status = A733PmicInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "A733Xhci2: no PMIC access (%r); the hub's 3.3 V rail (DCDC1) is unverified, "
      "so a ccs=0 below may be an unpowered hub rather than a wiring fault\n",
      Status
      ));
    return;
  }

  Status = A733PmicGetDcdc1 (&Before);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Xhci2: reading DCDC1 failed: %r\n", Status));
    return;
  }

  mHubRailEnabled = Before.Enabled;

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: hub rail DCDC1 (net VCC_3V3_USB20HUB, U6 pin 20 VD33): "
    "CTL1[0x10]=0x%02x enabled=%u DC1OUT_VOL[0x12]=0x%02x (%u mV)\n",
    Before.Ctl1,
    (UINT32)Before.Enabled,
    Before.Vol,
    Before.MilliVolt
    ));

  if (Before.Enabled) {
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: hub rail was already on, nothing written; the hub is out of reset "
      "(XRSTJ is a 100K pull-up to this rail) and should now be holding its upstream "
      "D+ pull-up, so the USB2 root port below should read ccs=1\n"
      ));
    return;
  }

  Status = A733PmicEnableDcdc1 (&Before);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Xhci2: enabling DCDC1 failed: %r\n", Status));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: DCDC1 enable written: CTL1[0x10]=0x%02x enabled=%u "
    "DC1OUT_VOL[0x12]=0x%02x (%u mV)\n",
    Before.Ctl1,
    (UINT32)Before.Enabled,
    Before.Vol,
    Before.MilliVolt
    ));

  //
  // The rail's own ramp (device tree: regulator-enable-ramp-delay 1000 us) plus
  // the hub's power-on: its 12 MHz crystal has to start and its internal reset
  // run before it terminates the upstream link.  USB 2.0 allows a hub 100 ms
  // (7.1.7.3, bPwrOn2PwrGood); 20 ms covers this part with margin.
  //
  MicroSecondDelay (A733_XHCI2_HUB_POWER_SETTLE_US);

  Status = A733PmicGetDcdc1 (&After);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "A733Xhci2: reading DCDC1 back failed: %r\n", Status));
    return;
  }

  mHubRailEnabled = After.Enabled;

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: DCDC1 read back after %u us: CTL1[0x10]=0x%02x enabled=%u "
    "DC1OUT_VOL[0x12]=0x%02x (%u mV); the hub should now be powered, out of reset "
    "and driving its upstream D+ pull-up, so the USB2 root port below should read ccs=1\n",
    A733_XHCI2_HUB_POWER_SETTLE_US,
    After.Ctl1,
    (UINT32)After.Enabled,
    After.Vol,
    After.MilliVolt
    ));
}

/**
  Drive the 5 V switch in front of the hub's downstream ports, and read the pin
  back.

  This is not the hub's supply - the hub is powered from DCDC1 by
  A733Xhci2HubRailOn above.  The pin is the board's USB_HOST_EN net (schematic
  page 14): EN of U5 (SGM2576), whose input is VCC5V0_SYS and whose output
  VCC5V0_USB20 is the 5 V of the USB2 Type-A receptacle and of the two-port USB2
  header that the hub's other downstream ports go to.  Its device-tree name is
  PcdA733UsbHubVbusPin, active high.

  A733UsbHostDxe drives the same pin for the USB2.0 host ports it publishes, so
  in the normal dispatch order this finds the switch already on and changes
  nothing; driving it here as well keeps this controller's bring-up
  self-contained, and the line below is what says the rail is on rather than
  that some earlier stage believed it was.

  The level bit and the pin's CFG nibble are printed from R_PIO: a level bit of
  1 with the pad left an input or an alternate function is a rail that is not
  being driven, which is exactly the state a single "wrote TRUE" line would hide.
**/
STATIC
VOID
A733Xhci2HubPowerOn (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Pin;
  UINT32      Dat;
  UINT32      Cfg;
  UINT32      Bit;
  UINT32      Function;

  Pin = FixedPcdGet32 (PcdA733UsbHubVbusPin);

  //
  // One clean power cycle per boot, before anything is expected on the port.
  // The hub on this board is rail-powered and keeps its upstream state machine
  // across a warm reset, so a boot whose enumeration died mid-session can leave
  // it in a post-session state that the next host-side reset lands in the middle
  // of.  The reference firmware cycles this rail every boot for exactly that
  // reason, and its comment names the symptom this port has been showing all
  // along: a link that never finishes speed detection.  Off, wait for the rail
  // to actually fall, back on, wait for the hub's own power-on init and
  // debounce.
  //
  (VOID)A733RpioSetOutput (Pin, FALSE);
  DEBUG ((DEBUG_INFO, "A733Xhci2: hub 5 V cycled off (%u ms) for a deterministic hub reset\n", 500));
  MicroSecondDelay (500000);

  Status = A733RpioSetOutput (Pin, TRUE);
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: Type-A port 5 V switch (VCC5V0_USB20) enable P%c%u high: %r\n",
    'L' + A733_RPIO_BANK_OF (Pin),
    A733_RPIO_NUM_OF (Pin),
    Status
    ));

  Bit      = A733_RPIO_NUM_OF (Pin);
  Dat      = MmioRead32 ((UINTN)PcdGet64 (PcdA733RpioBase)
                         + (UINTN)A733_RPIO_BANK_OF (Pin) * A733_RPIO_BANK_STRIDE
                         + A733_RPIO_DAT_OFFSET);
  Cfg      = MmioRead32 ((UINTN)PcdGet64 (PcdA733RpioBase)
                         + (UINTN)A733_RPIO_BANK_OF (Pin) * A733_RPIO_BANK_STRIDE
                         + A733_RPIO_CFG_OFFSET + (Bit >> 3) * 4U);
  Function = (Cfg >> ((Bit & 0x7U) * 4U)) & A733_RPIO_FUNC_MASK;

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: port 5 V switch readback: dat=0x%08x bit=%u cfg=0x%08x nibble=%u (%a)\n",
    Dat,
    (Dat >> Bit) & 1U,
    Cfg,
    Function,
    (Function == A733_RPIO_FUNC_OUTPUT) ? "output" :
    ((Function == 0) ? "input" : "not a plain output")
    ));

  DEBUG ((DEBUG_INFO, "A733Xhci2: hub 5 V back on (%u ms) for power-on init + debounce\n", 500));
  MicroSecondDelay (500000);
}

/**
  Name of an xHCI link state (spec 6.4.1).

  @param[in] Pls  PORTSC's PLS field.

  @return A constant string.
**/
STATIC
CONST CHAR8 *
A733Xhci2LinkStateName (
  IN UINT32  Pls
  )
{
  switch (Pls) {
    case 0:
      return "U0";
    case 1:
      return "U1";
    case 2:
      return "U2";
    case 3:
      return "U3";
    case 4:
      return "Disabled";
    case 5:
      return "RxDetect";
    case 6:
      return "Inactive";
    case 7:
      return "Polling";
    case 8:
      return "Recovery";
    case 9:
      return "HotReset";
    case 10:
      return "Compliance";
    case 11:
      return "Test";
    case 15:
      return "Resume";
    default:
      return "reserved";
  }
}

/**
  Name of an xHCI port speed, i.e. the protocol speed ID PORTSC reports.

  0-5 are the specification's default speed IDs (spec 7.2.2); anything above
  them is a protocol's own PSI entry and is printed as a bare number.

  @param[in] Speed  PORTSC's speed field.

  @return A constant string.
**/
STATIC
CONST CHAR8 *
A733Xhci2SpeedName (
  IN UINT32  Speed
  )
{
  switch (Speed) {
    case 0:
      return "no device";
    case 1:
      return "full";
    case 2:
      return "low";
    case 3:
      return "high";
    case 4:
      return "super";
    case 5:
      return "super-plus";
    default:
      return "psi";
  }
}

/**
  Walk the extended capability list and print every Supported Protocol entry.

  The Supported Protocol capability (spec 1.1 table 7-20) is the only thing
  that maps root port numbers to a USB generation, so it is read rather than
  inferred from the port index: DW0 carries the major revision and the first
  port number, DW1 the minor revision, the protocol speed ID count and the
  two-character name, DW2 the port offset and count, and PSIC dwords of speed
  IDs follow DW2.  Each entry is printed with its own offset, so a port line
  below can name the entry it was mapped from.

  Read-only: the list is walked but nothing is written.  The walk stops after
  A733_XHCI_LOG_MAX_EXT_CAPS entries, at a zero Next pointer, or at an offset
  outside the xHCI window - a corrupted pointer is a misread, not a list.

  @param[in]  HcCapParams1   HCCPARAMS1, whose xECP field starts the list.
  @param[out] Protocols      Receives the Supported Protocol entries found.
  @param[out] ProtocolCount  Receives how many were stored.

  @retval EFI_SUCCESS        At least one Supported Protocol entry was found.
  @retval EFI_NOT_FOUND      The list has none, or HCCPARAMS1 has no list.
**/
STATIC
EFI_STATUS
A733Xhci2DumpProtocols (
  IN  UINT32                HcCapParams1,
  OUT A733_XHCI2_PROTOCOL   *Protocols,
  OUT UINT32                *ProtocolCount
  )
{
  UINT32  Offset;
  UINT32  Tries;
  UINT32  Next;
  UINT32  Cap;
  UINT32  Dw1;
  UINT32  Dw2;
  UINT32  Id;
  UINT32  Major;
  UINT32  Minor;
  UINT32  Psic;
  UINT32  MinPort;
  UINT32  PortOffset;
  UINT32  PortCount;
  UINT32  Index;
  UINT32  Psi;
  CHAR8   Name[3];

  *ProtocolCount = 0;

  Offset = (HcCapParams1 & A733_XHCI_HCCPARAMS1_XECP_MASK) >> A733_XHCI_HCCPARAMS1_XECP_SHIFT;
  if (Offset == 0) {
    DEBUG ((DEBUG_INFO, "A733Xhci2: xHCI hccparams1 has no extended capability list (xECP=0)\n"));
    return EFI_NOT_FOUND;
  }

  //
  // xECP is a dword offset from the register base, as is every Next pointer.
  //
  Offset <<= 2;

  for (Tries = 0; Tries < A733_XHCI_LOG_MAX_EXT_CAPS; Tries++) {
    if (Offset >= A733_XHCI_WINDOW_SIZE) {
      DEBUG ((
        DEBUG_WARN,
        "A733Xhci2: xHCI extended capability pointer 0x%04x is outside the window; "
        "stopping the walk\n",
        Offset
        ));
      break;
    }

    Cap = MmioRead32 ((UINTN)A733_DWC3_BASE + Offset);
    Id  = A733_XHCI_EXT_CAP_ID (Cap);
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: xHCI extcap +0x%04x id=%u next=+0x%04x raw=0x%08x\n",
      Offset,
      Id,
      (UINT32)(A733_XHCI_EXT_CAP_NEXT (Cap) << 2),
      Cap
      ));

    if (Id == A733_XHCI_EXT_CAP_ID_SUPPORTED_PROTOCOL) {
      Dw1        = MmioRead32 ((UINTN)A733_DWC3_BASE + Offset + 4);
      Dw2        = MmioRead32 ((UINTN)A733_DWC3_BASE + Offset + 8);
      Major      = (Cap >> A733_XHCI_SUPPROT_MAJREV_SHIFT) & 0xFFU;
      Minor      = Dw1 & A733_XHCI_SUPPROT_MINREV_MASK;
      Psic       = (Dw1 & A733_XHCI_SUPPROT_PSIC_MASK) >> A733_XHCI_SUPPROT_PSIC_SHIFT;
      MinPort    = (Cap >> A733_XHCI_SUPPROT_MINPORT_SHIFT) & 0xFFU;
      PortOffset = Dw2 & A733_XHCI_SUPPROT_PORTOFF_MASK;
      PortCount  = (Dw2 & A733_XHCI_SUPPROT_PORTCOUNT_MASK) >> A733_XHCI_SUPPROT_PORTCOUNT_SHIFT;

      //
      // The name is two UTF-16LE characters in DW1 bits 31:16, so only its low
      // byte per character is printable ASCII.
      //
      Name[0] = (CHAR8)((Dw1 >> A733_XHCI_SUPPROT_NAME_SHIFT) & 0xFFU);
      Name[1] = (CHAR8)((Dw1 >> (A733_XHCI_SUPPROT_NAME_SHIFT + 8)) & 0xFFU);
      Name[2] = '\0';

      DEBUG ((
        DEBUG_INFO,
        "A733Xhci2: xHCI protocol rev=%u.%u ports %u..%u (minport=%u offset=%u count=%u) "
        "psic=%u name=\"%a\"\n",
        Major,
        Minor,
        PortOffset,
        PortOffset + PortCount - 1U,
        MinPort,
        PortOffset,
        PortCount,
        Psic,
        Name
        ));

      //
      // The speed IDs are what PORTSC's speed field will report for a device on
      // this protocol's ports; printing them is what makes that field readable
      // without assuming the specification's defaults apply.
      //
      for (Index = 0; Index < Psic; Index++) {
        Psi = MmioRead32 ((UINTN)A733_DWC3_BASE + Offset + 12 + 4 * Index);
        DEBUG ((
          DEBUG_INFO,
          "A733Xhci2: xHCI protocol psi[%u] psiv=%u psie=%u plt=%u pfd=%u\n",
          Index,
          Psi & A733_XHCI_PSI_PSIV_MASK,
          (Psi >> A733_XHCI_PSI_PSIE_SHIFT) & 0x3U,
          (Psi & A733_XHCI_PSI_PLT) != 0,
          (Psi & A733_XHCI_PSI_PFD) != 0
          ));
      }

      if (*ProtocolCount < A733_XHCI_LOG_MAX_PROTOCOLS) {
        Protocols[*ProtocolCount].FirstPort = PortOffset;
        //
        // bPortCount is a count, so the range is offset .. offset + count - 1.
        // A zero count is a malformed entry: LastPort then lands below
        // FirstPort and the range covers no port.
        //
        Protocols[*ProtocolCount].LastPort = PortOffset + PortCount - 1U;
        Protocols[*ProtocolCount].Major     = Major;
        Protocols[*ProtocolCount].Minor     = Minor;
        Protocols[*ProtocolCount].Psic      = Psic;
        (*ProtocolCount)++;
      }
    }

    //
    // A zero Next pointer ends the list; anything else is a dword offset to
    // the capability after this one.
    //
    Next = A733_XHCI_EXT_CAP_NEXT (Cap);
    if (Next == 0) {
      break;
    }

    Offset += 4 * Next;
  }

  return (*ProtocolCount != 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

/**
  Name the protocol entry a root port belongs to.

  @param[in] Port           Root port number, 1-based.
  @param[in] Protocols      The Supported Protocol entries found.
  @param[in] ProtocolCount  How many there are.

  @return The entry's major revision (2 or 3), or 0 if no entry covers the port.
**/
STATIC
UINT32
A733Xhci2PortMajor (
  IN UINT32                    Port,
  IN CONST A733_XHCI2_PROTOCOL  *Protocols,
  IN UINT32                    ProtocolCount
  )
{
  UINT32  Index;

  for (Index = 0; Index < ProtocolCount; Index++) {
    if ((Port >= Protocols[Index].FirstPort) &&
        (Port <= Protocols[Index].LastPort))
    {
      return Protocols[Index].Major;
    }
  }

  return 0;
}

/**
  Power every root port, then report what each one sees.

  PP has to be set before CCS means anything: an unpowered port's connect
  status is not sampled by the PHY, so a port left at PP=0 would report an
  empty port whether or not something is plugged in.  The write is a
  read-modify-write of PP alone - PORTSC's change bits are RW1C, PED is RW1CS
  and PR/WPR are RW1S, so writing the value back as read would clear every
  pending change and disable a port that is already up (see
  A733_XHCI_PORTSC_NO_WRITE_MASK).  The PLS bits are harmless to carry through
  because PLS only changes when LWS is written with it, and LWS is not set
  here.  Nothing else in the register is touched, and the controller is not
  started or reset: HCRST and RS belong to XhciDxe on bind, and its own init
  powers the ports.  The core is halted while this runs, so the PORTSC lines
  are the reset state, not a connect verdict.

  The read is then taken after the specification's 20 ms power-up allowance
  (spec 1.1, PORTSC's PP description: a port is powered within 20 ms of the bit
  being set), which is also long enough for the PHY's connect detector to
  report a line state.

  Nothing here is interpreted from the port index alone: the protocol each port
  belongs to comes from the Supported Protocol capabilities, and the summary
  line says whether a connect was seen and which protocol it landed on.

  @param[in] Op             Operational register base.
  @param[in] Ports          Root port count, already clamped.
  @param[in] Protocols      The Supported Protocol entries found.
  @param[in] ProtocolCount  How many there are.
**/
STATIC
VOID
A733Xhci2ReportPorts (
  IN UINT32                    Op,
  IN UINT32                    Ports,
  IN CONST A733_XHCI2_PROTOCOL  *Protocols,
  IN UINT32                    ProtocolCount
  )
{
  UINT32      Index;
  UINT32      Port;
  UINT32      PortSc;
  UINT32      Pls;
  UINT32      Speed;
  UINT32      Major;
  UINT32      ConnectedPort;
  CONST CHAR8 *Protocol;
  CONST CHAR8 *Class;

  //
  // No port-power write here: this core's HCCPARAMS1 reports PPC = 0 (no
  // port power control), PORTSC.PP reads 1 from reset and is not writable,
  // so the "power the ports, wait 200 ms" step this function once ran was
  // a no-op that cost 200 ms of every boot.  XhciDxe powers and scans the
  // ports for real after it binds.
  //
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: xHCI ports (ppc=0, pp reads 1 from reset): usbcmd=0x%08x (rs=%u)\n",
    MmioRead32 (Op + A733_XHCI_USBCMD),
    (MmioRead32 (Op + A733_XHCI_USBCMD) & A733_XHCI_USBCMD_RS) != 0
    ));

  ConnectedPort = 0;
  for (Index = 0; Index < Ports; Index++) {
    Port         = Index + 1;
    PortSc       = MmioRead32 (Op + A733_XHCI_PORTSC + A733_XHCI_PORTSC_STRIDE * Index);
    Pls          = (PortSc >> A733_XHCI_PORTSC_PLS_SHIFT) & A733_XHCI_PORTSC_PLS_MASK;
    Speed        = (PortSc >> A733_XHCI_PORTSC_SPEED_SHIFT) & A733_XHCI_PORTSC_SPEED_MASK;
    Major        = A733Xhci2PortMajor (Port, Protocols, ProtocolCount);
    Protocol     = (Major == 0) ? "unmapped" : ((Major >= 3) ? "USB3" : "USB2");

    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: port %u [%a] portsc=0x%08x ccs=%u ped=%u pls=%u(%a) pp=%u "
      "speed=%u(%a) csc=%u prc=%u\n",
      Port,
      Protocol,
      PortSc,
      (PortSc & A733_XHCI_PORTSC_CCS) != 0,
      (PortSc & A733_XHCI_PORTSC_PED) != 0,
      Pls,
      A733Xhci2LinkStateName (Pls),
      (PortSc & A733_XHCI_PORTSC_PP) != 0,
      Speed,
      A733Xhci2SpeedName (Speed),
      (PortSc & A733_XHCI_PORTSC_CSC) != 0,
      (PortSc & A733_XHCI_PORTSC_PRC) != 0
      ));

    if ((PortSc & A733_XHCI_PORTSC_CCS) != 0) {
      if (ConnectedPort == 0) {
        ConnectedPort = Port;
      }

      Class = (Major == 0) ? "unmapped protocol" : ((Major >= 3) ? "SuperSpeed" : "USB2");
      DEBUG ((
        DEBUG_INFO,
        "A733Xhci2: port %u sees a connect (%a, speed=%u %a, link state %a)\n",
        Port,
        Class,
        Speed,
        A733Xhci2SpeedName (Speed),
        A733Xhci2LinkStateName (Pls)
        ));
    }
  }

  if (ConnectedPort == 0) {
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: no port reports a connect (%u port(s) read, ccs=0 on all)\n",
      Ports
      ));
    //
    // A powered port with CCS=0 is not a controller problem any more: the core
    // answered, its ports answer, and the connect detector is the PHY's.  What
    // is left is the board - the hub's own 3.3 V rail (DCDC1) and its upstream
    // port's wiring - all of which the lines above already printed, so the hint
    // below names them rather than speculating.
    //
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: no connect with PP=1 and rs=%u points at the PHY/board, not at the core: "
      "check the U2P0 siddq/physts lines above, the DCDC1 read-back (enabled=%u) and that the "
      "hub's upstream port is wired to this controller's USB2 port\n",
      (MmioRead32 (Op + A733_XHCI_USBCMD) & A733_XHCI_USBCMD_RS) != 0,
      (UINT32)mHubRailEnabled
      ));
    return;
  }

  Major = A733Xhci2PortMajor (ConnectedPort, Protocols, ProtocolCount);
  if (Major >= 3) {
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: connect is on port %u: the device sits on the SuperSpeed protocol, "
      "so its lane is COMB0, which A733Xhci2Combo0On brought up\n",
      ConnectedPort
      ));
  } else if (Major == 2) {
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: connect is on port %u: the device sits on the USB2 protocol, i.e. the "
      "U2P0/femtoPHY lane this driver brought up\n",
      ConnectedPort
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: connect is on port %u, which no supported-protocol entry covers\n",
      ConnectedPort
      ));
  }
}

/**
  Report the xHCI registers the core presents once it is in host mode.

  CAPLENGTH is the specification's own window into the block and the cheapest
  proof that the core answered: a gated core, a core still arbitrating its role,
  or one whose PHY interface has no clock reads zero here, and XhciDxe asserts on
  exactly that (its XhciReg.c checks CapLength before anything else).  HCSPARAMS1
  gives the port count, HCCPARAMS1 the capability flags and the extended
  capability pointer, USBSTS the state the controller is in.

  Then the two things that say whether anything is attached:

  - The extended capability list, so the Supported Protocol entries state which
    root port is USB2 and which is USB3 instead of the log assuming it.
  - Every root port, powered first (PP) and read back as raw PORTSC with CCS,
    PED, PLS, speed, CSC and PRC decoded.  The port register sets are at
    "base + CAPLENGTH + 0x400 + 0x10 * (n - 1)" (spec 5.4.8; the manual's
    register-class table puts the whole xHCI window at 0x06A00000 + 0..0x7FFF
    and defers the layout to the xHCI specification - see <A733Dwc3.h>).

  The controller is halted when this runs: the port lines describe the reset
  state only.  XhciDxe performs the host-controller reset, starts the core and
  does the meaningful port scan after it binds.

  @return CAPLENGTH; 0 or 0xff means the block did not answer.
**/
STATIC
UINT32
A733Xhci2DumpXhci (
  VOID
  )
{
  UINT32                CapLength;
  UINT32                HcStructParams1;
  UINT32                HcCapParams1;
  UINT32                Ports;
  UINT32                Op;
  UINT32                UsbSts;
  A733_XHCI2_PROTOCOL   Protocols[A733_XHCI_LOG_MAX_PROTOCOLS];
  UINT32                ProtocolCount;

  CapLength       = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_XHCI_CAPLENGTH) & 0xFFU;
  HcStructParams1 = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_XHCI_HCSPARAMS1);
  HcCapParams1    = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_XHCI_HCCPARAMS1);
  Ports           = (HcStructParams1 & A733_XHCI_HCSPARAMS1_MAXPORTS_MASK) >>
                    A733_XHCI_HCSPARAMS1_MAXPORTS_SHIFT;

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: xHCI caplength=0x%02x hciversion=0x%04x hcsparams1=0x%08x "
    "(maxslots=%u maxports=%u) hccparams1=0x%08x (ppc=%u xecp=+0x%04x)\n",
    CapLength,
    (UINT32)(MmioRead32 ((UINTN)A733_DWC3_BASE + A733_XHCI_CAPLENGTH) >> 16),
    HcStructParams1,
    HcStructParams1 & A733_XHCI_HCSPARAMS1_MAXSLOTS_MASK,
    Ports,
    HcCapParams1,
    (HcCapParams1 & A733_XHCI_HCCPARAMS1_PPC) != 0,
    (UINT32)((HcCapParams1 & A733_XHCI_HCCPARAMS1_XECP_MASK) >> A733_XHCI_HCCPARAMS1_XECP_SHIFT) << 2
    ));

  if ((CapLength == 0) || (CapLength == 0xFF)) {
    DEBUG ((
      DEBUG_WARN,
      "A733Xhci2: the controller does not answer (caplength=0x%02x); its operational "
      "registers are unreadable and XhciDxe would assert on them\n",
      CapLength
      ));
    return CapLength;
  }

  Op     = (UINT32)A733_DWC3_BASE + CapLength;
  UsbSts = MmioRead32 (Op + A733_XHCI_USBSTS);
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: xHCI usbsts=0x%08x (hch=%u cnr=%u)\n",
    UsbSts,
    (UsbSts & A733_XHCI_USBSTS_HCH) != 0,
    (UsbSts & A733_XHCI_USBSTS_CNR) != 0
    ));

  A733Xhci2DumpProtocols (HcCapParams1, Protocols, &ProtocolCount);

  if (Ports > A733_XHCI_LOG_MAX_PORTS) {
    Ports = A733_XHCI_LOG_MAX_PORTS;
  }

  A733Xhci2ReportPorts (Op, Ports, Protocols, ProtocolCount);

  return CapLength;
}


/**
  Five-second post-publication probe: one line per read, no writes.

  Runs with a timer event so it lands after XhciDxe's bind + enumerate; see
  the call site in the entry point for what this read is for.
**/
STATIC
VOID
EFIAPI
A733Xhci2ProbeCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  CapLength;
  UINT32  Op;
  UINT32  PortSc0;
  UINT32  PortSc1;

  CapLength = MmioRead32 ((UINTN)A733_DWC3_BASE) & 0xFFU;
  Op        = (UINT32)A733_DWC3_BASE + CapLength;
  PortSc0   = MmioRead32 (Op + A733_XHCI_PORTSC);
  PortSc1   = MmioRead32 (Op + A733_XHCI_PORTSC + A733_XHCI_PORTSC_STRIDE);

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: probe@5s: usbcmd=0x%08x usbsts=0x%08x (rs=%u hch=%u cnr=%u)\n",
    MmioRead32 (Op + A733_XHCI_USBCMD),
    MmioRead32 (Op + A733_XHCI_USBSTS),
    (MmioRead32 (Op + A733_XHCI_USBCMD) & A733_XHCI_USBCMD_RS) != 0,
    (MmioRead32 (Op + A733_XHCI_USBSTS) & A733_XHCI_USBSTS_HCH) != 0,
    (MmioRead32 (Op + A733_XHCI_USBSTS) & A733_XHCI_USBSTS_CNR) != 0
    ));
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: probe@5s: portsc0=0x%08x (ccs=%u ped=%u pls=%u pp=%u speed=%u "
    "csc=%u prc=%u) portsc1=0x%08x\n",
    PortSc0,
    PortSc0 & A733_XHCI_PORTSC_CCS ? 1U : 0U,
    PortSc0 & A733_XHCI_PORTSC_PED ? 1U : 0U,
    (PortSc0 >> A733_XHCI_PORTSC_PLS_SHIFT) & A733_XHCI_PORTSC_PLS_MASK,
    PortSc0 & A733_XHCI_PORTSC_PP ? 1U : 0U,
    (PortSc0 >> A733_XHCI_PORTSC_SPEED_SHIFT) & A733_XHCI_PORTSC_SPEED_MASK,
    PortSc0 & A733_XHCI_PORTSC_CSC ? 1U : 0U,
    PortSc0 & A733_XHCI_PORTSC_PRC ? 1U : 0U,
    PortSc1
    ));
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: probe@5s: iscr=0x%08x (line=%u vbus=%u) phyctl=0x%08x "
    "CAL: 0xD0=0x%08x 0xD4=0x%08x 0xD8=0x%08x 0xDC=0x%08x\n",
    MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_ISCR)),
    (UINT32)((MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_ISCR)) >> 25) & 0x3U),
    (MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_ISCR)) & BIT24) ? 1U : 0U,
    MmioRead32 (Xhci2AppReg (A733_DWC3_U2P0_PHYCTL)),
    MmioRead32 (Xhci2AppReg (0xD0)),
    MmioRead32 (Xhci2AppReg (0xD4)),
    MmioRead32 (Xhci2AppReg (0xD8)),
    MmioRead32 (Xhci2AppReg (0xDC))
    ));
}

/**
  Entry point: bring the DRD up in host mode and publish its controller.

  Step order and the reason for it are in the head of this file; every step
  prints its own read-back, so a log that stops is a step that did not finish.

  @param[in] ImageHandle  The driver's image handle.
  @param[in] SystemTable  The boot services and runtime services tables.

  @retval EFI_SUCCESS       The controller was brought up and published.
  @retval Others            A step failed or the controller does not answer; it
                            is not published, and the log line above says which.
**/
//
// The u2phy (femtoPHY) application block at A733_USB2_PHY_BASE: one
// PHY_CTRL/PHYTUNE pair per port, 0x20 apart, and the shared ISCR.  The two
// written values are the boot0 defaults edk2-a733 reads back on this board;
// PHY_CTRL carries SIDDQ in bit 3 (1 = analog powered down).
//
#define A733_XHCI2_PHY_CTRL(n)     (0x0010U + (0x20U * (n)))
#define A733_XHCI2_PHYTUNE(n)      (0x0018U + (0x20U * (n)))
#define A733_XHCI2_PHY_CTRL_OFF    0x000E2418U   // SIDDQ=1, boot0 default
#define A733_XHCI2_PHYTUNE_DEFAULT 0x143333D4U

//
// GCTL as the core reports it before this driver writes anything: the
// power-on state the ExitBootServices callback restores.  Zero means the
// read never happened, in which case the callback leaves GCTL alone.
//
STATIC UINT32  mDwc3GctlPowerOn;

//
// ExitBootServices: park the DWC3 core so the OS inherits a controller it
// can reset, instead of a running host with schedules armed.
//
// XhciDxe leaves USBCMD.RS set with transfer rings live.  Linux's
// dwc3_core_init() then runs its own core soft reset against that state and
// times out ("DWC3 controller soft reset failed", -ETIMEDOUT), and the USB3
// port stays dead for the whole boot.  Holding the core and both PHY
// interfaces in soft reset here neutralises the xHCI view and stops all
// in-flight DMA; dwc3_core_init() re-runs exactly this assert/clear cycle
// before it touches anything else.
//
// CORESOFTRESET does not self-clear: leaving it asserted freezes the core and
// the OS's reset then times out for the same reason, so the cycle below
// releases it the way the bring-up does (PHYs first, core last).
//
// Boot services are gone when this runs: raw MMIO, bounded polls, no DEBUG.
//
STATIC
VOID
EFIAPI
A733Xhci2ExitBootServicesPark (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  CapLength;
  UINTN   OpBase;
  UINTN   Try;

  CapLength = MmioRead32 ((UINTN)A733_DWC3_BASE) & 0xFFU;
  OpBase    = (UINTN)A733_DWC3_BASE + CapLength;

  //
  // Stop the host and reset it: every step is best-effort, the core reset
  // below is what actually neutralises the controller.
  //
  MmioAnd32 (OpBase + A733_XHCI_USBCMD, ~(UINT32)A733_XHCI_USBCMD_RS);
  for (Try = 0; Try < 1000; Try++) {
    if ((MmioRead32 (OpBase + A733_XHCI_USBSTS) & A733_XHCI_USBSTS_HCH) != 0) {
      break;
    }
    MicroSecondDelay (10);
  }
  MmioOr32 (OpBase + A733_XHCI_USBCMD, A733_XHCI_USBCMD_HCRST);
  for (Try = 0; Try < 1000; Try++) {
    if ((MmioRead32 (OpBase + A733_XHCI_USBCMD) & A733_XHCI_USBCMD_HCRST) == 0) {
      break;
    }
    MicroSecondDelay (10);
  }

  //
  // Core + both PHY interfaces: assert, hold, then release in the order the
  // bring-up uses.
  //
  MmioOr32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL, A733_DWC3_GCTL_CORESOFTRESET);
  MmioOr32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB3PIPECTL (0), A733_DWC3_GUSB3PIPECTL_PHYSOFTRST);
  MmioOr32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB2PHYCFG (0), A733_DWC3_GUSB2PHYCFG_PHYSOFTRST);
  MicroSecondDelay (100 * 1000);

  MmioAnd32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB3PIPECTL (0), ~(UINT32)A733_DWC3_GUSB3PIPECTL_PHYSOFTRST);
  MmioAnd32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GUSB2PHYCFG (0), ~(UINT32)A733_DWC3_GUSB2PHYCFG_PHYSOFTRST);
  MicroSecondDelay (100 * 1000);

  MmioAnd32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL, ~(UINT32)A733_DWC3_GCTL_CORESOFTRESET);
  MicroSecondDelay (1000);

  //
  // GCTL back to the power-on snapshot this driver read at entry, which has
  // PRTCAPDIR at the reset role.  The bring-up left it in host mode, and a
  // role the core was not reset into is not what the OS's role switch
  // expects to start from.
  //
  if (mDwc3GctlPowerOn != 0) {
    MmioWrite32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL, mDwc3GctlPowerOn);
  }

  //
  // Hand the rest over the way a power-on boot would find it, because that is
  // the state the OS's own bring-up is written against.  edk2-a733 measured
  // each of these on this SoC (its X19/X22/X23 experiments), and the failure
  // they fix is the one this board shows when they are missing:
  //
  // 1. The femtoPHY goes back UNINITIALISED - SIDDQ set, boot0 tune - and the
  //    USB2 power domain off.  The kernel's phy_init then performs the SIDDQ
  //    1->0 transition that starts its calibration; a PHY handed over already
  //    powered up has no transition to give, its calibration never runs, and
  //    the core soft reset times out (-110).
  // 2. The SerDes mux select is released.  This driver takes COMB0 for the
  //    SuperSpeed lane, and PHY_USE_SEL left set is what made the kernel's
  //    first probe fail in edk2-a733's X17/X19 runs; the kernel's combophy
  //    driver owns the choice from here.
  // 3. COMB0 is left cold: IDDQ asserted and its PHY reset re-asserted.  The
  //    working hand-over state on this board is "combo never powered"
  //    (edk2-a733's X20), and the kernel powers it up itself.
  //
  //
  // The USB2 power domain is deliberately left ON.  edk2-a733 also powers it
  // down here (its X19), but on this board that takes GSNPSID with it: the
  // kernel then reports "this is not a DesignWare USB3 DRD Core" before it
  // reaches the reset, because the core has no clock to answer a register
  // read.  What the kernel actually needs from this hand-over is the SIDDQ
  // 1->0 transition, and that is the femtoPHY's own state, not the domain's.
  //
  MmioWrite32 ((UINTN)A733_USB2_PHY_BASE + A733_USB2_PHY_ISCR, 0);
  MmioWrite32 (
    (UINTN)A733_USB2_PHY_BASE + A733_XHCI2_PHY_CTRL (0),
    A733_XHCI2_PHY_CTRL_OFF
    );
  MmioWrite32 (
    (UINTN)A733_USB2_PHY_BASE + A733_XHCI2_PHYTUNE (0),
    A733_XHCI2_PHYTUNE_DEFAULT
    );

  MmioAnd32 (
    (UINTN)A733_SERDES_BASE + A733_SERDES_HSI_GBLC,
    ~(UINT32)A733_SERDES_HSI_GBLC_PHY_USE_SEL
    );

  MmioOr32 (
    (UINTN)A733_SERDES_BASE + A733_SERDES_COMBO0_LINK_CTRL2,
    A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SEL | A733_SERDES_COMBO0_CTRL2_IDDQ_EN_SOFT
    );
  MmioAnd32 (
    (UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR,
    ~(UINT32)A733_SERDES_USB3P1_BGR_U2P0_PHY_RSTN
    );
}

EFI_STATUS
EFIAPI
A733Xhci2DxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      CapLength;

  DEBUG ((DEBUG_INFO, "A733Xhci2: USB3.1 GEN2 DRD (xHCI2) host bring-up\n"));

  //
  // Snapshot the core before this driver writes anything.  CNR is set by the
  // core at power-on and cleared only when its own initialisation finishes, and
  // while it is set every write to the operational registers is ignored - which
  // is what made HCRST and RS read back unchanged.  Whether it is already set
  // here, before any of this driver's writes, is the difference between "our
  // reset put it there" and "it never came up at all", and only one of those is
  // worth debugging further.
  //
  {
    UINT32  EntryCap;
    UINT32  EntryOp;

    EntryCap = MmioRead32 ((UINTN)A733_DWC3_BASE);
    EntryOp  = (UINTN)A733_DWC3_BASE + ((EntryCap >> 0) & 0xFFU);
    mDwc3GctlPowerOn = MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL);

    DEBUG ((
      DEBUG_INFO,
      "A733Xhci2: before any write: CAPLENGTH=0x%02x GSNPSID=0x%08x GCTL=0x%08x "
      "DSTS=0x%08x USBCMD=0x%08x USBSTS=0x%08x (cnr=%u hch=%u)\n",
      EntryCap & 0xFFU,
      MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GSNPSID),
      MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL),
      MmioRead32 ((UINTN)A733_DWC3_BASE + 0x0000C118U),
      MmioRead32 (EntryOp + A733_XHCI_USBCMD),
      MmioRead32 (EntryOp + A733_XHCI_USBSTS),
      (UINT32)((MmioRead32 (EntryOp + A733_XHCI_USBSTS) & A733_XHCI_USBSTS_CNR) != 0),
      (UINT32)((MmioRead32 (EntryOp + A733_XHCI_USBSTS) & A733_XHCI_USBSTS_HCH) != 0)
      ));
  }

  Status = A733Xhci2PowerDomainOn ();
  if (EFI_ERROR (Status)) {
    //
    // Not fatal to the attempt: the reads below say whether the block answered
    // anyway, and U2P0's window reading back non-zero would say the domain was
    // on in practice.
    //
    DEBUG ((DEBUG_WARN, "A733Xhci2: continuing without the USB2 power domain\n"));
  }

  A733Xhci2CcuOn ();
  A733Xhci2SerdesOn ();

  //
  // The USB2.0 port's own PHY, and the board it sits on, before COMB0.  This is
  // the working firmware's order, and it is worth keeping: the femtoPHY's
  // power-up is what gives the core a UTMI clock, and COMB0's bring-up ends in a
  // poll of the PMA's common block, which is the one step of this driver that
  // costs milliseconds and that a PHY with nothing else awake yet can fail.
  //
  A733Xhci2HubRailOn ();
  A733Xhci2PhyRailOn ();
  A733Xhci2HubPowerOn ();
  A733Xhci2PhyUp ();

  //
  // COMB0 next: the SerDes subsystem's gates are up, and the core reset below
  // wants a PHY behind its PIPE interface.  A PHY that does not come ready is
  // logged and stepped over - the controller is worth registering either way.
  //
  //
  // COMB0 is deliberately NOT brought up.  The configuration that attaches the
  // board's hub is the one without it: the working history in edk2-a733 records
  // "USE_SEL=0, BIT21=0, combo skipped -> hub attaches at high speed in 3.5 s"
  // against "USE_SEL=1, BIT21=0 -> no attach", and the difference is the combo's
  // own live state - a PIPE interface driven by a running PMA is not what the
  // USB2 side of this core wants to see on its port.
  //
  // What the core does need is the UTMI clock, and SERDES+0x0008 BIT21 selects
  // it from the CCU rather than from COMB0; that bit is set for the length of the
  // core reset in A733Xhci2CoreInit and is what makes CNR clear.  So: clock from
  // the CCU, mux left alone.
  //
  Status = A733Xhci2Combo0On ();

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "A733Xhci2: continuing without COMB0 (%r); the SuperSpeed port will report an empty port\n",
      Status
      ));
  }

  //
  // X17 (the single-variable change in which this board's hub first
  // enumerated at 480 Mbit/s, then 20/20 cold boots): hand the core's USB2
  // UTMI clock from the CCU back to the femtoPHY now that COMB0 is up, and
  // before the DWC3 reset dance, so the core leaves reset on the PHY clock
  // and stays on it.  Steady state SERDES+0x0008 = 0x00030010, the golden
  // value.  With this select left set, ISCR reads line state J while the
  // running xHC never reports CCS; an HCRST run while it is set later makes
  // XhciDxe's own reset time out and lose the xHCI register view.
  //
  MmioAnd32 (
    (UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR,
    ~(UINT32)A733_SERDES_USB3P1_BGR_U2P0_UTMI_CLK_SEL
    );
  MicroSecondDelay (10);
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: USB2 UTMI clock handed back to the femtoPHY: SERDES+0x0008=0x%08x "
    "(utmi_clk_sel=%u)\n",
    MmioRead32 ((UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR),
    (MmioRead32 ((UINTN)A733_SERDES_BASE + A733_SERDES_SUBSYS_USB3P1_BGR) &
     A733_SERDES_USB3P1_BGR_U2P0_UTMI_CLK_SEL) != 0
    ));

  Status = A733Xhci2CoreInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: U2P0 CAL after core reset: 0xD0=0x%08x 0xD4=0x%08x 0xD8=0x%08x 0xDC=0x%08x\n",
    MmioRead32 (Xhci2AppReg (0xD0)),
    MmioRead32 (Xhci2AppReg (0xD4)),
    MmioRead32 (Xhci2AppReg (0xD8)),
    MmioRead32 (Xhci2AppReg (0xDC))
    ));

  //
  // Read-only readiness and port report.  No host-controller reset is run
  // here, and USBCMD.RS is not set: the working firmware (edk2-a733 X17/X23)
  // leaves both to the stock XhciDxe, whose HCRST is the only xHCI-block
  // reset in the boot.  Doing it here with the CCU UTMI clock selected and
  // then handing the running core to XhciDxe makes the driver's own HCRST
  // time out (it needs the femtoPHY clock the running port needs), while
  // pre-starting RS with no schedules programmed leaves the core in a state
  // XhciDxe then finds unreadable.  The core is halted here by design; CCS is
  // not maintained while halted, so the port lines are an observation of the
  // reset state, not a connect verdict - XhciDxe's scan after its own reset
  // and RS is.
  //
  CapLength = A733Xhci2DumpXhci ();
  if ((CapLength == 0) || (CapLength == 0xFF)) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Publish the controller.  NonCoherent is the right DMA type for this part:
  // the core's AXI master does not snoop the CPU caches, so buffers have to be
  // handed over with cache maintenance rather than assumed shared.  The device
  // appears to XhciDxe as a PCI device with the xHCI class code, which is what
  // makes the stock driver's Supported() claim it.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeXhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             (UINTN)A733_DWC3_BASE,
             (UINTN)A733_DWC3_WINDOW_SIZE
             );
  DEBUG ((
    DEBUG_INFO,
    "A733Xhci2: xHCI2 register: %r (caplength=0x%02x GCTL=0x%08x)\n",
    Status,
    CapLength,
    MmioRead32 ((UINTN)A733_DWC3_BASE + A733_DWC3_GCTL)
    ));

  //
  // One-shot re-read, five seconds after publication: by then XhciDxe has
  // bound, reset the host controller, started it and (with the board's hub on
  // the USB2 port) reset the port and addressed the device.  The port lines
  // below are therefore the post-enumeration state - the same state the
  // working reference's shell reads back as PORTSC0=0x00000E03 (CCS + PED +
  // U0 + high speed) - and the ISCR line state says which idle the wire is
  // in (J = full-speed idle, SE0 = high-speed idle).  This is the read that
  // distinguishes "the chirp after the port reset failed and the port sits
  // at full speed" from "the port is at high speed and software only read
  // it too early".
  //
  {
    EFI_STATUS  EventStatus;
    EFI_EVENT   ProbeEvent;

    EventStatus = gBS->CreateEvent (
                         EVT_NOTIFY_SIGNAL | EVT_TIMER,
                         TPL_CALLBACK,
                         A733Xhci2ProbeCallback,
                         NULL,
                         &ProbeEvent
                         );
    if (!EFI_ERROR (EventStatus)) {
      gBS->SetTimer (ProbeEvent, TimerRelative, 5 * 1000000U);  // 5 s, 100 ns units
    }
  }

  //
  // Park the core at ExitBootServices (see the callback) so the OS inherits a
  // controller it can reset instead of a running host.
  //
  {
    EFI_STATUS  EventStatus;
    EFI_EVENT   ExitBsEvent;

    EventStatus = gBS->CreateEvent (
                         EVT_SIGNAL_EXIT_BOOT_SERVICES,
                         TPL_CALLBACK,
                         A733Xhci2ExitBootServicesPark,
                         NULL,
                         &ExitBsEvent
                         );
    if (EFI_ERROR (EventStatus)) {
      DEBUG ((DEBUG_WARN, "A733Xhci2: EBS park event: %r\n", EventStatus));
    }
  }

  return Status;
}