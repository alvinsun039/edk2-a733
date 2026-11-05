/** @file
  USB 2.0 host bring-up for the Allwinner A733 (sun60iw2p1).

  This driver is the SoC-side glue only.  It powers the port, gives the
  controller its clocks, takes the PHY out of power-down, and then publishes the
  controller as a non-discoverable PCI device so that MdeModulePkg's stock
  EhciDxe binds to it; UsbBusDxe, UsbKbDxe and UsbMassStorageDxe sit above that
  and are not this driver's business.  Nothing here implements USB.

  What the port is.  The A733's "USB2.0 HOST" controller (manual 18.6) is one
  EHCI 1.0 controller plus one OHCI 1.0a companion sharing a single root port:
  high-speed devices are the EHCI's, full- and low-speed ones the OHCI's, and
  devices behind a high-speed hub are reached by the EHCI's split transactions
  whichever speed they run at.  They live side by side in the USB1 window
  (0x04200000: EHCI at +0x0000, OHCI at +0x0400, the PHY's own registers at
  +0x0800 - see <A733Usb.h>).

  Only the EHCI controller is published.  edk2 has no OHCI driver - there is no
  MdeModulePkg/Bus/Pci/OhciDxe in this tree - and the board's user-reachable
  ports are on the far side of an on-board high-speed hub, so a full- or
  low-speed device (a keyboard) reaches this controller through the hub's
  transaction translator and the EHCI's split transactions.  A full- or
  low-speed device plugged into the bare root port would belong to the OHCI
  companion instead, and would need a driver this tree does not have; the port
  status in the log says which of the two situations the hardware is in
  (PORTSC's owner bit), so that is the line to read before looking elsewhere.

  Order, and why it is that order:

  1. PCK600's USB2 domain on, if it is not already (the PHY's registers are in
     it; a gated domain reads back as zero).
  2. CCU: module clock, the two bus gates, the two block resets (manual
     4.1.6.196-4.1.6.197).  The PHY reset stays asserted.
  3. PHY, while it is still in reset: AHB configuration, SIDDQ cleared, tune and
     UTMI words written.  Then the PHY reset is released and the controller is
     left alone for EhciDxe to reset itself.
  4. Port power: the board's 5 V enables, through the R_PIO pins the board's
     device tree and schematic name.
  5. Claim the root ports for the EHCI.  A port whose PORTSC.PortOwner is set
     belongs to the OHCI companion, and the EHCI reports neither a reset nor a
     connected device for it, so the on-board hub behind such a port is
     invisible.  CONFIGFLAG's route bit comes first (ports are the companion's
     until it is set) and then each port's owner bit is cleared, with the port
     state printed before and after so the log says whether the hub is on this
     controller and whether anything is on the port at all.  USBCMD is not
     written: the controller stays halted, for EhciDxe to reset and start.
  6. Publish the controller.  EhciDxe resets it, starts it, and its root hub
     enumerates whatever is on the port.

  The OTG port is published too.  The USB0 window (0x04100000: the MUSB core,
  EHCI0 at +0x1000, and the PHY's "pmu0" at +0x1800) is a second physical port
  with a second EHCI controller, and it is the one the OTG connector belongs to.
  The reference firmware for this board registers both host halves - its host
  driver logs EHCI0, EHCI1, OHCI0, OHCI1 and xHCI2 - so a full EHCI0
  initialisation (controller reset, CONFIGFLAG, port scan) runs on USB0 there
  before the MUSB device takes the pads back.  This driver registers EHCI0 the
  same way, right after EHCI1, so that EhciDxe performs exactly that
  initialisation when it binds; and MUSB still enumerates as the fastboot device
  afterwards.

  What is deliberately not done from here, because EhciDxe does it when it
  binds: no controller reset, no CONFIGFLAG, no port claim.  The EHCI half's
  CCU gate and reset (USB0_BGR bits 4 and 20) are opened only if the block does
  not answer, which is the case only when A733MusbDeviceDxe's platform phase
  has not run yet - it runs at dispatch and already leaves those bits open along
  with a powered PHY0.  Nothing else in the window is written from here: no PHY
  register, not the route bit (0x04100420, the FES/flashing path), no CONFIGFLAG.
  A block that still reports no CAPLENGTH after the gate is opened is not
  registered at all: a controller that cannot identify itself is one EhciDxe can
  do nothing with.  An earlier version of this file ran the host bring-up on
  EHCI0 itself - the PHY recipe, CONFIGFLAG=1, its port claimed - and the port
  read back ccs=0 with all of that in place; that sequence is gone, and what
  remains is the registration the reference has.

  Two readings are taken beside that sequence and change nothing.  The MUSB core
  at 0x04100000 is read, so the log says whether a session is up on the OTG
  connector, and it is read again once the 5 V rails have settled, because its
  DEVCTL VBUS field is the one reading that shows 5 V arriving rather than a pin
  we believe is high.  That survey is strictly read-only: no MUSB register and
  not the OTG PHY's route bit (0x04100420, the FES/flashing path) is written,
  and the MUSB core's own gate and reset are left as found.  EHCI0's survey is
  read-only in the same sense - the CCU words first, then the controller's
  identification registers and its ports if the block answers without its gates.
  The two VBUS enables are read back from the R_PIO registers after they are
  driven, so the log shows the pads as outputs driving high rather than that a
  write was issued.

  Everything is logged at DEBUG_INFO, one line per step, because the boot log is
  where a bring-up like this is diagnosed: the CCU words, the PHY words, the
  controller's identification registers and the port status are all printed, in
  that order, so the first step that did not take effect can be seen directly.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733CcuLib.h>
#include <Library/A733GpioLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <A733Ccu.h>
#include <A733Rpio.h>
#include <A733Usb.h>

//
// Time allowed for the PHY to come out of reset and for its PLL to lock.  The
// manual gives no figure; 1 ms is longer than the generic Allwinner PHY code
// waits (it takes no delay at all) and far shorter than a boot's USB timeout.
//
#define A733_USB_PHY_SETTLE_US  1000U

//
// Time allowed for a powered hub to finish its own power-on before the host
// looks at the port.  USB 2.0 (7.1.7.3, bPwrOn2PwrGood) allows a hub up to
// 100 ms and the specification adds a connect debounce after it; 200 ms covers
// both with margin, and it is spent once, before the controller is published.
//
#define A733_USB_VBUS_SETTLE_US  200000U

//
// Time allowed for the controller to act on a port claim.  The specification
// gives no figure for the PortOwner handover; 50 ms is the connect debounce it
// does specify (USB 2.0, 7.1.7.3), so it is longer than the handover needs and
// short enough to spend once per port.
//
#define A733_USB_PORT_CLAIM_SETTLE_US  50000U

//
// How long to wait for the USB2 power domain to report itself on.
//
#define A733_PPU_TRIES      100U
#define A733_PPU_POLL_US    100U

/**
  Address a register inside the USB1 window.

  @param[in] Base    Block base, one of A733_USB1_EHCI/HCI_BASE.
  @param[in] Offset  Register offset inside the block.

  @return The MMIO address of the register.
**/
STATIC
UINTN
UsbReg (
  IN UINTN   Base,
  IN UINT32  Offset
  )
{
  return Base + Offset;
}

/**
  Address a CCU register.

  @param[in] Offset  Register offset inside the CCU window.

  @return The MMIO address of the register.
**/
STATIC
UINTN
CcuReg (
  IN UINT32  Offset
  )
{
  return (UINTN)PcdGet64 (PcdA733CcuBase) + Offset;
}

/**
  Switch the PCK600 USB2 power domain on if it is not already on.

  The PHY's registers are in this domain: with it off they read back as zero,
  and every PHY write below would be discarded.  Nothing is switched off - the
  power domain outlives this firmware, and the OS's own power-domain driver
  takes the reference count from there.

  @retval EFI_SUCCESS  The domain reports itself powered on.
  @retval EFI_TIMEOUT  It did not, and the PHY registers cannot be expected to
                       hold what is written to them.
**/
STATIC
EFI_STATUS
A733UsbPowerDomainOn (
  VOID
  )
{
  UINTN   Base;
  UINTN   Tries;
  UINT32  Status;

  Base   = (UINTN)A733_PPU_BASE + (UINTN)A733_PPU_USB2_DOMAIN * A733_PPU_DOMAIN_STRIDE;
  Status = MmioRead32 (Base + A733_PPU_STATUS_OFFSET);
  if ((Status & A733_PPU_STATUS_MASK) == A733_PPU_STATUS_ON) {
    DEBUG ((DEBUG_INFO, "A733Usb: USB2 power domain already on (status 0x%08x)\n", Status));
    return EFI_SUCCESS;
  }

  MmioWrite32 (Base + A733_PPU_PWR_OFFSET, A733_PPU_COMMAND_ON);

  for (Tries = 0; Tries < A733_PPU_TRIES; Tries++) {
    MicroSecondDelay (A733_PPU_POLL_US);
    Status = MmioRead32 (Base + A733_PPU_STATUS_OFFSET);
    if ((Status & A733_PPU_STATUS_MASK) == A733_PPU_STATUS_ON) {
      DEBUG ((DEBUG_INFO, "A733Usb: USB2 power domain switched on (status 0x%08x)\n", Status));
      return EFI_SUCCESS;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "A733Usb: USB2 power domain did not come up (status 0x%08x); the PHY will not answer\n",
    Status
    ));
  return EFI_TIMEOUT;
}

/**
  Give the USB1 window its clocks and take its blocks out of reset.

  Manual 4.1.6.196-4.1.6.197 are USB1's clock and bus-gating-reset registers.
  USB0 has the same layout at +0x1300/+0x1304 (manual 4.1.6.194/4.1.6.195) and
  is left alone - see the head of this file.
  A733CcuEnableModule covers the module clock and the OHCI half of the BGR -
  its sub-block 0 is USBn_OHCI_GATING/USBn_OHCI_RST on this block - so the EHCI
  half and the PHY's reset, which no other module has, are written here.

  The PHY reset is deliberately left asserted: the PHY is configured first (see
  A733UsbPhyUp) and only then released.

  @retval EFI_SUCCESS            The gates are open and the block resets are released.
  @retval EFI_INVALID_PARAMETER  The CCU library rejected the module.
**/
STATIC
EFI_STATUS
A733UsbCcuOn (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Bgr;

  Status = A733CcuEnableModule (A733_CCU_MODULE_USB1);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Usb: CCU enable of USB1 failed: %r\n", Status));
    return Status;
  }

  //
  // A733CcuEnableModule has released the OHCI half's reset and opened its
  // gate (sub-block 0 of this BGR is USBn_OHCI_GATING/USBn_OHCI_RST).  The
  // EHCI half is opened here: its gate first, then its reset - a block that
  // leaves reset without a clock drives nothing.
  //
  Bgr = MmioRead32 (CcuReg (A733_CCU_USB1_BGR));
  Bgr |= A733_CCU_USB_BGR_EHCI_GATE;
  MmioWrite32 (CcuReg (A733_CCU_USB1_BGR), Bgr);
  Bgr |= A733_CCU_USB_BGR_EHCI_RST;
  MmioWrite32 (CcuReg (A733_CCU_USB1_BGR), Bgr);

  //
  // The 24 MHz reference both PHY PLLs run from.  It is open out of reset
  // (0x8000_0000), so this is a no-op unless something before us closed it.
  //
  MmioOr32 (CcuReg (A733_CCU_USB01_REF_CLK), A733_CCU_USB_REF_CLK_GATE);

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: CCU USB1_CLK=0x%08x USB1_BGR=0x%08x REF_CLK=0x%08x\n",
    MmioRead32 (CcuReg (A733_CCU_USB1_CLK)),
    MmioRead32 (CcuReg (A733_CCU_USB1_BGR)),
    MmioRead32 (CcuReg (A733_CCU_USB01_REF_CLK))
    ));

  return EFI_SUCCESS;
}

/**
  Configure one port's PHY and release it from reset.

  The manual does not document these registers (see <A733Usb.h> for what each
  value is and where it comes from), so the sequence is the generic Allwinner
  one - AHB configuration, SIDDQ cleared, PHY reset released - plus what this
  SoC's own HCI code and a working Linux boot on this board add to it.

  The PHY reset is asserted on entry (the CCU enable leaves it so), which is
  what lets the words below be written into a PHY that is not yet running:
  SIDDQ is cleared and the tune words written while the block is held, and the
  release is the last write.

  This is USB1's PHY, the host port this driver owns: its window is
  0x0420800, the board device tree's usbphy "pmu1".  USB0's PHY ("pmu0",
  0x04101800) runs the same IP, but it is not configured here - USB0 belongs to
  A733MusbDeviceDxe, whose own bring-up deliberately does not write this recipe
  (see the head of this file).  Which values are facts from the manual, which
  are the generic Allwinner sequence and which are board-verified register state
  is recorded once, in <A733Usb.h>.

  @param[in] Tag        What the log line calls this PHY.  The USB1 port passes
                        "PHY", which is what its line has always read.
  @param[in] Hci        The port's HCI-side (PHY) register window.
  @param[in] ClkOffset  The port's CLK_REG, which holds its PHY reset bit.
**/
STATIC
VOID
A733UsbPhyUp (
  IN CONST CHAR8  *Tag,
  IN UINTN        Hci,
  IN UINT32       ClkOffset
  )
{
  UINT32  Value;

  MmioAnd32 (CcuReg (ClkOffset), ~(UINT32)A733_CCU_USB_CLK_PHY_RSTN);

  //
  // AHB master / PHY interface configuration.  This one is not optional: with
  // it cleared the controller's descriptors are never fetched.
  //
  Value  = MmioRead32 (UsbReg (Hci, A733_USB_HCI_AHB_CTRL));
  Value |= A733_USB_HCI_AHB_INCR8 | A733_USB_HCI_AHB_INCR4
           | A733_USB_HCI_AHB_INCRX | A733_USB_HCI_ULPI_BYPASS
           | A733_USB_HCI_AHB_SUN60IW2;
  MmioWrite32 (UsbReg (Hci, A733_USB_HCI_AHB_CTRL), Value);

  //
  // A host port must not be held in forced suspend, or the PHY's line receiver
  // stays parked and a connected device is invisible.
  //
  MmioAnd32 (UsbReg (Hci, A733_USB_HCI_CTRL3), ~(UINT32)A733_USB_HCI_CTRL3_FORCE_SUSPEND);

  //
  // SIDDQ high is power-down, so this is the write that turns the PHY on.
  //
  MmioAnd32 (UsbReg (Hci, A733_USB_HCI_PHY_CTRL), ~(UINT32)A733_USB_HCI_PHY_SIDDQ);

  MmioWrite32 (UsbReg (Hci, A733_USB_HCI_PHY_TUNE), A733_USB_HCI_PHY_TUNE_VALUE);
  MmioWrite32 (UsbReg (Hci, A733_USB_HCI_UTMI_CTRL), A733_USB_HCI_UTMI_CTRL_VALUE);

  MicroSecondDelay (100);

  //
  // Out of reset, and give the PLL time to lock before the controller is
  // allowed to drive a reset on the port.
  //
  MmioOr32 (CcuReg (ClkOffset), A733_CCU_USB_CLK_PHY_RSTN);
  MicroSecondDelay (A733_USB_PHY_SETTLE_US);

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: %a HCI ahb=0x%08x ctrl3=0x%08x phyctl=0x%08x tune=0x%08x utmi=0x%08x stat=0x%08x\n",
    Tag,
    MmioRead32 (UsbReg (Hci, A733_USB_HCI_AHB_CTRL)),
    MmioRead32 (UsbReg (Hci, A733_USB_HCI_CTRL3)),
    MmioRead32 (UsbReg (Hci, A733_USB_HCI_PHY_CTRL)),
    MmioRead32 (UsbReg (Hci, A733_USB_HCI_PHY_TUNE)),
    MmioRead32 (UsbReg (Hci, A733_USB_HCI_UTMI_CTRL)),
    MmioRead32 (UsbReg (Hci, A733_USB_HCI_UTMI_STAT))
    ));
}

/**
  Switch the board's USB 5 V enables on.

  Which pins those are is board data, from the two PCDs the platform sets.
  Both are the EN of a load switch (SGM2576) whose input is VCC5V0_SYS, i.e.
  the 5 V of a receptacle, not a supply of the on-board hub:

  - PcdA733UsbPortVbusPin is PL2, net USB0-DRVVBUS (schematic page 6: the ball
    the device tree calls usb0's drvvbus), EN of U2, whose output
    VCC5V0_USB30_OTG is the VBUS of the USB3 Type-A receptacle (CON2, the lower
    one; its USB2 pair goes to the USB1 PHY, which is why the device tree also
    hands this pin to ehci1);
  - PcdA733UsbHubVbusPin is PM5, net USB_HOST_EN (schematic page 14), EN of U5,
    whose output VCC5V0_USB20 is the 5 V of the USB2 Type-A receptacle (CON1)
    and of the two-port USB2 header - the ports behind the on-board hub.  The
    hub itself is not on either rail: its VD33/VDD5 are the PMIC's DCDC1 3.3 V
    (net VCC_3V3_USB20HUB, through R58 and FB3) with no switch in the path, and
    its XRSTJ is a 100K pull-up to that same rail.  A733Xhci2Dxe reads that rail
    back from the PMIC, because it is the controller the hub's upstream is on.

  Both are active-high enables of a load switch, and different vendor device
  trees name them the other way round, so both are driven: a rail that is
  already on stays on, and neither pin can switch anything off.
**/
STATIC
VOID
A733UsbVbusOn (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = A733RpioSetOutput (FixedPcdGet32 (PcdA733UsbPortVbusPin), TRUE);
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: port VBUS enable P%c%u high: %r\n",
    'L' + A733_RPIO_BANK_OF (FixedPcdGet32 (PcdA733UsbPortVbusPin)),
    A733_RPIO_NUM_OF (FixedPcdGet32 (PcdA733UsbPortVbusPin)),
    Status
    ));

  Status = A733RpioSetOutput (FixedPcdGet32 (PcdA733UsbHubVbusPin), TRUE);
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: Type-A port 5 V switch P%c%u high: %r\n",
    'L' + A733_RPIO_BANK_OF (FixedPcdGet32 (PcdA733UsbHubVbusPin)),
    A733_RPIO_NUM_OF (FixedPcdGet32 (PcdA733UsbHubVbusPin)),
    Status
    ));
}

/**
  Address a register inside one R_PIO bank.

  The R_PIO domain's layout is packed (see <A733Rpio.h>): one bank every
  A733_RPIO_BANK_STRIDE, the level word at +0x10 inside the bank.

  @param[in] Pin     Pin inside the R_PIO domain, A733_RPIO_PIN (Bank, Bit).
  @param[in] Offset  Register offset inside the bank.

  @return The MMIO address of the register.
**/
STATIC
UINTN
RpioPinReg (
  IN UINT32  Pin,
  IN UINT32  Offset
  )
{
  return (UINTN)PcdGet64 (PcdA733RpioBase)
         + (UINTN)A733_RPIO_BANK_OF (Pin) * A733_RPIO_BANK_STRIDE
         + Offset;
}

/**
  Name of an R_PIO port function, for the readback.

  @param[in] Function  4-bit selector.

  @return The selector's meaning; "alternate function" for the peripheral
          range.
**/
STATIC
CONST CHAR8 *
RpioFunctionName (
  IN UINT32  Function
  )
{
  if (Function == A733_RPIO_FUNC_OUTPUT) {
    return "output";
  }

  if (Function == 0) {
    return "input";
  }

  if (Function == A733_RPIO_FUNC_MASK) {
    return "disabled";
  }

  return "alternate function";
}

/**
  Print one VBUS enable pin as the pin controller holds it.

  The enable is driven through A733RpioSetOutput, and what that function
  returned says only that the writes were accepted.  This reads the two words
  back instead - the level word's bit and the pin's CFG nibble - so the log
  shows the pad is an output (selector 1) and that its level bit is high, which
  is what an active-high enable needs.  A level bit of 1 with an input or
  alternate selector is a pad that is not driving the rail, and that is exactly
  the state this line is there to catch.

  @param[in] Role  What the pin enables, as it should appear in the log.
  @param[in] Pin   Pin inside the R_PIO domain, A733_RPIO_PIN (Bank, Bit).
**/
STATIC
VOID
A733UsbVbusPinState (
  IN CONST CHAR8  *Role,
  IN UINT32       Pin
  )
{
  UINT32  Dat;
  UINT32  Cfg;
  UINT32  Bit;
  UINT32  Shift;
  UINT32  Function;

  Bit   = A733_RPIO_NUM_OF (Pin);
  Shift = (Bit & 0x7U) * 4U;

  Dat      = MmioRead32 (RpioPinReg (Pin, A733_RPIO_DAT_OFFSET));
  Cfg      = MmioRead32 (RpioPinReg (Pin, A733_RPIO_CFG_OFFSET + (Bit >> 3) * 4U));
  Function = (Cfg >> Shift) & A733_RPIO_FUNC_MASK;

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: %a P%c%u readback: dat=0x%08x bit=%u cfg[0x%02x]=0x%08x nibble=%u (%a)\n",
    Role,
    'L' + A733_RPIO_BANK_OF (Pin),
    Bit,
    Dat,
    (Dat >> Bit) & 1U,
    A733_RPIO_CFG_OFFSET + (Bit >> 3) * 4U,
    Cfg,
    Function,
    RpioFunctionName (Function)
    ));
}

/**
  Read back both VBUS enables from the pin registers.

  Called after the enables are driven and after the settle delay, so the log
  proves the pads are high and outputs at the moment the port is looked at -
  not that a write was issued at some earlier point.
**/
STATIC
VOID
A733UsbDumpVbusPins (
  VOID
  )
{
  A733UsbVbusPinState ("port VBUS enable", FixedPcdGet32 (PcdA733UsbPortVbusPin));
  A733UsbVbusPinState ("Type-A port 5 V switch", FixedPcdGet32 (PcdA733UsbHubVbusPin));
}

/**
  Report the controller's identification registers and its port state.

  CAPLENGTH is the EHCI specification's own window into the block: it is the
  first byte of the controller's register file, and the operational registers
  start that far in.  A CAPLENGTH that is neither 0 nor 0xff is the cheapest
  proof that the block answered at all - a gated or unpowered controller reads
  all-zero (or all-ones) here, and EhciDxe would then fail before it ever looks
  at a port.

  PORTSC's connection bit is the second half of the answer: it says whether the
  PHY sees a device, independently of anything above this driver.  Its owner bit
  and line state say the same thing one level down: owner set means what is
  plugged in belongs to the OHCI companion, which is where a full- or low-speed
  device on the bare root port ends up, and line state J or K with no connection
  is what a PHY sees when it is running but the port is not being reset.
**/
STATIC
VOID
A733UsbDumpController (
  IN CONST CHAR8  *Tag
  )
{
  UINT32  CapLength;
  UINT32  HcStructParams;
  UINT32  HcCapParams;
  UINT32  Op;
  UINT32  UsbCmd;
  UINT32  UsbSts;
  UINT32  PortSc;

  CapLength      = MmioRead32 (UsbReg (A733_USB1_EHCI_BASE, A733_EHCI_CAPLENGTH_REG));
  HcStructParams = MmioRead32 (UsbReg (A733_USB1_EHCI_BASE, A733_EHCI_HCSPARAMS_REG));
  HcCapParams    = MmioRead32 (UsbReg (A733_USB1_EHCI_BASE, A733_EHCI_HCCPARAMS_REG));
  Op             = A733_USB1_EHCI_BASE + (CapLength & 0xFFU);
  UsbCmd         = MmioRead32 (UsbReg (Op, A733_EHCI_USBCMD_REG));
  UsbSts         = MmioRead32 (UsbReg (Op, A733_EHCI_USBSTS_REG));
  PortSc         = MmioRead32 (UsbReg (Op, A733_EHCI_PORTSC_REG));

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: %a EHCI1 caplen=0x%02x hcsparams=0x%08x hccparams=0x%08x\n",
    Tag,
    CapLength & 0xFFU,
    HcStructParams,
    HcCapParams
    ));
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: %a EHCI1 usbcmd=0x%08x usbsts=0x%08x portsc=0x%08x (ccs=%u owner=%u line=%u)\n",
    Tag,
    UsbCmd,
    UsbSts,
    PortSc,
    (PortSc & A733_EHCI_PORTSC_CCS) != 0,
    (PortSc & A733_EHCI_PORTSC_OWNER) != 0,
    (PortSc >> A733_EHCI_PORTSC_LINE_SHIFT) & 0x3U
    ));
}

/**
  What a survey of EHCI0 found, in the terms a caller can act on.  The survey
  itself prints everything it reads; this is only the summary: whether the block
  answered at all, how many root ports it reports, and how many of those read
  ccs=1 (a device attached).
**/
typedef struct {
  BOOLEAN    Answered;
  UINT32     Ports;
  UINT32     Connected;
} A733_USB0_SURVEY;

/**
  Survey EHCI0, the controller of the OTG port next door, without touching it.

  EHCI1's root port being ours and empty (see A733UsbClaimPorts) says the hub is
  not behind that port; it does not say where it is.  The board has exactly two
  USB2.0 host controllers, and this is the other one: EHCI0 in the USB0 window
  (0x04101000 - the port next to the OTG connector, device tree ehci0).  Reading
  it costs nothing and records what that window holds, including whether
  something is already attached to the OTG port's host function.

  Read-only, deliberately, and that is the whole of the contract - the MUSB core
  of this window belongs to the device driver, so this survey writes nothing
  there (the one write this driver makes in the window is the EHCI half's CCU
  gate, in A733UsbRegisterEhci0, and it is outside this function):

  - USBCMD is not written (no controller start or reset), CONFIGFLAG is only
    read, no PORTSC write happens (so no port is claimed or reset), the OTG
    PHY's route bit - phy_ctrl, 0x04100420, part of the FES/flashing path - is
    not touched at all, and neither the CCU gates nor the PHY's HCI window are
    written: a survey that opened them would be configuring a port another
    driver owns;

  - the CCU words are read first, because a block whose gate or reset is closed
    may not answer at all: if the log ends on that line, the read of EHCI0 is
    what stopped, and the gate state says why.

  CAPLENGTH is the same test the EHCI1 dump uses: 0 or 0xff means the block did
  not answer, and its operational registers would be meaningless.

  The CCU readback is printed first on every call, so the survey can be re-run
  later in the boot and the answer read next to the gates that produced it.

  @return Whether the block answered, its root port count and how many of those
          ports report a device.
**/
STATIC
A733_USB0_SURVEY
A733UsbSurveyEhci0 (
  VOID
  )
{
  A733_USB0_SURVEY  Found;
  UINT32            Usb0Clk;
  UINT32            Usb0Bgr;
  UINT32            CapLength;
  UINT32            HcStructParams;
  UINT32            HcCapParams;
  UINT32            Ports;
  UINT32            Op;
  UINT32            Index;
  UINT32            PortSc;
  UINT32            Connected;

  Found.Answered  = FALSE;
  Found.Ports     = 0;
  Found.Connected = 0;

  Usb0Clk = MmioRead32 (CcuReg (A733_CCU_USB0_CLK));
  Usb0Bgr = MmioRead32 (CcuReg (A733_CCU_USB0_BGR));
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: survey EHCI0 CCU USB0_CLK=0x%08x (clken=%u phy_rstn=%u) USB0_BGR=0x%08x (ehci_gate=%u ehci_rst=%u)\n",
    Usb0Clk,
    (Usb0Clk & A733_CCU_USB_CLK_MODULE_EN) != 0,
    (Usb0Clk & A733_CCU_USB_CLK_PHY_RSTN) != 0,
    Usb0Bgr,
    (Usb0Bgr & A733_CCU_USB_BGR_EHCI_GATE) != 0,
    (Usb0Bgr & A733_CCU_USB_BGR_EHCI_RST) != 0
    ));

  CapLength      = MmioRead32 (UsbReg (A733_USB0_EHCI_BASE, A733_EHCI_CAPLENGTH_REG)) & 0xFFU;
  HcStructParams = MmioRead32 (UsbReg (A733_USB0_EHCI_BASE, A733_EHCI_HCSPARAMS_REG));
  HcCapParams    = MmioRead32 (UsbReg (A733_USB0_EHCI_BASE, A733_EHCI_HCCPARAMS_REG));
  Ports          = HcStructParams & A733_EHCI_HCSPARAMS_NPORTS_MASK;

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: survey EHCI0 (0x%08x) caplen=0x%02x hcsparams=0x%08x nports=%u npcc=%u hccparams=0x%08x\n",
    (UINT32)A733_USB0_EHCI_BASE,
    CapLength,
    HcStructParams,
    Ports,
    (HcStructParams >> A733_EHCI_HCSPARAMS_NPCC_SHIFT) & A733_EHCI_HCSPARAMS_NPCC_MASK,
    HcCapParams
    ));

  if ((CapLength == 0) || (CapLength == 0xFF)) {
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: survey EHCI0 did not answer (caplen=0x%02x); its operational registers are unreadable\n",
      CapLength
      ));
    return Found;
  }

  Found.Answered = TRUE;
  Found.Ports    = Ports;

  Op = A733_USB0_EHCI_BASE + CapLength;
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: survey EHCI0 usbcmd=0x%08x usbsts=0x%08x configflag=0x%08x (cf=%u)\n",
    MmioRead32 (UsbReg (Op, A733_EHCI_USBCMD_REG)),
    MmioRead32 (UsbReg (Op, A733_EHCI_USBSTS_REG)),
    MmioRead32 (UsbReg (Op, A733_EHCI_CONFIGFLAG_REG)),
    (MmioRead32 (UsbReg (Op, A733_EHCI_CONFIGFLAG_REG)) & A733_EHCI_CONFIGFLAG_ROUTE) != 0
    ));

  Connected = 0;
  for (Index = 0; Index < Ports; Index++) {
    PortSc = MmioRead32 (UsbReg (Op, A733_EHCI_PORTSC_REG + 4 * Index));
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: survey EHCI0 port %u: portsc=0x%08x ccs=%u owner=%u line=%u pe=%u pr=%u\n",
      Index + 1,
      PortSc,
      (PortSc & A733_EHCI_PORTSC_CCS) != 0,
      (PortSc & A733_EHCI_PORTSC_OWNER) != 0,
      (PortSc >> A733_EHCI_PORTSC_LINE_SHIFT) & 0x3U,
      (PortSc & A733_EHCI_PORTSC_PE) != 0,
      (PortSc & A733_EHCI_PORTSC_PR) != 0
      ));
    if ((PortSc & A733_EHCI_PORTSC_CCS) != 0) {
      Connected++;
    }
  }

  if (Ports == 0) {
    DEBUG ((DEBUG_WARN, "A733Usb: survey EHCI0 reports no root ports\n"));
  } else if (Connected == 0) {
    //
    // Worded so it cannot be read as a verdict on the wiring: this controller's
    // PHY is not configured by this driver (it belongs to the MUSB device
    // side), so ccs=0 here is what the port reads until that driver brings the
    // PHY up - it says nothing about whether a cable is attached.
    //
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: survey EHCI0: ccs=0 on all %u port(s) with the PHY left as found and CF=%u - "
      "an observation, not a verdict on what is attached\n",
      Ports,
      (MmioRead32 (UsbReg (Op, A733_EHCI_CONFIGFLAG_REG)) & A733_EHCI_CONFIGFLAG_ROUTE) != 0
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: survey EHCI0: %u of %u port(s) reports ccs=1 as found\n",
      Connected,
      Ports
      ));
  }

  Found.Connected = Connected;

  return Found;
}

/**
  Read the MUSB core of the OTG port, without touching it.

  The third place a device could be: behind the OTG connector, on the core the
  device tree calls usb_otg (0x04100000).  That core belongs to
  A733MusbDeviceDxe, which runs as a fastboot device; this survey never starts a
  session and never writes a MUSB register, so the line reports the core as found
  (or as that driver left it, depending on dispatch order):

  - FADDR: non-zero means the core has been enumerated as a device by somebody,
    which by itself says the OTG port is not the empty one a hub would sit on;
  - POWER: HSMODE says whether the core has been put in host mode at all - a
    hub behind the port is only reachable once something drives it there;
  - DEVCTL: HOSTMODE (bit 2) says which way the core is pointed, SESSION (bit
    0) whether it thinks a session is in progress, and the VBUS field what its
    comparator makes of the connector's 5 V (3 is above VBus-valid).  BDEVICE
    (bit 7) is a status bit - 1 means the core sees a B-side connector, not
    that it is in host mode.  That last one is independent of this firmware:
    it is the OTG port's own VBUS, so it reads the rail rather than anything
    here of ours.

  Read-only: MUSB's registers are read at their byte offsets and nothing is
  written, so a core the OS or a previous stage owns is left exactly as found.
**/
STATIC
VOID
A733UsbSurveyMusb (
  VOID
  )
{
  UINT8  Faddr;
  UINT8  Power;
  UINT8  DevCtl;

  Faddr  = MmioRead8 (UsbReg (A733_USB0_BASE, A733_MUSB_FADDR_REG));
  Power  = MmioRead8 (UsbReg (A733_USB0_BASE, A733_MUSB_POWER_REG));
  DevCtl = MmioRead8 (UsbReg (A733_USB0_BASE, A733_MUSB_DEVCTL_REG));

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: survey MUSB (0x%08x) faddr=0x%02x power=0x%02x (hsmode=%u hsen=%u) "
    "devctl=0x%02x (session=%u hostreq=%u hostmode=%u vbus=%u lsdev=%u bdevice=%u fsdev=%u)\n",
    (UINT32)A733_USB0_BASE,
    Faddr,
    Power,
    (Power & A733_MUSB_POWER_HSMODE) != 0,
    (Power & A733_MUSB_POWER_HSENAB) != 0,
    DevCtl,
    (DevCtl & A733_MUSB_DEVCTL_SESSION) != 0,
    (DevCtl & A733_MUSB_DEVCTL_HOSTREQ) != 0,
    (DevCtl & A733_MUSB_DEVCTL_HOSTMODE) != 0,
    (DevCtl >> A733_MUSB_DEVCTL_VBUS_SHIFT) & A733_MUSB_DEVCTL_VBUS_MASK,
    (DevCtl & A733_MUSB_DEVCTL_LSDEV) != 0,
    (DevCtl & A733_MUSB_DEVCTL_BDEVICE) != 0,
    (DevCtl & A733_MUSB_DEVCTL_FSDEV) != 0
    ));
}

/**
  Print one root port's state, one line, in the fields that say who owns the
  port and what the PHY sees on it.

  The controller's name is part of the line because more than one controller is
  read in one boot: two lines reading "port 1 before claim" would not say which
  controller they came from.

  @param[in] Name    The controller the port belongs to, "EHCI1".
  @param[in] Tag     What this reading is, as it should appear in the log.
  @param[in] Port    Root port number, 1-based.
  @param[in] PortSc  The port's PORTSC value.
**/
STATIC
VOID
A733UsbPortDump (
  IN CONST CHAR8  *Name,
  IN CONST CHAR8  *Tag,
  IN UINT32       Port,
  IN UINT32       PortSc
  )
{
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: %a port %u %a: portsc=0x%08x ccs=%u owner=%u line=%u pe=%u pr=%u\n",
    Name,
    Port,
    Tag,
    PortSc,
    (PortSc & A733_EHCI_PORTSC_CCS) != 0,
    (PortSc & A733_EHCI_PORTSC_OWNER) != 0,
    (PortSc >> A733_EHCI_PORTSC_LINE_SHIFT) & 0x3U,
    (PortSc & A733_EHCI_PORTSC_PE) != 0,
    (PortSc & A733_EHCI_PORTSC_PR) != 0
    ));
}

/**
  What a port claim found, in the terms a caller can act on.  The claim itself
  prints every port it reads before and after; this is the summary, and the
  conclusion the caller draws from it is its own - the two controllers are
  looked at under different conditions and must not share one wording.
**/
typedef struct {
  UINT32    Ports;
  UINT32    Connected;
  UINT32    StillCompanion;
} A733_USB_CLAIM;

/**
  Claim a controller's root ports for the EHCI controller.

  A port belongs to the companion controller while its PORTSC.PortOwner is set,
  and the EHCI then neither resets it nor reports a device behind it - so a hub
  on such a port is invisible to everything above this driver.  Software takes a
  port back by writing zero to that bit, and the controller hands it over on the
  next connect detect.

  CONFIGFLAG comes first because it decides whether that write means anything:
  while the Configured Flag is clear every port is routed to the companion, and
  PortOwner reads 1 as a consequence of the routing rather than because anything
  is plugged in.  Claiming a port in that state would be a write the hardware is
  entitled to ignore, and would leave a log that reads "the port never becomes
  ours" whatever the wiring is.  Setting CF is not running the controller -
  USBCMD is left exactly as found, halted - and it is what MdeModulePkg's
  EhcInitHC writes for itself after its own controller reset, so nothing written
  here survives in a configuration EhciDxe does not also produce.

  Every port is read before and after, so the log distinguishes the four
  outcomes that matter:

  - owner was already 0 with a device: the port is ours and something is on it;
  - owner was 1 and the claim leaves owner 0 with ccs 1: the port was the
    companion's and there is a device behind it - it is on this controller;
  - owner was 1 and the claim leaves owner 0 with ccs 0: the port is ours now
    and nothing is connected to it - the wiring or the hub's power is the
    problem, not the controller;
  - owner stays 1 after the claim: the hardware did not hand the port over.

  Both host controllers are claimed the same way; Name is only what the log
  calls the controller, so each line still sits under the controller it is
  about.

  @param[in]  Ehci    The controller's EHCI register block (A733_USBn_EHCI_BASE).
  @param[in]  Name    The controller's name in the log, "EHCI1" or "EHCI0".
  @param[out] Result  Root port count, how many ports report a device afterwards,
                      and how many never left the companion.
**/
STATIC
VOID
A733UsbClaimPorts (
  IN  UINTN             Ehci,
  IN  CONST CHAR8       *Name,
  OUT A733_USB_CLAIM    *Result
  )
{
  UINT32  CapLength;
  UINT32  HcStructParams;
  UINT32  Ports;
  UINT32  Op;
  UINT32  CfgFlag;
  UINT32  Index;
  UINT32  Offset;
  UINT32  PortSc;
  UINT32  Connected;
  UINT32  StillCompanion;

  CapLength      = MmioRead32 (UsbReg (Ehci, A733_EHCI_CAPLENGTH_REG)) & 0xFFU;
  HcStructParams = MmioRead32 (UsbReg (Ehci, A733_EHCI_HCSPARAMS_REG));
  Ports          = HcStructParams & A733_EHCI_HCSPARAMS_NPORTS_MASK;
  Op             = Ehci + CapLength;

  Result->Ports          = Ports;
  Result->Connected      = 0;
  Result->StillCompanion = 0;

  CfgFlag = MmioRead32 (UsbReg (Op, A733_EHCI_CONFIGFLAG_REG));
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: %a configflag=0x%08x (cf=%u) ports=%u companions=%u\n",
    Name,
    CfgFlag,
    (CfgFlag & A733_EHCI_CONFIGFLAG_ROUTE) != 0,
    Ports,
    (HcStructParams >> A733_EHCI_HCSPARAMS_NPCC_SHIFT) & A733_EHCI_HCSPARAMS_NPCC_MASK
    ));

  if (Ports == 0) {
    DEBUG ((DEBUG_ERROR, "A733Usb: %a reports no root ports; nothing to claim\n", Name));
    return;
  }

  if ((CfgFlag & A733_EHCI_CONFIGFLAG_ROUTE) == 0) {
    DEBUG ((DEBUG_INFO, "A733Usb: %a ports are routed to the companion (cf=0); routing to %a\n", Name, Name));
    MmioOr32 (UsbReg (Op, A733_EHCI_CONFIGFLAG_REG), A733_EHCI_CONFIGFLAG_ROUTE);
    MicroSecondDelay (A733_USB_PORT_CLAIM_SETTLE_US);
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: %a configflag now 0x%08x\n",
      Name,
      MmioRead32 (UsbReg (Op, A733_EHCI_CONFIGFLAG_REG))
      ));
  }

  Connected      = 0;
  StillCompanion = 0;

  for (Index = 0; Index < Ports; Index++) {
    Offset = A733_EHCI_PORTSC_REG + 4 * Index;

    PortSc = MmioRead32 (UsbReg (Op, Offset));
    A733UsbPortDump (Name, "before claim", Index + 1, PortSc);

    if ((PortSc & A733_EHCI_PORTSC_OWNER) == 0) {
      //
      // Already the EHCI's.  Nothing to do, and a device on it is this
      // controller's to enumerate.
      //
      if ((PortSc & A733_EHCI_PORTSC_CCS) != 0) {
        DEBUG ((
          DEBUG_INFO,
          "A733Usb: %a port %u already %a's with a device attached; left alone\n",
          Name,
          Index + 1,
          Name
          ));
        Connected++;
      } else {
        DEBUG ((DEBUG_INFO, "A733Usb: %a port %u already %a's, nothing attached\n", Name, Index + 1, Name));
      }

      continue;
    }

    //
    // Take the port back from the companion.  The change bits are left at zero
    // so the write does not acknowledge a status change EhciDxe has not seen;
    // port power and the enable and reset bits keep their read values, so
    // nothing but ownership changes.
    //
    PortSc &= ~(UINT32)A733_EHCI_PORTSC_CHANGE_MASK;
    PortSc &= ~(UINT32)A733_EHCI_PORTSC_OWNER;
    MmioWrite32 (UsbReg (Op, Offset), PortSc);
    MicroSecondDelay (A733_USB_PORT_CLAIM_SETTLE_US);

    PortSc = MmioRead32 (UsbReg (Op, Offset));
    A733UsbPortDump (Name, "after claim", Index + 1, PortSc);

    if ((PortSc & A733_EHCI_PORTSC_OWNER) != 0) {
      DEBUG ((DEBUG_WARN, "A733Usb: %a port %u is still the companion's after the claim\n", Name, Index + 1));
      StillCompanion++;
    } else if ((PortSc & A733_EHCI_PORTSC_CCS) != 0) {
      DEBUG ((DEBUG_INFO, "A733Usb: %a port %u claimed, device attached\n", Name, Index + 1));
      Connected++;
    } else {
      DEBUG ((DEBUG_INFO, "A733Usb: %a port %u claimed, nothing attached\n", Name, Index + 1));
    }
  }

  Result->Connected      = Connected;
  Result->StillCompanion = StillCompanion;
}

/**
  Print what a claim concluded about one controller.

  One line, because it is the whole conclusion, and worded by which of the two
  failures it is: a port that never left the companion means this controller is
  not the one the hub is on, while an empty port that is ours means the wiring
  or the hub's 5 V.  The other controllers are left alone - which one the hub is
  on is not something to guess at.

  This is EHCI1's conclusion, and the only claim this driver makes: EHCI0 is not
  brought up or claimed, because the OTG connector's host half is not this
  driver's port.

  @param[in] Name   The controller's name in the log.
  @param[in] Claim  What A733UsbClaimPorts found.
**/
STATIC
VOID
A733UsbReportClaim (
  IN CONST CHAR8     *Name,
  IN A733_USB_CLAIM  *Claim
  )
{
  if (Claim->Connected != 0) {
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: %u %a port(s) with a device attached after the claim\n",
      Claim->Connected,
      Name
      ));
    return;
  }

  if (Claim->StillCompanion != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Usb: no %a port has a device attached after the claim, and %u port(s) still report the "
      "companion as owner (owner=1): %a does not have this port\n",
      Name,
      Claim->StillCompanion,
      Name
      ));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: no %a port has a device attached after the claim (owner=0, ccs=0 on all %u); "
    "the ports are ours and empty - the hub is not on this controller, or its 5 V is not reaching it\n",
    Name,
    Claim->Ports
    ));
}

/**
  Publish EHCI0, the host half of the OTG port, the way the reference firmware
  does.

  The reference firmware's host driver registers EHCI0 next to EHCI1 (and the
  two OHCI companions and xHCI2), and on this port that registration is what
  runs a full controller initialisation - reset, CONFIGFLAG, port scan - before
  the MUSB device takes the pads back.  Our build has only ever registered
  EHCI1, so USB0's controller has never had that initialisation; this function
  adds it and nothing more.

  Two rules make that safe for the MUSB core beside it:

  - the CCU bits written here are the EHCI half's own gate and reset (USB0_BGR
    bits 4 and 20), and they are written only when the block does not answer.
    A733MusbDeviceDxe's platform phase opens those same bits at dispatch, so in
    the normal order this is a read and no write at all.  The device half's gate
    and reset (bits 8 and 24) keep their values - every write is a
    read-modify-write of the one word;

  - the controller itself is not touched: no reset, no CONFIGFLAG, no port
    claim, no PHY register.  EhciDxe does the reset and CONFIGFLAG itself when
    it binds, and doing either here as well would only duplicate it.

  CAPLENGTH is the test of whether the block answered - the same one the survey
  uses.  A block that reports 0 or 0xff after its gate is open is not registered:
  EhciDxe would fail on it before reaching a port, and a dead device node in the
  log is worse than a line saying why there is none.

  @retval EFI_SUCCESS       EHCI0 was registered.
  @retval EFI_DEVICE_ERROR  The block does not identify itself; nothing was
                            registered.
  @retval Others            RegisterNonDiscoverableMmioDevice failed.
**/
STATIC
EFI_STATUS
A733UsbRegisterEhci0 (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      CcuClk;
  UINT32      CcuBgr;
  UINT32      CapLength;

  CcuClk = MmioRead32 (CcuReg (A733_CCU_USB0_CLK));
  CcuBgr = MmioRead32 (CcuReg (A733_CCU_USB0_BGR));
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: EHCI0 (0x%08x) before register: CCU USB0_CLK=0x%08x (clken=%u phy_rstn=%u) "
    "USB0_BGR=0x%08x (ehci_gate=%u ehci_rst=%u)\n",
    (UINT32)A733_USB0_EHCI_BASE,
    CcuClk,
    (CcuClk & A733_CCU_USB_CLK_MODULE_EN) != 0,
    (CcuClk & A733_CCU_USB_CLK_PHY_RSTN) != 0,
    CcuBgr,
    (CcuBgr & A733_CCU_USB_BGR_EHCI_GATE) != 0,
    (CcuBgr & A733_CCU_USB_BGR_EHCI_RST) != 0
    ));

  CapLength = MmioRead32 (UsbReg (A733_USB0_EHCI_BASE, A733_EHCI_CAPLENGTH_REG)) & 0xFFU;
  if ((CapLength == 0) || (CapLength == 0xFF)) {
    //
    // Nothing in the window is clocked yet, which is what happens when this
    // driver is dispatched before A733MusbDeviceDxe's platform phase.  Open the
    // EHCI half only - gate first, then reset, because a block that leaves
    // reset without a clock drives nothing - and leave the device half, the
    // PHY window and the route bit alone.
    //
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: EHCI0 caplen=0x%02x before its gate was opened; opening the EHCI half only "
      "(USB0_BGR bits 4 and 20)\n",
      CapLength
      ));

    CcuBgr |= A733_CCU_USB_BGR_EHCI_GATE;
    MmioWrite32 (CcuReg (A733_CCU_USB0_BGR), CcuBgr);
    CcuBgr |= A733_CCU_USB_BGR_EHCI_RST;
    MmioWrite32 (CcuReg (A733_CCU_USB0_BGR), CcuBgr);
    MicroSecondDelay (A733_USB_PHY_SETTLE_US);

    CapLength = MmioRead32 (UsbReg (A733_USB0_EHCI_BASE, A733_EHCI_CAPLENGTH_REG)) & 0xFFU;
  }

  if ((CapLength == 0) || (CapLength == 0xFF)) {
    DEBUG ((
      DEBUG_WARN,
      "A733Usb: EHCI0 does not answer (caplen=0x%02x) with USB0_BGR=0x%08x, USB0_CLK=0x%08x; "
      "not registering it\n",
      CapLength,
      MmioRead32 (CcuReg (A733_CCU_USB0_BGR)),
      MmioRead32 (CcuReg (A733_CCU_USB0_CLK))
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // Same shape as EHCI1's registration: one MMIO region, the controller's own
  // 0x400 block, non-coherent DMA because the AHB master does not snoop.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             (UINTN)A733_USB0_EHCI_BASE,
             (UINTN)A733_USB_BLOCK_SIZE
             );

  DEBUG ((
    DEBUG_INFO,
    "A733Usb: EHCI0 registered: %r (caplen=0x%02x USB0_CLK=0x%08x USB0_BGR=0x%08x)\n",
    Status,
    CapLength,
    MmioRead32 (CcuReg (A733_CCU_USB0_CLK)),
    MmioRead32 (CcuReg (A733_CCU_USB0_BGR))
    ));

  //
  // OHCI0, the OTG window's full/low-speed companion at +0x1400.  Register it
  // only when EHCI0 answered: a silent block means the port is not wired for
  // host mode on this board, and publishing a controller that cannot be reset
  // would only make OhciDxe print errors.
  //
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "A733Usb: OHCI0 registered: %r\n",
      RegisterNonDiscoverableMmioDevice (
        NonDiscoverableDeviceTypeOhci,
        NonDiscoverableDeviceDmaTypeNonCoherent,
        NULL,
        NULL,
        1,
        (UINTN)A733_USB0_OHCI_BASE,
        (UINTN)A733_USB_BLOCK_SIZE
        )
      ));
  }

  return Status;
}

/**
  Entry point: bring the port up, then publish its controller - and the OTG
  port's controller with it.

  @param[in] ImageHandle  The driver's image handle.
  @param[in] SystemTable  The boot services and runtime services tables.

  @retval EFI_SUCCESS  The port was brought up and the controller published.
  @retval Others       A step failed; the controller is not published, and the
                       log line above says which step it was.
**/
EFI_STATUS
EFIAPI
A733UsbHostDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  A733_USB_CLAIM  Usb1Claim;

  DEBUG ((DEBUG_INFO, "A733Usb: USB1 host port bring-up\n"));

  Status = A733UsbPowerDomainOn ();
  if (EFI_ERROR (Status)) {
    //
    // Not fatal to the attempt: the domain may simply not exist on this
    // silicon revision, and the reads below will say whether the block
    // answered anyway.
    //
    DEBUG ((DEBUG_WARN, "A733Usb: continuing without the USB2 power domain\n"));
  }

  Status = A733UsbCcuOn ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  A733UsbDumpController ("before PHY");

  //
  // The OTG window, read-only (EHCI0 at 0x04101000 and the MUSB core at
  // 0x04100000), read before anything here writes to a PHY, a VBUS pin or
  // CONFIGFLAG, so what it reports is the state this firmware found rather than
  // one it produced.  USB0 belongs to A733MusbDeviceDxe: nothing in this driver
  // opens its gates, writes its PHY or claims its port, so these lines are a
  // record of the window and not a step towards bringing it up.
  //
  (VOID)A733UsbSurveyEhci0 ();
  A733UsbSurveyMusb ();

  A733UsbPhyUp ("PHY", A733_USB1_HCI_BASE, A733_CCU_USB1_CLK);
  A733UsbVbusOn ();

  MicroSecondDelay (A733_USB_VBUS_SETTLE_US);

  A733UsbDumpController ("after PHY");

  //
  // Both enables read back from the pin controller, and the OTG core read again
  // now that the rails have settled: its DEVCTL VBUS field is the one reading
  // that can show the 5 V actually arriving on a port, whichever port the hub
  // turns out to be behind.
  //
  A733UsbDumpVbusPins ();
  A733UsbSurveyMusb ();

  A733UsbClaimPorts (A733_USB1_EHCI_BASE, "EHCI1", &Usb1Claim);
  A733UsbReportClaim ("EHCI1", &Usb1Claim);

  //
  // Publish the EHCI controller.  NonCoherent is the right DMA type for this
  // part: the AHB master does not snoop the CPU caches, so buffers have to be
  // handed over with cache maintenance rather than assumed shared.  The device
  // appears to EhciDxe as a PCI device with the EHCI class code, which is what
  // makes the stock driver's Supported() claim it.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeEhci,
             NonDiscoverableDeviceDmaTypeNonCoherent,
             NULL,
             NULL,
             1,
             (UINTN)A733_USB1_EHCI_BASE,
             (UINTN)A733_USB_BLOCK_SIZE
             );
  DEBUG ((DEBUG_INFO, "A733Usb: EHCI1 registered: %r\n", Status));

  //
  // The full/low-speed companion in the next 0x400 block.  Its CCU gate and
  // reset live in the same USB1 word A733ClockInitUsb() already opens
  // (BIT(0) / BIT(16) of usb1_ohci), so only the window is published here;
  // SunxiOhciDxe binds it over the same non-discoverable PCI path EhciDxe
  // uses, and low-speed HID devices hang off it instead of the EHCI root
  // port.
  //
  DEBUG ((
    DEBUG_INFO,
    "A733Usb: OHCI1 registered: %r\n",
    RegisterNonDiscoverableMmioDevice (
      NonDiscoverableDeviceTypeOhci,
      NonDiscoverableDeviceDmaTypeNonCoherent,
      NULL,
      NULL,
      1,
      (UINTN)A733_USB1_OHCI_BASE,
      (UINTN)A733_USB_BLOCK_SIZE
      )
    ));

  //
  // And the OTG port's host half, the way the reference firmware does it: it
  // registers EHCI0 next to EHCI1, and EhciDxe's own controller reset and
  // CONFIGFLAG on that block are the initialisation this port has never had
  // here.  The call writes one CCU word's EHCI bits if - and only if - the block
  // does not answer, and registers nothing when it stays silent; the MUSB
  // device is not disturbed either way.  Its status is logged rather than
  // returned: the host port this driver owns is EHCI1's, and its registration
  // is what the caller's status reports.
  //
  (VOID)A733UsbRegisterEhci0 ();

  //
  // Then the OTG window once more, still read-only, so the log ends with what
  // that port holds on both of its halves: the CCU words, EHCI0's
  // identification and port state (its gate is open by now - opened by the
  // platform phase or, if this driver ran first, by the registration above),
  // and the MUSB core's own state beside it.  Nothing in this window is written
  // from here - no PHY word, no route bit, no CONFIGFLAG, no port claim.
  //
  (VOID)A733UsbSurveyEhci0 ();

  return Status;
}