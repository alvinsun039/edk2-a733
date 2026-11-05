/** @file
  MUSB device controller for the Allwinner A733 (sun60iw2p1), OTG port.

  This driver makes the board answer as a USB peripheral on the OTG connector
  and produces EFI_USB_DEVICE_PROTOCOL (EmbeddedPkg/Include/Protocol/UsbDevice.h)
  so that MdeModulePkg's stock AndroidFastboot transport can drive it.  Nothing
  above this file is SoC-specific: the transport owns the descriptors and the
  command engine, and this driver only moves the packets those two describe.

  What the hardware is.  Manual 18.5: USB0 is a USB2.0 dual-role device - one
  device controller (Mentor MUSB, at 0x04100000) and one EHCI+OHCI host
  controller (at 0x04101000) sharing PHY0 and an 8 KB FIFO SRAM.  The two are
  two functions of one physical port, so only one of them may drive it: USB0
  belongs to this driver.  A733UsbHostDxe reads the USB0 window and writes
  nothing in it, and this driver opens *both* halves in the order the A733 host
  firmware does: the host half first, because the HCI window at 0x04101800 is
  where PHY0 is actually powered up and programmed, then the device half (its
  dedicated CCU gate and reset, the wrapper's OTG control, the route and passby
  bits, and the core's own registers), and nothing after that closes or resets
  either half.  USB1 (0x04200000) is the host driver's port and is not touched
  here.

  When it runs.  Both phases run from the entry point at DXE dispatch, which is
  where the reference's device driver runs its own: the platform phase - steps 0,
  0a and 1, everything that belongs to the board and the PHY - first, then the
  device half - steps 2 to 10 - and the poll timer, after the protocol is
  published.  Start() therefore adds nothing to the hardware: it takes the
  transport's descriptors and callbacks, and the port has been pulling D+ up and
  being serviced since dispatch, exactly as the reference's self-starting
  SunxiMusbDeviceDxe does.  A second Start() redoes neither phase.  One
  consequence is deliberate: a host looking at this port after BDS finds a PHY
  that has been powered and programmed for minutes and a device that has been
  answering for minutes, which is the state the reference leaves it in.

  What that costs before a consumer exists.  Steps 2 to 9 need endpoint numbers;
  the transport's descriptors arrive with Start(), so the FIFO map is programmed
  for the layout the transport will declare (EP1 IN, EP2 OUT - see
  MusbAssumeTransportEndpoints) and Start() reprograms only that map if the
  descriptors name different endpoints.  A SETUP that arrives before the
  descriptors are known is answered as far as it can be: the string and the
  status/configuration requests are served, and GET_DESCRIPTOR(DEVICE) is stalled
  rather than served from a NULL descriptor.  If that happened, Start() arms the
  same one-shot re-attach window from its own end, so a host that gave up on the
  descriptor-less connect is given a fresh one.

  The one thing the driver does beyond the reference's sequence is insurance
  against that same history: a bus reset that no SETUP follows within three
  seconds gets the device half rewritten and the pull-up dropped for 50 ms and
  raised again, once per boot (MusbReattachPoll).  A host only samples a port
  whose connect changed, and this port's connect has been given up on by the host
  many times during development.  It writes nothing the bring-up does not write
  and costs nothing when the enumeration works.

  Order, and why it is that order (every step logs its write and the read-back
  of the register, so a write that did not land is a single line).  The port is
  brought up in the order the A733 host firmware uses, because that is the order
  verified to leave this silicon enumerating on the OTG connector:

  0. Platform, before the MUSB core is touched at all: the PCK600 USB2 power
     domain (R_CCU's CLK_R_PPU gate, then the domain's own COMMAND_ON), the USB
     subsystem's CCU gates (the keyed AHB master gate, CLK_MSI_LITE2,
     CLK_RES_DCAP_24M, the pll-peri0 output gates, USB0's reference clock),
     USB0's module clock and the *host* half of USB0_BGR - OHCI and EHCI gates
     open with both of their resets released - and then the SYSCFG resistance
     calibration for port 0.  See MusbPlatformOn.
  0a. PHY0 is powered and programmed with the pads still on the host half: the
     wrapper route select's bit 0 cleared, PHY_RSTN asserted, the HCI window's
     four words written while the PHY is held in reset, PHY_RSTN released, and
     then the USB2 PHY's own port-0 words at 0x06B00000.  See
     MusbPhy0HostHalfUp.  The host half stays open from here on.
  1. CCU, device half (MusbCcuOn): USB0_CLK's PHY_RSTN left released, then
     USB0_BGR's device gate opened with its device reset pulsed (manual
     4.1.6.195, bits 8 and 24).
  2. SRAMC: the word at 0x03000004 is written and dumped, for the record.  The
     vendor USB stack's identical write sits inside `#ifdef
     CONFIG_ARCH_SUN3IW1P1`, i.e. it does not run on A733, so this register is
     not this die's FIFO RAM select and its read-back is not evidence either
     way (see MusbSramFifoOn).
  3. PHY0, device half (MusbPhyUp): SIDDQ cleared and VBUSVLDEXT set in the MUSB
     wrapper's PHYCTL; then the wrapper's route select, one |= BIT0 and no other
     bit, which is where the pads become the MUSB core's; then the pmu0 passby
     word, whose ULPI bypass bit is what joins the MUSB UTMI port to the PHY at
     all.  Neither the PHY's reset nor the host half is touched again after this
     step - see the note on that in MusbPhyUp.
  4. OTG control, one read-modify-write of ISCR (MusbIscrOn): the ID and D+/D-
     pull-ups enabled, ID forced high (peripheral), VBUS forced valid, and the
     change-detect wake latches cleared.
  5. VEND0 = 0: PIO, no vendor DMA.
  6. POWER, in one write: HSENAB (the core may answer the host's high-speed
     chirp), SOFTCONN (the D+ pull-up, i.e. the moment the host can see a
     device) and SUSPENDM (bit 1).  ISOUPDATE is left as found, and no bit of
     this byte is cleared afterwards - the stack that enumerates on this board
     writes it once and leaves it.
  7. The FIFO map, FADDR = 0, endpoint 0 and the bulk FIFOs flushed.  The
     bulk endpoints' maximum packet size, type and intervals stay at their
     reset values until SET_CONFIGURATION - which is what the enumerating
     device-mode driver does - and every FIFO register is read back and
     printed, because a FIFO-address write that does not stick would leave the
     core with nowhere to put a packet.  Then, in the order the log shows:
       7a. the core's identity words once - CONFIGDATA (per-endpoint FIFOs or a
           shared packet buffer), RAMINFO (how much FIFO RAM it claims), HWVERS,
           EPINFO/LINKINFO/VPLEN and TMCTL.
  8. Interrupts masked here, then handed to the bulk data path: endpoint 0 and
     the bus events are serviced from the poll and stay masked for the life of
     the boot, and only the two bulk endpoints are unmasked - when the
     controller's interrupt can be claimed (step 10).  The enumerating
     device-mode driver keeps them all masked until a fastboot consumer
     activates the device.
  9. The core's DMA engine taken to a known state: every channel's gate cleared
     and the completion latches drained.  A stale channel bound to the bulk OUT
     endpoint would take the host's packet out of the FIFO and clear RXPKTRDY by
     itself, so the host sees an ACK and nothing arrives.  Nothing arms a channel
     afterwards: every byte this driver moves is read from the FIFO ports by the
     CPU, because this die gives software no DMA it can reach (MusbDmaOff states
     the verdict and the evidence).
  10. The controller's interrupt, if the platform has one yet: the two bulk
      endpoints are unmasked and serviced from a handler that stages the OUT
      FIFO into a ring, and endpoint 0 stays masked and polled (see
      MusbInterruptClaim).  A platform that does not install the interrupt
      protocol, or refuses the source, leaves the driver polled - never
      unstarted.

  The role is DEVCTL.HOSTMODE, bit 2.  DEVCTL's bit 7 is BDEVICE - a status bit
  that reads 1 for any peripheral on a B-side connector - and 0x80 therefore
  means "B device, VBUS below valid, no session", not "host mode"; reading it
  the other way sends a reader hunting for a role bug that is not there.
  DEVCTL is read and logged at every bring-up step and never written: the stack
  that enumerates on this board does not write it either, its role split being
  entirely ISCR (FORCE_ID, FORCE_VBUS) and POWER (SOFTCONN).  Nothing here
  depends on it: in particular DEVCTL.SESSION is not a precondition for any
  step, and a session-less byte is a reading to log, not a fault to repair.  The
  log ends with the decoded byte for exactly that reason.

  Provenance, stated plainly because what follows is not what the manual alone
  would give:

  - the host-half-then-device-half order itself, and the fact that nothing
    closes or resets PHY0 afterwards.  An earlier version of this driver powered
    the PHY through the host half and then closed that half and re-pulsed
    PHY_RSTN once the pads were the MUSB core's; it reasoned that an EHCI block
    left clocked on the same two wires would fight the device's pull-up.  With
    everything else identical, that port never enumerated (invisible to the
    host, or visible and unable to transmit).  The same firmware with this
    driver's own CCU/PHY/OTG/FIFO/EP0 code in the reference order enumerates as
    1f3a:1010, which is what moved these two writes out of the file;
  - the CCU gates of step 0 (AHB master gate, CLK_MSI_LITE2, CLK_RES_DCAP_24M,
    the pll-peri0 outputs, USB_REF) and the host half of USB0_BGR: the HCI
    window at 0x04101800 does not accept writes, and the PHY is not powered,
    without them;
  - SRAMC 0x03000004 bits 1:0 = 0b01, the 8 KB USB FIFO mode;
  - the ISCR pull-up enables at bits 17:16, which the manual's U2P0_ISCR table
    marks reserved;
  - POWER as one write of HSENAB | SOFTCONN | SUSPENDM, nothing cleared after
    it - see A733Musb.h, POWER;
  - the pmu0 PHY passby word as a step of its own (ULPI bypass, AHB INCRX
    align, INCR4 and ICHR8, on top of the two sun60iw2 bits the host half sets);
  - interrupts masked at bring-up with only the two bulk endpoints handed to the
    controller's interrupt afterwards (EP0 and INTRUSB stay polled), and the
    core's DMA engine turned off with its status drained;
  - and the fact that DEVCTL.SESSION = 0 does not block enumeration.

  What the bulk OUT path costs, measured rather than assumed.  A 512-byte packet
  took 165.91 us end to end when the split was profiled (3.15 MB/s), and the split
  is 26 us in this driver (24 us of it the 128 FIFO port reads) and 140 us waiting
  for the host's next transaction - so the CPU is not the limit, and a second data
  path would have to remove 24 us of a 166 us packet.  This die offers none: the
  core's own engine is not instantiated, and both system DMACs sit behind a
  security word that reads zero.  The drain now also spins for the packet behind
  the one it takes, which is where the 3.2-3.3 MB/s of the shipped path comes from
  (the profile predates it).  MUSB_RX_TIMING is the switch that produced the
  split, and MusbDmaOff records the DMA verdict with the readings behind it.

  Two readings recorded here have since been retired, and the file says so
  where they were implemented rather than leaving a reader to trust them: the
  SRAMC write is inside the vendor's `#ifdef CONFIG_ARCH_SUN3IW1P1` and never
  runs on A733, so it is a red herring kept only for a comparable log, and the
  FIFO loopback verdict, because a MUSB TX FIFO read back through its own port
  is the packet buffer's read side, not a memory readback.  The loopback is
  gone with it: it wrote a pattern into the endpoint FIFO ports, and with the
  pull-up raised in step 6 (as the reference raises it) that pattern would be
  written into a live packet buffer.  Forced full speed (POWER.HSENAB cleared)
  is gone for a better reason: it was a discriminator for a fault that is not in
  the core's speed negotiation, and the reference never clears HSENAB.

  How it is driven.  Endpoint 0 is polled: the core's bus events latch in
  INTRUSB whether or not they are unmasked, and every piece of control state the
  driver needs is a register read, so a 100 us periodic event services endpoint 0
  (control), acknowledges the bus events and drives the re-attach.  The bulk
  endpoints run on the controller's interrupt when the platform has one: the
  handler stages the OUT FIFO into a ring and the poll hands the ring to the
  transport's RX callback, because the callback is a boot service and the handler
  is not allowed one.  Bulk IN stays synchronous inside Send(), with the TPL
  raised so neither the poll nor the handler can interleave.  With no interrupt
  protocol the poll stages a batch of packets per tick and the driver is simply
  slower - it never takes a different path through the protocol, and neither
  direction can be acknowledged twice, because both paths check the same status
  bits and acknowledge only what they find.

  What the log is for, and how to read it.  Enumeration failure on this port has
  two very different causes that look the same from the host side ("Cannot
  enable.  Maybe the USB cable is bad?"): a core that never sees a USB reset, and
  a core that is reset, chirps, and then never receives a SETUP.  So every
  INTRUSB bit is named as it is cleared ("Musb: bus event RESET/SOF/CONNECT/..."
  plus a running tally), the reset handler records the state it found and the
  state it left, every SETUP records all five request fields and the state
  machine's decision, and POWER/DEVCTL/FADDR are compared with the previous tick
  and reported when they change.  Three rules keep it readable: a bit is printed
  once per occurrence (the core's clear is write-one-to-clear, so "set in this
  read" is "newly seen"), SOF - 8 kHz, on nearly every 100 us poll - is counted
  rather than printed, first appearance excepted, and its acknowledgement line
  is thinned to the first and then one per thousand, because a line costs ~8 ms
  on this console and a line per tick would mean servicing the SIE once per
  eighty ticks.  The state watch prints
  one line per burst rather than one per tick, for the same reason: a synchronous
  serial log that ran on every tick would starve the poll it is describing.  A
  scheduled heartbeat
  every 6 s says the poll is still running, which is what makes "the log went
  quiet" mean "the core latched nothing" rather than "the timer stopped".  None
  of it writes a register; the bring-up's own drain of INTRUSB is the one other
  place that writes that register, and it logs the byte first for the same
  reason.

  The console is quiet while a control transfer is in flight, and that is not a
  style choice.  DEBUG here is synchronous, one ~90-byte line costs about 8 ms at
  115200 8N1, USB 2.0 gives the device 2 ms to answer at the address
  SET_ADDRESS assigned it, and an EP0 data or status stage is answered in tens
  of microseconds - so a single line printed inside a transfer outlasts the
  host's timeout, the host re-sends SET_ADDRESS, and the boot ends in "Device
  not responding to setup address" describing this log rather than the silicon.
  So from the first SETUP of an enumeration until the device is Configured (and
  again for the duration of every later control transfer) mConsoleQuiet is set
  and nothing in the EP0 path prints; the phases are recorded as fixed-size
  records in a 32-entry ring and printed in one block when the window closes -
  at Configured, at the first stall, or after 5 s with no SETUP.  The block is
  compact, one line per event, tick-stamped, and the SETUP line carries all five
  request fields and the decision made for them:

    Musb: [t=1234] SETUP c0 05 1000 0000 0 (SET_ADDRESS) -> status stage armed, CSR0=0x0010
    Musb: [t=1234] Addressed 16 (FADDR read back 0x10, CSR0=0x0000)

  MUSB_ENUM_VERBOSE = 1 puts every one of the verbose lines back, unsuppressed
  and in the order they happen, for a console that does not stall the CPU.

  The second half of that failure - reset seen, SOF seen, no SETUP - is decided
  by readings that are printed so the next boot does not have to guess: every
  change of CSR0 (the register that says whether the core holds a packet for
  endpoint 0 at all, plus the first packet it ever holds), INTRTX/INTRRX (the
  endpoint latches, set whether or not they are masked, so INTRRX bit 0 is
  independent evidence that the SIE did deliver to endpoint 0 even if CSR0
  disagrees), the FIFO/size registers of all three endpoints at bring-up, the
  core's CONFIGDATA/RAMINFO/HWVERS (what FIFO model and how much FIFO RAM the
  core itself claims), and TMCTL (a residual test mode answers no token at
  all).  INTRRX bit 0 setting with CSR0.RXPKTRDY clear means this driver is
  reading the wrong bit; both staying clear leaves the receive path - the SIE,
  the PHY's receive half, the FIFO map - as the place to look, and the CSR0
  watch says which of those two it was.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <IndustryStandard/Usb.h>

#include <Guid/EventGroup.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/UsbDevice.h>
#include <Protocol/HardwareInterrupt.h>
#include <Protocol/HardwareInterrupt2.h>

#include <A733Ccu.h>
#include <A733Musb.h>
#include <A733Usb.h>

//
// The poll period.  It is the cadence of everything that is not bulk data -
// endpoint 0, the bus events, the state watch - and, while the bulk endpoints
// are interrupt-driven, the cadence at which staged packets are handed to the
// transport.  It used to be the bulk-OUT rate as well (one 512-byte packet per
// tick, which measured 50 KB/s), and it is not any more: the ring the handler
// fills absorbs a tick's worth of traffic, see MUSB_RX_RING_SLOTS.
//
#define MUSB_POLL_PERIOD_100NS  1000U

//
// The bulk OUT payload has one data path and only one: the CPU reading the FIFO
// port.  This die offers no other - the core's own bus-master engine is not
// instantiated and both system DMACs are behind a security word that reads zero -
// so there is no second drain here to switch on or off, and MusbDmaOff records
// the evidence for anyone who wants to revive the idea.
//

//
// How long to wait for the PCK600 USB2 power domain to report itself on: 100
// polls, 100 us apart, is what the A733 host firmware allows it.
//
#define MUSB_PPU_TRIES     100U
#define MUSB_PPU_POLL_US   100U

//
// The re-attach window: a bus reset that no SETUP follows within three seconds
// gets this device one clean re-attach (the pull-up down for 50 ms, the device
// half's state rewritten, the pull-up up again), because a host that gave up on
// a port whose connect it had already seen only looks at that port again when
// the connect changes.  Three seconds is a whole enumeration with room for the
// host's retries, and the timings are on the performance counter rather than on
// poll ticks: a tick is nominal 100 us, but every printed line costs ~8 ms here.
//
#define MUSB_REATTACH_AFTER_US   3000000U
#define MUSB_REATTACH_DETACH_US    50000U

//
// The largest wMaxPacketSize this driver will serve or program for a bulk
// endpoint, and the note that says why it is that number.  The port runs at
// whatever speed the host negotiates with POWER.HSENAB set (the reference's
// setting and this driver's), so the transport's descriptors and the core's own
// bulk maximum agree at 512; the cap below exists only so a descriptor that
// overshoots the core's limit is served as something the core can actually
// send.  It used to be 64 while POWER.HSENAB was forced clear - that
// discriminator is retired, see the file head.
//
#define MUSB_BULK_MAX_PACKET_LIMIT  A733_MUSB_BULK_MAX_PACKET
#define MUSB_BULK_LIMIT_NOTE        "high speed, the core's own bulk maximum"

//
// The endpoint numbers the FIFO map is programmed with before any transport has
// declared its own.  The device half runs at DXE dispatch, the descriptors arrive
// with Start(), and the FIFO map cannot be written without endpoint numbers - so
// it is written for the layout the reference's device-mode driver hardcodes and
// this board's fastboot transport declares: EP1 IN, EP2 OUT, 512 bytes each (see
// A733_MUSB_EP1_FIFO_BYTE/A733_MUSB_EP2_FIFO_BYTE for the addresses that go with
// them).  Start() compares what it parsed against mFifoInNumber/mFifoOutNumber
// and reprograms the map only if they differ, which is the one case where these
// two numbers are not the transport's.
//
#define MUSB_ASSUMED_BULK_IN_NUMBER   1U
#define MUSB_ASSUMED_BULK_OUT_NUMBER  2U

//
// The four register sweeps this debugging effort ran are gone from the file.
// What each one measured, and what it found, is here - the answers are the reason
// the bring-up looks the way it does:
//
//   CLK    USB0_CLK's gate bits.  Without CLKEN (bit 31) the PHY's VBUS detector
//          never reports valid, DEVCTL reads 0x80 and not one packet reaches the
//          core: the gate is required, and MusbCcuOn opens it.
//   ROUTE  the wrapper's route register, one extra bit at a time (1..7) and with
//          the EHCI half closed and re-opened, 2 s per variant.  Every variant
//          read back 0x00000001 and left UTMI_STAT and FRNUM at 0: the register
//          accepts no bit above 0, closing the host half changes nothing, and
//          one |= BIT0 in MusbPhyUp is the whole of what the route needs.
//   PHYCTL every bit that can be toggled without touching SIDDQ, VBUSVLDEXT or
//          bit 1 - bits 0, 2, 4, 7, 8, 9, 10, 11, 12, both states, 2 s each, from
//          the poll and only outside a transfer.  None of the ten variants let a
//          status stage complete, so no undocumented PHYCTL bit is the
//          transmitter; the bring-up writes the two the manual names - SIDDQ
//          clear, VBUSVLDEXT set - and leaves bit 1 where the PHY left it (see
//          MusbPhyUp).
//   UTMI   nine candidates for the UTMI+ control word at 0x0410181C, differing
//          only in bits 6:0 (XcvrSelect, TermSelect, OpMode, SuspendM), each
//          attached from scratch and held 3 s.  In device mode that register
//          reads back 0x00000053 whatever is written - the port cannot be
//          re-selected from here at all - so nothing could be varied and the
//          read-back is the answer.
//
// All four wrote their own register and nothing else, and only while no transfer
// was in flight.  None of them was a fix, and none left a write behind in the
// bring-up.
//

//
// How long Send() waits for one bulk-IN packet to leave.  Far longer than a
// high-speed packet needs (8 us for 512 bytes) and short enough that a wedged
// endpoint does not hang the boot.
//
#define MUSB_TX_PACKET_TIMEOUT_US  200000U
#define MUSB_TX_POLL_US            10U

//
// How many times the EP0 acknowledge may read CSR0 back before it gives up on
// RXPKTRDY clearing.  This is inside a control transfer, where the host allows
// the device microseconds, so it is a few microseconds' worth of iterations
// (one indexed MMIO read is tens of nanoseconds) and not the hundred thousand
// (tens of milliseconds) it used to be: a cap that spins longer than the host's
// timeout cannot serve any purpose.  Hitting the cap is recorded in the
// enumeration ring (MusbTagSpinCapped), never printed from here.
//
#define MUSB_EP0_ACK_SPIN  200U

//
// The largest single Send() this driver accepts.  The transport sends command
// responses ("OKAY", "DATAxxxxxxxx", a getvar value of at most 60 characters),
// so this only has to be comfortably more than one packet.
//
#define MUSB_TX_MAX_SIZE  4096U

//
// The controller's interrupt.  The MUSB core's INTR line is GIC SPI 156, i.e.
// GIC INTID 188, which is the number the closed firmware on this die enables
// for USB0 (its FES USB stack sets ISENABLER5 bit 28 - 160 + 28 = 188, the
// SPI-156 enable - and queries the same number from its main loop) and the one
// the reference device driver registers.  The interrupt-protocol call takes the
// INTID, not the SPI number: ArmGicDxe indexes its handler table and its
// distributor registers by INTID, and the handler it calls receives the INTID it
// acknowledged.  The platform's device tree calls the OTG node's interrupt
// GIC_SPI 29, which is the A523 numbering carried over and contradicts the
// closed firmware that runs here; 188 is the number this controller answers to.
//
#define MUSB_GIC_SPI          156U
#define MUSB_GIC_INTID        (32U + MUSB_GIC_SPI)

//
// Packets the interrupt may stage before the poll has to make room, and how
// many the poll moves out of the FIFO per tick when no interrupt is in use.
//
// Why a ring at all.  The handler cannot hand a packet to the transport - the
// callback runs boot services and the handler runs at TPL_HIGH - so it stages
// the bytes and the poll delivers them.  What that buys is the ACK: the host is
// told its packet landed as it lands, instead of one packet per poll tick.
//
// Why 64 slots.  Delivery is once per tick, so the ring's depth is what one tick
// can hand over: 64 slots is 32 KB, i.e. up to 64 packets per tick rather than
// the single packet the old poll moved, whatever the tick's real length turns out
// to be.  (The 50 KB/s this replaces was one 512-byte packet per 10.2 ms, and
// 10.2 ms is also the length of the line the EP0 watch printed for every one of
// them at 115200 - see MUSB_INTR_EP0_BIT.  Both ends of that are fixed here: the
// ACK moved to the interrupt, the per-packet line is gone, and the ring covers a
// tick's worth of traffic if the tick ever is the limit.)
//
#define MUSB_RX_RING_SLOTS  64U
#define MUSB_RX_RING_MASK   (MUSB_RX_RING_SLOTS - 1U)

//
// Per-packet timing of the bulk OUT path (see MusbBulkOutTimingReport).  This is
// the switch that produced the 26 us / 140 us split this file quotes, and it is
// off: it is a measurement tool, not part of the data path.
//
// Where a 512-byte packet's ~166 us goes was for a long time inferred rather than
// measured - the FIFO read is 128 port words, and 128 * 1.2 us was taken to be
// the whole packet time.  Measuring three intervals per packet said otherwise:
//
//   Musb: bulk OUT FIFO per packet: total 165.91 us (handler 25.95 us, of which
//         FIFO 24.45 us; gap 139.95 us, and 2.33 of that is the poll delivering),
//         8066 packet(s) in 8066 handler(s), 127 staged by the poll
//
// i.e. the CPU is not the limit: reading the FIFO costs 24 us (128 words at ~190
// ns, which is what an AHB read costs here) and the whole handler is 26 us, while
// the other 140 us is the endpoint waiting for the host.  512 bytes per 125 us
// microframe is 4.1 MB/s and the path measured 3.15 MB/s that day (3.27 after the
// drain learned to spin for the next packet), so the difference is bus scheduling
// on the hub chain, which no device-side driver can shorten - and it is why the
// DMA work would have bought 24 us of a 166 us packet.
//
// Three intervals per packet, all from the generic counter the rest of the
// driver already uses (ArmArchTimerLib on this platform: one `mrs cntpct_el0`
// per stamp, no ISB, no MMIO, no bus access).  That is what was instrumented, for
// the record, should the path ever have to be profiled again:
//
//   handler  MusbInterruptHandler's first stampable instruction -> the end of
//            the FIFO drain and its RXCSR acknowledgement.  Everything the
//            handler does per packet, minus the INTRRX latch write and the EOI
//            that follow the stamp (both land in the gap below, which is why
//            handler + gap is the packet's period by construction).
//   of which the time inside MusbFifoRead alone - the 128 port words - so
//            handler - fifo is the rest of the handler's work (INDEX select,
//            RXCSR, RXCOUNT, publish, the re-arm attempt).
//   gap      from the end of one packet's handling to the start of the next
//            one's, i.e. the time the endpoint spends waiting for the host or
//            for the next interrupt to be delivered.  The GIC acknowledge and
//            ArmGicDxe's dispatch before the handler's first instruction are in
//            here, because no stamp in this driver can be taken before them.
//
// The gap is everything the machine does between two packets, so it also holds
// this driver's own work outside the handler - the poll's delivery to the
// transport above all, a pool allocation and two copies per ring-full.  That
// part is measured too, once per MusbBulkOutDeliver rather than per packet, so
// a large gap can be read either way: gap minus that is the host and the
// interrupt's latency, the remainder is us.
//
// The stamps are two counter reads per interval (one interval per MusbFifoRead
// call and one per handler entry), i.e. four reads per packet against ~128 bus
// reads: the overhead is under 0.1 us per packet at 400+ MHz and is not
// corrected for.  The line prints what a stamp pair actually costs on the
// running machine, so the reader never has to assume that.  Set the switch to 1
// to measure again; at 0 the file is the code it was, byte for byte.
//
// Two honest limits, both far below the ~166 us the packet costs.  The read is
// `mrs cntpct_el0` with no barrier, so a stamp can be reordered against the
// Device-memory access it brackets by a few tens of cycles; and the C prologue
// (the stack frame save) runs before the first stamp, so the handler interval is
// short by that much.  Neither can move the measurement across the ~150 us the
// FIFO read and the host together account for, which is the question here.
//
#define MUSB_RX_TIMING  0

//
// How long a quiet run of packet counters has to sit still, with the ring empty,
// before the line above is printed: two milliseconds is a dozen packets at this
// path's rate, so it can only be seen while the host is between phases and a
// transfer in progress never leaves the counters alone that long.
//
#define MUSB_RX_TIMING_REPORT_IDLE_TICKS  20U

//
// A gap longer than this is not the interrupt's latency, it is a host with no
// packet to send: the last packet of a run, a phase boundary, a fastboot
// command the host is still composing.  Those are left out of the average and
// counted, so a run that ended on a millisecond of silence cannot be read as
// interrupt latency.  One millisecond is four packets at the 3.15 MB/s this
// path runs at, so it cannot clip a packet period.
//
#define MUSB_RX_TIMING_GAP_MAX_US  1000U

//
// Fewer packets than this in a run is not a run: the fastboot command phases are
// one or two packets, and averaging them would put a host round trip into the
// handler interval.  A phase below the bound is dropped silently - the download
// that follows is thousands of packets and is what the line is for.
//
#define MUSB_RX_TIMING_MIN_PACKETS  16U

//
// How many packets one staging call takes, and how long the no-interrupt path
// waits between them.  The bulk-OUT FIFO is one packet deep (see MusbFifoLayout),
// so a call finds one packet per reading of RXCOUNT; the batch is the ceiling on
// a tick, the FIFO's depth the ceiling on one burst.  Without the interrupt the
// host is still allowed a packet per microframe, so a call that found the FIFO
// empty waits a step at a time for the next one rather than ending the tick.
//
#define MUSB_RX_POLL_BATCH      32U
#define MUSB_RX_POLL_WAIT_US    20U
#define MUSB_RX_POLL_STEP_US    4U
#define MUSB_RX_HANDLER_BATCH   (MUSB_RX_RING_SLOTS - 1U)

//
// Packets one staging call may take out of the FIFO in a single burst, i.e. from
// one reading of RXCOUNT to the one acknowledgement that ends it.  The FIFO
// cannot hold more than this, so it is the FIFO's depth expressed in packets.
//
#define MUSB_RX_BURST_PACKETS   (A733_MUSB_BULK_RX_FIFO_BYTES / A733_MUSB_BULK_MAX_PACKET)

//
// The bounded spin-drain.
//
// A packet's period is not the handler's 26 us, it is 166 us, and the difference
// is time the endpoint spends with nothing reading it.  Part of that is the host
// (a packet per microframe, 125 us), but part of it is this driver: between the
// packet landing and the interrupt being delivered, the FIFO holds a full packet
// and the endpoint NAKs whatever the host offers next - and when it NAKs, the
// host has spent its microframe and the next chance is 125 us away.  A 20 us
// interrupt latency therefore costs more than 20 us.
//
// So during a transfer the drain does not end at the acknowledgement.  It looks
// at RXCSR once more, and if the packet is not there yet it waits for it for a
// slice of a microframe and drains whatever arrives inside that window.  A packet
// caught this way is a packet the host did not have to be NAKed for.
//
// Two bounds keep it from becoming a stall:
//
//   - only while a transfer is live, i.e. while the FIFO path drained a packet
//     within MUSB_RX_ACTIVE_WINDOW_US.  A quiet line exits the check on its first
//     counter read, and a phase that has just ended costs one read, not a slice;
//   - at most MUSB_RX_SPIN_SLICES slices per staging call, so at most 180 us.  A
//     host that stopped is a host that stopped: the TPL is holding every other
//     interrupt on the machine (timer, console) and it has to be given back.
//
// The slice is half a microframe: a host that is sending one packet per
// microframe puts the next one at most 125 us after the last, so 60 us covers
// the second half of the period and a call that finds one re-checks immediately
// for the one behind it.  The register is read once every
// MUSB_RX_SPIN_POLL_STRIDE counter reads rather than on every one: the counter is
// a core register and free, the endpoint's RXCSR is on the peripheral bus and is
// not, and one read per ~0.2 us resolves the packet far more finely than the
// 125 us it has to catch.
//
#define MUSB_RX_SPIN_DRAIN           1

#define MUSB_RX_SPIN_SLICE_US        60U
#define MUSB_RX_SPIN_SLICES          3U
#define MUSB_RX_SPIN_POLL_STRIDE     4U
#define MUSB_RX_ACTIVE_WINDOW_US     2000U

//
// How many staged packets one delivery hands to the transport as one buffer.
//
// Why not one.  Delivering per packet costs a pool allocation, a copy and a
// callback entry per 512 bytes, and at 6 200 packets/s that is 6 200 of each per
// second - a cost that is per packet, like the FIFO reads it now sits next to, so
// it shows up as a flat ceiling on throughput and not as a per-transfer one.
// Eight slots is 4 KiB, i.e. one page: the allocator serves it without splitting
// a larger chunk, the copy stays one call of a size the cache handles in a
// handful of lines, and the buffer a callback hands on is still small enough to
// be freed and reused immediately.  A bigger run buys less each time, because
// what is left after 8 is the FIFO reads and not this.
//
// The run never crosses a ring wrap, because the slots of a chunk have to be
// adjacent in mRxRingData for the copy to be one CopyMem, and it never contains a
// short packet, because a short packet is where the host ends a command or a
// transfer and the transport's command parser reads one command per buffer.
//
#define MUSB_RX_DELIVER_SLOTS   8U

//
// Consecutive handler entries that moved nothing before the driver concludes the
// line is being held by something it cannot clear and gives the data path back
// to the poll.  A handler that spins at TPL_HIGH on a line it cannot deassert
// would hang the machine; falling back costs throughput and nothing else.
//
#define MUSB_IRQ_NO_PROGRESS_LIMIT  1024U

//
// Endpoint 0's bit in INTRTX/INTRRX.  The endpoint watch reports the whole
// register but is only allowed to print when this bit changed.  The bulk
// endpoints' bits change once per packet, and the line that reported each such
// change was ~120 characters, i.e. ~10.4 ms at 115200 - the same 10.2 ms per
// 512-byte packet the old driver measured.  A watch that costs a packet per
// packet is not a watch, it is the data path.
//
#define MUSB_INTR_EP0_BIT  BIT0

STATIC_ASSERT ((MUSB_RX_RING_SLOTS & MUSB_RX_RING_MASK) == 0, "the staging ring's size must be a power of two");

//
// The FIFO map's two invariants, checked where the compiler can still see them:
// the bulk endpoints' buffers must not overlap and must fit the 8 KB (+64)
// window.  EP2's RX FIFO is the deep one and the one that moved, so it is the
// one that is easy to break with an address that "obviously" fits.
//
STATIC_ASSERT (
  (A733_MUSB_EP2_FIFO_BYTE >= (A733_MUSB_EP1_FIFO_BYTE + A733_MUSB_BULK_MAX_PACKET)) ||
  ((A733_MUSB_EP2_FIFO_BYTE + A733_MUSB_BULK_RX_FIFO_BYTES) <= A733_MUSB_EP1_FIFO_BYTE),
  "the bulk-OUT RX FIFO overlaps the bulk-IN TX FIFO"
  );
STATIC_ASSERT (
  (A733_MUSB_EP2_FIFO_BYTE + A733_MUSB_BULK_RX_FIFO_BYTES) <= (8192U + 64U),
  "the bulk-OUT RX FIFO does not fit the endpoint FIFO SRAM"
  );
STATIC_ASSERT (
  ((A733_MUSB_EP2_FIFO_BYTE & 0x7U) == 0),
  "a FIFO address must be a multiple of 8 for RXFIFOADD to name it exactly"
  );

//
// Instrumentation constants.  INTRUSB carries eight bus events, one per bit;
// the SOF bit is the one that arrives every 125 us at high speed, i.e. on
// almost every 100 us poll, so it is the only one that is counted rather than
// printed per occurrence (see MusbAccountBusEvents).  The heartbeat exists so
// that "the log stopped" can be told from "the poll stopped": it prints one
// line every MUSB_HEARTBEAT_TICKS polls whether or not anything happened.
//
#define MUSB_INTRUSB_BITS       8U
#define MUSB_INTRUSB_SOF_BIT    3U
#define MUSB_INTRUSB_RESET_BIT  2U
#define MUSB_HEARTBEAT_TICKS    60000U       // 6 s at MUSB_POLL_PERIOD_100NS
#define MUSB_WATCH_REPORT_TICKS 1000U        // 100 ms: the floor under a chattering watch

//
// How often the SOF acknowledgement is printed.  Every line on this console
// costs about 8 ms at 115200 baud, which is eighty poll periods: a line per
// tick does not describe the poll, it replaces it, and the SIE is then serviced
// once every eighty ticks instead of every tick.  SOF is the only event that
// arrives on nearly every tick (8 kHz against a 10 kHz poll), so it is the only
// one whose acknowledgement is thinned out: the first one, then one per
// MUSB_SOF_REPORT_EVERY, each carrying the running total.
//
#define MUSB_SOF_REPORT_EVERY   1000U

//
// The enumeration trace, and why it is a ring rather than lines.
//
// DEBUG on this console is synchronous: one ~90-byte line costs about 8 ms at
// 115200 8N1.  USB 2.0 gives a device 2 ms to answer at the address
// SET_ADDRESS assigned it, and an EP0 data or status stage is answered in tens
// of microseconds - so a single line printed inside a control transfer outlasts
// the host's whole timeout.  The host then re-sends SET_ADDRESS, and finally
// reports "Device not responding to setup address", which describes this log and
// not the silicon.
//
// So while a control transfer is in flight nothing is printed.  The hot path
// records fixed-size records in this ring instead (MusbRingAdd), and the ring is
// printed once the window closes: the device reaches Configured, a request is
// stalled, or 5 s pass with no SETUP.  mConsoleQuiet is the flag that closes the
// console; MusbQuietEnter/MusbQuietLeave are the only two places that move it.
//
// MUSB_ENUM_VERBOSE = 1 restores the pre-ring behaviour - every phase printed as
// it happens, no quiet window (so nothing is ever drained from the ring) - for
// a console that does not stall the CPU (a debug port, or a capture drained by
// hardware).  The verbose lines are all still in the source, and every one of
// them is a single edit away.
//
#define MUSB_ENUM_VERBOSE  0

//
// Records in the ring, and how long a window survives without a SETUP.  5 s is
// an order of magnitude more than a host needs between two control transfers of
// an enumeration, and short enough that a host which gave up is reported while
// the boot is still running.
//
#define MUSB_RING_ENTRIES      32U
//
// Ticks, not seconds: the poll period is nominally 100 us, but the timer's real
// granularity on this platform is coarser and every DEBUG line prints
// synchronously (~8 ms), so a tick count is the only unit that means the same
// thing in both cases.  200 ticks is a few seconds of a quiet bus - far more
// than the gap between two control transfers of one enumeration - and short
// enough that a host which gave up is reported while the boot is still running.
//
#define MUSB_QUIET_IDLE_TICKS  200U

#if MUSB_ENUM_VERBOSE
#define MUSB_TRACE(...)  DEBUG ((DEBUG_INFO, __VA_ARGS__))
#else
//
// Compiles the arguments, so every local they mention stays used, and folds the
// call away at build time: no DEBUG is executed while the console is quiet.
//
#define MUSB_TRACE(...)  do { if (MUSB_ENUM_VERBOSE) { DEBUG ((DEBUG_INFO, __VA_ARGS__)); } } while (FALSE)
#endif

//
// The two above are bit numbers, and A733Musb.h names the same two events as
// masks; a change to either file that separates them would silently turn the
// event log into a list of the wrong names.
//
STATIC_ASSERT ((A733_MUSB_INTRUSB_SOF >> MUSB_INTRUSB_SOF_BIT) == 1, "SOF bit number moved");
STATIC_ASSERT ((A733_MUSB_INTRUSB_RESET >> MUSB_INTRUSB_RESET_BIT) == 1, "RESET bit number moved");

//
// String descriptors.  The transport's device descriptor carries no string
// indices (all zero), so the host does not ask for these; they are answered
// anyway because a descriptor set that stalls index 0 makes a host retry it.
//
STATIC CHAR8  mStringLangId[]     = { 0x09, 0x04 };            // 0x0409, US English
STATIC CHAR8  mStringManufacturer[] = "Radxa";
STATIC CHAR8  mStringProduct[]    = "Cubie A7A";
STATIC CHAR8  mStringSerial[]     = "A7A00000001";

//
// Endpoint-0 control-transfer state.  Ep0Tx is a data-IN stage in progress,
// Ep0Status is a status stage armed with DATAEND and not yet completed.
//
typedef enum {
  MusbEp0Idle = 0,
  MusbEp0Tx,
  MusbEp0Status
} MUSB_EP0_STATE;

typedef struct {
  CONST UINT8    *Data;
  UINTN          Length;
  UINTN          Sent;
} MUSB_EP0_TX;

//
// One bulk endpoint as the transport's configuration descriptor describes it.
//
typedef struct {
  UINT8     Number;        // endpoint number, i.e. the low nibble of the address
  UINT8     Address;       // bEndpointAddress, direction bit included
  UINT16    MaxPacket;     // wMaxPacketSize
} MUSB_BULK_EP;

//
// One register of the state watch: what it read at the last report, what it
// reads now, and how many times it changed in between.  A burst is reported as
// one line rather than one per tick - see MusbWatchTick.
//
typedef struct {
  UINT8     Previous;      // value at the last report
  UINT8     Value;         // value read at the current tick
  UINT32    Changes;       // changes since the last report
  UINT32    FirstTick;     // tick of the first of them
  UINT32    LastTick;      // tick of the latest of them
} MUSB_WATCH;

//
// One record of the enumeration ring.  Fixed size, and no formatting work in
// the producer: everything a line needs is packed into A and B by the hot path
// and decoded when the ring is drained (MusbRingDrain).  Tick is the poll tick
// the record was made at, which is what puts a transfer on the log's timeline
// (100 us per tick).
//
typedef struct {
  UINT32    Tick;
  UINT16    Tag;
  UINT32    A;
  UINT32    B;
} MUSB_RING_ENTRY;

//
// What a record is.  MusbRingDrain is the only decoder, and each tag documents
// which halves of A and B it uses.
//
typedef enum {
  MusbTagSetup,          // A = bmRequestType | bRequest<<8 | wValue<<16, B = wIndex | wLength<<16
  MusbTagAnswer,         // A = MUSB_ANSWER, B = see that code (staged bytes, COUNT0, CSR0)
  MusbTagResponseShort,  // A = staged bytes, B = wLength (a short packet ends the transfer)
  MusbTagResponseLong,   // A = staged bytes, B = wLength (the core would send past wLength)
  MusbTagAddressed,      // A = address, B = FADDR read back | CSR0<<8
  MusbTagConfigured,     // A = bulk IN wMaxPacketSize, B = bulk OUT wMaxPacketSize
  MusbTagUnconfigured,   // A = 0, B = 0
  MusbTagReset,          // A = FADDR | POWER<<8 | DEVCTL<<16, B = CSR0 | COUNT0<<16
  MusbTagResetDone,      // A = FADDR | POWER<<8 | DEVCTL<<16, B = CSR0
  MusbTagBusEvent,       // A = INTRUSB byte
  MusbTagSetupEndNew,    // A = CSR0 before, B = CSR0 after | COUNT0<<16
  MusbTagSetupEndOnly,   // A = CSR0 after the acknowledge
  MusbTagRxPktFirst,     // A = CSR0, B = COUNT0 | INTRRX<<16
  MusbTagSpinCapped,     // A = iterations the poll was capped at
  MusbTagStatusCsr,      // A = CSR0 before, B = CSR0 after (a change seen with a status stage armed)
  MusbTagFaddr           // A = FADDR before, B = FADDR now (the core took an address)
} MUSB_RING_TAG;

//
// What the hot path decided for a SETUP.  Recorded as MusbTagAnswer and printed
// by the drain as the "->" clause of the SETUP line, so one request is one line.
//
typedef enum {
  MusbAnswerStatusArmed = 0,   // B = CSR0 read back after SVDRXPKTRDY|DATAEND
  MusbAnswerDataIn,            // B = bytes staged for the data-IN stage
  MusbAnswerStallCount,        // B = COUNT0 raw (an EP0 packet that is not a SETUP)
  MusbAnswerStallRefused,      // nothing this device answers the request with
  MusbAnswerStallOutData,      // the request would need an OUT data stage
  MusbAnswerStallType,         // not a standard request
  MusbAnswerStallConfig,       // no such configuration
  MusbAnswerStallFeature,      // no such feature, or the wrong recipient
  MusbAnswerStallInterface,    // one interface at alternate 0 only
  MusbAnswerStallRequest       // request not implemented
} MUSB_ANSWER;

STATIC USB_DEVICE_PROTOCOL    mUsbDeviceProtocol = {
  NULL,                    // Start, filled in below
  NULL                     // Send
};

STATIC USB_DEVICE_DESCRIPTOR  *mDeviceDescriptor;
STATIC VOID                   **mConfigDescriptors;
STATIC USB_DEVICE_RX_CALLBACK mRxCallback;
STATIC USB_DEVICE_TX_CALLBACK mTxCallback;

//
// The configuration descriptor this driver actually serves, and the RAM copy it
// is rewritten in when the speed in force needs a smaller bulk packet than the
// transport declared.  mConfigDescriptors stays the transport's array: it is not
// this driver's to edit, GET_DESCRIPTOR may be asked for the descriptor many
// times, and Start() may be called again.
//
#define MUSB_CONFIG_COPY_MAX  256U
STATIC UINT8  mConfigCopy[MUSB_CONFIG_COPY_MAX];
STATIC UINT8  *mConfigDescriptor;

STATIC MUSB_BULK_EP  mBulkIn;
STATIC MUSB_BULK_EP  mBulkOut;

//
// The endpoint numbers the FIFO map in force was written with, i.e. what
// MusbFifoLayout last programmed.  Before Start() these are
// MUSB_ASSUMED_BULK_*; Start() compares the descriptors it parsed against them
// and reprograms the map only when they differ.
//
STATIC UINT8  mFifoInNumber;
STATIC UINT8  mFifoOutNumber;

STATIC EFI_EVENT  mPollEvent;
STATIC BOOLEAN    mPollRunning;
STATIC BOOLEAN    mPollBusy;

//
// TRUE from the moment the device half is up (at DXE dispatch) - not from
// Start(): the poll services the port whether or not a consumer has taken the
// protocol, so the pull-up that is up is also answered.  Only the
// ExitBootServices handler clears it.
//
STATIC BOOLEAN    mStarted;

//
// The bring-up is in two phases, both from the entry point at DXE dispatch, as
// the reference's self-starting device driver runs them: the platform half
// (power domain, subsystem clocks, USB0's host half, the resistance calibration
// and PHY0 itself), then the device half (SRAM, PHY route and passby, ISCR,
// POWER, FIFO map, EP0) and the poll timer.  mPlatformUp and mDeviceUp are what
// make each phase run once: a Start() that finds either clear runs it, and a
// second Start() redoes neither.
//
STATIC BOOLEAN    mPlatformUp;
STATIC BOOLEAN    mDeviceUp;

//
// A GET_DESCRIPTOR(DEVICE) was refused because no transport had published a
// device descriptor yet.  Start() turns this into a re-attach window of its own:
// the host may have given up on the connect it saw, and it only samples a port
// whose connect changes.
//
STATIC BOOLEAN    mDescriptorMiss;

//
// The one re-attach after a reset no SETUP followed.  Armed by the reset
// handler, cleared by a SETUP or by the re-attach itself, and spent at most once
// per boot.
//
STATIC BOOLEAN    mReattachArmed;
STATIC BOOLEAN    mReattachDone;
STATIC UINT32     mReattachSetups;    // SETUPs handled when the window was armed
STATIC UINT64     mReattachResetAt;   // performance counter at that reset
STATIC UINT64     mReattachFrequency; // performance counter ticks per second, 0 = untimed
STATIC UINT32     mSetupsHandled;     // SETUPs handled since this driver came up

STATIC MUSB_EP0_STATE  mEp0State;
STATIC MUSB_EP0_TX     mEp0Tx;
STATIC UINT8           mEp0Scratch[64];
STATIC BOOLEAN         mEp0AddressPending;
STATIC UINT8           mEp0NewAddress;
STATIC BOOLEAN         mEp0ConfigPending;
STATIC UINT8           mEp0NewConfig;
STATIC UINT8           mAddress;
STATIC UINT8           mConfiguration;

//
// Instrumentation state.  None of it is read by anything that decides what to
// write: the counters and the watch baseline only describe.
//
STATIC UINT32   mIntrCount[MUSB_INTRUSB_BITS];   // bus events seen, per INTRUSB bit
STATIC UINT32   mPollTicks;                      // poll calls since the driver came up
STATIC UINT32   mLastEventTick;                  // tick of the last INTRUSB event of any kind
STATIC BOOLEAN  mWatchValid;                     // a baseline has been taken
STATIC MUSB_WATCH  mWatchPower;                  // POWER, compared tick to tick
STATIC MUSB_WATCH  mWatchDevctl;                 // DEVCTL, compared tick to tick
STATIC MUSB_WATCH  mWatchFaddr;                  // FADDR, compared tick to tick
STATIC UINTN    mEp0Requested;                   // wLength of the SETUP being answered
//
// Endpoint-0 watch.  CSR0 is the one register that says whether the core has a
// packet for this driver at all, so every change of it is printed, and
// INTRTX/INTRRX are read for the same reason: they latch a delivered packet
// whether or not the endpoint masks it, so INTRRX bit 0 set is independent
// evidence that the SIE did hand a SETUP to endpoint 0 even if CSR0.RXPKTRDY
// then reads clear.  Nothing here writes either register.
//
STATIC UINT16   mEp0WatchedCsr;                  // CSR0 as last printed
STATIC BOOLEAN  mEp0WatchValid;                  // a baseline has been printed
STATIC UINT32   mEp0RxPktTicks;                  // polls that saw CSR0.RXPKTRDY
STATIC UINT16   mEpWatchedIntrTx;                // INTRTX as last printed
STATIC UINT16   mEpWatchedIntrRx;                // INTRRX as last printed
STATIC BOOLEAN  mEpWatchValid;                   // a baseline has been printed

//
// The interrupt the bulk data path runs on.  Both pointers are the platform's
// and only one of them is set: ArmGicDxe installs HardwareInterrupt2 and
// HardwareInterrupt1 in one call, and this driver prefers the second for its
// trigger-type call.  They are kept rather than looked up per interrupt because
// the handler must end its own interrupt through the protocol it registered
// with - ArmGicDxe writes the EOI only for a source nothing has registered.
//
STATIC EFI_HARDWARE_INTERRUPT_PROTOCOL   *mInterrupt;
STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL  *mInterrupt2;
STATIC BOOLEAN  mIrqClaimed;                     // registered and enabled: the data path is the handler's
STATIC BOOLEAN  mIrqStuck;                       // the handler gave the line back; the poll owns the data path
STATIC BOOLEAN  mIrqAnnounced;                   // the first staged packet has been reported
STATIC BOOLEAN  mIrqSilent;                      // claimed but never delivered: reported
STATIC UINT32   mIrqCount;                       // entries into the handler
STATIC UINT32   mIrqNoProgress;                  // consecutive entries that moved nothing
STATIC UINT32   mIrqCompleted;                   // bulk-IN completions the handler saw

//
// The bulk OUT staging ring: the handler puts packets here (it cannot allocate
// pool or call the transport at TPL_HIGH) and the poll hands them over at
// TPL_NOTIFY.  Head is written only by the producer and tail only by the
// consumer, so the two contexts never need the same lock; the fences around them
// are in MusbBulkOutPublish and MusbBulkOutDeliver.
//
// The array is cache-line aligned so a slot never shares a line with its
// neighbour.  Nothing but the CPU writes a slot today, so the alignment is
// insurance rather than a requirement: it keeps a future engine-written slot from
// having to invalidate a line its neighbour is still being staged into.
//
STATIC UINT8    mRxRingData[MUSB_RX_RING_SLOTS * A733_MUSB_BULK_MAX_PACKET] __attribute__ ((aligned (64)));
STATIC UINT16   mRxRingLength[MUSB_RX_RING_SLOTS];
STATIC UINT32   mRxRingHead;                     // next free slot (producer)
STATIC UINT32   mRxRingTail;                     // oldest staged slot (consumer)
STATIC BOOLEAN  mRxHeld;                         // packets were left in the FIFO: no room for them
STATIC BOOLEAN  mTxRearm;                        // the IN source was masked by the handler
STATIC UINT32   mRxStaged;                       // packets staged into the ring
STATIC UINT32   mRxDelivered;                    // packets handed to the transport
STATIC UINT32   mRxDropped;                      // packets this driver could not take

//
// The per-packet timing accumulators (see MUSB_RX_TIMING).  Only the producer
// side - the handler - writes the sums, and the poll reads and clears them at
// TPL_HIGH when it prints, so no entry half way through a 64-bit add can be
// read.  mRxTimingFifoRun is the FIFO time of the entry being recorded: the
// handler zeroes it on entry and the record takes it, which keeps the time a
// poll-staged packet spent in the FIFO out of the handler's average.
//
#if MUSB_RX_TIMING
STATIC UINT64   mRxTimingFrequency;              // counter ticks per second, read once
STATIC UINT64   mRxTimingGapMaxTicks;            // MUSB_RX_TIMING_GAP_MAX_US, in ticks
STATIC UINT64   mRxTimingIrqStart;               // handler entry, current entry
STATIC UINT64   mRxTimingFifoStart;              // inside MusbFifoRead, current call
STATIC UINT64   mRxTimingFifoRun;                // FIFO time of the current entry
STATIC UINT64   mRxTimingEnd;                    // drain+ack end of the last recorded entry
STATIC UINT64   mRxTimingHandlerTicks;           // sum of the handler intervals
STATIC UINT64   mRxTimingFifoTicks;              // sum of the FIFO intervals
STATIC UINT64   mRxTimingGapTicks;               // sum of the gaps
STATIC UINT32   mRxTimingEntries;                // handler entries that staged a packet
STATIC UINT32   mRxTimingPackets;                // packets those entries staged
STATIC UINT32   mRxTimingGaps;                   // gaps in the sum
STATIC UINT32   mRxTimingGapsClipped;            // gaps past the bound, left out
STATIC BOOLEAN  mRxTimingEndValid;               // mRxTimingEnd holds an end to measure from
STATIC UINT32   mRxTimingPollPackets;            // packets staged by the poll, not the handler
STATIC BOOLEAN  mRxTimingPollWarned;             // the poll-only warning has been printed
STATIC UINT64   mRxTimingDeliverTicks;           // time the poll's delivery spent, all calls
STATIC UINT32   mRxTimingDelivered;              // packets that delivery handed over
STATIC UINT32   mRxTimingReportIdle;             // quiet ticks since the counters last moved
#endif

//
// The spin-drain's state (see MUSB_RX_SPIN_DRAIN).  mRxLastPacket is the only
// entry that is not instrumentation: it is the stamp the window is measured
// from, written by both producers - the handler's drain and the poll's - because
// either of them is evidence that a host is in the middle of a transfer.
//
#if MUSB_RX_SPIN_DRAIN
STATIC UINT64   mRxSpinTicksPerUs;               // counter ticks per microsecond, read once
STATIC UINT64   mRxLastPacket;                   // counter at the last drained packet, 0 = none
#if MUSB_RX_TIMING
STATIC UINT32   mRxSpinSlices;                   // slices entered
STATIC UINT32   mRxSpinHits;                     // slices that found a packet
STATIC UINT32   mRxSpinPackets;                  // packets the spin took rather than an interrupt
STATIC UINT64   mRxSpinTicks;                    // time spent in slices, all of them
#endif
#endif

//
// The quiet window and the ring that stands in for the console inside it.
// mConsoleQuiet is TRUE from the first SETUP of an enumeration until the device
// is Configured, and again while any later SETUP is being serviced
// (MusbQuietEnter); no DEBUG call may run while it is set.  Nothing here is a
// register or a decision: an entry in the ring is a line waiting to be printed.
//
STATIC BOOLEAN         mConsoleQuiet;            // the EP0 path must not print
STATIC UINT32          mRingLastSetupTick;       // poll tick of the last SETUP taken
STATIC MUSB_RING_ENTRY mRing[MUSB_RING_ENTRIES];
STATIC UINT32          mRingNext;                // where the next record goes
STATIC UINT32          mRingCount;               // records held, at most MUSB_RING_ENTRIES
STATIC UINT32          mRingLost;                // records dropped because the ring was full
STATIC BOOLEAN         mStallPending;            // a request was stalled; close the window

/**
  Address a register in the MUSB core's window.

  @param[in] Offset  Register offset, one of the A733_MUSB_* values.

  @return The MMIO address.
**/
STATIC
UINTN
MusbReg (
  IN UINT32  Offset
  )
{
  return (UINTN)A733_MUSB_BASE + Offset;
}

/**
  Address a CCU register.

  @param[in] Offset  Register offset inside the CCU window.

  @return The MMIO address.
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
  Select the endpoint the indexed registers and the FIFO port belong to.
**/
STATIC
VOID
MusbSelectEp (
  IN UINT8  Ep
  )
{
  MmioWrite8 (MusbReg (A733_MUSB_INDEX_REG), Ep);
}

//
// DEVCTL.VBUS[1:0] as text, indexed by the field's own value.  The field is a
// level, not a flag: only 0b11 ("above VBus-valid") is a session the core will
// act on, and 0 is the state this driver found it in on the first run.
//
STATIC CONST CHAR8 *CONST  mMusbVbusState[4] = {
  "below-valid",
  "above-valid",
  "above-session-end",
  "above-vbus-valid"
};

/**
  Print the core's role, session and speed state, decoded.

  One line, four registers: POWER for the pull-up and the high-speed handshake,
  DEVCTL for the role and for what the PHY's VBUS comparator reports, FADDR for
  the address a host has assigned, and INTRUSB for the bus events that have
  latched (CONNECT in particular says the core did drive D+, RESET that the
  host has reset it).  Called after each bring-up step, so the log shows which
  write changed what rather than what the state was at the end.

  DEVCTL is printed raw and decoded on purpose.  The raw byte is what a
  register dump from another tool compares against; the decode is what stops
  bit 7 (BDEVICE, a status bit that is 1 for any B-side peripheral) from being
  read as the role select, which is bit 2 (HOSTMODE) and is printed separately.

  @param[in] Tag  Where in the bring-up this reading was taken.
**/
STATIC
VOID
MusbLogCoreState (
  IN CONST CHAR8  *Tag
  )
{
  UINT8  Power;
  UINT8  Devctl;
  UINT8  Vbus;

  Power  = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
  Devctl = MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG));
  Vbus   = (UINT8)((Devctl >> A733_MUSB_DEVCTL_VBUS_SHIFT) & A733_MUSB_DEVCTL_VBUS_MASK);

  DEBUG ((
    DEBUG_INFO,
    "Musb: %a: POWER=0x%02x (softconn=%u hsenab=%u suspend=%u hsmode=%u) "
    "DEVCTL=0x%02x (%a-device fsdev=%u lsdev=%u vbus=%a hostmode=%u hostreq=%u session=%u) "
    "FADDR=%u INTRUSB=0x%02x\n",
    Tag,
    Power,
    (UINT32)((Power & A733_MUSB_POWER_SOFTCONN) != 0),
    (UINT32)((Power & A733_MUSB_POWER_HSENAB) != 0),
    (UINT32)((Power & A733_MUSB_POWER_SUSPEND) != 0),
    (UINT32)((Power & A733_MUSB_POWER_HSMODE) != 0),
    Devctl,
    ((Devctl & A733_MUSB_DEVCTL_BDEVICE) != 0) ? "B" : "A",
    (UINT32)((Devctl & A733_MUSB_DEVCTL_FSDEV) != 0),
    (UINT32)((Devctl & A733_MUSB_DEVCTL_LSDEV) != 0),
    mMusbVbusState[Vbus],
    (UINT32)((Devctl & A733_MUSB_DEVCTL_HOSTMODE) != 0),
    (UINT32)((Devctl & A733_MUSB_DEVCTL_HOSTREQ) != 0),
    (UINT32)((Devctl & A733_MUSB_DEVCTL_SESSION) != 0),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG)),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSB_REG))
    ));
}

/**
  Print the whole VBUS path in one line: the wrapper, the PHY and the core.

  DEVCTL says what the SIE believes; it cannot say why.  The three sources that
  decide it are all outside the core, and each of them is a different repair:

    ISCR bits 13:12  the value forced onto the SIE ("For SIE" in the manual)
    ISCR bits 11:10  which PHY detector that value (when not forced) comes from
    ISCR bit 29/30   the two detectors' own verdicts: VBUS pad, DP/DM line state
    PHYCTL           whether the PHY is powered and which comparator is enabled

  This driver has been through the first two of those fields already and the
  core still reported VBUS=below-valid, so the log needs the last two in the
  same line as DEVCTL to say where the signal stops.  Bit 29 clear with bit 30
  set is a VBUS pad that is not bonded or not driven - not a PHY fault, and not
  something any force field can fix.

  @param[in] Tag  Where in the bring-up this reading was taken.
**/
STATIC
VOID
MusbLogVbusPath (
  IN CONST CHAR8  *Tag
  )
{
  UINT32  Iscr;
  UINT32  PhyCtl;
  UINT8   Power;
  UINT8   Devctl;

  Iscr   = MmioRead32 (MusbReg (A733_MUSB_ISCR_REG));
  PhyCtl = MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG));
  Power  = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
  Devctl = MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG));

  DEBUG ((
    DEBUG_INFO,
    "Musb: VBUS path %a: ISCR=0x%08x (force vbus=0x%x id=0x%x src=0x%x "
    "vbus_pad=%u dpdm_line=%u merged=%u) PHYCTL=0x%08x (siddq=%u vbusvldext=%u "
    "vbusvldextsel=%u drvvbus=%u otgdisable=%u) POWER=0x%02x DEVCTL=0x%02x "
    "(vbus=%a session=%u) route=0x%08x\n",
    Tag,
    Iscr,
    (UINT32)((Iscr >> A733_MUSB_ISCR_VBUS_FORCE_SHIFT) & A733_MUSB_ISCR_FORCE_MASK),
    (UINT32)((Iscr >> A733_MUSB_ISCR_ID_FORCE_SHIFT) & A733_MUSB_ISCR_FORCE_MASK),
    (UINT32)((Iscr >> A733_MUSB_ISCR_VBUS_SRC_SHIFT) & A733_MUSB_ISCR_VBUS_SRC_MASK),
    (UINT32)((Iscr & A733_MUSB_ISCR_VBUS_VALID_FROM_VBUS) != 0),
    (UINT32)((Iscr & A733_MUSB_ISCR_VBUS_VALID_FROM_DATA) != 0),
    (UINT32)((Iscr & A733_MUSB_ISCR_MERGED_VBUS_STATUS) != 0),
    PhyCtl,
    (UINT32)((PhyCtl & A733_MUSB_PHYCTL_SIDDQ) != 0),
    (UINT32)((PhyCtl & A733_MUSB_PHYCTL_VBUSVLDEXT) != 0),
    (UINT32)((PhyCtl & A733_MUSB_PHYCTL_VBUSVLDEXTSEL) != 0),
    (UINT32)((PhyCtl & A733_MUSB_PHYCTL_DRVVBUS) != 0),
    (UINT32)((PhyCtl & A733_MUSB_PHYCTL_OTGDISABLE) != 0),
    Power,
    Devctl,
    mMusbVbusState[((Devctl >> A733_MUSB_DEVCTL_VBUS_SHIFT) & A733_MUSB_DEVCTL_VBUS_MASK)],
    (UINT32)((Devctl & A733_MUSB_DEVCTL_SESSION) != 0),
    (UINT32)MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG))
    ));
}

//
// ---------------------------------------------------------------------------
// Instrumentation
//
// Everything below reads registers the driver already reads and turns what it
// finds into one line each.  No bring-up write is added, removed or moved: the
// log says what the hardware did, not what the driver asked for, which is the
// only way to tell a host that never reset this device from one that reset it
// and then never sent a SETUP.
// ---------------------------------------------------------------------------
//

//
// Bit names, one table per register the log decodes.  Index = bit number, so a
// changed-bits mask can be turned into names without a chain of conditionals.
//
STATIC CONST CHAR8 *CONST  mMusbIntrName[MUSB_INTRUSB_BITS] = {
  "SUSPEND",
  "RESUME",
  "RESET",
  "SOF",
  "CONNECT",
  "DISCONNECT",
  "SESSION_REQ",
  "VBUS_ERROR"
};

STATIC CONST CHAR8 *CONST  mMusbPowerName[8] = {
  "ENSUSPEND",
  "SUSPEND",
  "RESUME",
  "RESET",
  "HSMODE",
  "HSENAB",
  "SOFTCONN",
  "ISOUPDATE"
};

STATIC CONST CHAR8 *CONST  mMusbDevctlName[8] = {
  "SESSION",
  "HOSTREQ",
  "HOSTMODE",
  "VBUS0",
  "VBUS1",
  "LSDEV",
  "FSDEV",
  "BDEVICE"
};

STATIC CONST CHAR8 *CONST  mMusbFaddrName[8] = {
  "ADDR0",
  "ADDR1",
  "ADDR2",
  "ADDR3",
  "ADDR4",
  "ADDR5",
  "ADDR6",
  "UPDATE"
};

//
// CSR0, peripheral mode (INDEX = 0), indexed by bit number - see A733Musb.h.
// Nine entries because bit 8 (FLUSHFIFO) is part of the word the log decodes.
//
STATIC CONST CHAR8 *CONST  mMusbCsr0Name[9] = {
  "RXPKTRDY",
  "TXPKTRDY",
  "SENTSTALL",
  "DATAEND",
  "SETUPEND",
  "SENDSTALL",
  "SVDRXPKTRDY",
  "SVDSETUPEND",
  "FLUSHFIFO"
};

/**
  Append one word to a '|'-separated list.

  A word that does not fit is dropped rather than truncated, so what a line
  prints is always a whole name.

  @param[in,out] Buffer  The list; NUL-terminated on entry and exit.
  @param[in]     Size    Capacity of Buffer in bytes.
  @param[in]     Name    The word to append.
**/
STATIC
VOID
MusbAppendName (
  IN OUT CHAR8        *Buffer,
  IN     UINTN        Size,
  IN     CONST CHAR8  *Name
  )
{
  UINTN  Used;
  UINTN  Add;

  Used = AsciiStrLen (Buffer);
  Add  = AsciiStrLen (Name);

  if (Used != 0) {
    if ((Used + 2) > Size) {
      return;
    }

    Buffer[Used++] = '|';
    Buffer[Used]   = '\0';
  }

  if ((Used + Add + 1) > Size) {
    return;
  }

  CopyMem (Buffer + Used, Name, Add + 1);
}

/**
  Name the bits that differ between two readings of one register.

  @param[in]  Diff    Exclusive-or of the two readings.
  @param[in]  Names   Bit-name table, indexed by bit number.
  @param[in]  Count   Entries in Names.
  @param[out] Buffer  Receives the '|'-separated list.
  @param[in]  Size    Capacity of Buffer in bytes.
**/
STATIC
VOID
MusbDiffNames (
  IN  UINT32              Diff,
  IN  CONST CHAR8 *CONST  *Names,
  IN  UINTN               Count,
  OUT CHAR8               *Buffer,
  IN  UINTN               Size
  )
{
  UINTN  Bit;

  Buffer[0] = '\0';

  for (Bit = 0; Bit < Count; Bit++) {
    if ((Diff & (1U << Bit)) == 0) {
      continue;
    }

    if (Names[Bit] != NULL) {
      MusbAppendName (Buffer, Size, Names[Bit]);
    }
  }

  if (Buffer[0] == '\0') {
    //
    // Diff 0 happens when a burst of changes ends where it started; the
    // "N change(s)" in the caller's line then says how it got there.
    //
    MusbAppendName (Buffer, Size, "no-net-bit-change");
  }
}

/**
  Name a standard request, or say that it is not one.

  @param[in] Request  bRequest from the SETUP packet.

  @return A constant string.
**/
STATIC
CONST CHAR8 *
MusbRequestName (
  IN UINT8  Request
  )
{
  switch (Request) {
    case USB_REQ_GET_STATUS:
      return "GET_STATUS";
    case USB_REQ_CLEAR_FEATURE:
      return "CLEAR_FEATURE";
    case USB_REQ_SET_FEATURE:
      return "SET_FEATURE";
    case USB_REQ_SET_ADDRESS:
      return "SET_ADDRESS";
    case USB_REQ_GET_DESCRIPTOR:
      return "GET_DESCRIPTOR";
    case USB_REQ_SET_DESCRIPTOR:
      return "SET_DESCRIPTOR";
    case USB_REQ_GET_CONFIG:
      return "GET_CONFIGURATION";
    case USB_REQ_SET_CONFIG:
      return "SET_CONFIGURATION";
    case USB_REQ_GET_INTERFACE:
      return "GET_INTERFACE";
    case USB_REQ_SET_INTERFACE:
      return "SET_INTERFACE";
    case USB_REQ_SYNCH_FRAME:
      return "SYNCH_FRAME";
    default:
      return "not-a-standard-request";
  }
}

/**
  Name bmRequestType's type field.

  @param[in] RequestType  bmRequestType from the SETUP packet.

  @return A constant string.
**/
STATIC
CONST CHAR8 *
MusbRequestTypeName (
  IN UINT8  RequestType
  )
{
  switch (RequestType & 0x60) {
    case USB_REQ_TYPE_STANDARD:
      return "standard";
    case USB_REQ_TYPE_CLASS:
      return "class";
    case USB_REQ_TYPE_VENDOR:
      return "vendor";
    default:
      return "reserved-type";
  }
}

/**
  Name bmRequestType's recipient field.

  @param[in] RequestType  bmRequestType from the SETUP packet.

  @return A constant string.
**/
STATIC
CONST CHAR8 *
MusbRequestTargetName (
  IN UINT8  RequestType
  )
{
  switch (RequestType & 0x1F) {
    case USB_TARGET_DEVICE:
      return "device";
    case USB_TARGET_INTERFACE:
      return "interface";
    case USB_TARGET_ENDPOINT:
      return "endpoint";
    case USB_TARGET_OTHER:
      return "other";
    default:
      return "reserved-recipient";
  }
}

/**
  Name endpoint 0's control-transfer state.

  @param[in] State  The state machine's current state.

  @return A constant string.
**/
STATIC
CONST CHAR8 *
MusbEp0StateName (
  IN MUSB_EP0_STATE  State
  )
{
  switch (State) {
    case MusbEp0Tx:
      return "data-IN";
    case MusbEp0Status:
      return "status";
    case MusbEp0Idle:
    default:
      return "idle";
  }
}

/**
  Record one event of the enumeration trace in the ring.

  Called from the EP0 hot path, where nothing may print: the record is a few
  stores and no formatting, and its contents are decoded later by
  MusbRingDrain.  A full ring keeps the newest MUSB_RING_ENTRIES records and
  counts what it lost, because losing the newest would hide the end of the
  window, which is the part a reader needs.

  @param[in] Tag  Which event, one of MUSB_RING_TAG.
  @param[in] A    First payload word.
  @param[in] B    Second payload word.
**/
STATIC
VOID
MusbRingAdd (
  IN UINT16  Tag,
  IN UINT32  A,
  IN UINT32  B
  )
{
  MUSB_RING_ENTRY  *Entry;

  Entry       = &mRing[mRingNext];
  Entry->Tick = mPollTicks;
  Entry->Tag  = Tag;
  Entry->A    = A;
  Entry->B    = B;

  mRingNext = (mRingNext + 1) % MUSB_RING_ENTRIES;
  if (mRingCount < MUSB_RING_ENTRIES) {
    mRingCount++;
  } else {
    mRingLost++;
  }
}

/**
  Record that a request was stalled, and ask for the ring to be printed.

  A stall ends the transfer, so nothing on the bus is waiting for that answer any
  more and the window can be closed.  It is closed from the poll - after the
  stall has gone out and the rest of the tick has been serviced - so the ~8 ms
  lines never run inside the request handler that produced them.

  @param[in] Answer  Which stall, one of the MUSB_ANSWER stall codes.
  @param[in] Detail  What the drain should print next to it.
**/
STATIC
VOID
MusbStall (
  IN UINT32  Answer,
  IN UINT32  Detail
  )
{
  MusbRingAdd (MusbTagAnswer, Answer, Detail);
  mStallPending = TRUE;
}

/**
  Name what the hot path decided for a SETUP.

  @param[in] Answer  One of MUSB_ANSWER.

  @return A constant string.
**/
STATIC
CONST CHAR8 *
MusbAnswerName (
  IN UINT32  Answer
  )
{
  switch (Answer) {
    case MusbAnswerDataIn:
      return "data-IN";
    case MusbAnswerStallCount:
      return "STALL, the EP0 packet is not a SETUP";
    case MusbAnswerStallRefused:
      return "STALL, nothing here answers this request";
    case MusbAnswerStallOutData:
      return "STALL, no request here has an OUT data stage";
    case MusbAnswerStallType:
      return "STALL, only standard requests are served";
    case MusbAnswerStallConfig:
      return "STALL, no such configuration";
    case MusbAnswerStallFeature:
      return "STALL, no such feature";
    case MusbAnswerStallInterface:
      return "STALL, one interface at alternate 0 only";
    case MusbAnswerStallRequest:
      return "STALL, request not implemented";
    case MusbAnswerStatusArmed:
    default:
      return "status stage armed";
  }
}

/**
  Print what the quiet window recorded, oldest first, and empty the ring.

  One line per event, with the poll tick as the timestamp: the SETUP line
  carries all five request fields and the decision the hot path made for it, so
  a control transfer reads as one line per request.  This is where the ~8 ms
  lines are allowed to happen - the caller has already left the quiet window -
  and it is why the record is fixed size rather than a formatted string.

  @param[in] Why  What closed the window, for the header line.
**/
STATIC
VOID
MusbRingDrain (
  IN CONST CHAR8  *Why
  )
{
  MUSB_RING_ENTRY  *Entry;
  MUSB_RING_ENTRY  *Answer;
  UINT32           Index;
  UINT32           Start;
  UINT8            RequestType;
  UINT8            Request;

  DEBUG ((
    DEBUG_INFO,
    "Musb: EP0 trace, %u record(s)%a, closed: %a (t = poll tick, 100 us each; device %04x:%04x)\n",
    mRingCount,
    (mRingLost != 0) ? ", older records lost to a full ring" : "",
    Why,
    (mDeviceDescriptor != NULL) ? mDeviceDescriptor->IdVendor : 0,
    (mDeviceDescriptor != NULL) ? mDeviceDescriptor->IdProduct : 0
    ));

  //
  // With a full ring the oldest record is the one mRingNext is about to
  // overwrite; before that the oldest is slot 0.
  //
  Start = (mRingCount == MUSB_RING_ENTRIES) ? mRingNext : 0;

  for (Index = 0; Index < mRingCount; Index++) {
    Entry  = &mRing[(Start + Index) % MUSB_RING_ENTRIES];
    Answer = &mRing[(Start + Index + 1) % MUSB_RING_ENTRIES];

    switch (Entry->Tag) {
      case MusbTagSetup:
        //
        // The answer for this SETUP is the record right after it, when the hot
        // path got that far: printed on the same line and consumed here.
        //
        RequestType = (UINT8)(Entry->A & 0xFF);
        Request     = (UINT8)((Entry->A >> 8) & 0xFF);

        if (((Index + 1) < mRingCount) && (Answer->Tag == MusbTagAnswer)) {
          if (Answer->A == MusbAnswerDataIn) {
            DEBUG ((
              DEBUG_INFO,
              "Musb: [t=%u] SETUP %02x %02x %04x %04x %u (%a) -> data-IN, %u of %u bytes asked\n",
              Entry->Tick,
              (UINT32)RequestType,
              (UINT32)Request,
              (UINT32)((Entry->A >> 16) & 0xFFFF),
              (UINT32)(Entry->B & 0xFFFF),
              (UINT32)(Entry->B >> 16),
              MusbRequestName (Request),
              Answer->B,
              (UINT32)(Entry->B >> 16)
              ));
          } else if (Answer->A == MusbAnswerStallCount) {
            DEBUG ((
              DEBUG_INFO,
              "Musb: [t=%u] SETUP -> %a (COUNT0 raw 0x%04x, 8 bytes expected)\n",
              Entry->Tick,
              MusbAnswerName (Answer->A),
              Answer->B
              ));
          } else if (Answer->A == MusbAnswerStatusArmed) {
            DEBUG ((
              DEBUG_INFO,
              "Musb: [t=%u] SETUP %02x %02x %04x %04x %u (%a) -> %a, CSR0=0x%04x "
              "(dataend=%u txpktrdy=%u)\n",
              Entry->Tick,
              (UINT32)RequestType,
              (UINT32)Request,
              (UINT32)((Entry->A >> 16) & 0xFFFF),
              (UINT32)(Entry->B & 0xFFFF),
              (UINT32)(Entry->B >> 16),
              MusbRequestName (Request),
              MusbAnswerName (Answer->A),
              Answer->B,
              (UINT32)((Answer->B & A733_CSR0_DATAEND) != 0),
              (UINT32)((Answer->B & A733_CSR0_TXPKTRDY) != 0)
              ));
          } else {
            DEBUG ((
              DEBUG_INFO,
              "Musb: [t=%u] SETUP %02x %02x %04x %04x %u (%a) -> %a\n",
              Entry->Tick,
              (UINT32)RequestType,
              (UINT32)Request,
              (UINT32)((Entry->A >> 16) & 0xFFFF),
              (UINT32)(Entry->B & 0xFFFF),
              (UINT32)(Entry->B >> 16),
              MusbRequestName (Request),
              MusbAnswerName (Answer->A)
              ));
          }

          Index++;      // the answer was part of this line
        } else {
          DEBUG ((
            DEBUG_INFO,
            "Musb: [t=%u] SETUP %02x %02x %04x %04x %u (%a) -> no answer recorded\n",
            Entry->Tick,
            (UINT32)RequestType,
            (UINT32)Request,
            (UINT32)((Entry->A >> 16) & 0xFFFF),
            (UINT32)(Entry->B & 0xFFFF),
            (UINT32)(Entry->B >> 16),
            MusbRequestName (Request)
            ));
        }

        break;

      case MusbTagAnswer:
        //
        // Only reachable when a SETUP could not be read at all, so there is no
        // SETUP line to hang the answer on.
        //
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] EP0 answer: %a\n",
          Entry->Tick,
          MusbAnswerName (Entry->A)
          ));
        break;

      case MusbTagResponseShort:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] data stage %u bytes where the host asked for %u (short: ends on a "
          "short packet, normal for a descriptor shorter than wLength)\n",
          Entry->Tick,
          Entry->A,
          Entry->B
          ));
        break;

      case MusbTagResponseLong:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] data stage %u bytes where the host asked for %u (LONGER: the core "
          "would send past wLength)\n",
          Entry->Tick,
          Entry->A,
          Entry->B
          ));
        break;

      case MusbTagAddressed:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] Addressed %u (FADDR read back 0x%02x, CSR0=0x%04x)\n",
          Entry->Tick,
          Entry->A,
          Entry->B & 0xFF,
          Entry->B >> 8
          ));
        break;

      case MusbTagConfigured:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] Configured: config 1, bulk IN 0x%02x (%u B), bulk OUT 0x%02x (%u B)\n",
          Entry->Tick,
          mBulkIn.Address,
          Entry->A,
          mBulkOut.Address,
          Entry->B
          ));
        break;

      case MusbTagUnconfigured:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] Unconfigured (config 0), bulk endpoints left programmed\n",
          Entry->Tick
          ));
        break;

      case MusbTagReset:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] RESET in: FADDR=0x%02x POWER=0x%02x DEVCTL=0x%02x CSR0=0x%04x "
          "COUNT0=0x%04x\n",
          Entry->Tick,
          Entry->A & 0xFF,
          (Entry->A >> 8) & 0xFF,
          (Entry->A >> 16) & 0xFF,
          Entry->B & 0xFFFF,
          Entry->B >> 16
          ));
        break;

      case MusbTagResetDone:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] RESET out: FADDR=0x%02x POWER=0x%02x DEVCTL=0x%02x CSR0=0x%04x "
          "(EP0 idle, bulk FIFOs flushed)\n",
          Entry->Tick,
          Entry->A & 0xFF,
          (Entry->A >> 8) & 0xFF,
          (Entry->A >> 16) & 0xFF,
          Entry->B
          ));
        break;

      case MusbTagBusEvent:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] bus event INTRUSB=0x%02x\n",
          Entry->Tick,
          Entry->A
          ));
        break;

      case MusbTagSetupEndNew:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] SETUP with SETUPEND latched: CSR0 0x%04x -> 0x%04x, COUNT0=%u, "
          "RXPKTRDY %a\n",
          Entry->Tick,
          Entry->A,
          Entry->B & 0xFFFF,
          Entry->B >> 16,
          ((Entry->B & 0x0001) != 0) ? "kept" : "CLEARED by the acknowledge, request lost"
          ));
        break;

      case MusbTagSetupEndOnly:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] SETUPEND without a packet (CSR0=0x%04x): the host abandoned the "
          "transfer; acknowledged, endpoint idled\n",
          Entry->Tick,
          Entry->A
          ));
        break;

      case MusbTagRxPktFirst:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] first RXPKTRDY: CSR0=0x%04x COUNT0=0x%04x INTRRX=0x%04x - the SIE "
          "handed endpoint 0 a packet\n",
          Entry->Tick,
          Entry->A,
          Entry->B & 0xFFFF,
          Entry->B >> 16
          ));
        break;

      case MusbTagStatusCsr:
        //
        // The record that answers "did the zero-length status packet leave?":
        // CSR0 changed on its own while the status stage was armed.  DATAEND
        // going 1 -> 0 is the core having handed that packet to the host;
        // TXPKTRDY is printed for the same reason (it is clear in the armed
        // state and must stay clear).
        //
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] CSR0 changed with the status stage armed: 0x%04x -> 0x%04x "
          "(rxpktrdy %u->%u, txpktrdy %u->%u, dataend %u->%u, setupend %u->%u)\n",
          Entry->Tick,
          Entry->A,
          Entry->B,
          (UINT32)((Entry->A & A733_CSR0_RXPKTRDY) != 0),
          (UINT32)((Entry->B & A733_CSR0_RXPKTRDY) != 0),
          (UINT32)((Entry->A & A733_CSR0_TXPKTRDY) != 0),
          (UINT32)((Entry->B & A733_CSR0_TXPKTRDY) != 0),
          (UINT32)((Entry->A & A733_CSR0_DATAEND) != 0),
          (UINT32)((Entry->B & A733_CSR0_DATAEND) != 0),
          (UINT32)((Entry->A & A733_CSR0_SETUPEND) != 0),
          (UINT32)((Entry->B & A733_CSR0_SETUPEND) != 0)
          ));
        break;

      case MusbTagSpinCapped:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] EP0 RXPKTRDY still set after %u SVDRXPKTRDY polls; spin capped\n",
          Entry->Tick,
          Entry->A
          ));
        break;

      case MusbTagFaddr:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] FADDR changed 0x%02x -> 0x%02x without this driver writing it\n",
          Entry->Tick,
          Entry->A,
          Entry->B
          ));
        break;

      default:
        DEBUG ((
          DEBUG_INFO,
          "Musb: [t=%u] record tag %u A=0x%08x B=0x%08x\n",
          Entry->Tick,
          (UINT32)Entry->Tag,
          Entry->A,
          Entry->B
          ));
        break;
    }
  }

  mRingNext  = 0;
  mRingCount = 0;
  mRingLost  = 0;
}

/**
  Is the console quiet, i.e. must the EP0 path print nothing?

  @retval TRUE   A control transfer is in flight or an enumeration is running.
  @retval FALSE  Printing is free.
**/
STATIC
BOOLEAN
MusbQuiet (
  VOID
  )
{
  return mConsoleQuiet;
}

/**
  Open the quiet window: from here until MusbQuietLeave, no DEBUG may run.

  Called at the top of MusbHandleSetup, so the window covers the whole of an
  enumeration - from the first SETUP to Configured - and re-opens for the
  duration of every later control transfer.  The last SETUP's tick is kept
  whether or not the window is in force, because it is the timeout that closes a
  window no host ever finishes.

  With MUSB_ENUM_VERBOSE the window is never entered: the full trace is what was
  asked for then, and nothing is drained from the ring.
**/
STATIC
VOID
MusbQuietEnter (
  VOID
  )
{
  mRingLastSetupTick = mPollTicks;

#if !MUSB_ENUM_VERBOSE
  mConsoleQuiet = TRUE;
#endif
}

/**
  Close the quiet window and print what it recorded.

  The ring is drained here, never from the hot path: thirty lines cost a quarter
  of a second on this console, so they are printed where no control transfer is
  waiting - after the status stage that completed a request, or from the poll
  when the host has gone quiet.

  @param[in] Why  What closed the window, for the drain's header line.
**/
STATIC
VOID
MusbQuietLeave (
  IN CONST CHAR8  *Why
  )
{
  if (!mConsoleQuiet) {
    return;
  }

  mConsoleQuiet = FALSE;
  MusbRingDrain (Why);
}

/**
  Record and report one INTRUSB reading, before it is cleared.

  A bit is reported once per occurrence and never twice, because the core's
  clear is write-one-to-clear: the value read here is exactly the set of events
  that happened since the previous clear, so "newly seen" and "set in this read"
  are the same thing and no shadow state is needed to tell them apart.

  SOF is the exception on the printing side only.  At high speed it arrives
  every 125 us, i.e. on nearly every 100 us poll, and a line per occurrence
  would bury every other event - including the RESET and the SETUPs this log
  exists to show - under a wall of frame ticks.  So SOF is counted, announced
  once when it first appears (that line is the evidence the host is driving
  frames at all), and thereafter reported only in the totals.

  Called from the poll and from the bring-up's own drain of INTRUSB, so the
  events latched during the bring-up are in the tally too.

  @param[in] Intr   Raw INTRUSB byte.
  @param[in] Where  What the log should call this reading.

  @retval TRUE   At least one bit other than SOF was set, i.e. the totals line
                 is worth printing.
  @retval FALSE  Nothing was set, or only SOF.
**/
STATIC
BOOLEAN
MusbAccountBusEvents (
  IN UINT8        Intr,
  IN CONST CHAR8  *Where
  )
{
  UINTN    Bit;
  BOOLEAN  Announced;

  Announced = FALSE;

  for (Bit = 0; Bit < MUSB_INTRUSB_BITS; Bit++) {
    if ((Intr & (1U << Bit)) == 0) {
      continue;
    }

    mIntrCount[Bit]++;
    mLastEventTick = mPollTicks;

    if (Bit == (UINTN)MUSB_INTRUSB_SOF_BIT) {
      if ((mIntrCount[Bit] == 1) && !MusbQuiet ()) {
        DEBUG ((
          DEBUG_INFO,
          "Musb: bus event SOF (%a, INTRUSB=0x%02x, poll tick %u): counted from here on, "
          "not printed per occurrence\n",
          Where,
          (UINT32)Intr,
          mPollTicks
          ));
      }

      continue;
    }

    Announced = TRUE;
    if (MusbQuiet ()) {
      //
      // Inside an enumeration the byte goes to the ring, in the same place on
      // the timeline as the SETUPs and the reset it belongs with.
      //
      MusbRingAdd (MusbTagBusEvent, (UINT32)Intr, 0);
    } else {
      DEBUG ((
        DEBUG_INFO,
        "Musb: bus event %a (%a, INTRUSB=0x%02x, poll tick %u)\n",
        mMusbIntrName[Bit],
        Where,
        (UINT32)Intr,
        mPollTicks
        ));
    }
  }

  return Announced;
}

/**
  Print the running tally of bus events.

  It is printed whenever a counter other than SOF moves, which is what makes a
  quiet log readable: no totals line after the bring-up means the core latched
  nothing at all, not that the tally was never printed.  The tick the last event
  was seen at is in the line, which is what separates "no event yet" from
  "events stopped after N".
**/
STATIC
VOID
MusbLogEventTotals (
  VOID
  )
{
  DEBUG ((
    DEBUG_INFO,
    "Musb: bus event totals: SUSPEND=%u RESUME=%u RESET=%u SOF=%u CONNECT=%u "
    "DISCONNECT=%u SESSION_REQ=%u VBUS_ERROR=%u (poll ticks=%u, last event at tick %u)\n",
    mIntrCount[0],
    mIntrCount[1],
    mIntrCount[2],
    mIntrCount[3],
    mIntrCount[4],
    mIntrCount[5],
    mIntrCount[6],
    mIntrCount[7],
    mPollTicks,
    mLastEventTick
    ));
}

/**
  Fold one tick's reading of a watched register in, and say whether to report.

  A report is deferred to the tick after the change, and a burst of changes is
  collapsed into one line carrying the count and the tick range.  That is not
  cosmetic: DEBUG output here goes out synchronously on the serial port at
  every tick, so a DEVCTL or POWER bit that chattered once per tick would spend
  the whole poll inside the UART and starve endpoint 0 - the log would then
  cause the failure it exists to explain.  A burst that never settles is still
  reported once per MUSB_WATCH_REPORT_TICKS, with its count, so continuous
  churn cannot hide either.

  @param[in,out] Watch  The register's watch state.
  @param[in]     Value  The value read this tick.

  @retval TRUE   Report now (and set Watch->Previous = Watch->Value,
                 Watch->Changes = 0 afterwards).
  @retval FALSE  Nothing to report.
**/
STATIC
BOOLEAN
MusbWatchTick (
  IN OUT MUSB_WATCH  *Watch,
  IN     UINT8       Value
  )
{
  if (Value != Watch->Value) {
    if (Watch->Changes == 0) {
      Watch->FirstTick = mPollTicks;
    }

    Watch->Changes++;
    Watch->LastTick = mPollTicks;
    Watch->Value    = Value;
  }

  if (Watch->Changes == 0) {
    return FALSE;
  }

  if (Watch->LastTick != mPollTicks) {
    return TRUE;
  }

  return (BOOLEAN)((mPollTicks - Watch->FirstTick) >= MUSB_WATCH_REPORT_TICKS);
}

/**
  Compare POWER, DEVCTL and FADDR with the previous tick and report any change.

  This is the line that shows a pull-up drop (POWER.SOFTCONN 1 -> 0), a role or
  session change (DEVCTL), or an address the core took without this driver
  writing it (FADDR) - the three ways the core can leave a state the rest of the
  log would not mention.  The first tick after the bring-up takes a baseline
  instead, so the pre-wait state is on record even when nothing ever changes.

  Reads only: no register is written here, and none of these three is
  endpoint-indexed, so the call cannot disturb an endpoint state machine.
**/
STATIC
VOID
MusbStateWatch (
  VOID
  )
{
  UINT8  Power;
  UINT8  Devctl;
  UINT8  Faddr;
  UINT8  Diff;
  CHAR8  Names[64];

  Power  = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
  Devctl = MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG));
  Faddr  = MmioRead8 (MusbReg (A733_MUSB_FADDR_REG));

  if (!mWatchValid) {
    mWatchValid = TRUE;

    ZeroMem (&mWatchPower, sizeof (mWatchPower));
    ZeroMem (&mWatchDevctl, sizeof (mWatchDevctl));
    ZeroMem (&mWatchFaddr, sizeof (mWatchFaddr));
    mWatchPower.Previous  = Power;
    mWatchPower.Value     = Power;
    mWatchDevctl.Previous = Devctl;
    mWatchDevctl.Value    = Devctl;
    mWatchFaddr.Previous  = Faddr;
    mWatchFaddr.Value     = Faddr;

    if (!MusbQuiet ()) {
      DEBUG ((
        DEBUG_INFO,
        "Musb: watch baseline at poll tick %u: POWER=0x%02x (softconn=%u hsenab=%u hsmode=%u) "
        "DEVCTL=0x%02x (%a-device hostmode=%u session=%u vbus=%a) FADDR=%u; changes from "
        "here on are printed at the tick after they settle\n",
        mPollTicks,
        (UINT32)Power,
        (UINT32)((Power & A733_MUSB_POWER_SOFTCONN) != 0),
        (UINT32)((Power & A733_MUSB_POWER_HSENAB) != 0),
        (UINT32)((Power & A733_MUSB_POWER_HSMODE) != 0),
        (UINT32)Devctl,
        ((Devctl & A733_MUSB_DEVCTL_BDEVICE) != 0) ? "B" : "A",
        (UINT32)((Devctl & A733_MUSB_DEVCTL_HOSTMODE) != 0),
        (UINT32)((Devctl & A733_MUSB_DEVCTL_SESSION) != 0),
        mMusbVbusState[((Devctl >> A733_MUSB_DEVCTL_VBUS_SHIFT) & A733_MUSB_DEVCTL_VBUS_MASK)],
        (UINT32)(Faddr & A733_MUSB_FADDR_MASK)
        ));
    }
    return;
  }

  if (MusbQuiet ()) {
    //
    // Inside an enumeration no line may cost a control transfer 8 ms, so the
    // readings are still folded in - the first report after the window then
    // carries the value in force - but nothing is printed and no burst is
    // accumulated across the window.  The one reading here that is evidence
    // worth keeping is FADDR: an address the core took on its own goes to the
    // ring, where the SETUPs it belongs with are.
    //
    (VOID)MusbWatchTick (&mWatchPower, Power);
    (VOID)MusbWatchTick (&mWatchDevctl, Devctl);
    (VOID)MusbWatchTick (&mWatchFaddr, Faddr);
    if (mWatchFaddr.Value != mWatchFaddr.Previous) {
      MusbRingAdd (MusbTagFaddr, (UINT32)mWatchFaddr.Previous, (UINT32)mWatchFaddr.Value);
    }

    mWatchPower.Previous  = mWatchPower.Value;
    mWatchPower.Changes   = 0;
    mWatchDevctl.Previous = mWatchDevctl.Value;
    mWatchDevctl.Changes  = 0;
    mWatchFaddr.Previous  = mWatchFaddr.Value;
    mWatchFaddr.Changes   = 0;
    return;
  }

  if (MusbWatchTick (&mWatchPower, Power)) {
    Diff = (UINT8)(mWatchPower.Previous ^ mWatchPower.Value);
    MusbDiffNames (Diff, mMusbPowerName, 8, Names, sizeof (Names));
    DEBUG ((
      DEBUG_INFO,
      "Musb: watch POWER 0x%02x -> 0x%02x (diff 0x%02x: %a) reported at tick %u, "
      "%u change(s) over tick(s) %u---%u; now softconn=%u hsenab=%u suspend=%u "
      "hsmode=%u resume=%u ensuspend=%u\n",
      (UINT32)mWatchPower.Previous,
      (UINT32)mWatchPower.Value,
      (UINT32)Diff,
      Names,
      mPollTicks,
      mWatchPower.Changes,
      mWatchPower.FirstTick,
      mWatchPower.LastTick,
      (UINT32)((mWatchPower.Value & A733_MUSB_POWER_SOFTCONN) != 0),
      (UINT32)((mWatchPower.Value & A733_MUSB_POWER_HSENAB) != 0),
      (UINT32)((mWatchPower.Value & A733_MUSB_POWER_SUSPEND) != 0),
      (UINT32)((mWatchPower.Value & A733_MUSB_POWER_HSMODE) != 0),
      (UINT32)((mWatchPower.Value & A733_MUSB_POWER_RESUME) != 0),
      (UINT32)((mWatchPower.Value & A733_MUSB_POWER_ENSUSPEND) != 0)
      ));
    mWatchPower.Previous = mWatchPower.Value;
    mWatchPower.Changes  = 0;
  }

  if (MusbWatchTick (&mWatchDevctl, Devctl)) {
    Diff = (UINT8)(mWatchDevctl.Previous ^ mWatchDevctl.Value);
    MusbDiffNames (Diff, mMusbDevctlName, 8, Names, sizeof (Names));
    DEBUG ((
      DEBUG_INFO,
      "Musb: watch DEVCTL 0x%02x -> 0x%02x (diff 0x%02x: %a) reported at tick %u, "
      "%u change(s) over tick(s) %u---%u; now %a-device hostmode=%u hostreq=%u "
      "session=%u vbus=%a fsdev=%u lsdev=%u\n",
      (UINT32)mWatchDevctl.Previous,
      (UINT32)mWatchDevctl.Value,
      (UINT32)Diff,
      Names,
      mPollTicks,
      mWatchDevctl.Changes,
      mWatchDevctl.FirstTick,
      mWatchDevctl.LastTick,
      ((mWatchDevctl.Value & A733_MUSB_DEVCTL_BDEVICE) != 0) ? "B" : "A",
      (UINT32)((mWatchDevctl.Value & A733_MUSB_DEVCTL_HOSTMODE) != 0),
      (UINT32)((mWatchDevctl.Value & A733_MUSB_DEVCTL_HOSTREQ) != 0),
      (UINT32)((mWatchDevctl.Value & A733_MUSB_DEVCTL_SESSION) != 0),
      mMusbVbusState[((mWatchDevctl.Value >> A733_MUSB_DEVCTL_VBUS_SHIFT)
                     & A733_MUSB_DEVCTL_VBUS_MASK)],
      (UINT32)((mWatchDevctl.Value & A733_MUSB_DEVCTL_FSDEV) != 0),
      (UINT32)((mWatchDevctl.Value & A733_MUSB_DEVCTL_LSDEV) != 0)
      ));
    mWatchDevctl.Previous = mWatchDevctl.Value;
    mWatchDevctl.Changes  = 0;
  }

  if (MusbWatchTick (&mWatchFaddr, Faddr)) {
    Diff = (UINT8)(mWatchFaddr.Previous ^ mWatchFaddr.Value);
    MusbDiffNames (Diff, mMusbFaddrName, 8, Names, sizeof (Names));
    DEBUG ((
      DEBUG_INFO,
      "Musb: watch FADDR 0x%02x -> 0x%02x (diff 0x%02x: %a) reported at tick %u, "
      "%u change(s) over tick(s) %u---%u; address %u -> %u\n",
      (UINT32)mWatchFaddr.Previous,
      (UINT32)mWatchFaddr.Value,
      (UINT32)Diff,
      Names,
      mPollTicks,
      mWatchFaddr.Changes,
      mWatchFaddr.FirstTick,
      mWatchFaddr.LastTick,
      (UINT32)(mWatchFaddr.Previous & A733_MUSB_FADDR_MASK),
      (UINT32)(mWatchFaddr.Value & A733_MUSB_FADDR_MASK)
      ));
    mWatchFaddr.Previous = mWatchFaddr.Value;
    mWatchFaddr.Changes  = 0;
  }
}

/**
  One line every MUSB_HEARTBEAT_TICKS polls, whether or not anything happened.

  Without it a silent log cannot distinguish "the bus is quiet" from "the timer
  stopped", and the whole point of the event log is that a quiet log is
  evidence.  Reads only.

  @param[in] Tag  Why this reading was taken (the scheduled heartbeat, or the
                  last one before the timer is cancelled).
**/
STATIC
VOID
MusbLogHeartbeat (
  IN CONST CHAR8  *Tag
  )
{
  UINT8  Power;
  UINT8  Devctl;

  Power  = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
  Devctl = MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG));
  MusbSelectEp (0);       // the EP0 registers below are indexed

  DEBUG ((
    DEBUG_INFO,
    "Musb: poll heartbeat %a at tick %u: POWER=0x%02x (softconn=%u hsmode=%u) "
    "DEVCTL=0x%02x (hostmode=%u session=%u vbus=%a) FADDR=%u INTRUSB=0x%02x "
    "INTRUSBE=0x%02x; RESET seen %u time(s), SOF %u, last event at tick %u; "
    "EP0 CSR0=0x%04x COUNT0=0x%04x RXPKTRDY on %u tick(s), INTRRX=0x%04x "
    "(EP0 RX latch=%u) INTRTX=0x%04x\n",
    Tag,
    mPollTicks,
    (UINT32)Power,
    (UINT32)((Power & A733_MUSB_POWER_SOFTCONN) != 0),
    (UINT32)((Power & A733_MUSB_POWER_HSMODE) != 0),
    (UINT32)Devctl,
    (UINT32)((Devctl & A733_MUSB_DEVCTL_HOSTMODE) != 0),
    (UINT32)((Devctl & A733_MUSB_DEVCTL_SESSION) != 0),
    mMusbVbusState[((Devctl >> A733_MUSB_DEVCTL_VBUS_SHIFT) & A733_MUSB_DEVCTL_VBUS_MASK)],
    (UINT32)(MmioRead8 (MusbReg (A733_MUSB_FADDR_REG)) & A733_MUSB_FADDR_MASK),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSB_REG)),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSBE_REG)),
    mIntrCount[(UINTN)MUSB_INTRUSB_RESET_BIT],
    mIntrCount[(UINTN)MUSB_INTRUSB_SOF_BIT],
    mLastEventTick,
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)),
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG)),
    mEp0RxPktTicks,
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG)),
    (UINT32)((MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG)) & BIT0) != 0),
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRTX_REG))
    ));
}

/**
  Step 4 of the bring-up: open the OTG control the device side needs.

  One read-modify-write of the wrapper's ISCR, which is the whole of what makes
  the core a visible peripheral on this board.  Field by field:

    bits 17:16  ID and D+/D- pull-up enables.  The manual's U2P0_ISCR table
                (18.4.5.1) prints both as reserved; they are the pull-up
                enables in the vendor register model, and the device-mode driver
                that enumerates on this board sets both in this same write.
                DPDM_PULLUP_EN is the physical precondition for a host to see a
                device at all - without it the D+ line stays down and nothing
                enumerates, whatever the rest of this byte says.
    bits 15:14  FORCE_ID = 0b11: there is no ID pin on a USB-C receptacle, so
                the ID input is forced high, i.e. B-side peripheral.
    bits 13:12  FORCE_VBUS = 0b11: VBUS forced valid (manual: "Force VBUS Valid
                (For SIE)").  The reference writes exactly this and enumerates,
                so it is the whole of this driver's VBUS story in ISCR.
    bits 7:4    the change-detect wake latches, cleared.  An earlier version of
                this function wrote the latched bits back (the manual calls the
                field write-one-to-clear) and left bits 11:10 - the VBUS source
                select - pointed at the DP/DM line state as well as the pad.
                Both are gone: the reference clears the latch field and never
                touches the source select, and the port's byte is now the same
                one that enumerates.

  Read-back is printed with every field decoded, so the log says whether the
  pull-ups, the two force fields and the latches are as intended.  (Bit 29/30 of
  that same read - VBUS valid from the pad, and from DP/DM - are printed by
  MusbLogVbusPath; they are status and are not written here.)
**/
STATIC
VOID
MusbIscrOn (
  VOID
  )
{
  UINT32  Before;
  UINT32  Value;
  UINT32  Latched;
  UINT32  After;

  Before  = MmioRead32 (MusbReg (A733_MUSB_ISCR_REG));
  Latched = Before & A733_MUSB_ISCR_WAKE_MASK;

  Value  = Before & ~(UINT32)A733_MUSB_ISCR_WAKE_MASK;   // reference clears the field
  Value |= A733_MUSB_ISCR_DPDM_PULLUP_EN | A733_MUSB_ISCR_ID_PULLUP_EN;
  Value &= ~(UINT32)(A733_MUSB_ISCR_FORCE_MASK << A733_MUSB_ISCR_ID_FORCE_SHIFT);
  Value |= (UINT32)(A733_MUSB_ISCR_FORCE_MASK << A733_MUSB_ISCR_ID_FORCE_SHIFT);
  Value &= ~(UINT32)(A733_MUSB_ISCR_FORCE_MASK << A733_MUSB_ISCR_VBUS_FORCE_SHIFT);
  Value |= (UINT32)(A733_MUSB_ISCR_FORCE_MASK << A733_MUSB_ISCR_VBUS_FORCE_SHIFT);

  MmioWrite32 (MusbReg (A733_MUSB_ISCR_REG), Value);
  After = MmioRead32 (MusbReg (A733_MUSB_ISCR_REG));

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 4 ISCR 0x%08x -> wrote 0x%08x, read back 0x%08x "
    "(dpdm_pullup=%u id_pullup=%u force_id=0x%x force_vbus=0x%x src=0x%x "
    "latches_now=0x%x had=0x%x)\n",
    Before,
    Value,
    After,
    (UINT32)((After & A733_MUSB_ISCR_DPDM_PULLUP_EN) != 0),
    (UINT32)((After & A733_MUSB_ISCR_ID_PULLUP_EN) != 0),
    (UINT32)((After >> A733_MUSB_ISCR_ID_FORCE_SHIFT) & A733_MUSB_ISCR_FORCE_MASK),
    (UINT32)((After >> A733_MUSB_ISCR_VBUS_FORCE_SHIFT) & A733_MUSB_ISCR_FORCE_MASK),
    (UINT32)((After >> A733_MUSB_ISCR_VBUS_SRC_SHIFT) & A733_MUSB_ISCR_VBUS_SRC_MASK),
    (UINT32)(After & A733_MUSB_ISCR_WAKE_MASK),
    (UINT32)Latched
    ));

  if (((After & A733_MUSB_ISCR_DPDM_PULLUP_EN) == 0) ||
      ((After & A733_MUSB_ISCR_ID_PULLUP_EN) == 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: an ISCR pull-up enable did not read back (0x%08x): the core is not "
      "driving D+, so no host can see this device\n",
      After
      ));
  }

  MusbLogVbusPath ("step 4: after the OTG control write");
}

/**
  Step 9 of the bring-up: start from a known engine state.

  Every byte this driver moves goes through the FIFO ports, so leaving the core's
  own engine out of the picture is not optional: a previous stage (the flashing
  path, or a boot that ran the vendor stack) can leave a channel armed for the
  bulk OUT endpoint, and with a stale channel bound to it the core takes the host's
  packet out of the FIFO and clears RXPKTRDY by itself - the host sees its ACK and
  nothing ever arrives at the transport, the "second command never lands" symptom.
  The comparison driver says the same and turns the engine off in its own bring-up;
  here every channel's gate is cleared and both global words are read back.

  Why nothing arms a channel afterwards, and what it would take.  The engine the
  vendor stack drives is not instantiated on this die: its window (0x500/0x504,
  per-channel 0x540 + 0x10n) reads back zero after being written, and a one-off
  scan of the whole 4 KB page agreed - 40 non-zero words, nothing DMA-shaped, the
  Mentor HSDMA layout's 0x200..0x3ff entirely zero, and the page decoding as 2 KB
  mirrored (0x800.. mirrors 0x000.., 0xc00.. mirrors 0x400..).  A733Musb.h keeps
  the vendor's register model for the record.  The manual's other answer is the
  SoC's DMAC (13.1.4.1 table 13-2: ports 27..34 = USB0_DEVICE_EP1..EP8), and
  A733DmacLib is a complete driver for it - but both DMACs are secure: SEC reads
  0x00000000 where the manual's reset value is 0x0000ffff, and AUTO_GATE and STA
  read zero with it, so normal-world software cannot program a transfer at all.
  Reviving the path is therefore a BL31 change, not a change here: with the
  security word cleared, one channel armed per packet into a ring slot would drain
  the FIFO for the CPU (the arm order and the hazard of a short packet are in
  A733Dmac.h), and it would remove the ~24 us of FIFO reads from a ~166 us packet.

  DMA_STATUS is one bit per channel and write-1-to-clear, so 0xff drains every
  latched completion rather than leaving one to look like a fresh event.
**/
STATIC
VOID
MusbDmaOff (
  VOID
  )
{
  UINT32  Enable;
  UINT32  Status;

  MmioWrite32 (MusbReg (A733_MUSB_DMA_ENABLE_REG), 0);
  MmioWrite32 (MusbReg (A733_MUSB_DMA_STATUS_REG), 0xFF);

  Enable = MmioRead32 (MusbReg (A733_MUSB_DMA_ENABLE_REG));
  Status = MmioRead32 (MusbReg (A733_MUSB_DMA_STATUS_REG));

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 9 core DMA engine: ENABLE wrote 0x00000000 read back 0x%08x, "
    "STATUS wrote 0x000000ff read back 0x%08x (no channel is armed by this driver; "
    "every payload byte is read from the FIFO ports)\n",
    Enable,
    Status
    ));

  if (Enable != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: the core's DMA engine reads back 0x%08x instead of 0 - a stale channel "
      "can take bulk OUT out of the FIFO and clear RXPKTRDY behind this driver\n",
      Enable
      ));
  }
}

/**
  Push bytes into an endpoint's FIFO.

  The port is 32 bits wide; the tail is written a byte at a time, which the
  core accepts because the FIFO is a byte stream.
**/
STATIC
VOID
MusbFifoWrite (
  IN UINT8        Ep,
  IN UINTN        Length,
  IN CONST UINT8  *Buffer
  )
{
  UINTN  Port  = MusbReg (A733_MUSB_FIFO_PORT (Ep));
  UINTN  Index = 0;
  UINT32 Word;

  while ((Length - Index) >= 4) {
    CopyMem (&Word, Buffer + Index, sizeof (Word));
    MmioWrite32 (Port, Word);
    Index += 4;
  }

  while (Index < Length) {
    MmioWrite8 (Port, Buffer[Index]);
    Index++;
  }
}

/**
  Pull bytes out of an endpoint's FIFO.

  With MUSB_RX_TIMING the bulk OUT endpoint's reads are timed here rather than
  around the call, so the interval is the read loop alone: no INDEX select, no
  RXCOUNT, no publish.  Every caller's read is a whole chunk - one packet in the
  caller's accounting - and the handler takes the sum on its way out.
**/
STATIC
VOID
MusbFifoRead (
  IN  UINT8  Ep,
  IN  UINTN  Length,
  OUT UINT8  *Buffer
  )
{
  UINTN  Port  = MusbReg (A733_MUSB_FIFO_PORT (Ep));
  UINTN  Index = 0;
  UINT32 Word;
#if MUSB_RX_TIMING
  BOOLEAN  Timed;
#endif

#if MUSB_RX_TIMING
  Timed = (BOOLEAN)((mBulkOut.Number != 0) && (Ep == mBulkOut.Number));
  if (Timed) {
    mRxTimingFifoStart = GetPerformanceCounter ();
  }
#endif

  while ((Length - Index) >= 4) {
    Word = MmioRead32 (Port);
    CopyMem (Buffer + Index, &Word, sizeof (Word));
    Index += 4;
  }

  while (Index < Length) {
    Buffer[Index] = MmioRead8 (Port);
    Index++;
  }

#if MUSB_RX_TIMING
  if (Timed) {
    mRxTimingFifoRun += GetPerformanceCounter () - mRxTimingFifoStart;
  }
#endif
}

/**
  Step 2 of the bring-up: write the FIFO-mode field of the SRAMC word that the
  vendor USB stack uses to hand the FIFO SRAM to the device controller.

  Bits 1:0 of the word at 0x03000004 are a two-bit mode field and 0b01 is the
  USB FIFO mode (the whole word is logged precisely because the manual does not
  document it - see A733Musb.h).  Writing only bit 0, as an earlier version of
  this function did, left whatever high bit of the field was there before; the
  field is cleared and then set, which is what the vendor stack does.

  Important, and it retires an earlier reading of this log: that vendor write
  sits inside `#ifdef CONFIG_ARCH_SUN3IW1P1` in usbc.c's
  USBC_ConfigFIFO_Base(), so it is conditional on an older SoC and does not run
  on A733 at all.  The register is therefore not where this die's FIFO RAM is
  chosen, and its read-back is not evidence either way - a field that does not
  stick means "not this SoC's mechanism", not "no packet can be transferred".
  The write stays because it has been harmless on every boot and keeps the log
  comparable with earlier ones; the window dump below stays because an all-zero
  window would still mean the block does not answer on this bus.
**/
STATIC
VOID
MusbSramFifoOn (
  VOID
  )
{
  UINT32  Before;
  UINT32  Written;
  UINT32  After;

  Before  = MmioRead32 ((UINTN)A733_SRAMC_BASE + A733_SRAMC_USB_FIFO);
  Written = Before & ~(UINT32)A733_SRAMC_USB_FIFO_MODE_MASK;
  Written |= A733_SRAMC_USB_FIFO_ON;
  MmioWrite32 ((UINTN)A733_SRAMC_BASE + A733_SRAMC_USB_FIFO, Written);
  After = MmioRead32 ((UINTN)A733_SRAMC_BASE + A733_SRAMC_USB_FIFO);

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 2 SRAMC USB FIFO 0x%08x -> wrote 0x%08x, read back 0x%08x (mode=%u)\n",
    Before,
    Written,
    After,
    (UINT32)(After & A733_SRAMC_USB_FIFO_MODE_MASK)
    ));

  //
  // The whole head of the window, because a word that does not read back is
  // two very different things: a write-only/self-clearing field (the FIFO mode
  // is one - the vendor code writes it and never reads it), or a block that
  // does not answer at all.  Read only, and nothing below waits on them.
  //
  DEBUG ((
    DEBUG_INFO,
    "Musb: step 2 SRAMC window 0x%08x: +00=0x%08x +04=0x%08x +08=0x%08x +0c=0x%08x "
    "+10=0x%08x\n",
    (UINTN)A733_SRAMC_BASE,
    MmioRead32 ((UINTN)A733_SRAMC_BASE + 0x00),
    MmioRead32 ((UINTN)A733_SRAMC_BASE + 0x04),
    MmioRead32 ((UINTN)A733_SRAMC_BASE + 0x08),
    MmioRead32 ((UINTN)A733_SRAMC_BASE + 0x0C),
    MmioRead32 ((UINTN)A733_SRAMC_BASE + 0x10)
    ));

  if ((After & A733_SRAMC_USB_FIFO_MODE_MASK) != (UINT32)A733_SRAMC_USB_FIFO_ON) {
    DEBUG ((
      DEBUG_INFO,
      "Musb: step 2 SRAMC mode field does not read back (0x%08x); that is expected on A733 - "
      "the vendor writes this word only for sun3iw1p1 (#ifdef CONFIG_ARCH_SUN3IW1P1), so it is "
      "not the FIFO RAM select on this die and says nothing about whether a packet can land. "
      "The EP0 packet path is visible directly in the CSR0 watch from step 7 onwards\n",
      After
      ));
  }
}

/**
  Step 1 of the bring-up: the device half of USB0's clock and reset word.

  Two writes, in the reference's order:

    USB0_CLK |= PHY_RSTN (bit 30).  The module gate (bit 31) and PHY_RSTN are
    already set by step 0 - this repeats only the release, because the reference
    does and because it costs one read-modify-write to be sure the PHY is out of
    reset before its bus clock is ungated.

    USB0_BGR: the device bus gate (bit 8) opened, then the device block reset
    (bit 24) pulsed low for 10 us and released.  The pulse is what makes the
    core start from its documented reset state whatever FES or a previous boot
    left in it - this is the point at which this driver takes the core over.

  The host half's bits (0, 4, 16 and 20) are untouched: they were opened in step
  0 and stay open.  Nothing in this file closes them again - see MusbPhyUp.
**/
STATIC
EFI_STATUS
MusbCcuOn (
  VOID
  )
{
  UINT32  Bgr;
  UINT32  Clk;

  MmioOr32 (CcuReg (A733_CCU_USB0_CLK), A733_CCU_USB_CLK_PHY_RSTN);

  Bgr  = MmioRead32 (CcuReg (A733_CCU_USB0_BGR));
  Bgr |= A733_CCU_USB_BGR_DEVICE_GATE;
  Bgr &= ~(UINT32)A733_CCU_USB_BGR_DEVICE_RST;
  MmioWrite32 (CcuReg (A733_CCU_USB0_BGR), Bgr);
  NanoSecondDelay (10000);
  Bgr |= A733_CCU_USB_BGR_DEVICE_RST;
  MmioWrite32 (CcuReg (A733_CCU_USB0_BGR), Bgr);

  Clk = MmioRead32 (CcuReg (A733_CCU_USB0_CLK));
  Bgr = MmioRead32 (CcuReg (A733_CCU_USB0_BGR));

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 1 CCU USB0_CLK=0x%08x (clken=%u phy_rstn=%u) "
    "USB0_BGR=0x%08x (device_gate=%u device_rst=%u ehci_gate=%u ehci_rst=%u)\n",
    Clk,
    (UINT32)((Clk & A733_CCU_USB_CLK_MODULE_EN) != 0),
    (UINT32)((Clk & A733_CCU_USB_CLK_PHY_RSTN) != 0),
    Bgr,
    (UINT32)((Bgr & A733_CCU_USB_BGR_DEVICE_GATE) != 0),
    (UINT32)((Bgr & A733_CCU_USB_BGR_DEVICE_RST) != 0),
    (UINT32)((Bgr & A733_CCU_USB_BGR_EHCI_GATE) != 0),
    (UINT32)((Bgr & A733_CCU_USB_BGR_EHCI_RST) != 0)
    ));

  return EFI_SUCCESS;
}

/**
  Step 0a: PHY0 powered and programmed, with the pads still on the host half.

  This is the part of USB0 the device half cannot reach on its own.  PHY0's
  register window at 0x04101800 belongs to the EHCI/OHCI host half of the port
  (the device tree's usbphy "pmu0"), and it only answers once USB0's host half is
  clocked - so the route select is pointed at the host half, the PHY is taken out
  of reset, programmed, and released, and then the route is left alone until
  MusbPhyUp hands the pads to the MUSB core.  Everything here is the sequence
  the A733 host firmware runs before it registers its host controllers, in its
  order:

    - route select (0x04100420): bit 0 cleared - PHY0 to the host half, which is
      what has to own the window while it is programmed;
    - PHY_RSTN asserted (CCU USB0_CLK bit 30), held 200 us, so the four writes
      below land in a PHY that is not yet running;
    - HCI +0x808: FORCE_SUSPEND cleared, or the line receiver stays parked;
    - HCI +0x810: SIDDQ cleared - this is the write that powers the analog half;
    - HCI +0x800: the passby / AHB-master word, A733_USB_HCI_PASSBY_VALUE;
    - HCI +0x818: the PHY tune word; HCI +0x81C: the UTMI control word;
    - PHY_RSTN released, 1 ms for the PLL to lock;
    - SYSCFG +0x160 bit 10 (the res200 trim select) and +0x168 bits 15:8
      cleared, so the resistance calibration drives the trim instead of the
      manual's default;
    - the USB2 PHY's own port-0 words at 0x06B00000 - PHY_CTRL with SIDDQ clear
      and the host tune, PHYTUNE, and ISCR's FORCE_VBUS_VALID (bits 13:12).  A
      PHY still in SIDDQ has no UTMI clock to give whatever else is programmed,
      and this block is the only place that switch lives.

  What is deliberately not here: nothing writes USB1's PHY (0x04201800) or its
  HCI window.  Both belong to A733UsbHostDxe, which brings that port up for the
  hub before this driver runs, and a second reset of it from here would drop the
  hub's connection.

  Every write is followed by the read-back of the word it produced, so a window
  that does not answer is one line and not a silent failure.
**/
STATIC
VOID
MusbPhy0HostHalfUp (
  VOID
  )
{
  UINT32  Value;

  //
  // Route PHY0 to the host half for as long as the window is being programmed.
  //
  Value = MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG));
  DEBUG ((
    DEBUG_INFO,
    "Musb: step 0a route select 0x%08x -> clearing bit 0 (PHY0 to the host half)\n",
    Value
    ));
  Value &= ~(UINT32)BIT0;
  MmioWrite32 (MusbReg (A733_MUSB_ROUTE_REG), Value);

  //
  // PHY_RSTN asserted, and the window written while the PHY is held.
  //
  MmioAnd32 (CcuReg (A733_CCU_USB0_CLK), ~(UINT32)A733_CCU_USB_CLK_PHY_RSTN);
  MicroSecondDelay (200);

  Value = MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_CTRL3);
  MmioWrite32 (
    A733_USB0_HCI_BASE + A733_USB_HCI_CTRL3,
    Value & ~(UINT32)A733_USB_HCI_CTRL3_FORCE_SUSPEND
    );

  Value = MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_PHY_CTRL);
  MmioWrite32 (
    A733_USB0_HCI_BASE + A733_USB_HCI_PHY_CTRL,
    Value & ~(UINT32)A733_USB_HCI_PHY_SIDDQ
    );

  Value = MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL);
  MmioWrite32 (
    A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL,
    Value | A733_USB_HCI_PASSBY_VALUE
    );

  MmioWrite32 (A733_USB0_HCI_BASE + A733_USB_HCI_PHY_TUNE, A733_USB_HCI_PHY_TUNE_VALUE);
  MmioWrite32 (A733_USB0_HCI_BASE + A733_USB_HCI_UTMI_CTRL, A733_USB_HCI_UTMI_CTRL_VALUE);

  MmioOr32 (CcuReg (A733_CCU_USB0_CLK), A733_CCU_USB_CLK_PHY_RSTN);
  MicroSecondDelay (1000);

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 0a PHY0 up on the host half: CLK=0x%08x BGR=0x%08x "
    "HCI +800=0x%08x +808=0x%08x +810=0x%08x +818=0x%08x +81c=0x%08x +824=0x%08x\n",
    MmioRead32 (CcuReg (A733_CCU_USB0_CLK)),
    MmioRead32 (CcuReg (A733_CCU_USB0_BGR)),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_CTRL3),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_PHY_CTRL),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_PHY_TUNE),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_UTMI_CTRL),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_UTMI_STAT)
    ));

  //
  // Deliberately NOT here: the SYSCFG rescal/trim words and the USB2 PHY's own
  // port-0 group (PHY_CTRL, PHYTUNE, ISCR force) at 0x06B00000.  That block is
  // the DWC3's U2P0 femtoPHY - the hub's port, owned end to end by
  // A733Xhci2Dxe - and rewriting it here lands in the middle of that driver's
  // analog calibration: the working reference's device driver never touches
  // it, and a same-value rewrite of PHYTUNE after the calibration engine has
  // started shows up as the USB2 port's high-speed chirp failing (the port
  // falls back to full speed every boot).  PHY0's own HCI window above is a
  // different PHY on different pads; that stays.
  //

  MicroSecondDelay (10);

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 0a SYSCFG +160=0x%08x +164=0x%08x +168=0x%08x; "
    "u2phy +10=0x%08x +18=0x%08x iscr=0x%08x\n",
    MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RESCAL_CTRL_REG),
    MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RES0_CTRL_REG),
    MmioRead32 (A733_SYSCFG_BASE + A733_SYSCFG_RES1_CTRL_REG),
    MmioRead32 (A733_USB2_PHY_BASE + A733_USB2_PHY_CTRL (A733_USB2_PHY_PORT_USB0)),
    MmioRead32 (A733_USB2_PHY_BASE + A733_USB2_PHY_TUNE (A733_USB2_PHY_PORT_USB0)),
    MmioRead32 (A733_USB2_PHY_BASE + A733_USB2_PHY_ISCR)
    ));
}

/**
  Step 0, part 1: the PCK600 "USB2" power domain.

  PHY0's analog half and the USB2 PHY's MMIO window at 0x06B00000 sit in this
  domain; with it off they read as zero and every write to them is dropped, so
  it comes first of all.  Two writes and a poll, in the reference's order: open
  the block's bus clock (CLK_R_PPU in the always-on R_CCU - PCK600 answers
  nothing until that gate is open), write COMMAND_ON to the domain's power
  register, then poll its status until the low nibble reads "on".

  The reference does not check whether the domain is already up before writing
  COMMAND_ON, and neither does this: a second command to a powered domain is a
  no-op on this block, and skipping the write would make the log line depend on
  what an earlier boot stage happened to leave.
**/
STATIC
VOID
MusbPowerDomainOn (
  VOID
  )
{
  UINTN   Base;
  UINTN   Tries;
  UINT32  Status;

  MmioOr32 (A733_R_CCU_BASE + A733_R_CCU_R_PPU_GATE, A733_R_CCU_R_PPU_GATE_BIT);

  Base = (UINTN)A733_PPU_BASE + (UINTN)A733_PPU_USB2_DOMAIN * (UINTN)A733_PPU_DOMAIN_STRIDE;
  MmioWrite32 (Base + A733_PPU_PWR_OFFSET, A733_PPU_COMMAND_ON);

  Status = MmioRead32 (Base + A733_PPU_STATUS_OFFSET);
  for (Tries = 0; Tries < MUSB_PPU_TRIES; Tries++) {
    if ((Status & A733_PPU_STATUS_MASK) == A733_PPU_STATUS_ON) {
      break;
    }

    MicroSecondDelay (MUSB_PPU_POLL_US);
    Status = MmioRead32 (Base + A733_PPU_STATUS_OFFSET);
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 0 power domain USB2 (PCK600 @0x%08x): R_CCU gate=0x%08x, "
    "status=0x%08x after %u poll(s)%a\n",
    (UINT32)Base,
    MmioRead32 (A733_R_CCU_BASE + A733_R_CCU_R_PPU_GATE),
    Status,
    (UINT32)Tries,
    ((Status & A733_PPU_STATUS_MASK) == A733_PPU_STATUS_ON) ? "" : " - NOT ON"
    ));
}

/**
  Step 0, part 3: the USB subsystem's CCU gates, USB0's module clock and the host
  half of USB0's bus gating/reset word.

  These are the clocks the host firmware's USB clock init opens, every one of
  them a read-modify-write so the boot chain's own bits survive:

    0x05C0  the keyed AHB master gate; USB's gate is bit 9.  The low three bits
            are the key, not gates: OR-ing the key lights six unrelated bus
            masters, so they are cleared after the OR (this is why the write is
            a read-modify-write and not a plain OR).
    0x05A4  CLK_MSI_LITE2 with its AHB and MBUS resets.
    0x1A00  CLK_RES_DCAP_24M bit 3, the 24 MHz mclk of the USB2 PHY window.
    0x1300  USB0_CLK: CLKEN (bit 31) and PHY_RSTN released (bit 30).  Releasing
            the reset here is what lets step 0a assert it again deliberately.
    0x1304  USB0_BGR host half: OHCI and EHCI gates open with both resets
            released (bits 0, 4, 16, 20).  This is what makes the HCI window a
            clocked block, and the window is where PHY0 is powered up - so it is
            a precondition for step 0a, whatever role the port then runs in.
            The device half's own gate and reset (bits 8, 24) are left to
            MusbCcuOn.
    0x00A0  the pll-peri0 output gates.  A divider whose source output is gated
            falls back to SYS_CLK24M without saying so.
    0x1340  the 24 MHz reference both PHYs' PLLs run from: gate on, mux field
            cleared (SYS_CLK24M, the reference's value).

  What is not done here: the SuperSpeed/serdes clock group at 0x1348-0x13C4 and
  USB1's own CLK/BGR words.  The first belongs to the DWC3 core, which
  A733Xhci2Dxe brings up for the board's USB2 hub; the second to
  A733UsbHostDxe.
**/
STATIC
VOID
MusbSubsysClocksOn (
  VOID
  )
{
  UINT32  Value;

  Value  = MmioRead32 (CcuReg (A733_CCU_AHB_GATE_REG));
  Value |= A733_CCU_AHB_GATE_USB_SYS;
  Value &= ~(UINT32)A733_CCU_AHB_GATE_STRAY;
  MmioWrite32 (CcuReg (A733_CCU_AHB_GATE_REG), Value);

  MmioOr32 (CcuReg (A733_CCU_MSI_LITE2_REG), A733_CCU_MSI_LITE2_ON);
  MmioOr32 (CcuReg (A733_CCU_RES_DCAP_REG), A733_CCU_RES_DCAP_USB_PHY_GATE);
  MicroSecondDelay (10);

  MmioOr32 (
    CcuReg (A733_CCU_USB0_CLK),
    A733_CCU_USB_CLK_MODULE_EN | A733_CCU_USB_CLK_PHY_RSTN
    );
  MmioOr32 (CcuReg (A733_CCU_USB0_BGR), A733_CCU_USB_BGR_HOST_HALF);

  MmioOr32 (CcuReg (A733_CCU_PLL_PERI0_REG), A733_CCU_PLL_PERI0_OUT_GATES);

  MmioOr32 (CcuReg (A733_CCU_USB01_REF_CLK), A733_CCU_USB_REF_CLK_GATE);
  MmioAnd32 (CcuReg (A733_CCU_USB01_REF_CLK), ~(UINT32)A733_CCU_USB_REF_CLK_SEL_MASK);

  MicroSecondDelay (50);

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 0 CCU: AHB=0x%08x MSI_LITE2=0x%08x RES_DCAP=0x%08x "
    "PERI0=0x%08x\n",
    MmioRead32 (CcuReg (A733_CCU_AHB_GATE_REG)),
    MmioRead32 (CcuReg (A733_CCU_MSI_LITE2_REG)),
    MmioRead32 (CcuReg (A733_CCU_RES_DCAP_REG)),
    MmioRead32 (CcuReg (A733_CCU_PLL_PERI0_REG))
    ));
  DEBUG ((
    DEBUG_INFO,
    "Musb: step 0 CCU: USB0_CLK=0x%08x USB0_BGR=0x%08x (host half: ohci=%u ehci=%u "
    "ohci_rst=%u ehci_rst=%u) USB_REF=0x%08x\n",
    MmioRead32 (CcuReg (A733_CCU_USB0_CLK)),
    MmioRead32 (CcuReg (A733_CCU_USB0_BGR)),
    (UINT32)((MmioRead32 (CcuReg (A733_CCU_USB0_BGR)) & A733_CCU_USB_BGR_OHCI_GATE) != 0),
    (UINT32)((MmioRead32 (CcuReg (A733_CCU_USB0_BGR)) & A733_CCU_USB_BGR_EHCI_GATE) != 0),
    (UINT32)((MmioRead32 (CcuReg (A733_CCU_USB0_BGR)) & A733_CCU_USB_BGR_OHCI_RST) != 0),
    (UINT32)((MmioRead32 (CcuReg (A733_CCU_USB0_BGR)) & A733_CCU_USB_BGR_EHCI_RST) != 0),
    MmioRead32 (CcuReg (A733_CCU_USB01_REF_CLK))
    ));
}

/**
  Step 0: everything USB0 needs before the MUSB core is touched.

  Three parts, in the reference's order, and all of them before the device half:

    1. MusbPowerDomainOn   - PCK600's USB2 domain, so PHY0's analog half and the
                             USB2 PHY window answer at all;
    2. MusbSubsysClocksOn  - the USB subsystem's CCU gates and USB0's host half;
    3. MusbPhy0HostHalfUp  - PHY0 itself through the HCI window.  The SYSCFG
       rescal/trim words and the USB2 PHY window (0x06B00000) are NOT touched:
       that is the DWC3 U2P0 femtoPHY's block, owned by A733Xhci2Dxe, and a
       late rewrite there breaks that port's analog calibration.

  Nothing here is undone later.  The host half stays clocked and PHY0 stays out
  of reset for the rest of the boot; only the route select moves, in MusbPhyUp,
  when the pads become the MUSB core's.  See MusbPhyUp for why that is a
  measured requirement on this port and not a preference.
**/
STATIC
VOID
MusbPlatformOn (
  VOID
  )
{
  MusbPowerDomainOn ();
  MusbSubsysClocksOn ();
  MusbPhy0HostHalfUp ();
}

/**
  The platform phase, run once: steps 0, 0a and 1 of the bring-up, i.e. what
  belongs to the board and the PHY rather than to the MUSB device core.

  This is the half the reference runs at DXE dispatch - its host driver powers
  the port and programs PHY0 before BDS starts, then its device driver comes up
  on the same pads.  This driver is one driver and does the same two phases from
  one entry point: the platform phase here, then the device half and the poll
  (MusbDeviceBringUp), both at DXE dispatch, so the port is powered, pulling D+
  up and serviced minutes before the host looks at it and before any consumer has
  called Start().

  Failure is not fatal, and there is no status to return: the steps are register
  writes whose only report is their read-back, so the two readings that say the
  platform is actually up are checked here - USB0_BGR's device gate still open,
  and the PHY window's SIDDQ still clear, which is only true if the window
  accepted step 0a's writes.  A console that shows a failure here still gets the
  device half, which is where the enumeration is decided.

  @retval TRUE   The platform reads back as up.
  @retval FALSE  It does not; the caller logs it and carries on.
**/
STATIC
BOOLEAN
MusbPlatformBringUp (
  VOID
  )
{
  UINT32  Bgr;
  UINT32  PhyCtrl;

  if (mPlatformUp) {
    return TRUE;
  }

  //
  // What the previous stage left, before this driver writes anything: SIDDQ,
  // the OTGDISABLE/DRVVBUS comparator enables, ISCR's detector verdicts and the
  // route word.  A run in which the PHY was already powered (siddq=0) differs
  // from one in which this driver powers it up, and only this line can say
  // which happened.
  //
  MusbLogVbusPath ("as found, before this driver wrote anything");

  //
  // 0, 0a. The power domain, the USB subsystem clocks, USB0's host half, the
  // resistance calibration, then PHY0's own bring-up through the HCI window.
  //
  MusbPlatformOn ();
  MusbLogCoreState ("step 0: platform up, PHY0 programmed on the host half");

  //
  // 1. The device half's clock and reset pulse.
  //
  MusbCcuOn ();
  MusbLogCoreState ("step 1: device clock gated, device reset released");

  Bgr     = MmioRead32 (CcuReg (A733_CCU_USB0_BGR));
  PhyCtrl = MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_PHY_CTRL);
  mPlatformUp = (BOOLEAN)(((Bgr & A733_CCU_USB_BGR_DEVICE_GATE) != 0) &&
                          ((PhyCtrl & A733_USB_HCI_PHY_SIDDQ) == 0));

  if (!mPlatformUp) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: platform phase did not read back as up (USB0_BGR=0x%08x device_gate=%u, "
      "PHY0 window PHY_CTRL=0x%08x siddq=%u) - carrying on to the device half anyway\n",
      Bgr,
      (UINT32)((Bgr & A733_CCU_USB_BGR_DEVICE_GATE) != 0),
      PhyCtrl,
      (UINT32)((PhyCtrl & A733_USB_HCI_PHY_SIDDQ) != 0)
      ));
  }

  return mPlatformUp;
}

/**
  Step 3 of the bring-up: PHY0 handed to the MUSB core, with the data path open.

  Three writes to the MUSB-side wrapper, and they are what the device-mode driver
  that enumerates on this board does at this point.  The wrapper's own registers
  (PHYCTL at +0x410, route select at +0x420) are inside the MUSB block itself.

  3.  Wrapper PHYCTL: SIDDQ cleared (the PHY is powered) and VBUSVLDEXT set
      (VBUS valid comes from outside this chip, which on this board is the only
      power input).  Both are manual 18.4.5.2 fields.  One read-modify-write,
      with no SIDDQ pulse before it: an earlier version asserted SIDDQ, held it
      10 ms and released it to "power-cycle the analog half", which is not what
      the reference does and is not needed - SIDDQ is already the switch that
      step 0a's window writes drove.
  3a. Wrapper route select at 0x04100420: |= BIT0 and nothing else.  This is
      where the pads become the MUSB core's, and it is the last time the route is
      written.  Earlier versions flipped it back and forth around a PHY restart;
      see below.
  3b. The pmu0 passby word: ULPI bypass plus the AHB burst/align enables.  The
      host half already set its own two sun60iw2 bits in this same word (step
      0a), so what this adds is ICHR8, INCR4, INCRX align and ULPI bypass - and
      ULPI bypass is the bit that joins the core's UTMI port to the PHY at all.
      (The manual has no register list for this window; the field positions are
      the kernel's phy-sun4i-usb passby bits.)

  What is deliberately *not* done here, and this is the whole point of the step:
  the host half is not closed, and PHY_RSTN is not pulsed again.  Both were in
  this file, in sequence, on the theory that an EHCI block left clocked on the
  same two wires would fight the device's pull-up, and that a PHY brought up
  while the host half held the port keeps the role it came up in.  Measured:
  with everything else in this file identical, those two writes are the
  difference between a port the host never sees and a port that enumerates as a
  fastboot device.  The PHY is powered once, in step 0a, through the window the
  host half owns; from here on only the route bit moves, and the host half stays
  clocked but idle (it is never given a controller - A733UsbHostDxe publishes
  USB1 only).

  Every write is followed by its read-back.
**/
STATIC
VOID
MusbPhyUp (
  VOID
  )
{
  UINT32  Value;
  UINT32  Route;
  UINT32  Passby;

  //
  // 3. Wrapper PHYCTL: powered, and told that VBUS valid comes from outside.
  //
  Value  = MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG));
  Value &= ~(UINT32)A733_MUSB_PHYCTL_SIDDQ;
  Value |= A733_MUSB_PHYCTL_VBUSVLDEXT;
  MmioWrite32 (MusbReg (A733_MUSB_PHYCTL_REG), Value);

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 3 wrapper PHYCTL wrote 0x%08x, read back 0x%08x "
    "(siddq=%u vbusvldext=%u vbusvldextsel=%u drvvbus=%u otgdisable=%u)\n",
    Value,
    MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)) & A733_MUSB_PHYCTL_SIDDQ) != 0),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)) & A733_MUSB_PHYCTL_VBUSVLDEXT) != 0),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)) & A733_MUSB_PHYCTL_VBUSVLDEXTSEL) != 0),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)) & A733_MUSB_PHYCTL_DRVVBUS) != 0),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)) & A733_MUSB_PHYCTL_OTGDISABLE) != 0)
    ));

  //
  // 3a. Route select: one bit, and only that bit - the rest of this register is
  // the FES path's.
  //
  MmioOr32 (MusbReg (A733_MUSB_ROUTE_REG), BIT0);
  Route = MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG));

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 3a route select |= 0x00000001, read back 0x%08x (bit0=%u)\n",
    Route,
    (UINT32)((Route & BIT0) != 0)
    ));

  if ((Route & BIT0) == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: wrapper route bit 0 did not read back set (0x%08x) - PHY0 is still "
      "routed away from the MUSB core\n",
      Route
      ));
  }

  //
  // 3b. PHY passby word at pmu0+0x00: the four bits that open the data path.
  //
  Passby  = A733_USB_HCI_ULPI_BYPASS | A733_USB_HCI_AHB_INCRX
          | A733_USB_HCI_AHB_INCR4 | A733_USB_HCI_AHB_INCR8;
  MmioOr32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL, Passby);

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 3b PHY passby word |= 0x%04x, read back 0x%08x "
    "(ulpi_bypass=%u ahb_incrx_align=%u ahb_incr4=%u ahb_ichr8=%u)\n",
    Passby,
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL),
    (UINT32)((MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL) & A733_USB_HCI_ULPI_BYPASS) != 0),
    (UINT32)((MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL) & A733_USB_HCI_AHB_INCRX) != 0),
    (UINT32)((MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL) & A733_USB_HCI_AHB_INCR4) != 0),
    (UINT32)((MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL) & A733_USB_HCI_AHB_INCR8) != 0)
    ));

  if ((MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL) & A733_USB_HCI_ULPI_BYPASS) == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: PHY passby ULPI bypass did not read back set: the core's UTMI port is "
      "not connected to PHY0 and nothing will enumerate\n"
      ));
  }

  //
  // The whole hand-over in one line: the route, the wrapper's PHYCTL, and the
  // window as step 0a left it.  Nothing in this function writes that window -
  // the four words there are the host half's, written once with the PHY held in
  // reset - so a changed value here means something other than this driver
  // touched it.
  //
  DEBUG ((
    DEBUG_INFO,
    "Musb: PHY0 handed over: route=0x%08x wrapper PHYCTL=0x%08x; window "
    "pmu0 ahb=0x%08x ctrl3=0x%08x phyctl=0x%08x tune=0x%08x utmi=0x%08x stat=0x%08x "
    "CLK=0x%08x BGR=0x%08x\n",
    MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG)),
    MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_AHB_CTRL),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_CTRL3),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_PHY_CTRL),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_PHY_TUNE),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_UTMI_CTRL),
    MmioRead32 (A733_USB0_HCI_BASE + A733_USB_HCI_UTMI_STAT),
    MmioRead32 (CcuReg (A733_CCU_USB0_CLK)),
    MmioRead32 (CcuReg (A733_CCU_USB0_BGR))
    ));
}

/**
  Describe the bulk endpoints the FIFO map is written for before the transport
  has declared any.

  The device half runs at DXE dispatch and the transport's descriptors arrive
  with Start(), but MusbFifoLayout needs endpoint numbers: it sizes EP1's and
  EP2's FIFOs and zeroes the endpoints fastboot does not use, and it cannot
  guess which numbers belong to which direction.  So the map is written for the
  layout the reference's device-mode driver hardcodes and this board's fastboot
  transport declares - EP1 IN, EP2 OUT, the core's 512-byte bulk maximum - and
  Start() reprograms it if the descriptors name other endpoints
  (mFifoInNumber/mFifoOutNumber record what was actually written).

  This sets only the driver's idea of the endpoints, never a register.

  @param[out] In   The bulk IN endpoint to program the map for.
  @param[out] Out  The bulk OUT endpoint to program the map for.
**/
STATIC
VOID
MusbAssumeTransportEndpoints (
  OUT MUSB_BULK_EP  *In,
  OUT MUSB_BULK_EP  *Out
  )
{
  In->Number    = MUSB_ASSUMED_BULK_IN_NUMBER;
  In->Address   = (UINT8)(MUSB_ASSUMED_BULK_IN_NUMBER | USB_ENDPOINT_DIR_IN);
  In->MaxPacket = (UINT16)MUSB_BULK_MAX_PACKET_LIMIT;

  Out->Number    = MUSB_ASSUMED_BULK_OUT_NUMBER;
  Out->Address   = MUSB_ASSUMED_BULK_OUT_NUMBER;
  Out->MaxPacket = (UINT16)MUSB_BULK_MAX_PACKET_LIMIT;
}

/**
  Program the endpoint FIFO map described in A733Musb.h.

  The reference's FIFO setup, with one address moved: endpoint 0 at 64 B and byte
  0, EP1's TX FIFO at the vendor's byte 1124, EP2's RX FIFO at byte 2048 (the
  vendor's 100 cannot hold the space reserved for it without running into EP1 -
  see A733Musb.h), and the endpoints fastboot does not use sized to zero.
  Endpoint 0 has no programmable FIFO on this core - index 0's size registers
  read back 0x00 - but the reference writes them anyway, and this driver does the
  same rather than leaving two known-good stacks differing in a write that costs
  nothing.

  EP2 is programmed one packet deep (512 B) and not eight, which is the one place
  this map does not use all the space it reserved: eight packets of buffering was
  measured on this board at 3.02-3.16 MB/s against 3.15 without it, because the
  limit is the CPU reading the FIFO and not the NAK the depth would remove.  The
  comment at the write below carries that measurement.  The bulk endpoints'
  maximum packet size, type and intervals are not set here: they belong to
  SET_CONFIGURATION, which is where the reference programs them.
**/
STATIC
VOID
MusbFifoLayout (
  VOID
  )
{
  UINT8    Ep;
  EFI_TPL  OldTpl;

  //
  // Per-endpoint writes through INDEX, so the handler must not run in the middle
  // of them (it leaves INDEX at 0, which would send the next write to EP0).
  //
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  MusbSelectEp (0);
  MmioWrite8 (MusbReg (A733_MUSB_TXFIFOSZ_REG), A733_MUSB_FIFOSZ_SIZE_64);
  MmioWrite16 (MusbReg (A733_MUSB_TXFIFOADD_REG), 0);
  MmioWrite8 (MusbReg (A733_MUSB_RXFIFOSZ_REG), A733_MUSB_FIFOSZ_SIZE_64);
  MmioWrite16 (MusbReg (A733_MUSB_RXFIFOADD_REG), 0);

  MusbSelectEp (mBulkIn.Number);
  MmioWrite8 (MusbReg (A733_MUSB_TXFIFOSZ_REG), A733_MUSB_FIFOSZ_SIZE_512);
  MmioWrite16 (MusbReg (A733_MUSB_TXFIFOADD_REG), A733_MUSB_EP1_FIFO_BYTE / 8);

  MusbSelectEp (mBulkOut.Number);
  //
  // One packet, not the eight the reserved space would allow.  Eight packets was
  // built and measured on this board: 3.02-3.16 MB/s against 3.15 one packet
  // deep, i.e. no change, because the limit is the CPU reading the FIFO (128 MMIO
  // words a packet) and not the NAK a deeper FIFO would remove - and it added a
  // 1.2 ms TPL_HIGH hold per burst for nothing.  DPB (bit 4) stays clear for the
  // same reason, measured at 3.06 MB/s against 3.18 MB/s without it.
  //
  MmioWrite8 (MusbReg (A733_MUSB_RXFIFOSZ_REG), A733_MUSB_FIFOSZ_SIZE_512);
  MmioWrite16 (MusbReg (A733_MUSB_RXFIFOADD_REG), A733_MUSB_EP2_FIFO_BYTE / 8);

  //
  // The endpoints fastboot does not use, sized to zero - never the two the
  // configuration descriptor just named, whatever numbers those are.
  //
  for (Ep = 3; Ep < (UINT8)A733_MUSB_NUM_EPS; Ep++) {
    if ((Ep == mBulkIn.Number) || (Ep == mBulkOut.Number)) {
      continue;
    }

    MusbSelectEp (Ep);
    MmioWrite8 (MusbReg (A733_MUSB_TXFIFOSZ_REG), 0);
    MmioWrite8 (MusbReg (A733_MUSB_RXFIFOSZ_REG), 0);
  }

  MusbSelectEp (0);

  //
  // What the map was written for, so Start() can tell a transport whose
  // descriptors name these same endpoints (the normal case, nothing to do) from
  // one that does not (the map is reprogrammed once).
  //
  mFifoInNumber  = mBulkIn.Number;
  mFifoOutNumber = mBulkOut.Number;

  gBS->RestoreTPL (OldTpl);
}

/**
  Print one endpoint's FIFO size and address registers as they read back.

  The size field is 2^(field + 3) bytes and DPB (bit 4) doubles the buffer, so
  both the raw byte and the decoded byte count are printed: a field that did
  not stick reads as a different size here, and that is the whole point of the
  line.

  @param[in] Ep    Endpoint number to select and report.
  @param[in] Note  What this endpoint's FIFO is for.
**/
STATIC
VOID
MusbLogEpFifo (
  IN UINT8        Ep,
  IN CONST CHAR8  *Note
  )
{
  UINT8   TxFifoSz;
  UINT8   RxFifoSz;
  UINT16  TxFifoAdd;
  UINT16  RxFifoAdd;
  UINTN   TxBytes;
  UINTN   RxBytes;

  MusbSelectEp (Ep);
  TxFifoSz   = MmioRead8 (MusbReg (A733_MUSB_TXFIFOSZ_REG));
  TxFifoAdd  = MmioRead16 (MusbReg (A733_MUSB_TXFIFOADD_REG));
  RxFifoSz   = MmioRead8 (MusbReg (A733_MUSB_RXFIFOSZ_REG));
  RxFifoAdd  = MmioRead16 (MusbReg (A733_MUSB_RXFIFOADD_REG));

  if (Ep == 0) {
    //
    // Not a zero-byte FIFO: INDEX 0 has no FIFO size or address registers at
    // all.  Endpoint 0's FIFO is fixed inside the core (64 B, one buffer shared
    // by both directions in full-speed mode) and lives at byte 0, so the writes
    // MusbFifoLayout makes here read back 0x00 and mean nothing - they are made
    // only because the reference makes them.  The packet size limit that matters
    // for EP0 is TXMAXP, printed here as the real evidence.
    //
    DEBUG ((
      DEBUG_INFO,
      "Musb: FIFO EP0 (%a): no TXFIFOSZ/RXFIFOSZ at INDEX 0 (reads 0x%02x/0x%02x) - the "
      "control FIFO is fixed 64 B at byte 0, shared by both directions; TXMAXP reads 0x%04x\n",
      Note,
      (UINT32)TxFifoSz,
      (UINT32)RxFifoSz,
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXMAXP_REG))
      ));
    return;
  }

  TxBytes    = ((TxFifoSz & A733_MUSB_FIFOSZ_SIZE_MASK) == 0)
               ? 0 : ((UINTN)1 << ((TxFifoSz & A733_MUSB_FIFOSZ_SIZE_MASK) + 3));
  RxBytes    = ((RxFifoSz & A733_MUSB_FIFOSZ_SIZE_MASK) == 0)
               ? 0 : ((UINTN)1 << ((RxFifoSz & A733_MUSB_FIFOSZ_SIZE_MASK) + 3));

  DEBUG ((
    DEBUG_INFO,
    "Musb: FIFO EP%u (%a): TXFIFOSZ=0x%02x (%u B%a) TXFIFOADD=0x%04x (byte %u) "
    "RXFIFOSZ=0x%02x (%u B%a) RXFIFOADD=0x%04x (byte %u)\n",
    (UINT32)Ep,
    Note,
    (UINT32)TxFifoSz,
    (UINT32)TxBytes,
    ((TxFifoSz & A733_MUSB_FIFOSZ_DPB) != 0) ? " x2 DPB" : "",
    (UINT32)TxFifoAdd,
    (UINT32)TxFifoAdd * 8U,
    (UINT32)RxFifoSz,
    (UINT32)RxBytes,
    ((RxFifoSz & A733_MUSB_FIFOSZ_DPB) != 0) ? " x2 DPB" : "",
    (UINT32)RxFifoAdd,
    (UINT32)RxFifoAdd * 8U
    ));

  if ((Ep != 0) && (TxBytes == 0) && (RxBytes == 0)) {
    DEBUG ((
      DEBUG_WARN,
      "Musb: EP%u has no FIFO at all (both size fields read 0) - a packet for it "
      "cannot land\n",
      (UINT32)Ep
      ));
  }
}

/**
  Print every FIFO register back, so the layout in force is the log's, not this
  file's intention.

  @param[in] Tag  Where in the bring-up this reading was taken.
**/
STATIC
VOID
MusbLogFifoLayout (
  IN CONST CHAR8  *Tag
  )
{
  DEBUG ((
    DEBUG_INFO,
    "Musb: %a FIFO map in force: EP0 64 B @0, EP1 TX 512 B @%u, EP2 RX %u B @%u "
    "(%u packets deep, vendor layout with the bulk-OUT FIFO deepened; DPB clear)\n",
    Tag,
    (UINT32)A733_MUSB_EP1_FIFO_BYTE,
    (UINT32)A733_MUSB_BULK_RX_FIFO_BYTES,
    (UINT32)A733_MUSB_EP2_FIFO_BYTE,
    (UINT32)MUSB_RX_BURST_PACKETS
    ));

  MusbLogEpFifo (0, "control, both directions");
  MusbLogEpFifo (mBulkIn.Number, "bulk IN, TX only");
  MusbLogEpFifo (mBulkOut.Number, "bulk OUT, RX only");
  MusbSelectEp (0);
}

/**
  Print the core's identity and capability words once, decoded.

  Four things are being asked here, and each of them has been a guess until now:

    CONFIGDATA   MPTXE/MPRXE say whether this core has one shared packet buffer
                 per direction ("soft cone", which is what the classic MUSB and
                 this vendor stack both assume) or a FIFO per endpoint, and
                 DYNFIFO_SIZING says whether TXFIFOSZ/RXFIFOSZ are honoured at
                 all.  A core reading MPTXE=1 would ignore the whole of
                 MusbFifoLayout, and no SETUP would ever land whatever the SIE
                 did - so this byte decides whether the FIFO model in A733Musb.h
                 is the right one.  The offset is disputed between three maps
                 (see A733Musb.h), so all three candidate bytes are printed side
                 by side and one boot settles which one answers.
    RAMINFO      the FIFO RAM size as the core itself reports it, in the spec's
                 log2-of-bytes field - the number this driver has been guessing
                 at from the vendor's 8 KB claim.
    HWVERS       which revision of the core this is, which is what decides
                 whether a register difference is a bug here or a different
                 silicon generation.
    TMCTL        must read 0.  A residual TEST_SE0_NAK / TEST_K / TEST_J /
                 FORCE_FS / FORCE_HS left by an earlier stage makes the SIE
                 answer no token at all while SOF and RESET still arrive - one
                 of the shapes of "no SETUP ever reaches CSR0".

  Reads only, once, from the device half's bring-up.

  @param[in] Tag  Where in the bring-up this reading was taken.
**/
STATIC
VOID
MusbLogCoreIdentity (
  IN CONST CHAR8  *Tag
  )
{
  UINT8   ConfigData;
  UINT8   HdrcConfig;
  UINT8   VendorConfig;
  UINT16  HwVers;
  UINT8   EpInfo;
  UINT8   RamInfo;
  UINT8   LinkInfo;
  UINT8   VpLen;
  UINT8   Tmctl;
  UINTN   RamBytes;

  ConfigData   = MmioRead8 (MusbReg (A733_MUSB_CONFIGDATA_REG));
  HdrcConfig   = MmioRead8 (MusbReg (A733_MUSB_CONFIGDATA_ALT_HDRC));
  VendorConfig = MmioRead8 (MusbReg (A733_MUSB_CONFIGDATA_ALT_VENDOR));
  HwVers       = MmioRead16 (MusbReg (A733_MUSB_HWVERS_REG));
  EpInfo       = MmioRead8 (MusbReg (A733_MUSB_EPINFO_REG));
  RamInfo      = MmioRead8 (MusbReg (A733_MUSB_RAMINFO_REG));
  LinkInfo     = MmioRead8 (MusbReg (A733_MUSB_LINKINFO_REG));
  VpLen        = MmioRead8 (MusbReg (A733_MUSB_VPLEN_REG));
  Tmctl        = MmioRead8 (MusbReg (A733_MUSB_TMCTL_REG));
  RamBytes     = 0;

  //
  // RAMINFO's low byte is the spec's RAMBITS field: the FIFO RAM size as a
  // power of two in bytes.  Decoded only across the range where a power of two
  // is a plausible answer, so a byte that is not that field reads as "raw"
  // rather than as 4 GB.
  //
  if ((RamInfo >= 6U) && (RamInfo <= 24U)) {
    RamBytes = (UINTN)1 << RamInfo;
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: %a core identity: CONFIGDATA@0x0f=0x%02x (mprxe=%u mptxe=%u bigendian=%u hbrxe=%u "
    "hbtxe=%u dynfifo_sizing=%u softcone=%u utmi_datawidth=%u)\n",
    Tag,
    (UINT32)ConfigData,
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_MPRXE) != 0),
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_MPTXE) != 0),
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_BIGENDIAN) != 0),
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_HBRXE) != 0),
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_HBTXE) != 0),
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_DYNFIFO_SIZING) != 0),
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_SOFTCONE) != 0),
    (UINT32)((ConfigData & A733_MUSB_CONFIGDATA_UTMI_DATAWIDTH) != 0)
    ));

  DEBUG ((
    DEBUG_INFO,
    "Musb: %a CONFIGDATA offset check: 0x0f=0x%02x 0x10=0x%02x 0xc0=0x%02x "
    "(classic MUSB has TESTMODE at 0x0f and CONFIGDATA at 0x10; the vendor usb.h for this core "
    "family has CONFIGDATA at 0xc0 - whichever of the three decodes as a sane byte is the core's)\n",
    Tag,
    (UINT32)ConfigData,
    (UINT32)HdrcConfig,
    (UINT32)VendorConfig
    ));

  DEBUG ((
    DEBUG_INFO,
    "Musb: %a HWVERS=0x%04x (major=%u minor=%u if the usual 15:11 / 10:0 split) EPINFO=0x%02x "
    "RAMINFO=0x%02x (%a%u B) LINKINFO=0x%02x VPLEN=0x%02x TMCTL=0x%02x\n",
    Tag,
    (UINT32)HwVers,
    (UINT32)((HwVers >> 11) & 0x1F),
    (UINT32)(HwVers & 0x7FF),
    (UINT32)EpInfo,
    (UINT32)RamInfo,
    (RamBytes != 0) ? "rambits says " : "not a power of two: raw ",
    (UINT32)RamBytes,
    (UINT32)LinkInfo,
    (UINT32)VpLen,
    (UINT32)Tmctl
    ));

  DEBUG ((
    DEBUG_INFO,
    "Musb: %a TMCTL fields: force_host=%u fifo_access=%u force_fs=%u force_hs=%u test_packet=%u "
    "test_k=%u test_j=%u test_se0_nak=%u\n",
    Tag,
    (UINT32)((Tmctl & A733_MUSB_TMCTL_FORCE_HOST) != 0),
    (UINT32)((Tmctl & A733_MUSB_TMCTL_FIFO_ACCESS) != 0),
    (UINT32)((Tmctl & A733_MUSB_TMCTL_FORCE_FS) != 0),
    (UINT32)((Tmctl & A733_MUSB_TMCTL_FORCE_HS) != 0),
    (UINT32)((Tmctl & A733_MUSB_TMCTL_TEST_PACKET) != 0),
    (UINT32)((Tmctl & A733_MUSB_TMCTL_TEST_K) != 0),
    (UINT32)((Tmctl & A733_MUSB_TMCTL_TEST_J) != 0),
    (UINT32)((Tmctl & A733_MUSB_TMCTL_TEST_SE0_NAK) != 0)
    ));

  if (Tmctl != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: TMCTL reads 0x%02x: a test mode left set by an earlier stage makes the "
      "SIE answer no token at all, whatever the endpoints say\n",
      (UINT32)Tmctl
      ));
  }
}


/**
  Discard whatever the two bulk FIFOs hold, and leave their data toggles at 0.

  This is the whole of what the bring-up does to the data endpoints, which is
  exactly what the reference does: their maximum packet size, type and interval
  are programmed at SET_CONFIGURATION, not before it
  (MusbConfigureBulkEndpoints).  The two values are the reference's, field for
  field - TXCSR.MODE with CLRDATATOG and FLUSHFIFO (0x2048), and RXCSR with the
  three write-one-to-clear status bits (RXPKTRDY, SENTSTALL, OVERRUN) plus
  FLUSHFIFO and CLRDATATOG, ISO clear (0x00d5).  A reset does the same flush, so
  a restart of the transport cannot hand the next session a stale packet.

  The same flush value also clears the endpoint's DMA-request bits, which is why
  this function is the one place an endpoint handed back from an engine would be
  made a FIFO endpoint again.  No engine exists here (MusbDmaOff), and nothing
  writes those bits, so the pair of writes below is the whole of the endpoint's
  state.
**/
STATIC
VOID
MusbFlushBulkEndpoints (
  VOID
  )
{
  EFI_TPL  OldTpl;

  //
  // The bulk endpoints can now be interrupt-driven, and the handler selects an
  // endpoint and leaves INDEX at 0.  A write that was aimed at EP1's TXCSR and
  // landed on EP0's CSR0 because the handler ran in between would flush the
  // control endpoint instead - so the sequence is taken at TPL_HIGH.
  //
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  MusbSelectEp (mBulkIn.Number);
  MmioWrite16 (
    MusbReg (A733_MUSB_TXCSR_REG),
    A733_TXCSR_MODE | A733_TXCSR_CLRDATATOG | A733_TXCSR_FLUSHFIFO
    );

  MusbSelectEp (mBulkOut.Number);
  MmioWrite16 (
    MusbReg (A733_MUSB_RXCSR_REG),
    (A733_RXCSR_ACK_MASK | A733_RXCSR_FLUSHFIFO | A733_RXCSR_CLRDATATOG) &
    ~(UINT16)A733_RXCSR_ISO
    );
  MusbSelectEp (0);

  gBS->RestoreTPL (OldTpl);
}

/**
  Discard anything the core was holding for endpoint 0 and idle its state.

  The flush is the whole of it, and it is the reference's reset for this
  endpoint: FADDR is zeroed by the caller and this writes CSR0 = FLUSHFIFO
  (0x0100).  Neither implementation that enumerates on this board writes a
  maximum-packet value for endpoint 0 - the vendor device stack's
  USBC_Dev_ConfigEp(EP0) and SunxiMusbDeviceDxe both write nothing but that
  flush - so the TXMAXP write an earlier version of this function made is gone:
  index 0's maximum packet is hard-wired 64 in the core, and writing a value the
  reference does not write is a difference the host is entitled to notice.

  SETUPEND is deliberately *not* acknowledged either, for the same reason: the
  reference writes only FLUSHFIFO and enumerates, and the state machine in this
  file handles a SETUP that arrives with SETUPEND already latched (the core
  latches it as a bus-reset artefact) by acknowledging it in the SETUP path,
  where it belongs.
**/
STATIC
VOID
MusbEp0Reset (
  VOID
  )
{
  MusbSelectEp (0);
  MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_FLUSHFIFO);
  mEp0State            = MusbEp0Idle;
  mEp0AddressPending   = FALSE;
  mEp0ConfigPending    = FALSE;
  mEp0NewAddress       = 0;
  mEp0NewConfig        = 0;
  mEp0Tx.Data          = NULL;
  mEp0Tx.Length        = 0;
  mEp0Tx.Sent          = 0;
}

/**
  Program the two bulk endpoints from the configuration descriptor being served.

  The transport declares one interface with an IN endpoint and an OUT endpoint
  and calls Send() with the IN endpoint's index; the descriptor is the only
  place those numbers come from, so nothing about them is hardcoded here.  The
  value programmed is the one MusbParseConfiguration read out of
  mConfigDescriptor - the copy the transport's descriptor was taken into - so the
  wire and the core agree.

  Called from SET_CONFIGURATION only, which is where the enumerating device-mode
  driver programs them too: before a configuration is chosen the core is left
  with the endpoints' FIFOs flushed and nothing else (MusbFlushBulkEndpoints).
  The call is made after the status stage has been armed (see MusbHandleSetup),
  because a long register sequence with EP0's RXPKTRDY still latched makes the
  core NAK the status token and the host reports a protocol error.
**/
STATIC
VOID
MusbConfigureBulkEndpoints (
  VOID
  )
{
  EFI_TPL  OldTpl;

  //
  // Per-endpoint writes through INDEX, at SET_CONFIGURATION - which is exactly
  // when the interrupt may already be claimed.  The handler leaves INDEX at 0,
  // so it must not run inside this sequence.
  //
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);

  MusbSelectEp (mBulkIn.Number);
  MmioWrite16 (MusbReg (A733_MUSB_TXMAXP_REG), mBulkIn.MaxPacket);
  MmioWrite8 (MusbReg (A733_MUSB_TXTYPE_REG), 0);        // protocol 00 = bulk; speed is the bus's
  MmioWrite8 (MusbReg (A733_MUSB_TXINTERVAL_REG), 0);
  //
  // Written twice, as the reference does: the first write's FLUSHFIFO can still
  // be in progress when a second configuration change would look at the FIFO,
  // and CLRDATATOG has to land whatever the flush did.
  //
  MmioWrite16 (
    MusbReg (A733_MUSB_TXCSR_REG),
    A733_TXCSR_MODE | A733_TXCSR_CLRDATATOG | A733_TXCSR_FLUSHFIFO
    );
  MmioWrite16 (
    MusbReg (A733_MUSB_TXCSR_REG),
    A733_TXCSR_MODE | A733_TXCSR_CLRDATATOG | A733_TXCSR_FLUSHFIFO
    );

  MusbSelectEp (mBulkOut.Number);
  MmioWrite16 (MusbReg (A733_MUSB_RXMAXP_REG), mBulkOut.MaxPacket);
  MmioWrite8 (MusbReg (A733_MUSB_RXTYPE_REG), 0);        // protocol 00 = bulk; speed is the bus's
  MmioWrite8 (MusbReg (A733_MUSB_RXINTERVAL_REG), 0);
  //
  // The three write-one-to-clear status bits go back too, so a stale OVERRUN or
  // SENTSTALL from the previous session is not inherited as a live fault; ISO
  // is cleared because this endpoint is bulk.
  //
  MmioWrite16 (
    MusbReg (A733_MUSB_RXCSR_REG),
    (A733_RXCSR_ACK_MASK | A733_RXCSR_FLUSHFIFO | A733_RXCSR_CLRDATATOG) &
    ~(UINT16)A733_RXCSR_ISO
    );
  MmioWrite16 (
    MusbReg (A733_MUSB_RXCSR_REG),
    (A733_RXCSR_ACK_MASK | A733_RXCSR_FLUSHFIFO | A733_RXCSR_CLRDATATOG) &
    ~(UINT16)A733_RXCSR_ISO
    );

  //
  // The same value clears the endpoint's DMA-request bits, so a reconfigured
  // endpoint is a FIFO endpoint whatever it was before - which is the state the
  // rest of this driver reads packets in.
  //
  MusbSelectEp (0);

  gBS->RestoreTPL (OldTpl);
}

/**
  Take a RAM copy of the transport's configuration descriptor and describe it
  at the speed the core is running at.

  The transport's blob is a compile-time constant (FastbootTransportUsb.c) that
  declares 512-byte bulk endpoints, which the core also implements, so normally
  nothing about it is rewritten.  The cap below is a guard, not a policy: a bulk
  wMaxPacketSize above MUSB_BULK_MAX_PACKET_LIMIT could not be programmed into
  the core (and a full-speed host, if the port ever negotiated full speed, is
  entitled to refuse a descriptor asking for more than 64), so such a value is
  brought down to the limit in the copy - and that copy is both what
  GET_DESCRIPTOR serves and what MusbParseConfiguration programs the endpoints
  from: one number, in one place, for the wire and for the core.

  The copy exists so the transport's array is never edited: it is the
  transport's, GET_DESCRIPTOR may be asked for the same descriptor many times,
  and Start() may run again.  Endpoint numbers and addresses, the interface, and
  the device descriptor's VID/PID are the transport's and are not touched -
  wMaxPacketSize is the only field this function rewrites.

  @retval EFI_SUCCESS            mConfigDescriptor points at the descriptor to serve.
  @retval EFI_INVALID_PARAMETER  No descriptor, or not a configuration.
  @retval EFI_BUFFER_TOO_SMALL   Longer than MUSB_CONFIG_COPY_MAX.
**/
STATIC
EFI_STATUS
MusbPrepareConfiguration (
  VOID
  )
{
  USB_CONFIG_DESCRIPTOR    *Config;
  USB_ENDPOINT_DESCRIPTOR  *Endpoint;
  UINT8                    *Walk;
  UINTN                    Offset;
  UINT8                    Length;
  UINT8                    Type;
  UINT16                   Original;

  mConfigDescriptor = NULL;

  if ((mConfigDescriptors == NULL) || (mConfigDescriptors[0] == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Config = (USB_CONFIG_DESCRIPTOR *)mConfigDescriptors[0];
  if (Config->DescriptorType != USB_DESC_TYPE_CONFIG) {
    return EFI_INVALID_PARAMETER;
  }

  if (Config->TotalLength > sizeof (mConfigCopy)) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: configuration descriptor is %u bytes, more than the %u-byte copy this driver "
      "keeps: not served\n",
      (UINT32)Config->TotalLength,
      (UINT32)sizeof (mConfigCopy)
      ));
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (mConfigCopy, Config, Config->TotalLength);
  Config            = (USB_CONFIG_DESCRIPTOR *)mConfigCopy;
  mConfigDescriptor = mConfigCopy;

  Walk = mConfigCopy;
  for (Offset = 0; Offset + 2 <= Config->TotalLength; ) {
    Length = Walk[Offset];
    Type   = Walk[Offset + 1];
    if (Length < 2) {
      break;
    }

    if ((Type == USB_DESC_TYPE_ENDPOINT) && (Length >= sizeof (USB_ENDPOINT_DESCRIPTOR))) {
      Endpoint = (USB_ENDPOINT_DESCRIPTOR *)(Walk + Offset);
      if (((Endpoint->Attributes & 0x3) == USB_ENDPOINT_BULK) &&
          (Endpoint->MaxPacketSize > MUSB_BULK_MAX_PACKET_LIMIT))
      {
        Original                = Endpoint->MaxPacketSize;
        Endpoint->MaxPacketSize = (UINT16)MUSB_BULK_MAX_PACKET_LIMIT;
        DEBUG ((
          DEBUG_INFO,
          "Musb: configuration descriptor rewritten: bulk endpoint 0x%02x wMaxPacketSize "
          "%u -> %u (%a); the endpoint numbers, the interface and %04x:%04x are as the "
          "transport declared them\n",
          (UINT32)Endpoint->EndpointAddress,
          (UINT32)Original,
          (UINT32)Endpoint->MaxPacketSize,
          MUSB_BULK_LIMIT_NOTE,
          mDeviceDescriptor->IdVendor,
          mDeviceDescriptor->IdProduct
          ));
      }
    }

    Offset += Length;
  }

  return EFI_SUCCESS;
}

/**
  Take the endpoints the transport wants out of its configuration descriptor.

  Reads the descriptor MusbPrepareConfiguration left in mConfigDescriptor, i.e.
  the rewritten copy, so the value programmed into the endpoint is the value the
  host was told.

  @retval EFI_SUCCESS            One bulk IN and one bulk OUT endpoint were found.
  @retval EFI_INVALID_PARAMETER  The descriptor is not a configuration.
  @retval EFI_NOT_FOUND          It carries no bulk endpoint pair.
**/
STATIC
EFI_STATUS
MusbParseConfiguration (
  VOID
  )
{
  USB_CONFIG_DESCRIPTOR  *Config;
  UINT8                  *Walk;
  UINTN                  Offset;
  UINT8                  Length;
  UINT8                  Type;
  USB_ENDPOINT_DESCRIPTOR *Endpoint;

  if (mConfigDescriptor == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Config = (USB_CONFIG_DESCRIPTOR *)mConfigDescriptor;
  if (Config->DescriptorType != USB_DESC_TYPE_CONFIG) {
    return EFI_INVALID_PARAMETER;
  }

  mBulkIn.Number    = 0;
  mBulkIn.Address   = 0;
  mBulkIn.MaxPacket = 0;
  mBulkOut         = mBulkIn;

  Walk = mConfigDescriptor;
  for (Offset = 0; Offset + 2 <= Config->TotalLength; ) {
    Length = Walk[Offset];
    Type   = Walk[Offset + 1];
    if (Length < 2) {
      break;
    }

    if ((Type == USB_DESC_TYPE_ENDPOINT) && (Length >= sizeof (USB_ENDPOINT_DESCRIPTOR))) {
      Endpoint = (USB_ENDPOINT_DESCRIPTOR *)(Walk + Offset);
      if ((Endpoint->Attributes & 0x3) == USB_ENDPOINT_BULK) {
        if ((Endpoint->EndpointAddress & USB_ENDPOINT_DIR_IN) != 0) {
          if (mBulkIn.Number == 0) {
            mBulkIn.Number    = (UINT8)(Endpoint->EndpointAddress & 0x0F);
            mBulkIn.Address   = Endpoint->EndpointAddress;
            mBulkIn.MaxPacket = MIN (Endpoint->MaxPacketSize, (UINT16)MUSB_BULK_MAX_PACKET_LIMIT);
          }
        } else {
          if (mBulkOut.Number == 0) {
            mBulkOut.Number    = (UINT8)(Endpoint->EndpointAddress & 0x0F);
            mBulkOut.Address   = Endpoint->EndpointAddress;
            mBulkOut.MaxPacket = MIN (Endpoint->MaxPacketSize, (UINT16)MUSB_BULK_MAX_PACKET_LIMIT);
          }
        }
      }
    }

    Offset += Length;
  }

  if ((mBulkIn.Number == 0) || (mBulkOut.Number == 0)) {
    return EFI_NOT_FOUND;
  }

  if ((mBulkIn.MaxPacket == 0) || (mBulkOut.MaxPacket == 0)) {
    return EFI_NOT_FOUND;
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: transport descriptors: %u interface(s), bulk IN 0x%02x (EP%u, %u B), "
    "bulk OUT 0x%02x (EP%u, %u B)\n",
    (UINT32)Config->NumInterfaces,
    mBulkIn.Address,
    (UINT32)mBulkIn.Number,
    (UINT32)mBulkIn.MaxPacket,
    mBulkOut.Address,
    (UINT32)mBulkOut.Number,
    (UINT32)mBulkOut.MaxPacket
    ));

  return EFI_SUCCESS;
}

/**
  Wait, briefly, for CSR0.RXPKTRDY to clear after an acknowledge was written.

  @retval TRUE   The packet in the FIFO was acknowledged.
  @retval FALSE  The poll hit its cap; a MusbTagSpinCapped record was made.
**/
STATIC
BOOLEAN
MusbEp0AckTaken (
  VOID
  )
{
  UINT32  Spin;

  for (Spin = 0; Spin < MUSB_EP0_ACK_SPIN; Spin++) {
    if ((MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)) & A733_CSR0_RXPKTRDY) == 0) {
      return TRUE;
    }
  }

  //
  // The cap, not the core, ended the poll.  Recorded rather than printed: this
  // runs inside the SETUP path, where one line would cost the transfer 8 ms -
  // longer than the host's whole timeout - so the warning is a ring entry and
  // appears with the rest of the window's trace.
  //
  MusbRingAdd (MusbTagSpinCapped, MUSB_EP0_ACK_SPIN, 0);
  return FALSE;
}

/**
  Acknowledge the packet sitting in endpoint 0's FIFO (the SETUP packet), for the
  data-IN case where the status stage is still ahead.

  A single acknowledge, no DATAEND: the data stage follows, and its last packet
  carries DATAEND itself (see MusbEp0SendNext).
**/
STATIC
VOID
MusbEp0Ack (
  VOID
  )
{
  MusbSelectEp (0);
  MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY);
  (VOID)MusbEp0AckTaken ();
}

/**
  Arm the status stage of a request that has no data stage.

  One write of SVDRXPKTRDY|DATAEND, and no TXPKTRDY - this is the vendor device
  stack's sequence for exactly this shape, and the difference from what this
  driver did before is what kept enumerating from getting past SET_ADDRESS:

    __USBC_Dev_ep0_ReadDataComplete (usbc_dev.c:130-134) writes
    (1 << SERVICED_RX_PKT_READY) | (1 << DATA_END) and nothing else.  For a
    request with no data stage it is reached from
    sunxi_udc_set_address -> USBC_Dev_ReadDataStatus(..., EP0, 1)
    (usb_base_platform.c:1011-1015) and, for every EP0 packet the stack reads,
    from __usb_readcomplete (usb_base_platform.c:573-589 -> 705).

  The acknowledge and "the data phase is over" therefore go out together, and
  the core answers the host's status IN token itself.  TXPKTRDY is never raised
  for that packet: it means "a data packet is loaded and waiting", the FIFO is
  empty, and this core then neither sends a zero-length packet nor clears the
  bit - CSR0 stayed at 0x000A until the next bus reset while the host re-sent
  SET_ADDRESS for ever.
**/
STATIC
VOID
MusbEp0ArmStatus (
  VOID
  )
{
  MusbSelectEp (0);
  //
  // Two writes, not one.  The reference device driver that enumerates on this
  // board acknowledges the SETUP on its own, waits for RXPKTRDY to read back
  // clear, and only then writes DATAEND - its comment says a single write of
  // both bits "samples DATAEND while RXPKTRDY is still asserted, so the status
  // stage never completes".  That is exactly the failure this port has been
  // showing, so the reference's order is what runs here.
  //
  MusbEp0Ack ();
  MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_DATAEND);
  mEp0State = MusbEp0Status;
  MusbRingAdd (MusbTagAnswer, MusbAnswerStatusArmed, (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)));

  //
  // The read-back is what the next boot is read for: DATAEND set and TXPKTRDY
  // clear is the armed state, and the poll's MusbTagStatusCsr record then says
  // whether DATAEND ever clears on its own - which is the zero-length packet
  // leaving the core.
  //
  MUSB_TRACE (
    "Musb: status stage armed: CSR0 read back 0x%04x (dataend=%u txpktrdy=%u)\n",
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)),
    (UINT32)((MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)) & A733_CSR0_DATAEND) != 0),
    (UINT32)((MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)) & A733_CSR0_TXPKTRDY) != 0)
    );

  //
  // If the acknowledge part of that write did not take, RXPKTRDY is still
  // latched and the record says so; the status stage itself was still armed.
  //
  (VOID)MusbEp0AckTaken ();
}

/**
  Load the next (at most 64-byte) packet of the current data-IN stage.

  The last packet carries DATAEND, which is what makes the core run the status
  stage by itself once the host has taken the data.
**/
STATIC
VOID
MusbEp0SendNext (
  VOID
  )
{
  UINTN    Remaining;
  UINTN    Chunk;
  BOOLEAN  Last;

  Remaining = mEp0Tx.Length - mEp0Tx.Sent;
  Chunk     = MIN (Remaining, (UINTN)A733_MUSB_EP0_MAX_PACKET);
  Last      = (Chunk < A733_MUSB_EP0_MAX_PACKET) || ((mEp0Tx.Sent + Chunk) == mEp0Tx.Length);

  //
  // Report a data stage that is not the length the host asked for, once, before
  // the first packet goes in.  Shorter is legal - a descriptor is as long as it
  // is, and a wLength larger than it ends the transfer on a short packet - but
  // it is also what a truncated reply looks like, so the numbers are recorded
  // either way and printed with the rest of the window's trace.  Longer cannot
  // happen through MusbReplyToInRequest, which takes the minimum of wLength and
  // the descriptor; if it does, the core would send more than wLength and the
  // host would see a babble, so it is an error here.
  //
  if (mEp0Tx.Sent == 0) {
    if (mEp0Tx.Length < mEp0Requested) {
      MusbRingAdd (MusbTagResponseShort, (UINT32)mEp0Tx.Length, (UINT32)mEp0Requested);
      MUSB_TRACE (
        "Musb: response staged %u bytes where the host asked for %u (shorter: ends "
        "on a short packet, normal when the descriptor is shorter than wLength)\n",
        (UINT32)mEp0Tx.Length,
        (UINT32)mEp0Requested
        );
    } else if (mEp0Tx.Length > mEp0Requested) {
      MusbRingAdd (MusbTagResponseLong, (UINT32)mEp0Tx.Length, (UINT32)mEp0Requested);
      MUSB_TRACE (
        "Musb: response staged %u bytes where the host asked for %u (longer: the "
        "core would send past wLength)\n",
        (UINT32)mEp0Tx.Length,
        (UINT32)mEp0Requested
        );
    }
  }

  //
  // The data stage is staged from mEp0Tx.Data, which MusbHandleSetup sets from
  // the reply it built and MusbEp0Reset clears to NULL.  A state machine that
  // is still in MusbEp0Tx without a buffer - a reset that cleared the buffer
  // while the endpoint was left armed, or a reply whose pointer was never
  // published - would write the FIFO from address 0, which is a data abort on
  // this board.  Stall the request instead: the host sees a STALL, which it
  // retries or reports, rather than a dead CPU.
  //
  if ((mEp0Tx.Data == NULL) || (mEp0Tx.Sent > mEp0Tx.Length)) {
    MusbStall (MusbAnswerStallRefused, (UINT32)mEp0Tx.Length);
    MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SENDSTALL);
    mEp0State = MusbEp0Idle;
    return;
  }

  MusbSelectEp (0);
  MusbFifoWrite (0, Chunk, mEp0Tx.Data + mEp0Tx.Sent);
  mEp0Tx.Sent += Chunk;

  MmioWrite16 (
    MusbReg (A733_MUSB_TXCSR_REG),
    (UINT16)(A733_CSR0_TXPKTRDY | (Last ? A733_CSR0_DATAEND : 0))
    );

  mEp0State = Last ? MusbEp0Status : MusbEp0Tx;
}

/**
  Answer GET_DESCRIPTOR for a string index.

  @return The descriptor length written to Buffer, or 0 for an index that does
          not exist (which the caller stalls).
**/
STATIC
UINTN
MusbBuildString (
  IN  UINT8  Index,
  OUT UINT8  *Buffer,
  IN  UINTN  BufferSize
  )
{
  CONST CHAR8  *Text;
  UINTN        Length;
  UINTN        Size;
  UINTN        Char;

  switch (Index) {
    case 0:
      //
      // Language ID table.  Not a string at all, but the same request.
      //
      if (BufferSize < 4) {
        return 0;
      }

      Buffer[0] = 4;
      Buffer[1] = USB_DESC_TYPE_STRING;
      CopyMem (&Buffer[2], mStringLangId, sizeof (mStringLangId));
      return 4;
    case 1:
      Text = mStringManufacturer;
      break;
    case 2:
      Text = mStringProduct;
      break;
    case 3:
      Text = mStringSerial;
      break;
    default:
      return 0;
  }

  Length = AsciiStrLen (Text);
  Size   = 2 + 2 * Length;
  if (Size > BufferSize) {
    return 0;
  }

  Buffer[0] = (UINT8)Size;
  Buffer[1] = USB_DESC_TYPE_STRING;
  for (Char = 0; Char < Length; Char++) {
    Buffer[2 + 2 * Char]     = (UINT8)Text[Char];
    Buffer[2 + 2 * Char + 1] = 0;
  }

  return Size;
}

/**
  Answer a standard request that has a data-IN stage.

  @param[in]  Setup  The request.
  @param[out] Data   The buffer to send.
  @param[out] Length How much of it to send.

  @retval TRUE   The request is answered.
  @retval FALSE  It is not one this device implements, and must be stalled.
**/
STATIC
BOOLEAN
MusbReplyToInRequest (
  IN  USB_DEVICE_REQUEST  *Setup,
  OUT CONST UINT8         **Data,
  OUT UINTN               *Length
  )
{
  UINT8                   DescType;
  UINT8                   DescIndex;
  UINT8                   *Scratch;
  USB_CONFIG_DESCRIPTOR   *Config;

  if ((Setup->RequestType & 0x60) != USB_REQ_TYPE_STANDARD) {
    return FALSE;
  }

  switch (Setup->Request) {
    case USB_REQ_GET_DESCRIPTOR:
      DescType  = (UINT8)(Setup->Value >> 8);
      DescIndex = (UINT8)(Setup->Value & 0xFF);

      switch (DescType) {
        case USB_DESC_TYPE_DEVICE:
          //
          // mDeviceDescriptor is set by Start() and by nothing else, so it is
          // NULL for any SETUP served without one - the device is up at DXE
          // dispatch, and a host that enumerates before the transport has
          // published its descriptor blob asks exactly this.  Refuse rather than
          // hand the caller a NULL buffer to stage a data stage from, and record
          // the miss: Start() gives that host a fresh connect to re-enumerate.
          //
          if (mDeviceDescriptor == NULL) {
            mDescriptorMiss = TRUE;
            return FALSE;
          }

          *Data   = (CONST UINT8 *)mDeviceDescriptor;
          *Length = MIN ((UINTN)Setup->Length, sizeof (USB_DEVICE_DESCRIPTOR));
          if (Setup->Length >= sizeof (USB_DEVICE_DESCRIPTOR)) {
            MUSB_TRACE (
              "Musb: serving device descriptor %04x:%04x, class 0x%02x, %u config(s)\n",
              mDeviceDescriptor->IdVendor,
              mDeviceDescriptor->IdProduct,
              (UINT32)mDeviceDescriptor->DeviceClass,
              (UINT32)mDeviceDescriptor->NumConfigurations
              );
          }

          return TRUE;

        case USB_DESC_TYPE_CONFIG:
          //
          // The copy MusbPrepareConfiguration left, not the transport's blob:
          // it is what GET_DESCRIPTOR must serve once any bulk size has been
          // brought down to the core's limit, and it is what the endpoints were
          // programmed from.
          //
          if ((DescIndex != 0) || (mConfigDescriptor == NULL)) {
            return FALSE;
          }

          Config  = (USB_CONFIG_DESCRIPTOR *)mConfigDescriptor;
          *Data   = (CONST UINT8 *)Config;
          *Length = MIN ((UINTN)Setup->Length, (UINTN)Config->TotalLength);
          return TRUE;

        case USB_DESC_TYPE_STRING:
          *Length = MusbBuildString (DescIndex, mEp0Scratch, sizeof (mEp0Scratch));
          if (*Length == 0) {
            return FALSE;
          }

          *Length = MIN ((UINTN)Setup->Length, *Length);
          *Data   = mEp0Scratch;
          return TRUE;

        case 0x06:
          //
          // DEVICE_QUALIFIER.  The device is high-speed capable, so the
          // full-speed half this describes has to exist; it is the device
          // descriptor with the other speed's endpoint size.
          //
          // Unlike the DEVICE case above, nothing downstream can notice a
          // missing device descriptor here: the six fields below are read
          // inside this function, so a NULL mDeviceDescriptor (set only by
          // Start(), and NULL when the transport has not published its blob
          // yet) is dereferenced before the caller's Data == NULL test can
          // run.  Refuse the qualifier instead, and let the request be
          // stalled.
          //
          if (mDeviceDescriptor == NULL) {
            return FALSE;
          }

          Scratch     = mEp0Scratch;
          Scratch[0]  = 10;
          Scratch[1]  = 0x06;
          Scratch[2]  = (UINT8)(mDeviceDescriptor->BcdUSB & 0xFF);
          Scratch[3]  = (UINT8)(mDeviceDescriptor->BcdUSB >> 8);
          Scratch[4]  = mDeviceDescriptor->DeviceClass;
          Scratch[5]  = mDeviceDescriptor->DeviceSubClass;
          Scratch[6]  = mDeviceDescriptor->DeviceProtocol;
          Scratch[7]  = A733_MUSB_EP0_MAX_PACKET;
          Scratch[8]  = mDeviceDescriptor->NumConfigurations;
          Scratch[9]  = 0;
          *Data       = Scratch;
          *Length     = MIN ((UINTN)Setup->Length, 10U);
          return TRUE;

        default:
          return FALSE;
      }

    case USB_REQ_GET_STATUS:
      //
      // No remote wake-up, no self-powered-beyond-what-config-says, and no
      // halted endpoint: two zero bytes answer all three.
      //
      mEp0Scratch[0] = 0;
      mEp0Scratch[1] = 0;
      *Data          = mEp0Scratch;
      *Length        = MIN ((UINTN)Setup->Length, 2U);
      return TRUE;

    case USB_REQ_GET_CONFIG:
      mEp0Scratch[0] = mConfiguration;
      *Data          = mEp0Scratch;
      *Length        = MIN ((UINTN)Setup->Length, 1U);
      return TRUE;

    case USB_REQ_GET_INTERFACE:
      //
      // One interface, alternate setting 0 only.
      //
      mEp0Scratch[0] = 0;
      *Data          = mEp0Scratch;
      *Length        = MIN ((UINTN)Setup->Length, 1U);
      return TRUE;

    default:
      return FALSE;
  }
}

/**
  Handle one SETUP packet on endpoint 0.

  The packet is read first, then acknowledged, then answered, because the core
  will not switch the FIFO to transmit until the SETUP is acknowledged.
**/
STATIC
VOID
MusbHandleSetup (
  VOID
  )
{
  USB_DEVICE_REQUEST  Setup;
  CONST UINT8         *Data;
  UINTN               Length;
  UINT16              Count0;
  UINT8               Count;
  BOOLEAN             InDataPhase;

  MusbSelectEp (0);
  //
  // The console goes quiet for the whole of this request - and stays quiet for
  // the enumeration once this is its first SETUP.  Everything the verbose log
  // used to say from here on is recorded in the ring instead and printed when
  // the window closes; see MUSB_ENUM_VERBOSE at the top of the file for the
  // switch that brings the lines back.
  //
  MusbQuietEnter ();

  //
  // COUNT0 is a 16-bit register on this core - the vendor stack reads it with
  // USBC_Readw(COUNT0) - while RXCOUNT next door (0x88 at any other index)
  // belongs to the data endpoints.  The byte count is the low byte; the raw
  // 16-bit value is recorded, so a SETUPPKT-style flag in the upper half would
  // be visible rather than masked away by an 8-bit read.
  //
  Count0 = MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG));
  Count  = (UINT8)(Count0 & 0xFF);
  if (Count != sizeof (Setup)) {
    MusbStall (MusbAnswerStallCount, (UINT32)Count0);
    MUSB_TRACE (
      "Musb: SETUP answer: STALL - EP0 packet of %u bytes where a SETUP was due "
      "(COUNT0 raw 0x%04x, CSR0=0x%04x)\n",
      (UINT32)Count,
      (UINT32)Count0,
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG))
      );
    MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
    mEp0State = MusbEp0Idle;
    return;
  }

  MusbFifoRead (0, sizeof (Setup), (UINT8 *)&Setup);

  //
  // The eight bytes as the FIFO handed them over, before any field decoding,
  // packed into one record: a request the driver reports as one thing while the
  // host's usbmon shows another can only be settled by looking at the bytes, and
  // a host that STALLs while the log says SET_ADDRESS means exactly that.
  //
  MusbRingAdd (
    MusbTagSetup,
    (UINT32)Setup.RequestType
    | ((UINT32)Setup.Request << 8)
    | ((UINT32)Setup.Value << 16),
    (UINT32)Setup.Index | ((UINT32)Setup.Length << 16)
    );

  //
  // Counted as well as recorded: a SETUP after a reset means the host is
  // enumerating that reset, so the one-shot re-attach (MusbReattachPoll) is not
  // needed and the window is closed.
  //
  mSetupsHandled++;

  MUSB_TRACE (
    "Musb: SETUP raw %02x %02x %02x %02x %02x %02x %02x %02x "
    "(COUNT0=0x%04x CSR0=0x%04x, struct size %u)\n",
    ((UINT8 *)&Setup)[0],
    ((UINT8 *)&Setup)[1],
    ((UINT8 *)&Setup)[2],
    ((UINT8 *)&Setup)[3],
    ((UINT8 *)&Setup)[4],
    ((UINT8 *)&Setup)[5],
    ((UINT8 *)&Setup)[6],
    ((UINT8 *)&Setup)[7],
    (UINT32)Count0,
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)),
    (UINT32)sizeof (Setup)
    );

  //
  // wLength is kept for the data-stage comparison below: a response that is
  // shorter than the host asked for ends the transfer with a short packet,
  // which some hosts take as a stall or as a protocol error, and the only place
  // that can be reported is here.
  //
  mEp0Requested = Setup.Length;

  MUSB_TRACE (
    "Musb: SETUP bmRequestType=0x%02x bRequest=0x%02x (%a) wValue=0x%04x "
    "wIndex=0x%04x wLength=%u (dir=%a type=%a recipient=%a) INTRUSB=0x%02x\n",
    (UINT32)Setup.RequestType,
    (UINT32)Setup.Request,
    MusbRequestName (Setup.Request),
    (UINT32)Setup.Value,
    (UINT32)Setup.Index,
    (UINT32)Setup.Length,
    ((Setup.RequestType & USB_ENDPOINT_DIR_IN) != 0) ? "IN" : "OUT",
    MusbRequestTypeName (Setup.RequestType),
    MusbRequestTargetName (Setup.RequestType),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSB_REG))
    );

  InDataPhase = ((Setup.RequestType & USB_ENDPOINT_DIR_IN) != 0) && (Setup.Length != 0);

  if (InDataPhase) {
    //
    // Acknowledge the SETUP before the data stage: the core will not accept a
    // packet in the IN FIFO while RXPKTRDY is latched.
    //
    MusbEp0Ack ();
    Data   = NULL;
    Length = 0;
    if (!MusbReplyToInRequest (&Setup, &Data, &Length) || (Data == NULL) || (Length == 0)) {
      MusbStall (MusbAnswerStallRefused, 0);
      MUSB_TRACE (
        "Musb: SETUP answer: data-IN refused - STALL, nothing this device answers "
        "request 0x%02x with\n",
        (UINT32)Setup.Request
        );
      MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SENDSTALL);
      mEp0State = MusbEp0Idle;
      return;
    }

    mEp0Tx.Data   = Data;
    mEp0Tx.Length = Length;
    mEp0Tx.Sent   = 0;
    MusbRingAdd (MusbTagAnswer, MusbAnswerDataIn, (UINT32)Length);
    MUSB_TRACE (
      "Musb: SETUP answer: data-IN, %u bytes staged where the host asked for %u "
      "(EP0 packets of up to %u bytes)\n",
      (UINT32)Length,
      (UINT32)mEp0Requested,
      (UINT32)A733_MUSB_EP0_MAX_PACKET
      );
    MusbEp0SendNext ();
    return;
  }

  if (Setup.Length != 0) {
    //
    // An OUT data stage.  Nothing this device implements has one (SET_DESCRIPTOR
    // is not a fastboot request), so stall rather than accept bytes nobody
    // asked for.
    //
    MusbStall (MusbAnswerStallOutData, 0);
    MUSB_TRACE (
      "Musb: SETUP answer: STALL - request 0x%02x would need an OUT data stage "
      "of %u bytes and nothing here takes one\n",
      (UINT32)Setup.Request,
      (UINT32)Setup.Length
      );
    MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
    mEp0State = MusbEp0Idle;
    return;
  }

  if ((Setup.RequestType & 0x60) != USB_REQ_TYPE_STANDARD) {
    MusbStall (MusbAnswerStallType, 0);
    MUSB_TRACE (
      "Musb: SETUP answer: STALL - %a request 0x%02x; only standard requests are "
      "implemented\n",
      MusbRequestTypeName (Setup.RequestType),
      (UINT32)Setup.Request
      );
    MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
    mEp0State = MusbEp0Idle;
    return;
  }

  switch (Setup.Request) {
    case USB_REQ_SET_ADDRESS:
      //
      // USB 2.0 9.4.6: the address takes effect after the status stage, so it
      // is only armed here and written when the status stage completes.
      //
      mEp0NewAddress     = (UINT8)(Setup.Value & A733_MUSB_FADDR_MASK);
      mEp0AddressPending = TRUE;
      MUSB_TRACE (
        "Musb: SET_ADDRESS answer: address %u armed, written when the status stage "
        "has completed; FADDR stays %u for now\n",
        (UINT32)mEp0NewAddress,
        (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG))
        );
      break;

    case USB_REQ_SET_CONFIG:
      if (Setup.Value > 1) {
        MusbStall (MusbAnswerStallConfig, (UINT32)Setup.Value);
        MUSB_TRACE (
          "Musb: SET_CONFIGURATION answer: STALL - config %u requested, only 0 and 1 "
          "exist here\n",
          (UINT32)Setup.Value
          );
        MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
        mEp0State = MusbEp0Idle;
        return;
      }

      mEp0NewConfig     = (UINT8)Setup.Value;
      mEp0ConfigPending = TRUE;
      //
      // The status stage is armed first and only then are the endpoints
      // programmed.  With EP0's RXPKTRDY still latched the core NAKs the
      // status token, and a host that never sees the status stage reports a
      // protocol error - so the acknowledgement cannot wait for the register
      // sequence below.  Programming them here rather than when the status
      // stage completes also keeps the host's first bulk OUT packet from
      // meeting a FIFO flush.
      //
      MUSB_TRACE (
        "Musb: SET_CONFIGURATION answer: config %u armed, status stage first, then "
        "the bulk endpoints are %a\n",
        (UINT32)mEp0NewConfig,
        (mEp0NewConfig == 1) ? "programmed from the served descriptor" : "left as they are"
        );
      MusbEp0ArmStatus ();
      if (mEp0NewConfig == 1) {
        MusbConfigureBulkEndpoints ();
      }

      return;

    case USB_REQ_SET_FEATURE:
    case USB_REQ_CLEAR_FEATURE:
      //
      // Device remote wake-up and endpoint halt are the only features a device
      // has to implement.  Remote wake-up is accepted and not used; a halt on a
      // bulk endpoint is applied so a host that clears one gets a clean slate.
      //
      if (Setup.Value == 0x01) {
        if ((Setup.RequestType & 0x1F) != USB_TARGET_DEVICE) {
          MusbStall (MusbAnswerStallFeature, (UINT32)Setup.Value);
          MUSB_TRACE (
            "Musb: %a answer: STALL - feature 1 (remote wake-up) with recipient %a\n",
            MusbRequestName (Setup.Request),
            MusbRequestTargetName (Setup.RequestType)
            );
          MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
          mEp0State = MusbEp0Idle;
          return;
        }

        MUSB_TRACE (
          "Musb: %a answer: device remote wake-up accepted and not used, status stage next\n",
          MusbRequestName (Setup.Request)
          );
      } else if (Setup.Value == 0x00) {
        if ((Setup.RequestType & 0x1F) != USB_TARGET_ENDPOINT) {
          MusbStall (MusbAnswerStallFeature, (UINT32)Setup.Value);
          MUSB_TRACE (
            "Musb: %a answer: STALL - feature 0 (endpoint halt) with recipient %a\n",
            MusbRequestName (Setup.Request),
            MusbRequestTargetName (Setup.RequestType)
            );
          MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
          mEp0State = MusbEp0Idle;
          return;
        }

        if ((Setup.Index & 0x7F) == mBulkIn.Number) {
          MusbSelectEp (mBulkIn.Number);
          MmioWrite16 (
            MusbReg (A733_MUSB_TXCSR_REG),
            A733_TXCSR_MODE
            | A733_TXCSR_CLRDATATOG
            | ((Setup.Request == USB_REQ_SET_FEATURE) ? A733_TXCSR_SENDSTALL : 0)
            );
        } else if ((Setup.Index & 0x7F) == mBulkOut.Number) {
          MusbSelectEp (mBulkOut.Number);
          MmioWrite16 (
            MusbReg (A733_MUSB_RXCSR_REG),
            (A733_RXCSR_CLRDATATOG
             | ((Setup.Request == USB_REQ_SET_FEATURE) ? A733_RXCSR_SENDSTALL : 0))
            & ~(UINT16)A733_RXCSR_ISO
            );
        }

        MusbSelectEp (0);
        MUSB_TRACE (
          "Musb: %a answer: endpoint 0x%02x %a, data toggle cleared\n",
          MusbRequestName (Setup.Request),
          (UINT32)Setup.Index,
          (Setup.Request == USB_REQ_SET_FEATURE) ? "halted" : "unhalted"
          );
      } else {
        MusbStall (MusbAnswerStallFeature, (UINT32)Setup.Value);
        MUSB_TRACE (
          "Musb: %a answer: STALL - feature selector %u is not one this device has\n",
          MusbRequestName (Setup.Request),
          (UINT32)Setup.Value
          );
        MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
        mEp0State = MusbEp0Idle;
        return;
      }

      break;

    case USB_REQ_SET_INTERFACE:
      if ((Setup.Index != 0) || (Setup.Value != 0)) {
        MusbStall (MusbAnswerStallInterface, (UINT32)Setup.Value);
        MUSB_TRACE (
          "Musb: SET_INTERFACE answer: STALL - interface %u alternate %u, this device "
          "has one interface at alternate 0\n",
          (UINT32)Setup.Index,
          (UINT32)Setup.Value
          );
        MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
        mEp0State = MusbEp0Idle;
        return;
      }

      MUSB_TRACE (
        "Musb: SET_INTERFACE answer: interface 0 alternate 0 accepted, status stage next\n"
        );
      break;

    case USB_REQ_GET_STATUS:
    case USB_REQ_GET_CONFIG:
    case USB_REQ_GET_INTERFACE:
      //
      // The three reads.  Each has a data phase in practice (MusbReplyToInRequest
      // answers it); a host that asks for zero bytes still gets a completed
      // control transfer rather than a stall.
      //
      MUSB_TRACE (
        "Musb: %a answer: zero-length read, status stage only\n",
        MusbRequestName (Setup.Request)
        );
      break;

    default:
      MusbStall (MusbAnswerStallRequest, (UINT32)Setup.Request);
      MUSB_TRACE (
        "Musb: SETUP answer: STALL - request 0x%02x (%a) is not implemented\n",
        (UINT32)Setup.Request,
        MusbRequestName (Setup.Request)
        );
      MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDRXPKTRDY | A733_CSR0_SENDSTALL);
      mEp0State = MusbEp0Idle;
      return;
  }

  MusbEp0ArmStatus ();
}

/**
  Finish a control transfer whose status stage has completed.

  This is where SET_ADDRESS and SET_CONFIGURATION take effect, which makes the
  three enumeration states visible in the log: Reset (bus reset), Addressed
  (address written) and Configured (endpoints live).
**/
STATIC
VOID
MusbEp0Complete (
  VOID
  )
{
  mEp0State = MusbEp0Idle;

  if (mEp0AddressPending) {
    mEp0AddressPending = FALSE;
    mAddress           = mEp0NewAddress;
    //
    // Written now, after the status stage has completed, and not while it was
    // still ahead: the core uses FADDR for every token it receives, so a write
    // before the status stage makes the core ignore that stage - the host then
    // reports "device not responding to setup address" and re-sends the
    // request.  The vendor stack defers the write the same way (its
    // __sunxi_udc_set_address runs from the next service pass, when EP0 has no
    // packet ready).
    //
    MmioWrite8 (MusbReg (A733_MUSB_FADDR_REG), mAddress);
    //
    // FADDR's bit 7 is "UPDATE" in the Mentor register model: a write with it
    // set asks the core to take the address immediately.  Whether this core
    // needs it is exactly what the two read-backs below answer - the log shows
    // the plain write, then the write with UPDATE, then what the core kept.
    //
    {
      UINT8  Plain;

      Plain = MmioRead8 (MusbReg (A733_MUSB_FADDR_REG));
      MmioWrite8 (MusbReg (A733_MUSB_FADDR_REG), (UINT8)(mAddress | A733_MUSB_FADDR_UPDATE));
      MusbRingAdd (
        MusbTagAddressed,
        (UINT32)mAddress,
        (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG))
        | ((UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)) << 8)
        );
      MUSB_TRACE (
        "Musb: Addressed: address %u written to FADDR (plain write reads 0x%02x), "
        "then written with UPDATE bit set (reads 0x%02x), status stage complete, "
        "CSR0=0x%04x\n",
        (UINT32)mAddress,
        (UINT32)Plain,
        (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG)),
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG))
        );
    }
  }

  if (mEp0ConfigPending) {
    mEp0ConfigPending = FALSE;
    mConfiguration    = mEp0NewConfig;
    if (mConfiguration == 1) {
      MusbRingAdd (MusbTagConfigured, (UINT32)mBulkIn.MaxPacket, (UINT32)mBulkOut.MaxPacket);
      MUSB_TRACE (
        "Musb: Configured: config 1, bulk IN 0x%02x (%u B), bulk OUT 0x%02x (%u B), "
        "status stage complete, CSR0=0x%04x\n",
        mBulkIn.Address,
        (UINT32)mBulkIn.MaxPacket,
        mBulkOut.Address,
        (UINT32)mBulkOut.MaxPacket,
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG))
        );
      //
      // Configured closes the window: the device has finished enumerating, so
      // nothing on the bus is waiting for a stage, and the trace recorded since
      // the first SETUP is printed now - once, in one place, off the hot path.
      //
      MusbQuietLeave ("configured");
    } else {
      MusbRingAdd (MusbTagUnconfigured, 0, 0);
      MUSB_TRACE (
        "Musb: Unconfigured (config 0), bulk endpoints left programmed\n"
        );
    }
  }
}

/**
  Print every change of CSR0, and say once when RXPKTRDY first appears.

  CSR0 is the only register that says whether the core is holding a packet for
  this driver, and the failure this watch exists for is "the host sends a
  SETUP and the driver never runs its SETUP path": either the bit never sets
  (the SIE did not hand the packet over) or it sets and something clears it
  before it is acted on.  A change line for every transition makes those two
  distinguishable, and the count of polls that saw RXPKTRDY - printed by the
  heartbeat - makes "seen once, then nothing" visible too.

  A change seen while the status stage is armed is also recorded as
  MusbTagStatusCsr, because that is the only evidence that the core advanced the
  stage by itself: DATAEND clearing there is the zero-length status packet
  having left the core, which is what "enumeration is past SET_ADDRESS" rests
  on.

  Reads only: the caller owns the write sequence.

  @param[in] Csr  CSR0 as just read, before the caller touches anything.
**/
STATIC
VOID
MusbEpWatch (
  IN UINT16  Csr
  )
{
  CHAR8   Names[96];
  UINT16  Diff;

  //
  // A status stage that advances on its own is the one thing the quiet window
  // must not swallow, so it is recorded before the gate below: TXPKTRDY
  // clearing in a data-IN stage means the host took the packet, and DATAEND
  // clearing in an armed status stage means the core handed the zero-length
  // packet over.  The state is the one the previous tick left, i.e. the stage
  // the change happened in - the arm itself is recorded by MusbEp0ArmStatus and
  // is not repeated here.
  //
  if (mEp0WatchValid && (Csr != mEp0WatchedCsr) && (mEp0State == MusbEp0Status)) {
    MusbRingAdd (MusbTagStatusCsr, (UINT32)mEp0WatchedCsr, (UINT32)Csr);
  }

  //
  // A CSR0 change line inside a control transfer costs that transfer 8 ms, so
  // while the console is quiet nothing is printed and the baseline is simply
  // kept current: the ring already carries the phases of the transfer, and the
  // first line after the window then describes only what came after it.
  //
  if (MusbQuiet ()) {
    mEp0WatchValid = TRUE;
    mEp0WatchedCsr = Csr;
    return;
  }

  if (!mEp0WatchValid) {
    mEp0WatchValid = TRUE;
    mEp0WatchedCsr = Csr;
    DEBUG ((
      DEBUG_INFO,
      "Musb: EP0 watch baseline at poll tick %u: CSR0=0x%04x COUNT0=0x%04x TXMAXP=0x%04x "
      "RXMAXP=0x%04x INTRTX=0x%04x INTRRX=0x%04x (state %a); every CSR0 change, the first "
      "packet it holds and the heartbeat's RXPKTRDY tally are printed from here on\n",
      mPollTicks,
      (UINT32)Csr,
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG)),
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXMAXP_REG)),
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXMAXP_REG)),
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRTX_REG)),
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG)),
      MusbEp0StateName (mEp0State)
      ));
    return;
  }

  if (Csr == mEp0WatchedCsr) {
    return;
  }

  Diff = (UINT16)(Csr ^ mEp0WatchedCsr);
  MusbDiffNames (Diff, mMusbCsr0Name, 9, Names, sizeof (Names));
  DEBUG ((
    DEBUG_INFO,
    "Musb: EP0 CSR0 0x%04x -> 0x%04x (diff 0x%04x moving %a) at poll tick %u, "
    "state %a\n",
    (UINT32)mEp0WatchedCsr,
    (UINT32)Csr,
    (UINT32)Diff,
    Names,
    mPollTicks,
    MusbEp0StateName (mEp0State)
    ));

  mEp0WatchedCsr = Csr;

  if ((Csr & A733_CSR0_RXPKTRDY) != 0) {
    DEBUG ((
      DEBUG_INFO,
      "Musb: EP0 CSR0 now holds a packet for this driver (CSR0=0x%04x COUNT0=0x%04x)\n",
      (UINT32)Csr,
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG))
      ));
  }
}

/**
  Print INTRTX/INTRRX when they change.

  The endpoint interrupt latches are set by the core whether or not the
  endpoint's mask bit is set (this driver masks every endpoint it does not run on
  the interrupt), so they are a second, independent report of "the SIE delivered
  something": INTRRX bit 0 is endpoint 0's OUT half, i.e. exactly the SETUP path.
  If that bit sets while CSR0.RXPKTRDY never does, the packet arrived and the
  status bit is what this driver is misreading; if it never sets, the SIE never
  accepted the packet at all.  Nothing here clears them - the write would be W1C
  and would destroy the evidence.

  Reads only.
**/
STATIC
VOID
MusbEpIntrWatch (
  VOID
  )
{
  UINT16  Tx;
  UINT16  Rx;

  //
  // Same reason as MusbEpWatch: nothing may print inside a control transfer.
  // The latches keep their state, so the line after the window still shows the
  // change this reading would have described.
  //
  if (MusbQuiet ()) {
    return;
  }

  Tx = MmioRead16 (MusbReg (A733_MUSB_INTRTX_REG));
  Rx = MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG));

  if (!mEpWatchValid) {
    mEpWatchValid     = TRUE;
    mEpWatchedIntrTx  = Tx;
    mEpWatchedIntrRx  = Rx;
    DEBUG ((
      DEBUG_INFO,
      "Musb: endpoint interrupt baseline at poll tick %u: INTRTX=0x%04x INTRRX=0x%04x "
      "(EP0 TX latch=%u, EP0 RX latch=%u) - masked, so these only report\n",
      mPollTicks,
      (UINT32)Tx,
      (UINT32)Rx,
      (UINT32)((Tx & BIT0) != 0),
      (UINT32)((Rx & BIT0) != 0)
      ));
    return;
  }

  if ((Tx == mEpWatchedIntrTx) && (Rx == mEpWatchedIntrRx)) {
    return;
  }

  //
  // Only endpoint 0's own latch bit is a reason to print.  The bulk endpoints'
  // bits change once per packet - and when they are interrupt-driven the handler
  // is what clears them - so a line per change here would either be a line per
  // packet (8 ms on this console, which is what held the old polled data path at
  // 50 KB/s) or a report about a latch the data path owns.  The whole register is
  // still what the line carries, and the baseline is kept current, so nothing is
  // hidden: what changes is only when it is worth a line.
  //
  if ((((Tx ^ mEpWatchedIntrTx) | (Rx ^ mEpWatchedIntrRx)) & MUSB_INTR_EP0_BIT) == 0) {
    mEpWatchedIntrTx = Tx;
    mEpWatchedIntrRx = Rx;
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: endpoint interrupts at poll tick %u: INTRTX 0x%04x -> 0x%04x, "
    "INTRRX 0x%04x -> 0x%04x (EP0 TX latch=%u EP0 RX latch=%u)\n",
    mPollTicks,
    (UINT32)mEpWatchedIntrTx,
    (UINT32)Tx,
    (UINT32)mEpWatchedIntrRx,
    (UINT32)Rx,
    (UINT32)((Tx & BIT0) != 0),
    (UINT32)((Rx & BIT0) != 0)
    ));

  mEpWatchedIntrTx = Tx;
  mEpWatchedIntrRx = Rx;
}

/**
  Service endpoint 0: bus-level handshakes, new SETUP packets, and the two
  control-transfer stages that advance on their own.

  The order is the one both implementations that enumerate on this board use,
  and it matters: the packet latch is looked at before anything is written, a
  SETUP is always serviced even while a status stage is still running, and the
  two stages that advance on their own are only touched once CSR0.DATAEND has
  cleared - with DATAEND still latched the status stage has not gone through,
  and nothing this function could do would advance it.

  SETUPEND is the one bit that cannot be handled by a plain acknowledge, and the
  order around it is the vendor device stack's (usb_base_platform.c:1176-1182):
  acknowledge SETUPEND *while keeping RXPKTRDY as it was read*, then read the
  request, then let the normal completion acknowledge RXPKTRDY.  SETUPEND with
  RXPKTRDY is "a new SETUP arrived", not a fault; SETUPEND alone is the host
  abandoning the previous transfer.  See the two branches below for why the
  write has to preserve bit 0 on this core.
**/
STATIC
VOID
MusbEp0Service (
  VOID
  )
{
  UINT16  Csr;

  MusbSelectEp (0);
  Csr = MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG));
  MusbEpWatch (Csr);
  MusbEpIntrWatch ();

  if ((Csr & A733_CSR0_SENTSTALL) != 0) {
    MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), (UINT16)(Csr & ~A733_CSR0_SENTSTALL));
    mEp0State = MusbEp0Idle;
    Csr       = MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG));
  }

  if ((Csr & A733_CSR0_SETUPEND) != 0) {
    if ((Csr & A733_CSR0_RXPKTRDY) != 0) {
      UINT16  AfterAck;

      //
      // A new SETUP is in the FIFO and the SETUPEND latch came with it.  This is
      // the normal shape of the first packet after a bus reset on this core, and
      // it is not an error: SETUPEND is acknowledged first and the request is
      // read straight after.
      //
      // The write keeps RXPKTRDY at the value it was read with, which is the
      // whole point.  On this core a 0 in bit 0 clears RXPKTRDY (A733Musb.h),
      // so a bare SVDSETUPEND write (0x0080) acknowledges SETUPEND and throws
      // the SETUP away in the same stroke - CSR0 then reads 0x0000, the packet
      // is never read, and the host reports "Device not responding to setup
      // address" for a SETUP the device did receive.  The vendor device stack
      // does the same read-modify-write (__USBC_Dev_ep0_ClearSetupEnd ->
      // USBC_REG_set_bit_w, which is "read | (1 << bp), write back": usbc_dev.c
      // 169-172) and calls it before it tests RXPKTRDY, at the top of
      // ep0_recv_op (sunxi_usb_src/usb_base_platform.c:1176-1182).  Everything
      // else read from CSR0 is deliberately not written back: a 1 in DATAEND
      // arms a status stage and a 1 in TXPKTRDY re-sends a packet, and a new
      // SETUP has just made both meaningless.
      //
      MmioWrite16 (
        MusbReg (A733_MUSB_TXCSR_REG),
        (UINT16)((Csr & A733_CSR0_RXPKTRDY) | A733_CSR0_SVDSETUPEND)
        );
      AfterAck = MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG));
      MusbRingAdd (
        MusbTagSetupEndNew,
        (UINT32)Csr,
        (UINT32)AfterAck | ((UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG)) << 16)
        );
      MUSB_TRACE (
        "Musb: EP0 SETUP with SETUPEND latched: CSR0 0x%04x -> 0x%04x after acknowledging "
        "SETUPEND alone (COUNT0=%u, state %a) - RXPKTRDY %a, the request is read next\n",
        (UINT32)Csr,
        (UINT32)AfterAck,
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG)),
        MusbEp0StateName (mEp0State),
        ((AfterAck & A733_CSR0_RXPKTRDY) != 0)
          ? "survived the acknowledge"
          : "CLEARED by the acknowledge - the request is lost"
        );
      Csr = AfterAck;
    } else {
      //
      // SETUPEND on its own: the host ended the previous transfer early (USB 2.0
      // 8.5.3.4).  Nothing more will come for it, so acknowledging SETUPEND and
      // idling the endpoint is all there is to do.
      //
      MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_CSR0_SVDSETUPEND);
      mEp0State = MusbEp0Idle;
      Csr       = MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG));
      MusbRingAdd (MusbTagSetupEndOnly, (UINT32)Csr, 0);
      MUSB_TRACE (
        "Musb: EP0 SETUPEND without a packet (CSR0=0x%04x): the host abandoned the transfer "
        "in progress; acknowledged, endpoint idled\n",
        (UINT32)Csr
        );
    }
  }

  if ((Csr & A733_CSR0_RXPKTRDY) != 0) {
    if (mEp0RxPktTicks == 0) {
      //
      // The record that ends the argument this log exists for: a SETUP does
      // reach endpoint 0.  Made once, before the packet is read, so it cannot
      // be lost to a later failure in the request handler.
      //
      MusbRingAdd (
        MusbTagRxPktFirst,
        (UINT32)Csr,
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG))
        | ((UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG)) << 16)
        );
      MUSB_TRACE (
        "Musb: EP0 RXPKTRDY set for the first time at poll tick %u (CSR0=0x%04x "
        "COUNT0=0x%04x INTRRX=0x%04x): the SIE has handed endpoint 0 a packet\n",
        mPollTicks,
        (UINT32)Csr,
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG)),
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG))
        );
    }

    mEp0RxPktTicks++;
    MusbHandleSetup ();
    return;
  }

  //
  // The status stage of the current transfer has not completed yet; the
  // enumerating device-mode driver returns here for the same reason
  // (musb_g_ep0_irq's DATAEND guard).  A new SETUP that arrived in the
  // meantime was already serviced above, so nothing is delayed by this.
  //
  if ((Csr & A733_CSR0_DATAEND) != 0) {
    return;
  }

  switch (mEp0State) {
    case MusbEp0Tx:
      //
      // The host took the packet we loaded (TXPKTRDY clears on its own), so
      // the next one goes in.
      //
      if ((Csr & A733_CSR0_TXPKTRDY) == 0) {
        MusbEp0SendNext ();
      }

      break;

    case MusbEp0Status:
      //
      // DATAEND cleared above by the guard, i.e. the status stage has been
      // through: the request is complete.
      //
      MusbEp0Complete ();
      break;

    default:
      break;
  }
}

/**
  The bit of INTRTX/INTRRX that belongs to an endpoint, or 0 for "none".

  Endpoint 0 is never one of the two: a mask bit of 0 in INTRTXE/INTRRXE is what
  keeps the control path out of the handler, and the control path's poll is the
  only owner of the quiet window, the enumeration ring and the status stage.

  @param[in] Ep  Endpoint number, 0 when no endpoint is in use.

  @retval The endpoint's mask bit in INTRTX/INTRRX, or 0.
**/
STATIC
UINT16
MusbIntrEpBit (
  IN UINT8  Ep
  )
{
  return (Ep == 0) ? 0 : (UINT16)(1U << Ep);
}

/**
  Write the two endpoint interrupt masks the data path needs.

  Only the two bulk endpoints are unmasked: the IN endpoint's bit in INTRTXE and
  the OUT endpoint's bit in INTRRXE.  INTRUSBE stays 0, because INTRUSB latches
  the bus events whether or not it is unmasked and its RESET latch is what the
  poll's rewind acts on - a handler cannot run that rewind.  The EP0 bits stay 0
  for the same kind of reason: one owner per endpoint state machine.

  Called when the interrupt is claimed, again after Start(), which may have
  reprogrammed the FIFO map for other endpoint numbers, and from the poll when a
  source the handler had to mask is recovered.  Writing the whole mask is
  deliberate: it is the one description of "which endpoints are interrupt-driven"
  and each call restores exactly that.
**/
STATIC
VOID
MusbIntrMaskWrite (
  VOID
  )
{
  MmioWrite16 (MusbReg (A733_MUSB_INTRTXE_REG), MusbIntrEpBit (mBulkIn.Number));
  MmioWrite16 (MusbReg (A733_MUSB_INTRRXE_REG), MusbIntrEpBit (mBulkOut.Number));
}

/**
  Publish one packet to the staging ring.

  One description of "a packet is now the consumer's": the bytes and the length
  are written before the head that makes them visible, and the compiler is not
  relied on to keep that order.
**/
STATIC
VOID
MusbBulkOutPublish (
  IN UINT8   Slot,
  IN UINT16  Length
  )
{
  mRxRingLength[Slot] = Length;
  MemoryFence ();
  mRxRingHead = ((UINT32)Slot + 1U) & MUSB_RX_RING_MASK;
  mRxStaged++;
}

/**
  How many packet slots the staging ring can still take.

  One slot is always left empty, which is what tells a full ring from an empty
  one, so the answer is 0..MUSB_RX_RING_SLOTS-1.  The producer is the only writer
  of the head, so the answer cannot grow stale within one staging call.
**/
STATIC
UINTN
MusbBulkOutFreeSlots (
  VOID
  )
{
  return ((mRxRingTail - mRxRingHead - 1U) & MUSB_RX_RING_MASK);
}

#if MUSB_RX_TIMING
/**
  Convert counter ticks to microseconds times a hundred, for two printed
  decimals.

  @param[in] Ticks      The interval, in counter ticks.
  @param[in] Frequency  Counter ticks per second.

  @return The interval in hundredths of a microsecond.
**/
STATIC
UINT32
MusbRxTimingUs100 (
  IN UINT64  Ticks,
  IN UINT64  Frequency
  )
{
  return (UINT32)((Ticks * 100000000ULL) / Frequency);
}

/**
  Account one handler entry that staged packets: its handler interval, the FIFO
  interval inside it, and the gap that preceded it.

  The gap is measured from the end of the previous recorded entry, so handler +
  gap is exactly one packet's period at 1:1 - which is the whole point of
  splitting it this way: the sum cannot drift from what the byte rate says.  A
  gap longer than MUSB_RX_TIMING_GAP_MAX_US is a host with nothing to send (the
  end of a run, a phase boundary) and is counted, not averaged, so it cannot be
  read as interrupt latency.

  Called from the handler only, after the packet is acknowledged and its latch
  cleared.  No register is read and nothing is allocated: it is five adds and a
  compare against the 128 port words it describes.

  @param[in] End      Counter at the end of the drain and its RXCSR acknowledge.
  @param[in] Packets  Packets this entry staged (one, unless the host ran ahead).
**/
STATIC
VOID
MusbRxTimingRecord (
  IN UINT64  End,
  IN UINTN   Packets
  )
{
  UINT64  GapTicks;

  if ((Packets == 0) || (End < mRxTimingIrqStart)) {
    return;
  }

  mRxTimingHandlerTicks += End - mRxTimingIrqStart;
  mRxTimingFifoTicks   += mRxTimingFifoRun;
  mRxTimingFifoRun      = 0;
  mRxTimingPackets     += (UINT32)Packets;
  mRxTimingEntries++;

  if (mRxTimingEndValid) {
    //
    // Unsigned subtraction: a counter that wrapped between the two stamps
    // yields the true delta, and a hardware counter at these intervals does
    // not wrap between one packet and the next in any case.
    //
    GapTicks = mRxTimingIrqStart - mRxTimingEnd;
    if ((mRxTimingGapMaxTicks == 0) || (GapTicks <= mRxTimingGapMaxTicks)) {
      mRxTimingGapTicks += GapTicks;
      mRxTimingGaps++;
    } else {
      mRxTimingGapsClipped++;
    }
  }

  mRxTimingEnd      = End;
  mRxTimingEndValid = TRUE;
}

/**
  Print what one 512-byte packet costs on the FIFO path, and where it goes.

  The trigger is a quiet one: the packet counters have not moved for
  MUSB_RX_TIMING_REPORT_IDLE_TICKS ticks and the ring is empty, which is a host
  between phases and never a transfer in progress, so the ~8 ms a line costs on
  this console cannot land inside one.  Each line covers the packets since the
  previous line, so a session with several phases prints one line per phase.

  The two ways to read it:

    - handler small, gap large -> the endpoint is idle between packets, and the
      part of the gap that is not the poll's delivery is the host and the
      interrupt's latency (the GIC acknowledge and dispatch are inside it,
      nothing in this driver can be stamped before them).  If that is where the
      time is, the answer is to stop using the interrupt in the data phase - not
      to read the FIFO differently;
    - handler large and most of it inside MusbFifoRead -> the 128 port words are
      the wall, and the only lever left on them is the memory attribute of the
      page the FIFO port is mapped with.

  On this board the first reading is the true one (26 us of handler, 140 us of
  gap), so the line is a tool that has already answered its question; the numbers
  are in MUSB_RX_TIMING's comment and it is that comment the switch is set from.

  Called from the poll only: it prints, and the handler may not.
**/
STATIC
VOID
MusbBulkOutTimingReport (
  VOID
  )
{
  EFI_TPL  OldTpl;
  UINT64   ReadStart;
  UINT64   ReadTicks;
  UINT64   HandlerTicks;
  UINT64   FifoTicks;
  UINT64   GapTicks;
  UINT64   Frequency;
  UINT32   Entries;
  UINT32   Packets;
  UINT32   Gaps;
  UINT32   Clipped;
  UINT32   Polled;
  UINT32   Delivered;
  UINT64   DeliverTicks;
  UINT32   HandlerUs100;
  UINT32   FifoUs100;
  UINT32   GapUs100;
  UINT32   TotalUs100;
  UINT32   ReadUs100;
  UINT32   DeliverUs100;
#if MUSB_RX_SPIN_DRAIN
  UINT32   SpinPackets;
  UINT32   Slices;
  UINT32   Hits;
  UINT64   SpinTicks;
  UINT32   SpinPct100;
  UINT32   SpinUs100;
#endif

  if (MusbQuiet () || (mRxRingHead != mRxRingTail) || mRxHeld ||
      ((mRxTimingEntries == 0) && (mRxTimingPollPackets == 0)))
  {
    mRxTimingReportIdle = 0;
    return;
  }

  if (++mRxTimingReportIdle < MUSB_RX_TIMING_REPORT_IDLE_TICKS) {
    return;
  }

  Frequency = mRxTimingFrequency;
  if (Frequency == 0) {
    Frequency = GetPerformanceCounterProperties (NULL, NULL);
    if (Frequency == 0) {
      return;
    }

    mRxTimingFrequency   = Frequency;
    mRxTimingGapMaxTicks = (Frequency / 1000000ULL) * MUSB_RX_TIMING_GAP_MAX_US;
  }

  //
  // Snapshot and clear at TPL_HIGH: the handler adds to these between two ticks,
  // and half of a 64-bit sum read while it is being added would print an average
  // that describes nothing.  The cost of a stamp is measured here too - outside
  // the data path - so the line carries its own overhead rather than assuming it
  // under 0.1 us.
  //
  OldTpl    = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  ReadStart = GetPerformanceCounter ();
  ReadTicks = GetPerformanceCounter () - ReadStart;

  HandlerTicks = mRxTimingHandlerTicks;
  FifoTicks    = mRxTimingFifoTicks;
  GapTicks     = mRxTimingGapTicks;
  Entries      = mRxTimingEntries;
  Packets      = mRxTimingPackets;
  Gaps         = mRxTimingGaps;
  Clipped      = mRxTimingGapsClipped;
  Polled       = mRxTimingPollPackets;
  Delivered    = mRxTimingDelivered;
  DeliverTicks = mRxTimingDeliverTicks;
#if MUSB_RX_SPIN_DRAIN
  SpinPackets = mRxSpinPackets;
  Slices      = mRxSpinSlices;
  Hits        = mRxSpinHits;
  SpinTicks   = mRxSpinTicks;
#endif

  mRxTimingHandlerTicks = 0;
  mRxTimingFifoTicks    = 0;
  mRxTimingGapTicks     = 0;
  mRxTimingEntries      = 0;
  mRxTimingPackets      = 0;
  mRxTimingGaps         = 0;
  mRxTimingGapsClipped  = 0;
  mRxTimingPollPackets  = 0;
  mRxTimingFifoRun      = 0;
  mRxTimingEndValid     = FALSE;
  mRxTimingDeliverTicks = 0;
  mRxTimingDelivered    = 0;
#if MUSB_RX_SPIN_DRAIN
  mRxSpinPackets = 0;
  mRxSpinSlices  = 0;
  mRxSpinHits    = 0;
  mRxSpinTicks   = 0;
#endif

  gBS->RestoreTPL (OldTpl);

  mRxTimingReportIdle = 0;

  if ((Entries == 0) || (Packets == 0)) {
    //
    // The poll moved every packet: there is no handler interval to average, and
    // saying so once is the reading that keeps a "slow" run from being blamed on
    // the FIFO reads when the interrupt never carried it at all.
    //
    if (!mRxTimingPollWarned && (Polled != 0)) {
      mRxTimingPollWarned = TRUE;
      DEBUG ((
        DEBUG_WARN,
        "Musb: bulk OUT timing: %u packet(s) staged by the poll and none by the handler; the "
        "interrupt is not carrying the data path, so there is no per-packet split to report\n",
        Polled
        ));
    }

    return;
  }

  if (Entries < MUSB_RX_TIMING_MIN_PACKETS) {
    //
    // Too short to average - a fastboot command phase is one or two packets, and
    // their interval is a host round trip, not this path's cost.  Dropped, and
    // not printed: the line costs ~8 ms on this console, which is 50 packets.
    //
    return;
  }

  //
  // Per packet for the interval the handler covers (it can stage several in one
  // entry), per gap sample for the gap, and per packet for the period the two add
  // up to.  At 1:1 - which is every fastboot download packet unless the spin
  // takes one - all three are the same packet.
  //
  // The total is divided by the packets and not by the entries because the spin
  // is what breaks the 1:1 the line was written for: an entry that spun for the
  // next packet covers that packet inside its own handler interval, so the
  // entries count the periods while the packets count what was drained in them,
  // and (handler + gap) / packets is the per-packet period of a run either way.
  // The time spent spinning is part of the handler interval and is reported on
  // its own, so handler - spin is the drain cost the line carried before.
  //
  HandlerUs100 = MusbRxTimingUs100 (HandlerTicks / Packets, Frequency);
  FifoUs100    = MusbRxTimingUs100 (FifoTicks / Packets, Frequency);
  GapUs100     = (Gaps == 0) ? 0 : MusbRxTimingUs100 (GapTicks / Gaps, Frequency);
  TotalUs100   = MusbRxTimingUs100 ((HandlerTicks + GapTicks) / Packets, Frequency);
  ReadUs100    = MusbRxTimingUs100 (ReadTicks, Frequency);
  DeliverUs100 = (Delivered == 0) ? 0 : MusbRxTimingUs100 (DeliverTicks / Delivered, Frequency);
#if MUSB_RX_SPIN_DRAIN
  SpinPct100 = (Packets + Polled == 0) ? 0 :
               (UINT32)(((UINT64)SpinPackets * 10000ULL) / (UINT64)(Packets + Polled));
  SpinUs100  = (SpinPackets == 0) ? 0 : MusbRxTimingUs100 (SpinTicks / SpinPackets, Frequency);
#endif

  DEBUG ((
    DEBUG_INFO,
    "Musb: bulk OUT FIFO per packet: total %u.%02u us (handler %u.%02u us, of which FIFO "
    "%u.%02u us; gap %u.%02u us, and %u.%02u of that is the poll delivering), %u packet(s) in "
    "%u handler(s), %u staged by the poll, %u gap(s) past %u us left out"
#if MUSB_RX_SPIN_DRAIN
    "; %u of %u taken by the spin (%u.%02u%%), %u.%02u us of spin per spun packet"
#endif
    "; counter %Lu Hz, a stamp pair (two reads) %u.%02u us\n",
    TotalUs100 / 100U,
    TotalUs100 % 100U,
    HandlerUs100 / 100U,
    HandlerUs100 % 100U,
    FifoUs100 / 100U,
    FifoUs100 % 100U,
    GapUs100 / 100U,
    GapUs100 % 100U,
    DeliverUs100 / 100U,
    DeliverUs100 % 100U,
    Packets,
    Entries,
    Polled,
    Clipped,
    (UINT32)MUSB_RX_TIMING_GAP_MAX_US,
#if MUSB_RX_SPIN_DRAIN
    SpinPackets,
    Packets + Polled,
    SpinPct100 / 100U,
    SpinPct100 % 100U,
    SpinUs100 / 100U,
    SpinUs100 % 100U,
#endif
    Frequency,
    ReadUs100 / 100U,
    ReadUs100 % 100U
    ));

#if MUSB_RX_SPIN_DRAIN
  //
  // The spin's own bounds and hit rate, which is what tells the two readings
  // above apart.  A slice count that is all misses with no packets taken says
  // the gap is the host's microframe and not interrupt latency, and that the
  // spin is only spending TPL; slices that find a packet are the case the spin
  // exists for - the host was offering it while the interrupt had not arrived,
  // and the drain took it instead of NAKing it.
  //
  DEBUG ((
    DEBUG_INFO,
    "Musb: bulk OUT spin: %u slice(s) run and %u found a packet; %u us per slice, at most %u "
    "slice(s) per call, and only while a packet was drained within the last %u us\n",
    Slices,
    Hits,
    (UINT32)MUSB_RX_SPIN_SLICE_US,
    (UINT32)MUSB_RX_SPIN_SLICES,
    (UINT32)MUSB_RX_ACTIVE_WINDOW_US
    ));
#endif
}
#endif

#if MUSB_RX_SPIN_DRAIN
/**
  Counter ticks per microsecond, read once.

  The timing line keeps its own copy for the arithmetic it prints; this one is
  for the spin, which runs when MUSB_RX_TIMING is off as well.  Read lazily
  because the first packet can be drained before any line has printed the
  counter's frequency, and a zero return is retried rather than cached: a
  frequency that could not be read yet must not turn the spin off for the boot.

  @return Ticks per microsecond, or 0 if the counter's frequency is unknown.
**/
STATIC
UINT64
MusbSpinTicksPerUs (
  VOID
  )
{
  if (mRxSpinTicksPerUs == 0) {
    mRxSpinTicksPerUs = GetPerformanceCounterProperties (NULL, NULL) / 1000000ULL;
  }

  return mRxSpinTicksPerUs;
}

/**
  Whether the bulk-OUT path may spin for the next packet: a transfer is live,
  i.e. a packet was drained within MUSB_RX_ACTIVE_WINDOW_US.

  This is read at TPL_HIGH and does nothing but one counter read, so a quiet line
  costs one read and not a slice.  A window that has passed ends the spin for
  good; the next packet starts a new one, because it is drained by the handler or
  by the poll and stamps mRxLastPacket again.

  @retval TRUE   The last packet is recent enough to wait for the next one.
  @retval FALSE  The line is idle, or the counter's frequency is unknown.
**/
STATIC
BOOLEAN
MusbBulkOutSpinActive (
  VOID
  )
{
  UINT64  TicksPerUs;

  if (mRxLastPacket == 0) {
    return FALSE;
  }

  TicksPerUs = MusbSpinTicksPerUs ();
  if (TicksPerUs == 0) {
    return FALSE;
  }

  //
  // Unsigned subtraction: the window is 2 ms and the counter wraps in decades,
  // so a wrapped pair still yields the true distance.
  //
  return (BOOLEAN)((GetPerformanceCounter () - mRxLastPacket) <=
                   (TicksPerUs * MUSB_RX_ACTIVE_WINDOW_US));
}

/**
  Wait for one packet in one bounded slice, at the FIFO.

  Called with the bulk-OUT endpoint selected, from inside the staging loop, at
  whatever TPL that loop runs at - which is TPL_HIGH in both callers, so this is
  the one place in the driver where waiting happens with every other interrupt on
  the machine held.  MUSB_RX_SPIN_SLICES of these per staging call is the bound.

  The packet is not read here: an RXPKTRDY seen by this function is drained by
  the caller's normal path, so the framing rules - one slot per chunk, one
  acknowledgement per burst, a short packet ending a delivery - are the ones that
  already exist and are not restated here.

  @retval TRUE   RXPKTRDY is set: the caller drains it.
  @retval FALSE  The slice expired with the FIFO still empty.
**/
STATIC
BOOLEAN
MusbBulkOutSpinSlice (
  VOID
  )
{
  UINT64  TicksPerUs;
  UINT64  SliceTicks;
  UINT64  Start;
  UINTN   Index;
#if MUSB_RX_TIMING
  UINT64  Elapsed;
#endif

  TicksPerUs = MusbSpinTicksPerUs ();
  if (TicksPerUs == 0) {
    return FALSE;
  }

  SliceTicks = TicksPerUs * MUSB_RX_SPIN_SLICE_US;
  Start      = GetPerformanceCounter ();

  for (Index = 0; ; Index++) {
    if ((Index & (MUSB_RX_SPIN_POLL_STRIDE - 1U)) == 0) {
      if ((MmioRead16 (MusbReg (A733_MUSB_RXCSR_REG)) & A733_RXCSR_RXPKTRDY) != 0) {
#if MUSB_RX_TIMING
        Elapsed = GetPerformanceCounter () - Start;
        mRxSpinSlices++;
        mRxSpinHits++;
        mRxSpinTicks += Elapsed;
#endif
        return TRUE;
      }
    }

    if ((GetPerformanceCounter () - Start) >= SliceTicks) {
      break;
    }
  }

#if MUSB_RX_TIMING
  mRxSpinSlices++;
  mRxSpinTicks += GetPerformanceCounter () - Start;
#endif

  return FALSE;
}
#endif

/**
  Move packets out of the bulk OUT FIFO and into the staging ring.

  The FIFO handling is the polled driver's, unchanged: COUNT, then the bytes,
  then RXCSR with RXPKTRDY, SENTSTALL and OVERRUN written as zeros, which is how
  this core acknowledges a packet.  One description of what an acknowledged
  packet is, used by both callers.

  The bulk-OUT FIFO is programmed one packet deep (see MusbFifoLayout: EP2 has
  the 4096 bytes of space a deeper FIFO would need, and the measurement said the
  depth bought nothing, so 512 B is what is selected).  A deeper FIFO is what the
  burst path below is for, and it stays because it costs nothing when COUNT
  describes one packet and is correct if a future layout ever reports more: COUNT
  is the bytes waiting in the FIFO, and while bytes remain they are copied out in
  chunks of at most wMaxPacketSize, one chunk per ring slot and one length per
  slot, because a FIFO holding several packets is the packets' bytes one after
  another with no framing between them:

    - the host sends every packet of a run at exactly wMaxPacketSize, so a
      512-byte chunk is a packet boundary by construction, and
    - only the last packet of a run is shorter, so a partial chunk is that
      packet - a short packet, which ends the burst (and, for the transport, the
      delivery chunk: see MusbBulkOutDeliver).

  The endpoint is acknowledged once, after the whole burst, which is correct
  because RXPKTRDY is one bit for "the FIFO's packet sequencer has something":
  the write that clears it (RXCSR's ACK mask written as zeros) retires everything
  drained so far.  Nothing is read until the ring is known to have a slot for
  every chunk of the burst, so a burst is either taken whole and acknowledged, or
  not touched at all - a half-read burst that could not be acknowledged would
  leave the FIFO's framing and the core's RXPKTRDY disagreeing, and there is no
  reading that resolves that.

  With a single packet (COUNT <= wMaxPacketSize, which is every packet while the
  FIFO is one packet deep) the path is exactly the one this function has always
  had: one slot, one length, one acknowledgement.

  Two callers, and the difference is why the bulk path is no longer one packet
  per tick.  The handler stages everything the FIFO holds in one entry, so a host
  that had several packets queued costs one interrupt; the poll stages a batch
  when no interrupt is in use, which is what lifts the poll-only case off one
  packet per tick - after the ACK the host's next packet follows within
  microseconds and this loop is still there to take it.

  A packet this function cannot take is left in the FIFO with RXPKTRDY intact:
  the endpoint then NAKs the host's next packet, which is the backpressure USB
  has for exactly this.  The handler's caller masks its source in that case (see
  MusbInterruptHandler), because a level line that is still asserted would
  re-enter the handler immediately.

  Spin, when the caller allows it (see MUSB_RX_SPIN_DRAIN): a loop that would
  have ended because the FIFO was empty instead waits for the next packet for up
  to MUSB_RX_SPIN_SLICES slices of MUSB_RX_SPIN_SLICE_US, as long as the last
  packet is recent.  Waiting is what the loop above already does with WaitUs, in
  one large step and without looking at the FIFO while it waits; the spin looks
  at RXCSR throughout, so it takes a packet the moment the SIE latches it rather
  than at the next interrupt entry - which is the point: the endpoint is never
  left holding a packet the host is asking about, so it never NAKs a transaction
  it could have accepted.  A slice that finds the FIFO non-empty drains it
  through the same code above and then re-checks, which is the case that takes a
  microframe out of the period instead of a packet out of the transfer.

  @param[in] Cap      FIFO readings to take at most.  A burst is one reading, so
                      this bounds the loop and not the packets: the ring's free
                      slots bound how much one reading can stage, and a reading
                      this function could not take still counts.
  @param[in] WaitUs   Microseconds to wait for the next packet once one has been
                      staged.  0 from the handler, which must not wait.
  @param[in] InIrq    TRUE when called from the handler: the TPL is already
                      TPL_HIGH, and nothing is printed.
  @param[in] Spin     TRUE when this call may spin for the next packet: set by
                      the handler always, and by the poll only while the
                      interrupt is not the one carrying the data path.
  @param[in] MayPrint TRUE when a line about an unusable packet is allowed.

  @retval The number of packets staged.
**/
STATIC
UINTN
MusbBulkOutStage (
  IN UINTN    Cap,
  IN UINTN    WaitUs,
  IN BOOLEAN  InIrq,
  IN BOOLEAN  Spin,
  IN BOOLEAN  MayPrint
  )
{
  EFI_TPL  OldTpl;
  UINTN    Chunk;
  UINTN    Oversized;
  UINTN    Packets;
  UINTN    Remaining;
#if MUSB_RX_SPIN_DRAIN
  UINTN    SpinUsed;
#endif
#if MUSB_RX_SPIN_DRAIN && MUSB_RX_TIMING
  UINTN    SpinStart;
#endif
  UINTN    Slot;
  UINTN    Staged;
  UINTN    Tries;
  UINTN    Waited;
  UINT16   Csr;
  UINT16   Count;
#if MUSB_RX_SPIN_DRAIN && MUSB_RX_TIMING
  BOOLEAN  Spun;
#endif

#if !MUSB_RX_SPIN_DRAIN
  //
  // The switch removes every use of it; the parameter stays so that both callers
  // read the same and the two builds differ only in this body.
  //
  (VOID)Spin;
#endif

  if ((mBulkOut.Number == 0) || (mBulkOut.MaxPacket == 0)) {
    return 0;
  }

  Staged    = 0;
  Waited    = 0;
  Oversized = 0;
#if MUSB_RX_SPIN_DRAIN
  SpinUsed  = 0;
#endif
#if MUSB_RX_SPIN_DRAIN && MUSB_RX_TIMING
  Spun      = FALSE;
  SpinStart = 0;
#endif

  if (InIrq) {
    OldTpl = TPL_HIGH_LEVEL;
  } else {
    //
    // The handler reads and acknowledges the same FIFO.  A packet read half way
    // through what the handler believes is one packet would hand the transport
    // bytes assembled from two, so the poll takes the endpoint for the length of
    // this loop.
    //
    OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  }

  for (Tries = 0; Tries < Cap; Tries++) {
    MusbSelectEp (mBulkOut.Number);
    Csr = MmioRead16 (MusbReg (A733_MUSB_RXCSR_REG));
    if ((Csr & A733_RXCSR_RXPKTRDY) == 0) {
#if MUSB_RX_SPIN_DRAIN
      //
      // Nothing to take yet, but the transfer may still be live: wait for the
      // packet here instead of ending the call and letting the interrupt bring
      // it.  A slice that finds one leaves RXPKTRDY set and falls through to the
      // drain below, so a packet caught by the spin is staged by the code above,
      // and the reading at the top of the next iteration - taken with no wait at
      // all - asks whether a second one arrived behind it.
      //
      // SpinUsed is the call's budget, not this reading's: a call that keeps
      // catching packets keeps waiting, and three slices is the whole of what it
      // may spend at TPL_HIGH, however many packets it takes.
      //
      while (Spin && (SpinUsed < MUSB_RX_SPIN_SLICES) && MusbBulkOutSpinActive ()) {
        SpinUsed++;

        if (!MusbBulkOutSpinSlice ()) {
          continue;
        }

#if MUSB_RX_TIMING
        if (!Spun) {
          Spun      = TRUE;
          SpinStart = Staged;
        }
#endif

        Csr = MmioRead16 (MusbReg (A733_MUSB_RXCSR_REG));
        break;
      }
#endif

      if ((Csr & A733_RXCSR_RXPKTRDY) == 0) {
        if ((Staged == 0) || (WaitUs == 0) || (Waited >= WaitUs)) {
          break;
        }

        MicroSecondDelay (MUSB_RX_POLL_STEP_US);
        Waited += MUSB_RX_POLL_STEP_US;
        continue;
      }
    }

#if MUSB_RX_SPIN_DRAIN
    //
    // A packet is in the FIFO with this endpoint selected: that is the stamp the
    // spin's window is measured from, whether the packet was the interrupt's or
    // the spin's.
    //
    mRxLastPacket = GetPerformanceCounter ();
#endif

    Count = MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG));
    Csr  &= (UINT16)~A733_RXCSR_ACK_MASK;

    if (Count > A733_MUSB_BULK_RX_FIFO_BYTES) {
      //
      // Larger than the FIFO itself, so this is not a burst of packets: the
      // count has no meaning this driver can act on, and flushing is what it has
      // always done with a reading it cannot frame.  Counted, not printed: this
      // can be called at TPL_HIGH, where a line would cost every interrupt on the
      // machine ~8 ms.  The poll prints it once it has the TPL back.
      //
      MmioWrite16 (MusbReg (A733_MUSB_RXCSR_REG), (UINT16)(Csr | A733_RXCSR_FLUSHFIFO));
      mRxDropped++;
      Oversized++;
      continue;
    }

    if (Count == 0) {
      //
      // No fastboot data: the protocol is length delimited and the host sends
      // exactly the bytes it announced, so an empty packet is acknowledged and
      // dropped rather than handed up as an empty command.
      //
      MmioWrite16 (MusbReg (A733_MUSB_RXCSR_REG), Csr);
      mRxDropped++;
      continue;
    }

    //
    // How many packets this one reading describes: one unless the host ran ahead
    // of the drain.  Since every packet of a run is exactly wMaxPacketSize, the
    // chunks below fall on packet boundaries and the last one is the short packet
    // that ends the run.
    //
    Packets = ((UINTN)Count + mBulkOut.MaxPacket - 1U) / mBulkOut.MaxPacket;

    if (Packets == 1) {
      //
      // The single-packet path, unchanged.
      //
      Slot = mRxRingHead;
      if (((Slot + 1U) & MUSB_RX_RING_MASK) == mRxRingTail) {
        //
        // No free slot: the packet stays where it is (see this function's head).
        // The poll drains the ring and comes back for it.
        //
        mRxHeld = TRUE;
        break;
      }

      MusbFifoRead (mBulkOut.Number, Count, &mRxRingData[Slot * A733_MUSB_BULK_MAX_PACKET]);
      MusbBulkOutPublish ((UINT8)Slot, Count);
      MmioWrite16 (MusbReg (A733_MUSB_RXCSR_REG), Csr);
      Staged++;
      continue;
    }

    //
    // A burst.  Every chunk needs a slot before the first byte is read, so that
    // the acknowledgement at the end of the burst always retires a FIFO this call
    // has emptied completely (see this function's head).
    //
    if (MusbBulkOutFreeSlots () < Packets) {
      mRxHeld = TRUE;
      break;
    }

    Remaining = Count;
    while (Remaining != 0) {
      Chunk = (Remaining > mBulkOut.MaxPacket) ? (UINTN)mBulkOut.MaxPacket : Remaining;
      Slot  = mRxRingHead;

      MusbFifoRead (mBulkOut.Number, Chunk, &mRxRingData[Slot * A733_MUSB_BULK_MAX_PACKET]);
      //
      // Each chunk is published with its own length, so everything above this
      // function - delivery, the coalescing of full packets, the command parser -
      // still sees packets and not a burst.
      //
      MusbBulkOutPublish ((UINT8)Slot, (UINT16)Chunk);
      Staged++;

      //
      // A partial chunk is a short packet, and a short packet is only ever the
      // last packet of the FIFO's contents, so the loop ends with it: it is what
      // exhausts COUNT, and it is what ends a delivery chunk one level up (see
      // MusbBulkOutDeliver).
      //
      Remaining -= Chunk;
    }

    MmioWrite16 (MusbReg (A733_MUSB_RXCSR_REG), Csr);
  }

  MusbSelectEp (0);

  if (!InIrq) {
    gBS->RestoreTPL (OldTpl);
  }

  if (MayPrint && (Oversized != 0)) {
    DEBUG ((
      DEBUG_WARN,
      "Musb: %u bulk OUT reading(s) exceeded the %u-byte RX FIFO and were flushed\n",
      (UINT32)Oversized,
      (UINT32)A733_MUSB_BULK_RX_FIFO_BYTES
      ));
  }

#if MUSB_RX_TIMING
  //
  // Packets the poll took rather than the handler: the timing line says so,
  // because a run the interrupt did not carry has no handler interval to
  // average and would otherwise look like a run with a very long gap.
  //
  if (!InIrq) {
    mRxTimingPollPackets += (UINT32)Staged;
  }
#endif

#if MUSB_RX_SPIN_DRAIN && MUSB_RX_TIMING
  //
  // Packets this call took only because it waited for them: everything staged
  // after the first slice that found a packet, which is the population the line's
  // spin share is computed from.  A call that spun and found nothing adds nothing
  // here but its slices, and the line shows both - that pair is what tells "the
  // spin is taking the packets the interrupt was late for" from "the spin waits
  // and nothing comes".
  //
  if (Spun) {
    mRxSpinPackets += (UINT32)(Staged - SpinStart);
  }
#endif

  return Staged;
}

/**
  Hand the staged packets to the transport's RX callback, MUSB_RX_DELIVER_SLOTS
  full packets per buffer.

  The interrupt cannot do this: a pool allocation, the callback and the event it
  signals are boot services, and the handler runs at TPL_HIGH.  So the poll does
  it, once per tick, and the ring is what lets one tick's delivery cover a tick's
  worth of traffic instead of one packet.

  A chunk is a run of consecutive full packets, so the transport sees the same
  byte stream it saw when every packet was its own buffer, with fewer allocations
  and fewer callback entries: its callback queues what it is given and its data
  phase copies a length, not a packet (see FastbootTransportUsb.c and
  AndroidFastbootApp.c AcceptData).  A short packet is never part of a run of more
  than itself, which is what keeps the command phase intact - the command parser
  takes one command per buffer and refuses anything longer than 64 bytes, and the
  host sends a command as a short packet.

  The slots are released before the callback runs, so a callback that takes its
  time cannot make the handler see a full ring; the bytes have been copied out by
  then, and the slots are only reused once the producer has wrapped all the way
  round.

  @retval The number of packets delivered, i.e. drained from the ring.
**/
STATIC
UINTN
MusbBulkOutDeliver (
  VOID
  )
{
  UINTN   ChunkLength;
  UINTN   Count;
  UINTN   Index;
  UINTN   Packet;
  UINTN   Run;
  UINTN   Slot;
  UINT8   *Buffer;
#if MUSB_RX_TIMING
  UINT64  Start;
#endif

#if MUSB_RX_TIMING
  //
  // Delivery runs in the poll, i.e. between two handler entries, so its cost is
  // inside the timing line's gap.  Measured separately for exactly that reason:
  // a gap this accounts for is the machine's own work and not interrupt latency.
  //
  Start = GetPerformanceCounter ();
#endif

  //
  // One snapshot of the producer's index: the ring only grows from here, and a
  // loop over a count it re-read from a live host would never end.
  //
  MemoryFence ();
  Count = (mRxRingHead - mRxRingTail) & MUSB_RX_RING_MASK;

  for (Index = 0; Index < Count; Index += Run) {
    Slot = mRxRingTail;
    Run  = 1;

    //
    // Extend the run over the packets behind it, and only over full ones: a
    // short packet ends a chunk, so a chunk is 1..MUSB_RX_DELIVER_SLOTS full
    // packets, or exactly one packet of any other length.  The slots of a run are
    // adjacent in mRxRingData because the run stops at the wrap, which is what
    // makes the copy below a single CopyMem.
    //
    if (mRxRingLength[Slot] == mBulkOut.MaxPacket) {
      while ((Run < MUSB_RX_DELIVER_SLOTS) &&
             ((Index + Run) < Count) &&
             ((Slot + Run) < MUSB_RX_RING_SLOTS) &&
             (mRxRingLength[Slot + Run] == mBulkOut.MaxPacket))
      {
        Run++;
      }
    }

    ChunkLength = 0;
    for (Packet = 0; Packet < Run; Packet++) {
      ChunkLength += mRxRingLength[Slot + Packet];
    }

    Buffer = NULL;
    if ((mRxCallback != NULL) && (ChunkLength != 0)) {
      Buffer = AllocatePool (ChunkLength);
      if (Buffer != NULL) {
        CopyMem (Buffer, &mRxRingData[Slot * A733_MUSB_BULK_MAX_PACKET], ChunkLength);
      }
    }

    mRxRingTail = (Slot + Run) & MUSB_RX_RING_MASK;

    if (Buffer == NULL) {
      mRxDropped += Run;
      continue;
    }

    mRxDelivered += Run;
    mRxCallback (ChunkLength, Buffer);
  }

#if MUSB_RX_TIMING
  if (Count != 0) {
    mRxTimingDeliverTicks += GetPerformanceCounter () - Start;
    mRxTimingDelivered    += (UINT32)Count;
  }
#endif

  return Count;
}

/**
  Drop what is staged, for a bus reset.

  A reset ends the session the staged packets belong to, exactly like the FIFO
  flush the reset path already does: handing them to the transport afterwards
  would present the next session's command parser with the previous session's
  data.

  Called from the poll's reset branch, which is the only reset handler.
**/
STATIC
VOID
MusbBulkOutDiscardStaged (
  VOID
  )
{
  mRxDropped += (mRxRingHead - mRxRingTail) & MUSB_RX_RING_MASK;
  mRxRingTail = mRxRingHead;
  mRxHeld     = FALSE;
}

/**
  Service the bulk data path for one poll tick.

  Delivery first, then whatever the FIFO holds, then delivery again: one tick
  hands over everything staged before it and everything it can stage, so a full
  ring costs a retry and not a second tick.

  With the interrupt claimed the FIFO is the handler's and this is only the
  recovery path for a packet a full ring left behind - it does not wait for
  traffic the interrupt will announce anyway.  Without it, this is the data path
  and it batches (see MusbBulkOutStage).
**/
STATIC
VOID
MusbBulkOutService (
  VOID
  )
{
  UINTN    Cap;
  UINTN    WaitUs;
  BOOLEAN  Spin;

  if (mBulkOut.Number == 0) {
    return;
  }

  if (mIrqStuck) {
    //
    // The handler found a line it could not clear and masked both sources (see
    // MusbInterruptHandler).  Reported here, once, because a handler may not
    // print.
    //
    mIrqStuck   = FALSE;
    mIrqClaimed = FALSE;
    DEBUG ((
      DEBUG_ERROR,
      "Musb: the controller's interrupt moved nothing %u times in a row and has been "
      "masked; the bulk data path is back on the poll, %u packets per tick\n",
      MUSB_IRQ_NO_PROGRESS_LIMIT,
      MUSB_RX_POLL_BATCH
      ));
  }

  MusbBulkOutDeliver ();

  //
  // The poll batches until the interrupt has proved it works.  mIrqCount is the
  // proof: once an entry has been taken the FIFO is the handler's and this is
  // only here for a packet a full ring left behind, so it looks once and never
  // waits.  Before that - the instant after the claim, or a source that is
  // registered and enabled but never delivered - the poll keeps its batch, which
  // is also what the driver measures 50 KB/s without.
  //
  // Spin is granted on the same proof, for the opposite reason: a handler that is
  // carrying the data path spins for the next packet inside its own entry, so a
  // tick that also spun would hold the TPL_HIGH twice per packet for the same
  // packet - once from the poll, which would take it away from the handler, and
  // once from the handler.  Nothing is lost by that: the poll only looks when the
  // interrupt has not fired, which is the case where it is the data path.
  //
  if (mIrqClaimed && (mIrqCount != 0) && !mRxHeld) {
    Cap    = 1;
    WaitUs = 0;
    Spin   = FALSE;
  } else {
    Cap    = MUSB_RX_POLL_BATCH;
    WaitUs = mIrqClaimed ? 0 : MUSB_RX_POLL_WAIT_US;
    Spin   = TRUE;
  }

  if (MusbBulkOutStage (Cap, WaitUs, FALSE, Spin, !MusbQuiet ()) != 0) {
    //
    // A packet the poll had to take while the interrupt is claimed and has never
    // fired is the one reading that says the interrupt is not reaching this
    // driver - the wrong source, a distributor that is not forwarding it.  Said
    // once, with the numbers that decide it, because it is the difference between
    // "slow" and "the interrupt is not wired up".
    //
    if (mIrqClaimed && (mIrqCount == 0) && !mIrqSilent) {
      mIrqSilent = TRUE;
      DEBUG ((
        DEBUG_ERROR,
        "Musb: GIC INTID %u is claimed and enabled but has not fired once; the poll is "
        "staging the packets (%u so far).  The handler is registered for SPI %u - if the "
        "GIC state line above shows it enabled, this interrupt is not the MUSB line.\n",
        MUSB_GIC_INTID,
        mRxStaged,
        MUSB_GIC_SPI
        ));
    }
  }

  MusbBulkOutDeliver ();

  //
  // The one line that says the interrupt is what carries the traffic, printed
  // where it costs nothing: a tick that finds the ring empty and nothing waiting
  // in the FIFO is not in the middle of a transfer, so the ~8 ms this line takes
  // cannot hold a packet up.
  //
  if (!mIrqAnnounced && mIrqClaimed && (mIrqCount != 0) &&
      (mRxRingHead == mRxRingTail) && !mRxHeld && !MusbQuiet ())
  {
    mIrqAnnounced = TRUE;
    DEBUG ((
      DEBUG_INFO,
      "Musb: bulk OUT is interrupt-driven: %u interrupt(s) so far, %u packet(s) staged, "
      "%u delivered to the transport, %u bulk-IN completion(s)\n",
      mIrqCount,
      mRxStaged,
      mRxDelivered,
      mIrqCompleted
      ));
  }

#if MUSB_RX_TIMING
  //
  // The per-packet split of the FIFO path, on the same quiet trigger as the line
  // above and next to it, so a phase of a download prints it between two
  // transfers rather than inside one.
  //
  MusbBulkOutTimingReport ();
#endif

  if (mRxHeld && (((mRxRingHead + 1U) & MUSB_RX_RING_MASK) != mRxRingTail)) {
    //
    // There is room again, so the source the handler masked when it left a packet
    // in the FIFO is re-armed here - from the context that can also drain the
    // ring, which is what the handler could not do.  MusbIntrMaskWrite restores
    // the whole mask, so this is the same write the claim made.
    //
    mRxHeld = FALSE;
    if (mIrqClaimed) {
      MusbIntrMaskWrite ();
    }
  } else if (mTxRearm) {
    //
    // The same for the bulk IN endpoint, which the handler masks while the host
    // still has its last packet: one retry per tick, never a line held up at
    // TPL_HIGH.
    //
    mTxRearm = FALSE;
    if (mIrqClaimed) {
      MusbIntrMaskWrite ();
    }
  }
}

/**
  Report what the distributor holds for this controller's interrupt.

  Every call that configures the interrupt returns a status, and a status can be
  right while the line still goes nowhere: an SPI in group 0 has its non-secure
  enable bit ignored, a distributor that is not forwarding group 1 passes
  nothing on, and a level source left edge-configured delivers exactly one
  interrupt.  These four words tell those apart, and they are read-only.

  Read by hand rather than through ArmGicLib because the driver has no other use
  for the GIC library, and one register read per line is not worth a dependency.

  @param[in] Where  Where in the driver this was asked.
**/
STATIC
VOID
MusbLogGicState (
  IN CONST CHAR8  *Where
  )
{
  UINTN   Distributor;
  UINTN   Index;
  UINTN   ConfigIndex;
  UINT32  Mask;
  UINT32  Group;
  UINT32  Enable;
  UINT32  Config;

  Distributor = (UINTN)PcdGet64 (PcdGicDistributorBase);
  if (Distributor == 0) {
    return;
  }

  //
  // GICv3 distributor words, laid out as ArmGicLib.h names them: the group
  // register holds one bit per interrupt, the set-enable register one bit per
  // interrupt, and the configuration register two bits per interrupt with bit 1
  // clear meaning level.
  //
  Index       = MUSB_GIC_INTID / 32U;
  ConfigIndex = MUSB_GIC_INTID / 16U;
  Mask        = (UINT32)(1U << (MUSB_GIC_INTID % 32U));

  Group  = MmioRead32 (Distributor + 0x080U + (4U * Index));
  Enable = MmioRead32 (Distributor + 0x100U + (4U * Index));
  Config = MmioRead32 (Distributor + 0xC00U + (4U * ConfigIndex));

  DEBUG ((
    DEBUG_INFO,
    "Musb: GIC INTID %u (SPI %u) at %a: CTLR 0x%08x (grp1 enabled=%u), IGROUPR+0x%x 0x%08x "
    "(group1=%u), ISENABLER+0x%x 0x%08x (enabled=%u), ICFGR+0x%x 0x%08x (bits %u = %a)\n",
    MUSB_GIC_INTID,
    MUSB_GIC_SPI,
    Where,
    MmioRead32 (Distributor + 0x000U),
    (MmioRead32 (Distributor + 0x000U) & BIT1) != 0,
    (UINT32)(0x080U + (4U * Index)),
    Group,
    (Group & Mask) != 0,
    (UINT32)(0x100U + (4U * Index)),
    Enable,
    (Enable & Mask) != 0,
    (UINT32)(0xC00U + (4U * ConfigIndex)),
    Config,
    (Config >> (2U * (MUSB_GIC_INTID % 16U))) & 0x3U,
    (((Config >> (2U * (MUSB_GIC_INTID % 16U))) & BIT1) != 0) ? "edge" : "level"
    ));
}

/**
  The controller's interrupt: the bulk data path, and nothing else.

  Short by construction and print-free by rule.  It runs at TPL_HIGH: a DEBUG
  line costs this console ~8 ms and no boot service may be called there, so the
  handler moves bytes - the OUT FIFO into the staging ring, the IN endpoint's
  completion into a counter - and leaves every decision to the poll.

  What it services, and what it must not.  INTRTX and INTRRX are read, and only
  the two bulk endpoints' bits are written back.  Both registers are
  write-one-to-clear, and the EP0 latch bits are the poll's evidence that the SIE
  delivered a SETUP (MusbEpIntrWatch): clearing them here would delete a line
  from the log for nothing, since EP0's mask bit is 0 and cannot be why the line
  asserted.  INTRUSB is read for the record and deliberately not cleared: the
  RESET latch it carries is what the poll's rewind acts on, and taking it here
  would leave the core at the old address with the old FIFOs.

  INDEX goes back to EP0 before returning.  The poll's control path addresses
  CSR0 through the indexed registers and does not re-select on every access, so
  the handler has to leave the bank it found.

  Both latches are cleared after the work they announced, not before: the OUT
  latch is a write-one-to-clear bit that the core sets again while a packet is
  still in the FIFO, so clearing it first would put one packet behind two
  interrupts.  A source that cannot be satisfied here - a bulk IN packet the host
  has not taken yet - is masked and left to the poll to re-arm, which is what
  keeps a level line from holding this handler at TPL_HIGH.

  A bulk OUT entry stages what the FIFO holds into the ring (MusbBulkOutStage) and
  then, if a transfer was live, spins for the packet behind it in bounded slices -
  the reading that keeps the host from being NAKed for a packet this driver could
  have taken.  The bounds are in the two places that impose them: a quiet line
  costs one counter read and one comparison (MusbBulkOutSpinActive), and the TPL
  goes back to the machine after at most MUSB_RX_SPIN_SLICES slices of a
  microframe, because a spin at TPL_HIGH is the one thing here that could hang the
  machine.

  The EOI is this handler's job: ArmGicDxe writes it only for a source nothing
  has registered, so a registered handler that returns without one leaves the
  interrupt active at the distributor - and a level line that stays active is
  never delivered again.

  @param[in] Source         The interrupt that fired, i.e. MUSB_GIC_INTID.
  @param[in] SystemContext  Unused.
**/
STATIC
VOID
EFIAPI
MusbInterruptHandler (
  IN HARDWARE_INTERRUPT_SOURCE  Source,
  IN EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  UINT16   TxIrq;
  UINT16   RxIrq;
  UINT16   InBit;
  UINT16   OutBit;
  UINTN    Staged;
  BOOLEAN  Progress;
#if MUSB_RX_TIMING
  UINT64   RxEnd;
#endif

#if MUSB_RX_TIMING
  //
  // The earliest point this handler can stamp: the instruction before even the
  // entry counter.  What came before it - the GIC acknowledge, ArmGicDxe's
  // dispatch, the register save the exception frame costs - is interrupt
  // latency and is inside the gap of the previous packet instead, because there
  // is no stamp in this driver that can be taken before them.
  //
  mRxTimingIrqStart = GetPerformanceCounter ();
  mRxTimingFifoRun  = 0;
#endif

  mIrqCount++;

  TxIrq  = MmioRead16 (MusbReg (A733_MUSB_INTRTX_REG));
  RxIrq  = MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG));
  InBit  = MusbIntrEpBit (mBulkIn.Number);
  OutBit = MusbIntrEpBit (mBulkOut.Number);

  Staged   = 0;
  Progress = FALSE;

  if ((OutBit != 0) && ((RxIrq & OutBit) != 0)) {
    Staged = MusbBulkOutStage (MUSB_RX_HANDLER_BATCH, 0, TRUE, TRUE, FALSE);
    if (mRxHeld) {
      //
      // The ring was full: the packet is still in the FIFO, and this source has
      // to be masked or the level line would re-enter here immediately.
      // MusbBulkOutService puts the mask back once the poll has made room.
      //
      MmioWrite16 (MusbReg (A733_MUSB_INTRRXE_REG), 0);
    }

#if MUSB_RX_TIMING
    //
    // End of this packet's drain and of the RXCSR acknowledgement behind it: the
    // two writes below (the latch, then the EOI at the end of the handler) are
    // this handler's only work that lands after the stamp, which is what makes
    // handler + gap one packet's period by construction.
    //
    RxEnd = GetPerformanceCounter ();
#endif

    //
    // The latch goes last, after the packets are acknowledged: with RXPKTRDY
    // cleared there is nothing left for the core to latch again, so one packet
    // costs one interrupt.  Cleared first, the write would be undone by the
    // packet still sitting in the FIFO.
    //
    MmioWrite16 (MusbReg (A733_MUSB_INTRRX_REG), OutBit);

#if MUSB_RX_TIMING
    MusbRxTimingRecord (RxEnd, Staged);
#endif
  }

  if ((InBit != 0) && ((TxIrq & InBit) != 0)) {
    MusbSelectEp (mBulkIn.Number);
    if ((MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)) & A733_TXCSR_TXPKTRDY) == 0) {
      //
      // The host has taken the packet Send() loaded.  Recorded, not acted on:
      // Send() polls the same bit and stays the only owner of its buffer, so a
      // completion that arrives before Send() looks for it is not lost.
      //
      Progress = TRUE;
      mIrqCompleted++;
    } else {
      //
      // Still waiting for the host.  The latch is cleared and the source masked,
      // and the poll re-arms it at the next tick: whether this core's INTRTX bit
      // is an event or follows TXPKTRDY, one packet waiting for a slow host can
      // then never hold this handler up.  Masking it counts as progress - it is
      // this handler's answer, not a line it failed to clear.
      //
      MmioWrite16 (MusbReg (A733_MUSB_INTRTXE_REG), 0);
      mTxRearm = TRUE;
      Progress = TRUE;
    }

    MmioWrite16 (MusbReg (A733_MUSB_INTRTX_REG), InBit);
  }

  MusbSelectEp (0);

  //
  // Progress, or a guard against a line nothing here can clear.  A handler that
  // kept re-entering at TPL_HIGH without moving a byte would stop the machine,
  // so after a run of such entries both sources are masked and the poll - which
  // can see the FIFO, wait, and print - takes the data path back.
  //
  if ((Staged != 0) || Progress) {
    mIrqNoProgress = 0;
  } else if (++mIrqNoProgress >= MUSB_IRQ_NO_PROGRESS_LIMIT) {
    MmioWrite16 (MusbReg (A733_MUSB_INTRTXE_REG), 0);
    MmioWrite16 (MusbReg (A733_MUSB_INTRRXE_REG), 0);
    mIrqStuck = TRUE;
  }

  if (mInterrupt2 != NULL) {
    mInterrupt2->EndOfInterrupt (mInterrupt2, Source);
  } else if (mInterrupt != NULL) {
    mInterrupt->EndOfInterrupt (mInterrupt, Source);
  }
}

/**
  Claim the controller's interrupt, or say once why the poll keeps the data path.

  The platform's ArmGicDxe installs both interrupt protocols in one call; the
  second is preferred because it can also set the trigger type, and this line is
  a level - the core holds INTR asserted until the endpoint status behind it is
  acknowledged, so an edge configuration delivers exactly one interrupt and then
  silence.

  Failure is not fatal and never was: this driver was polled for its entire life
  before the bulk path moved onto the interrupt, so a platform without the
  protocol, a source the platform refuses, or a handler that cannot be registered
  all leave the same driver working, slower, and say so once.  Nothing above the
  endpoints depends on which of the two is in force: the poll checks the same
  status bits and acknowledges only what it finds, so the two cannot double-ack.

  Called twice.  From the bring-up, where the GIC may not exist yet - the entry
  point runs at dispatch, ArmGicDxe from an arch-protocol notify - and again from
  Start(), which is when a fastboot consumer appears and by which time the
  platform's interrupt support is certainly installed.

  @param[in] Where  Who asked, for the log.
**/
STATIC
VOID
MusbInterruptClaim (
  IN CONST CHAR8  *Where
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  TriggerStatus;
  EFI_STATUS  RegisterStatus;
  EFI_STATUS  EnableStatus;

  //
  // Already claimed: Start() may have reprogrammed the FIFO map for other
  // endpoint numbers, so the masks are rewritten and nothing else is touched.
  //
  if (mIrqClaimed) {
    MusbIntrMaskWrite ();
    return;
  }

  mInterrupt     = NULL;
  mInterrupt2    = NULL;
  mIrqStuck      = FALSE;
  mIrqNoProgress = 0;

  TriggerStatus  = EFI_NOT_FOUND;
  RegisterStatus = EFI_NOT_FOUND;
  EnableStatus   = EFI_NOT_FOUND;

  Status = gBS->LocateProtocol (&gHardwareInterrupt2ProtocolGuid, NULL, (VOID **)&mInterrupt2);
  if (!EFI_ERROR (Status)) {
    TriggerStatus = mInterrupt2->SetTriggerType (
                                  mInterrupt2,
                                  (HARDWARE_INTERRUPT_SOURCE)MUSB_GIC_INTID,
                                  EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH
                                  );
    RegisterStatus = mInterrupt2->RegisterInterruptSource (
                                    mInterrupt2,
                                    (HARDWARE_INTERRUPT_SOURCE)MUSB_GIC_INTID,
                                    MusbInterruptHandler
                                    );
  } else {
    Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL, (VOID **)&mInterrupt);
    if (EFI_ERROR (Status)) {
      mInterrupt = NULL;
      DEBUG ((
        DEBUG_INFO,
        "Musb: no hardware interrupt protocol at %a yet: the bulk data path stays on the poll "
        "for now (%u packets per %u us tick), and Start() asks again\n",
        Where,
        MUSB_RX_POLL_BATCH,
        MUSB_POLL_PERIOD_100NS / 10U
        ));
      return;
    }

    RegisterStatus = mInterrupt->RegisterInterruptSource (
                                   mInterrupt,
                                   (HARDWARE_INTERRUPT_SOURCE)MUSB_GIC_INTID,
                                   MusbInterruptHandler
                                   );
  }

  if (EFI_ERROR (RegisterStatus)) {
    mInterrupt  = NULL;
    mInterrupt2 = NULL;
    DEBUG ((
      DEBUG_WARN,
      "Musb: the interrupt handler cannot be registered for GIC INTID %u (SPI %u) at %a: %r; "
      "the bulk data path stays on the poll, %u packets per tick\n",
      MUSB_GIC_INTID,
      MUSB_GIC_SPI,
      Where,
      RegisterStatus,
      MUSB_RX_POLL_BATCH
      ));
    return;
  }

  //
  // Registering a handler already enables the source in ArmGicDxe, but the call
  // is made here as well: the driver's dependence on it is then explicit and
  // survives a platform whose register call does not enable.
  //
  if (mInterrupt2 != NULL) {
    EnableStatus = mInterrupt2->EnableInterruptSource (mInterrupt2, (HARDWARE_INTERRUPT_SOURCE)MUSB_GIC_INTID);
  } else {
    EnableStatus = mInterrupt->EnableInterruptSource (mInterrupt, (HARDWARE_INTERRUPT_SOURCE)MUSB_GIC_INTID);
  }

  if (EFI_ERROR (EnableStatus)) {
    //
    // The source may or may not be enabled at the distributor; either way the
    // endpoint masks stay 0, so the line cannot assert and nothing here can
    // storm.  The poll keeps the data path.
    //
    DEBUG ((
      DEBUG_WARN,
      "Musb: GIC INTID %u cannot be enabled at %a: %r; the bulk data path stays on the poll\n",
      MUSB_GIC_INTID,
      Where,
      EnableStatus
      ));
    return;
  }

  mIrqClaimed = TRUE;

  //
  // Start from a known slate.  The endpoint latches are write-one-to-clear and
  // they set whether or not their mask bit is, so a latch the core raised while
  // nothing was listening - the bring-up's own FIFO flushes are enough - would
  // assert the line the instant the mask goes on.  A packet genuinely waiting in
  // the FIFO is not lost by this: the poll drains the FIFO whatever the latch
  // says.
  //
  MmioWrite16 (MusbReg (A733_MUSB_INTRTX_REG), MusbIntrEpBit (mBulkIn.Number));
  MmioWrite16 (MusbReg (A733_MUSB_INTRRX_REG), MusbIntrEpBit (mBulkOut.Number));

  MusbIntrMaskWrite ();

  DEBUG ((
    DEBUG_INFO,
    "Musb: the controller's interrupt is claimed at %a: GIC INTID %u (SPI %u), "
    "SetTriggerType(level-high) %r, RegisterInterruptSource %r, EnableInterruptSource %r; "
    "the bulk endpoints are interrupt-driven from here (INTRTXE=0x%04x INTRRXE=0x%04x), "
    "EP0 and INTRUSB stay masked and polled\n",
    Where,
    MUSB_GIC_INTID,
    MUSB_GIC_SPI,
    TriggerStatus,
    RegisterStatus,
    EnableStatus,
    MmioRead16 (MusbReg (A733_MUSB_INTRTXE_REG)),
    MmioRead16 (MusbReg (A733_MUSB_INTRRXE_REG))
    ));

  MusbLogGicState (Where);
}

/**
  Acknowledge the bus events the core latched and rewind on a bus reset.

  A reset has to put the core back to address 0 with empty FIFOs and data
  toggles of 0; the host does the same on its side, and a device that keeps
  either makes the next enumeration fail in a way that looks like a stall.

  Every event in the byte is named before the clear, and the clear is logged
  with its operand, because this is the only place in the poll path where
  INTRUSB is written: an event that is dropped here would otherwise leave no
  trace at all.  The reset branch logs the state it found, what it rewrote and
  the state it left, since "the host reset me" and "the host reset me and my
  address write did not take" are different faults.
**/
STATIC
VOID
MusbBusEventService (
  VOID
  )
{
  UINT8    Intr;
  UINT8    NonSof;
  UINT32   SofCount;
  BOOLEAN  PrintClear;
  UINT8    Power;
  UINT8    Devctl;
  BOOLEAN  Announced;

  Intr = MmioRead8 (MusbReg (A733_MUSB_INTRUSB_REG));
  if (Intr == 0) {
    return;
  }

  Announced = MusbAccountBusEvents (Intr, "poll");

  //
  // SOF is set on nearly every tick (8 kHz of SOFs against a 10 kHz poll), and
  // one DEBUG line costs about 8 ms on this console - eighty tick periods.  So
  // printing the acknowledgement per tick does not report the poll, it starves
  // it: the SIE is serviced once per eighty ticks, which is the same effect as
  // not servicing it at all while a host waits milliseconds for a control
  // stage.  The clear is printed for the first SOF and then once per
  // MUSB_SOF_REPORT_EVERY, with the running total in the line; every byte that
  // carries anything other than SOF is printed every time, and the CSR0 watch
  // is untouched.
  //
  NonSof   = (UINT8)(Intr & ~(UINT8)A733_MUSB_INTRUSB_SOF);
  SofCount = mIntrCount[(UINTN)MUSB_INTRUSB_SOF_BIT];
  PrintClear = (BOOLEAN)((NonSof != 0) ||
                         ((SofCount == 1) ||
                          ((SofCount != 0) && ((SofCount % MUSB_SOF_REPORT_EVERY) == 0))));

  if (PrintClear && !MusbQuiet ()) {
    if (NonSof == 0) {
      DEBUG ((
        DEBUG_INFO,
        "Musb: INTRUSB 0x%02x -> write 0x%02x (W1C) at poll tick %u (SOF only: total %u, "
        "printed first and then every %u)\n",
        (UINT32)Intr,
        (UINT32)(Intr & A733_MUSB_INTRUSB_W1C),
        mPollTicks,
        SofCount,
        MUSB_SOF_REPORT_EVERY
        ));
    } else {
      DEBUG ((
        DEBUG_INFO,
        "Musb: INTRUSB 0x%02x -> write 0x%02x (W1C) at poll tick %u\n",
        (UINT32)Intr,
        (UINT32)(Intr & A733_MUSB_INTRUSB_W1C),
        mPollTicks
        ));
    }
  }

  MmioWrite8 (MusbReg (A733_MUSB_INTRUSB_REG), (UINT8)(Intr & A733_MUSB_INTRUSB_W1C));

  if (Announced && !MusbQuiet ()) {
    MusbLogEventTotals ();
  }

  if ((Intr & A733_MUSB_INTRUSB_RESET) != 0) {
    Power  = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
    Devctl = MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG));
    //
    // CSR0 and COUNT0 are indexed: EP0 has to be selected before the lines
    // below read them, because the previous tick ended on a data endpoint.
    //
    MusbSelectEp (0);
    //
    // The reset is recorded before anything it rewrites, in the same ring the
    // SETUPs of the session it interrupted are in - and a reset never empties
    // that ring, because the history of what the host did before it is exactly
    // what makes the reset readable.  The two verbose lines are the same
    // readings, unpacked.
    //
    MusbRingAdd (
      MusbTagReset,
      (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG))
      | ((UINT32)Power << 8)
      | ((UINT32)Devctl << 16),
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG))
      | ((UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG)) << 16)
      );
    if (!MusbQuiet ()) {
      DEBUG ((
        DEBUG_INFO,
        "Musb: RESET handler in: FADDR=0x%02x POWER=0x%02x (softconn=%u hsenab=%u hsmode=%u) "
        "DEVCTL=0x%02x (%a-device hostmode=%u session=%u vbus=%a) INTRUSB=0x%02x "
        "CSR0=0x%04x COUNT0=0x%04x INTRRX=0x%04x; "
        "will write FADDR=0, flush EP0 and set its state idle, clear the bulk data "
        "toggles and flush both bulk FIFOs\n",
        (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG)),
        (UINT32)Power,
        (UINT32)((Power & A733_MUSB_POWER_SOFTCONN) != 0),
        (UINT32)((Power & A733_MUSB_POWER_HSENAB) != 0),
        (UINT32)((Power & A733_MUSB_POWER_HSMODE) != 0),
        (UINT32)Devctl,
        ((Devctl & A733_MUSB_DEVCTL_BDEVICE) != 0) ? "B" : "A",
        (UINT32)((Devctl & A733_MUSB_DEVCTL_HOSTMODE) != 0),
        (UINT32)((Devctl & A733_MUSB_DEVCTL_SESSION) != 0),
        mMusbVbusState[((Devctl >> A733_MUSB_DEVCTL_VBUS_SHIFT) & A733_MUSB_DEVCTL_VBUS_MASK)],
        (UINT32)Intr,
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)),
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_RXCOUNT_REG)),
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG))
        ));

      DEBUG ((DEBUG_INFO, "Musb: Reset (bus reset seen, back to address 0)\n"));
    }

    mAddress       = 0;
    mConfiguration = 0;
    MmioWrite8 (MusbReg (A733_MUSB_FADDR_REG), 0);
    MusbEp0Reset ();
    //
    // The re-attach window starts here: a host that resets the port and then
    // sends no SETUP for three seconds has given up on this connect, and one
    // clean re-attach is what gives it a connect to look at again (see
    // MusbReattachPoll).  It is armed on every reset but spent at most once, and
    // a SETUP after the reset clears it in the poll.
    //
    if ((mReattachFrequency != 0) && !mReattachDone) {
      mReattachArmed   = TRUE;
      mReattachSetups  = mSetupsHandled;
      mReattachResetAt = GetPerformanceCounter ();
    }

    //
    // The bulk FIFOs are flushed here as well as at bring-up.  This is one
    // place where the enumerating device-mode driver does less: it only flushes
    // them when it starts, not on every bus reset.  A reset is exactly when a
    // stale bulk packet would look like data the next session sent, so the
    // flush stays - it writes no status bit EP0 depends on.
    //
    MusbFlushBulkEndpoints ();
    //
    // The staging ring is flushed with them, for the same reason: a packet the
    // interrupt staged before this reset is the previous session's data, and
    // handing it to the transport afterwards would feed the next session's
    // command parser with it.
    //
    MusbBulkOutDiscardStaged ();

    Power  = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
    Devctl = MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG));
    MusbRingAdd (
      MusbTagResetDone,
      (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG))
      | ((UINT32)Power << 8)
      | ((UINT32)Devctl << 16),
      (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG))
      );
    if (!MusbQuiet ()) {
      DEBUG ((
        DEBUG_INFO,
        "Musb: RESET handler out: FADDR=0x%02x POWER=0x%02x (softconn=%u hsmode=%u) "
        "DEVCTL=0x%02x (hostmode=%u session=%u vbus=%a) CSR0=0x%04x INTRUSB=0x%02x "
        "(EP0 %a)\n",
        (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG)),
        (UINT32)Power,
        (UINT32)((Power & A733_MUSB_POWER_SOFTCONN) != 0),
        (UINT32)((Power & A733_MUSB_POWER_HSMODE) != 0),
        (UINT32)Devctl,
        (UINT32)((Devctl & A733_MUSB_DEVCTL_HOSTMODE) != 0),
        (UINT32)((Devctl & A733_MUSB_DEVCTL_SESSION) != 0),
        mMusbVbusState[((Devctl >> A733_MUSB_DEVCTL_VBUS_SHIFT) & A733_MUSB_DEVCTL_VBUS_MASK)],
        (UINT32)MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)),
        (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSB_REG)),
        MusbEp0StateName (mEp0State)
        ));
    }
  }
}

/**
  The one re-attach after a bus reset that no SETUP followed.

  Why it exists.  A host only looks at a port whose connect changed.  During this
  port's development the host has given up on it many times ("Cannot enable.
  Maybe the USB cable is bad?"), and it does not look again while the pull-up
  stays up - so one clean re-attach right after the first attempt fails is cheap
  insurance: the connect changes, the host resets the port, and it enumerates
  against a device whose endpoint-0 state was rewritten for it.

  What "failed" means here.  Three seconds after a bus reset with no SETUP taken
  since it: a host that has seen the reset and is enumerating asks for its device
  descriptor within milliseconds, so three seconds of silence is the host, not
  this device, and the line is printed for that reason alone (the console is
  quiet during a transfer, but there is no transfer - a SETUP clears the window
  before anything is printed).

  What it writes.  The device half only, and in this order: the pull-up down
  (POWER.SOFTCONN cleared) and held down for 50 ms so the host's detector sees
  the disconnect; FADDR = 0 and EP0 flushed with its state machine idle, while
  nothing is attached; the FIFO map reprogrammed, because a re-attach that left
  a FIFO address of the previous attempt behind would hand the next enumeration
  nowhere to put a packet; and then POWER in the bring-up's own single write,
  with SOFTCONN going back up - the moment the host can see the device again.
  The platform, PHY0, the CCU and the wrapper's route/passby/ISCR words are not
  touched: none of them was part of what failed.

  Once per boot.  mReattachDone is set before the writes, so nothing below can
  re-enter it, and a run whose enumeration works spends nothing on it.

  Called from the poll and only from there.
**/
STATIC
VOID
MusbReattachPoll (
  VOID
  )
{
  UINT64  ElapsedUs;
  UINT8   Power;
  UINT8   NewPower;

  if (!mStarted || mReattachDone || !mReattachArmed || (mReattachFrequency == 0)) {
    return;
  }

  //
  // A SETUP since the reset means the host is enumerating that reset, which is
  // what the window was waiting for; the re-attach stays unspent.
  //
  if (mSetupsHandled != mReattachSetups) {
    mReattachArmed = FALSE;
    return;
  }

  ElapsedUs = ((GetPerformanceCounter () - mReattachResetAt) * 1000000ULL)
              / mReattachFrequency;
  if (ElapsedUs < MUSB_REATTACH_AFTER_US) {
    return;
  }

  mReattachArmed = FALSE;
  mReattachDone  = TRUE;

  //
  // Down first, so the host sees a disconnect and will sample the port again;
  // the device half is rewritten while nothing is attached, so the connect the
  // host acts on belongs to the state written below.
  //
  Power = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
  MmioAnd8 (MusbReg (A733_MUSB_POWER_REG), (UINT8)~A733_MUSB_POWER_SOFTCONN);
  MicroSecondDelay (MUSB_REATTACH_DETACH_US);

  MmioWrite8 (MusbReg (A733_MUSB_FADDR_REG), 0);
  mAddress       = 0;
  mConfiguration = 0;
  MusbEp0Reset ();
  MusbFifoLayout ();
  MusbSelectEp (0);

  //
  // The bring-up's POWER write, unchanged: HSENAB, SOFTCONN and SUSPENDM, read
  // and written once, nothing cleared afterwards.
  //
  NewPower = (UINT8)(MmioRead8 (MusbReg (A733_MUSB_POWER_REG)) |
                     A733_MUSB_POWER_HSENAB | A733_MUSB_POWER_SOFTCONN |
                     A733_MUSB_POWER_SUSPEND);
  MmioWrite8 (MusbReg (A733_MUSB_POWER_REG), NewPower);

  DEBUG ((
    DEBUG_INFO,
    "Musb: re-attach: %u us after a bus reset and no SETUP since it - dropped the "
    "pull-up for %u ms (POWER was 0x%02x), rewrote FADDR=0, EP0 and the FIFO map, and "
    "raised it again (wrote 0x%02x, read back 0x%02x); once per boot\n",
    (UINT32)(ElapsedUs / 1000U),
    MUSB_REATTACH_DETACH_US / 1000U,
    (UINT32)Power,
    (UINT32)NewPower,
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_POWER_REG))
    ));

  MusbLogCoreState ("re-attach: the host has a fresh connect to enumerate");
}

/**
  The periodic service: bus events, control endpoint, bulk OUT.

  The tick counter and the two instrumentation calls do not affect any of the
  three services.  The watch runs last so that a change made by one of them is
  reported in the same tick it happened in.
**/
STATIC
VOID
EFIAPI
MusbPollNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (!mStarted || mPollBusy) {
    return;
  }

  mPollBusy = TRUE;
  mPollTicks++;

  //
  // The heartbeat is the one line that says the poll is still running, so it is
  // also the one line that must not be printed while an enumeration is in
  // flight - it would be the 8 ms gap the host times out in.  Quiet ticks are
  // accounted for in the drain instead.
  //
  if (!MusbQuiet () && ((mPollTicks % MUSB_HEARTBEAT_TICKS) == 0)) {
    MusbLogHeartbeat ("(scheduled)");
  }

  MusbBusEventService ();
  MusbEp0Service ();
  MusbBulkOutService ();
  MusbStateWatch ();

  //
  // The re-attach, after this tick's services: it only fires when no SETUP has
  // arrived for three seconds after a reset, so nothing is in flight and the
  // disconnect cannot cost a transfer.
  //
  MusbReattachPoll ();

  //
  // A stall ended the transfer it belonged to, so the window is closed here -
  // after the stall has gone out and the tick's services have run, which is the
  // earliest moment the trace can be printed without costing a transfer.
  //
  if (mStallPending) {
    mStallPending = FALSE;
    MusbQuietLeave ("stall, the request was not answered");
  }

  //
  // A window no SETUP has extended for 5 s is closed here too, from the poll and
  // not from the EP0 path: the host has stopped (or is waiting on something that
  // never came), so printing the trace cannot cost a transfer any more.
  //
  if (MusbQuiet () && ((mPollTicks - mRingLastSetupTick) >= MUSB_QUIET_IDLE_TICKS)) {
    MusbQuietLeave ("no SETUP for 5 s");
  }

  mPollBusy = FALSE;
}

/**
  Put one bulk-IN buffer on the wire.

  Blocking, and holding the TPL at HIGH for its whole length.  HIGH rather than
  NOTIFY since the bulk data path moved onto the interrupt: the handler selects
  an endpoint and leaves INDEX at 0, and this loop's TXCSR access belongs to the
  IN endpoint it selected once at the top - a handler entering between the FIFO
  write and the TXCSR write would send the write to endpoint 0.  Nothing else
  needs to fill a packet-sized hole in the interrupt's attention: the worst case
  is the timeout below, which is the one path where this holds the TPL that long,
  and it is a wedged host, not a transfer.

  @param[in] EndpointIndex  Endpoint index as the protocol defines it.
  @param[in] Size           Bytes to send.
  @param[in] Buffer         Data.

  @retval EFI_SUCCESS        The data was sent.
  @retval EFI_INVALID_PARAMETER  Wrong endpoint, or more than one Send() should
                                 ever carry.
  @retval EFI_TIMEOUT        The host never took a packet.
**/
STATIC
EFI_STATUS
MusbDeviceSend (
  IN UINT8        EndpointIndex,
  IN UINTN        Size,
  IN CONST VOID   *Buffer
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;
  UINTN       Sent;
  UINTN       Chunk;
  UINTN       Waited;

  if (EndpointIndex != mBulkIn.Number) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: Send() for endpoint index %u, but the bulk IN endpoint is %u\n",
      (UINT32)EndpointIndex,
      (UINT32)mBulkIn.Number
      ));
    return EFI_INVALID_PARAMETER;
  }

  if (Size > MUSB_TX_MAX_SIZE) {
    DEBUG ((DEBUG_ERROR, "Musb: Send() of %u bytes is larger than one response\n", (UINT32)Size));
    return EFI_INVALID_PARAMETER;
  }

  Status = EFI_SUCCESS;
  Sent   = 0;

  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  MusbSelectEp (mBulkIn.Number);

  while (Sent < Size) {
    Waited = 0;
    while ((MmioRead16 (MusbReg (A733_MUSB_TXCSR_REG)) & A733_TXCSR_TXPKTRDY) != 0) {
      if (Waited >= MUSB_TX_PACKET_TIMEOUT_US) {
        Status = EFI_TIMEOUT;
        break;
      }

      MicroSecondDelay (MUSB_TX_POLL_US);
      Waited += MUSB_TX_POLL_US;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    Chunk = MIN (Size - Sent, (UINTN)mBulkIn.MaxPacket);
    MusbFifoWrite (mBulkIn.Number, Chunk, (CONST UINT8 *)Buffer + Sent);
    MmioWrite16 (MusbReg (A733_MUSB_TXCSR_REG), A733_TXCSR_MODE | A733_TXCSR_TXPKTRDY);
    Sent += Chunk;
  }

  //
  // Leave EP0 selected, the bank every other reader in this driver expects to
  // find - the same invariant the interrupt handler keeps.
  //
  MusbSelectEp (0);

  gBS->RestoreTPL (OldTpl);

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: bulk IN timed out after %u of %u bytes (host not reading?)\n",
      (UINT32)Sent,
      (UINT32)Size
      ));
  }

  if (mTxCallback != NULL) {
    mTxCallback (EndpointIndex);
  }

  return Status;
}

/**
  Create the poll event if it does not exist yet, and arm it.

  The timer is what makes the port answered, so it is armed by the bring-up at
  DXE dispatch - the device is up and its pull-up is up from there, and a device
  that raises D+ without being serviced is worse than an absent one.  Start()
  calls this too, which only re-arms an already running periodic timer.

  @retval EFI_SUCCESS  The timer is running.
  @retval others       The event could not be created or armed.
**/
STATIC
EFI_STATUS
MusbPollStart (
  VOID
  )
{
  EFI_STATUS  Status;

  if (!mPollRunning) {
    Status = gBS->CreateEvent (
                    EVT_TIMER | EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    MusbPollNotify,
                    NULL,
                    &mPollEvent
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Musb: cannot create the service timer: %r\n", Status));
      return Status;
    }

    mPollRunning = TRUE;
  }

  Status = gBS->SetTimer (mPollEvent, TimerPeriodic, MUSB_POLL_PERIOD_100NS);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Musb: cannot start the service timer: %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Bring the device half of the controller up and start answering the host.

  This runs from the entry point at DXE dispatch, which is when the reference's
  self-starting SunxiMusbDeviceDxe brings its device up: the port is pulling D+
  up and being serviced before BDS, minutes before the fastboot transport calls
  Start().  Start() adds the descriptors and nothing else; a Start() that finds
  the device already up does not run this again.

  The platform phase - the power domain, the subsystem clocks, USB0's host half,
  the resistance calibration, PHY0 and the device half's clock and reset - is
  done by the entry point immediately before this.  What is left here is the
  MUSB core's own side: the FIFO SRAM, the PHY route and passby words, ISCR,
  POWER, the FIFO map, endpoint 0, the masked interrupts and the DMA engine off,
  then the poll timer.  The platform phase is repeated only if its first attempt
  did not read back as up.

  The bulk endpoints are named by the transport's descriptors, which do not exist
  yet - so the caller describes the layout the transport will declare
  (MusbAssumeTransportEndpoints) and this writes the FIFO map for it.  Start()
  reprograms the map if the descriptors name different endpoints.

  @param[in] Where  What called this ("dispatch"), for the log only.

  @retval EFI_SUCCESS  The device half is up and the poll is armed.
  @retval others       The poll timer could not be started; the core is up.
**/
STATIC
EFI_STATUS
MusbDeviceBringUp (
  IN CONST CHAR8  *Where
  )
{
  EFI_STATUS  Status;
  UINT8       Intr;
  UINT8       Power;
  UINT8       NewPower;
  BOOLEAN     Announced;

  //
  // Phase 1: the platform, PHY0 and the device half's clock.  The entry point
  // has already done this at DXE dispatch, which is where the reference does it
  // - so this call is normally a no-op and the log says so.  It is repeated here
  // only when that phase did not read back as up, and never twice.
  //
  if (MusbPlatformBringUp ()) {
    DEBUG ((
      DEBUG_INFO,
      "Musb: device half only - the platform and PHY0 are already up (DXE), not "
      "redone here; POWER=0x%02x DEVCTL=0x%02x route=0x%08x\n",
      MmioRead8 (MusbReg (A733_MUSB_POWER_REG)),
      MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)),
      MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG))
      ));
  } else {
    DEBUG ((
      DEBUG_WARN,
      "Musb: the platform phase had not taken and is being run here; the device "
      "half follows anyway\n"
      ));
  }

  //
  // 2. The FIFO SRAM, in 8 KB FIFO mode.
  //
  MusbSramFifoOn ();

  //
  // 3, 3a, 3b. PHY powered, routed to this core, UTMI data path open.
  //
  MusbPhyUp ();
  MusbLogCoreState ("step 3: PHY0 up, routed to the MUSB core");

  //
  // 4. OTG control: the pull-ups, both force fields and the wake latches, in one
  // read-modify-write.
  //
  MusbIscrOn ();

  //
  // 5. PIO mode, no vendor DMA.
  //
  MmioWrite8 (MusbReg (A733_MUSB_VEND0_REG), 0);
  DEBUG ((
    DEBUG_INFO,
    "Musb: step 5 VEND0 wrote 0x00, read back 0x%02x\n",
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_VEND0_REG))
    ));

  //
  // 6. POWER, in one write: high-speed enable, the pull-up, and suspend
  // monitoring.
  //
  // This is the reference's POWER write, bit for bit: HSENAB (bit 5, the core
  // may answer the host's high-speed chirp), SOFTCONN (bit 6, the D+ pull-up -
  // this is the moment the host can see a device) and SUSPENDM (bit 1, suspend
  // monitoring).  ISOUPDATE (bit 7) is left as found: it is the isochronous-mode
  // select and reads 0 on this core, so bulk and control traffic run with it
  // clear without this file clearing it.
  //
  // Nothing here is cleared afterwards.  An earlier version of this driver held
  // SOFTCONN down until after a FIFO loopback and pushed HSMODE (bit 4, a status
  // bit the PHY drives) back to 0 once the pull-up was up; both were written
  // because the port was invisible, and the port was invisible for a different
  // reason (the host half was being closed and PHY0 re-pulsed behind it, see
  // MusbPhyUp).  Matching the reference is what the host verifies, so the byte
  // is written once and left alone.
  //
  Power    = MmioRead8 (MusbReg (A733_MUSB_POWER_REG));
  NewPower = (UINT8)(Power | A733_MUSB_POWER_HSENAB | A733_MUSB_POWER_SOFTCONN |
                     A733_MUSB_POWER_SUSPEND);
  MmioWrite8 (MusbReg (A733_MUSB_POWER_REG), NewPower);

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 6 POWER: read 0x%02x, wrote 0x%02x (hsenab=%u softconn=%u "
    "suspendm=%u read back 0x%02x) - the pull-up is up from here\n",
    (UINT32)Power,
    (UINT32)NewPower,
    (UINT32)((NewPower & A733_MUSB_POWER_HSENAB) != 0),
    (UINT32)((NewPower & A733_MUSB_POWER_SOFTCONN) != 0),
    (UINT32)((NewPower & A733_MUSB_POWER_SUSPEND) != 0),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_POWER_REG))
    ));
  MusbLogCoreState ("step 6: POWER written, pull-up up");

  DEBUG ((
    DEBUG_INFO,
    "Musb: serving bulk IN 0x%02x and bulk OUT 0x%02x at %u bytes each "
    "(the layout the transport declares; Start() confirms it or reprograms the map)\n",
    mBulkIn.Address,
    mBulkOut.Address,
    (UINT32)MUSB_BULK_MAX_PACKET_LIMIT
    ));

  //
  // 7. The FIFO map, address 0, endpoint 0, and the bulk FIFOs discarded.
  // Their maximum packet size is left to SET_CONFIGURATION, which is where the
  // enumerating device-mode driver programs it: before that the only thing a
  // bring-up has to say about EP1/EP2 is "whatever a previous stage left in
  // them is not data this session sent".  Every FIFO register is then read back
  // and printed, so the layout in force - and whether the writes stuck - is a
  // line in the log rather than this comment.
  //
  MusbFifoLayout ();
  MmioWrite8 (MusbReg (A733_MUSB_FADDR_REG), 0);
  DEBUG ((
    DEBUG_INFO,
    "Musb: step 7 FADDR wrote 0, read back %u\n",
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG))
    ));
  MusbEp0Reset ();
  MusbFlushBulkEndpoints ();
  MusbLogFifoLayout ("step 7");
  MusbLogCoreState ("step 7: FIFO map, EP0 and the bulk FIFOs programmed");

  //
  // 7a. The core's identity words, once: CONFIGDATA says whether this core has
  // per-endpoint FIFOs or one shared buffer per direction, RAMINFO says how
  // much FIFO RAM it thinks it has.  Both have been assumptions in this file
  // until now.
  //
  MusbLogCoreIdentity ("step 7a");
  //
  // A window probe and a page scan used to run here, once each: every offset a
  // DMA block could sit at, read and printed, plus a pass over the whole 4 KB
  // page.  Both answered (no block anywhere in the page - see MusbDmaOff) and
  // both were read-only, so what is left of them is the verdict.
  //

  //
  // 8. Interrupts masked here.  (The FIFO ports are not written: a loopback that
  // put a test pattern into EP1's and EP0's FIFO ports used to run here, and it
  // was safe only while the pull-up was held down through step 7 - step 6 raises
  // it, as the reference does, so that pattern would now land in a live packet
  // buffer.  Its verdict had already been retired as uninformative: a MUSB TX
  // FIFO read back through its own port is the packet buffer's read side, not a
  // memory readback.)  Endpoint 0 and the bus events are serviced from the poll
  // for the life of the boot: the core's bus events latch in INTRUSB whether or
  // not they are unmasked, and every state the control path needs is a register
  // read, so neither of those sources is ever enabled - one owner per state
  // machine, and the RESET latch has to stay the poll's.  What step 10 unmasks is
  // the two bulk endpoints and nothing else.  Latched bus events are drained here
  // so the poll starts from a known slate.
  //
  MmioWrite16 (MusbReg (A733_MUSB_INTRTXE_REG), 0);
  MmioWrite16 (MusbReg (A733_MUSB_INTRRXE_REG), 0);
  MmioWrite8 (MusbReg (A733_MUSB_INTRUSBE_REG), 0);
  //
  // INTRTX/INTRRX are then left alone, not drained: the endpoint masks are 0,
  // so they raise nothing, and their lapped bits are the only independent
  // evidence that the SIE delivered a packet to endpoint 0 (INTRRX bit 0) even
  // when CSR0.RXPKTRDY reads clear.  They are printed when they change.
  //
  DEBUG ((
    DEBUG_INFO,
    "Musb: step 8 endpoint latches as found: INTRTX=0x%04x INTRRX=0x%04x (kept, "
    "masked, reported when they change)\n",
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRTX_REG)),
    (UINT32)MmioRead16 (MusbReg (A733_MUSB_INTRRX_REG))
    ));
  //
  // The write below drains INTRUSB, and anything the core latched during steps
  // 4---7 - a RESET after the pull-up went up, a CONNECT - would otherwise
  // vanish without a trace, which is exactly the reading this log needs.  So
  // the byte is read and reported first, and counted into the same tally the
  // poll uses.  The write itself, and everything around it, is unchanged.
  //
  Intr      = MmioRead8 (MusbReg (A733_MUSB_INTRUSB_REG));
  Announced = MusbAccountBusEvents (Intr, "step 8 drain, before the poll started");
  if (Announced) {
    MusbLogEventTotals ();
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: step 8 draining INTRUSB: read 0x%02x, writing 0x%02x (W1C)\n",
    (UINT32)Intr,
    (UINT32)A733_MUSB_INTRUSB_W1C
    ));
  MmioWrite8 (MusbReg (A733_MUSB_INTRUSB_REG), (UINT8)A733_MUSB_INTRUSB_W1C);
  DEBUG ((
    DEBUG_INFO,
    "Musb: step 8 interrupts masked: INTRTXE read back 0x%04x, INTRRXE 0x%04x, "
    "INTRUSBE 0x%02x, INTRUSB drained to 0x%02x (endpoint 0 and the bus events are "
    "polled; the bulk endpoints are unmasked in step 10 if the interrupt can be claimed)\n",
    MmioRead16 (MusbReg (A733_MUSB_INTRTXE_REG)),
    MmioRead16 (MusbReg (A733_MUSB_INTRRXE_REG)),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSBE_REG)),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSB_REG))
    ));

  //
  // 9. The core's DMA engine off, with its completion latches drained.
  //
  MusbDmaOff ();

  //
  // 10. The controller's interrupt, if the platform has one yet.  The two bulk
  // endpoints stop being one-packet-per-tick: the handler stages the OUT FIFO
  // into a ring and the poll hands the ring over, so the host's packets are
  // acknowledged as they arrive rather than at the tick's pace.  Endpoint 0 and
  // INTRUSB are not part of this (step 8) and stay polled.
  //
  // At dispatch the GIC protocol usually does not exist yet - this driver has no
  // depex on purpose, because a device that cannot get an interrupt must still
  // bring the port up - so this is the first of two attempts and Start() is the
  // one that normally succeeds.  Either way the failure is a log line, never a
  // failed bring-up: the poll is a complete data path, just a slow one.
  //
  MusbInterruptClaim ("dispatch");

  DEBUG ((
    DEBUG_INFO,
    "Musb: bring-up done: POWER=0x%02x DEVCTL=0x%02x ISCR=0x%08x PHYCTL=0x%08x route=0x%08x\n",
    MmioRead8 (MusbReg (A733_MUSB_POWER_REG)),
    MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)),
    MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)),
    MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)),
    MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG))
    ));

  //
  // DEVCTL is read and reported, never acted on.  HOSTMODE set is the one real
  // fault: a host-mode core drives no D+ pull-up and answers no SETUP.  SESSION
  // clear is not a fault - the driver that enumerates on this board runs with
  // SESSION clear and never writes DEVCTL - so it is an information line, with
  // the VBUS path next to it for the case where enumeration does not follow.
  //
  if ((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_HOSTMODE) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: DEVCTL.HOSTMODE (bit 2) is still set: the core is running as a host, "
      "so it will neither drive D+ nor answer SETUP\n"
      ));
  } else {
    DEBUG ((
      DEBUG_INFO,
      "Musb: DEVCTL reads 0x%02x (vbus=%a session=%u) - logged, not waited on: "
      "SESSION clear does not stop this core enumerating (ISCR force vbus=0x%x "
      "src=0x%x, PHYCTL.VBUSVLDEXT=%u, ISCR bit 29 pad=%u bit 30 line=%u)\n",
      MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)),
      mMusbVbusState[(MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) >> A733_MUSB_DEVCTL_VBUS_SHIFT)
                     & A733_MUSB_DEVCTL_VBUS_MASK],
      (UINT32)((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_SESSION) != 0),
      (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) >> A733_MUSB_ISCR_VBUS_FORCE_SHIFT)
               & A733_MUSB_ISCR_FORCE_MASK),
      (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) >> A733_MUSB_ISCR_VBUS_SRC_SHIFT)
               & A733_MUSB_ISCR_VBUS_SRC_MASK),
      (UINT32)((MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)) & A733_MUSB_PHYCTL_VBUSVLDEXT) != 0),
      (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) & A733_MUSB_ISCR_VBUS_VALID_FROM_VBUS) != 0),
      (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) & A733_MUSB_ISCR_VBUS_VALID_FROM_DATA) != 0)
      ));
  }

  mAddress       = 0;
  mConfiguration = 0;
  mEp0State      = MusbEp0Idle;
  mWatchValid    = FALSE;          // the first poll tick takes a new baseline
  mEp0WatchValid = FALSE;          // same, for the endpoint-0 registers
  mEpWatchValid  = FALSE;
  //
  // mStarted is what lets the poll touch the hardware, so it is set here, where
  // the pull-up goes up - not in Start().  A host may look at this port before any
  // consumer has taken the protocol, and a connect that nothing answers is worse
  // than no connect at all.
  //
  mStarted  = TRUE;
  mDeviceUp = TRUE;

  //
  // The re-attach window is timed on the performance counter, and a run in which
  // that counter has no frequency is a run without the window - it is insurance,
  // not a step of the bring-up.
  //
  mReattachFrequency = GetPerformanceCounterProperties (NULL, NULL);
  if (mReattachFrequency == 0) {
    DEBUG ((
      DEBUG_WARN,
      "Musb: re-attach after a reset is off: the performance counter reports no "
      "frequency, so the three-second window cannot be timed\n"
      ));
  }

  Status = MusbPollStart ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: the device half is up but nothing services it - every packet the host "
      "sends will be missed: %r\n",
      Status
      ));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: device up at %a: POWER=0x%02x DEVCTL=0x%02x ISCR=0x%08x route=0x%08x "
    "INTRUSBE=0x%02x INTRTXE=0x%04x INTRRXE=0x%04x, FIFO map for bulk IN EP%u / bulk OUT "
    "EP%u, %u us poll armed - the pull-up is up and the port is serviced from here; "
    "Start() adds only the transport's descriptors\n",
    Where,
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_POWER_REG)),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)),
    MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)),
    MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG)),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSBE_REG)),
    MmioRead16 (MusbReg (A733_MUSB_INTRTXE_REG)),
    MmioRead16 (MusbReg (A733_MUSB_INTRRXE_REG)),
    (UINT32)mBulkIn.Number,
    (UINT32)mBulkOut.Number,
    MUSB_POLL_PERIOD_100NS / 10U
    ));

  //
  // The state the poll starts from, in one line, so the log does not have to be
  // read backwards from the bring-up dump to know what "waiting" meant.  Read
  // after the timer was armed, which is the state the first tick will see.
  //
  DEBUG ((
    DEBUG_INFO,
    "Musb: pre-wait state: POWER=0x%02x (softconn=%u hsenab=%u suspend=%u hsmode=%u) "
    "DEVCTL=0x%02x (%a-device hostmode=%u session=%u vbus=%a) ISCR=0x%08x "
    "(dpdm_pullup=%u force_vbus=0x%x src=0x%x pad=%u line=%u) FADDR=0x%02x "
    "INTRUSBE=0x%02x\n",
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_POWER_REG)),
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_POWER_REG)) & A733_MUSB_POWER_SOFTCONN) != 0),
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_POWER_REG)) & A733_MUSB_POWER_HSENAB) != 0),
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_POWER_REG)) & A733_MUSB_POWER_SUSPEND) != 0),
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_POWER_REG)) & A733_MUSB_POWER_HSMODE) != 0),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)),
    ((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_BDEVICE) != 0) ? "B" : "A",
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_HOSTMODE) != 0),
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_SESSION) != 0),
    mMusbVbusState[(MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) >> A733_MUSB_DEVCTL_VBUS_SHIFT)
                   & A733_MUSB_DEVCTL_VBUS_MASK],
    MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) & A733_MUSB_ISCR_DPDM_PULLUP_EN) != 0),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) >> A733_MUSB_ISCR_VBUS_FORCE_SHIFT)
             & A733_MUSB_ISCR_FORCE_MASK),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) >> A733_MUSB_ISCR_VBUS_SRC_SHIFT)
             & A733_MUSB_ISCR_VBUS_SRC_MASK),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) & A733_MUSB_ISCR_VBUS_VALID_FROM_VBUS) != 0),
    (UINT32)((MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)) & A733_MUSB_ISCR_VBUS_VALID_FROM_DATA) != 0),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG)),
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_INTRUSBE_REG))
    ));

  return EFI_SUCCESS;
}

/**
  Take the transport's descriptors and callbacks; the device is already up.

  The device half - steps 2 to 9 and the poll - ran from the entry point at DXE
  dispatch, as the reference's self-starting SunxiMusbDeviceDxe brings its device
  up from its own entry point, so by the time a consumer calls this the pull-up
  has been up and the port serviced for as long as the boot has been running.
  What is left for Start() is the transport's half of the agreement: the
  descriptor set to serve, the two callbacks, and the bulk endpoint numbers the
  FIFO map was written for before the descriptors existed.

  This never brings the controller up again.  The host may have enumerated the
  device already, and re-running the bring-up would pulse the device's CCU reset
  and PHY_RSTN under a live session - the reference refuses to restart for
  exactly that reason (a restart wipes the endpoint configuration the host has
  already SET_CONFIGURATION'd, and because SOFTCONN never drops the host does not
  re-enumerate, so bulk traffic then fails with a protocol error).  The one
  bring-up write a Start() can add is the FIFO map, and only when these
  descriptors name endpoints other than the ones it was written for.

  @param[in] DeviceDescriptor  The device descriptor to present.
  @param[in] Descriptors       One configuration descriptor per configuration.
  @param[in] RxCallback        Called with each bulk OUT packet.
  @param[in] TxCallback        Called when a queued bulk IN buffer has been sent.

  @retval EFI_SUCCESS            The descriptor set is in force and the port is
                                 being serviced.
  @retval EFI_INVALID_PARAMETER  The descriptors are not a usable set.
**/
STATIC
EFI_STATUS
EFIAPI
MusbDeviceStart (
  IN USB_DEVICE_DESCRIPTOR   *DeviceDescriptor,
  IN VOID                    **Descriptors,
  IN USB_DEVICE_RX_CALLBACK  RxCallback,
  IN USB_DEVICE_TX_CALLBACK  TxCallback
  )
{
  EFI_STATUS  Status;

  if ((DeviceDescriptor == NULL) || (Descriptors == NULL) ||
      (DeviceDescriptor->NumConfigurations == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  mDeviceDescriptor  = DeviceDescriptor;
  mConfigDescriptors = Descriptors;
  mRxCallback        = RxCallback;
  mTxCallback        = TxCallback;

  //
  // Copy the configuration descriptor and describe it at the speed in force
  // before anything reads or serves it: what the host is told, what the
  // endpoints are programmed with and what Send() chunks at all come from the
  // same numbers.
  //
  Status = MusbPrepareConfiguration ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: the transport's configuration descriptor cannot be served: %r\n",
      Status
      ));
    return Status;
  }

  Status = MusbParseConfiguration ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Musb: the transport's descriptors carry no bulk endpoint pair: %r\n", Status));
    return Status;
  }

  //
  // This controller's control endpoint is fixed at 64 bytes, and one
  // configuration is what the protocol's descriptor set is walked for.
  //
  if (DeviceDescriptor->MaxPacketSize0 != A733_MUSB_EP0_MAX_PACKET) {
    DEBUG ((
      DEBUG_WARN,
      "Musb: device descriptor asks for a %u-byte control endpoint; the core has 64\n",
      (UINT32)DeviceDescriptor->MaxPacketSize0
      ));
  }

  if (DeviceDescriptor->NumConfigurations != 1) {
    DEBUG ((
      DEBUG_WARN,
      "Musb: %u configurations offered, only the first is served\n",
      (UINT32)DeviceDescriptor->NumConfigurations
      ));
  }

  if (mDeviceUp) {
    //
    // The FIFO map was written at dispatch, before any descriptor existed, for
    // the layout the transport declares (MusbAssumeTransportEndpoints).  These
    // descriptors are the authority, so a different pair of endpoint numbers is
    // the one case where the map is rewritten - and the endpoints it previously
    // owned are zeroed with it, because MusbFifoLayout sizes every endpoint the
    // pair does not name to no FIFO at all.  Nothing else about the device half
    // is touched: no CCU reset, no PHY_RSTN, no POWER write, so the host's
    // session and the address it assigned are left standing.
    //
    if ((mBulkIn.Number != mFifoInNumber) || (mBulkOut.Number != mFifoOutNumber)) {
      DEBUG ((
        DEBUG_WARN,
        "Musb: Start(): the transport's bulk endpoints (IN EP%u, OUT EP%u) are not the "
        "ones the FIFO map was written for at dispatch (IN EP%u, OUT EP%u) - "
        "reprogramming the map and nothing else\n",
        (UINT32)mBulkIn.Number,
        (UINT32)mBulkOut.Number,
        (UINT32)mFifoInNumber,
        (UINT32)mFifoOutNumber
        ));
      //
      // The masks go down first: they name the endpoints the map is about to
      // stop serving, and an interrupt that arrived between the two writes would
      // be reported against the new numbers.  MusbInterruptClaim below puts them
      // back from the same numbers the map was written with.
      //
      MmioWrite16 (MusbReg (A733_MUSB_INTRTXE_REG), 0);
      MmioWrite16 (MusbReg (A733_MUSB_INTRRXE_REG), 0);
      MusbFifoLayout ();
      MusbLogFifoLayout ("Start()");
      MusbSelectEp (0);
    }

    //
    // The bulk data path's interrupt is claimed here for the normal case: the
    // entry point ran at dispatch, before ArmGicDxe had installed the protocol,
    // and a fastboot consumer only appears long after it has.  The claim also
    // (re)writes the endpoint masks, so a map just reprogrammed for other
    // endpoint numbers is what the handler is handed.
    //
    MusbInterruptClaim ("Start()");

    DEBUG ((
      DEBUG_INFO,
      "Musb: Start(): the device half has been up since DXE dispatch - not redone; "
      "descriptors and callbacks taken, POWER=0x%02x DEVCTL=0x%02x FADDR=%u\n",
      (UINT32)MmioRead8 (MusbReg (A733_MUSB_POWER_REG)),
      (UINT32)MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)),
      (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG))
      ));
  } else {
    //
    // Only reachable if the dispatch-time attempt did not get the device up.  The
    // descriptors are in force by now, so the map is written for the real
    // endpoints rather than the assumed ones.
    //
    DEBUG ((
      DEBUG_WARN,
      "Musb: Start(): the device half did not run at dispatch - running it here\n"
      ));
    Status = MusbDeviceBringUp ("Start()");
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  //
  // The poll runs from dispatch and Start() only makes sure of it: mStarted is
  // read by every tick, and only ExitBootServices clears it.
  //
  mStarted = TRUE;
  Status   = MusbPollStart ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // A SETUP can arrive before this call - the device is up at dispatch and the
  // transport is not - and GET_DESCRIPTOR(DEVICE) was stalled because there was
  // no descriptor to serve.  A host that took that stall as a dead port does not
  // look at the connect again, so the one-shot re-attach window is armed here,
  // from now: a SETUP within three seconds means the host is still enumerating
  // (and is answered now), and silence means it gave up and needs the connect to
  // change.
  //
  if (mDescriptorMiss) {
    mDescriptorMiss = FALSE;
    if (!mReattachDone && (mReattachFrequency != 0)) {
      mReattachArmed   = TRUE;
      mReattachSetups  = mSetupsHandled;
      mReattachResetAt = GetPerformanceCounter ();
      DEBUG ((
        DEBUG_INFO,
        "Musb: Start(): the host asked for the device descriptor before this call and "
        "was refused (%u SETUP(s) handled so far); if none follows within %u ms the one-shot "
        "re-attach gives it a fresh connect to enumerate\n",
        mSetupsHandled,
        MUSB_REATTACH_AFTER_US / 1000U
        ));
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: connected, waiting for the host (device %04x:%04x), bulk IN 0x%02x (%u B), "
    "bulk OUT 0x%02x (%u B)\n",
    mDeviceDescriptor->IdVendor,
    mDeviceDescriptor->IdProduct,
    mBulkIn.Address,
    (UINT32)mBulkIn.MaxPacket,
    mBulkOut.Address,
    (UINT32)mBulkOut.MaxPacket
    ));

  return EFI_SUCCESS;
}

/**
  Stop the service timer when the OS takes over.

  The timer exists to poll a controller that nothing else has claimed; leaving
  it armed across ExitBootServices would keep a 10 kHz notify running against
  an MMIO window the OS is about to reset.
**/
STATIC
VOID
EFIAPI
MusbExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mPollRunning) {
    //
    // The interrupt goes first, both ends of it: the endpoint masks inside the
    // controller and then the source at the distributor.  A level line left
    // enabled would keep asserting into whatever runs next, and this driver's
    // handler is about to become part of a torn-down image.
    //
    if (mIrqClaimed) {
      MmioWrite16 (MusbReg (A733_MUSB_INTRTXE_REG), 0);
      MmioWrite16 (MusbReg (A733_MUSB_INTRRXE_REG), 0);
      if (mInterrupt2 != NULL) {
        mInterrupt2->DisableInterruptSource (mInterrupt2, (HARDWARE_INTERRUPT_SOURCE)MUSB_GIC_INTID);
      } else if (mInterrupt != NULL) {
        mInterrupt->DisableInterruptSource (mInterrupt, (HARDWARE_INTERRUPT_SOURCE)MUSB_GIC_INTID);
      }

      mIrqClaimed = FALSE;
    }

    //
    // A window still open is closed first, so a trace recorded after the last
    // drain is not lost with the timer.
    //
    MusbQuietLeave ("ExitBootServices");
    //
    // One last state line at the moment the poll stops, so the end of the log
    // is not mistaken for the end of the events.
    //
    MusbLogHeartbeat ("(last, timer about to be cancelled)");
    gBS->SetTimer (mPollEvent, TimerCancel, 0);
    mPollRunning = FALSE;
    mStarted     = FALSE;
  }
}

/**
  Bring the platform and PHY0 up at DXE dispatch, publish the protocol, then bring
  the device half up and service it.

  Both phases run here, in the order the reference runs them across its two
  drivers: the board and the PHY are powered and programmed first, then the MUSB
  core is brought up and its poll armed, all before BDS runs and minutes before
  the fastboot transport asks for the controller.  The reference's device driver
  is self-starting in exactly this sense - it brings its device up from its own
  entry point and publishes the protocol on the way - and the port's one remaining
  difference from it was that this driver waited for Start() to run the device
  half.

  The state the driver inherited is printed first, before any of its own writes
  land: the CCU words, the core's role and address, and the wrapper's three
  registers - a run in which the PHY was already powered differs from one in
  which this driver powers it up, and that line is what says which happened.

  Failure is not fatal.  A platform or device half that does not read back as up
  is logged and the protocol is installed anyway: a driver that refused to
  publish would turn a PHY problem into "no fastboot at all" with no log to read,
  and Start() retries the device half if it is not up by then.
**/
EFI_STATUS
EFIAPI
A733MusbDeviceEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ExitEvent;

  DEBUG ((
    DEBUG_INFO,
    "Musb: USB0 before this driver runs: CCU USB0_CLK=0x%08x USB0_BGR=0x%08x "
    "(device_gate=%u device_rst=%u) POWER=0x%02x DEVCTL=0x%02x "
    "(%a-device hostmode=%u session=%u vbus=%a) FADDR=%u "
    "wrapper ISCR=0x%08x PHYCTL=0x%08x route=0x%08x\n",
    MmioRead32 (CcuReg (A733_CCU_USB0_CLK)),
    MmioRead32 (CcuReg (A733_CCU_USB0_BGR)),
    (MmioRead32 (CcuReg (A733_CCU_USB0_BGR)) & A733_CCU_USB_BGR_DEVICE_GATE) != 0,
    (MmioRead32 (CcuReg (A733_CCU_USB0_BGR)) & A733_CCU_USB_BGR_DEVICE_RST) != 0,
    MmioRead8 (MusbReg (A733_MUSB_POWER_REG)),
    MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)),
    ((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_BDEVICE) != 0) ? "B" : "A",
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_HOSTMODE) != 0),
    (UINT32)((MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) & A733_MUSB_DEVCTL_SESSION) != 0),
    mMusbVbusState[(MmioRead8 (MusbReg (A733_MUSB_DEVCTL_REG)) >> A733_MUSB_DEVCTL_VBUS_SHIFT)
                   & A733_MUSB_DEVCTL_VBUS_MASK],
    (UINT32)MmioRead8 (MusbReg (A733_MUSB_FADDR_REG)),
    MmioRead32 (MusbReg (A733_MUSB_ISCR_REG)),
    MmioRead32 (MusbReg (A733_MUSB_PHYCTL_REG)),
    MmioRead32 (MusbReg (A733_MUSB_ROUTE_REG))
    ));

  //
  // Phase 1, at dispatch: the power domain, the USB subsystem clocks, USB0's
  // host half, the resistance calibration, PHY0 through the HCI window, and the
  // device half's clock and reset.  This call's own failure is logged and not
  // acted on: the device half follows anyway, and the protocol is published
  // either way (see this function's head).
  //
  MusbPlatformBringUp ();

  DEBUG ((
    DEBUG_INFO,
    "Musb: dispatch: platform and PHY0 up before the protocol is published "
    "(power domain, subsystem clocks, USB0's host half, resistance calibration, PHY0 "
    "through the HCI window, USB0's device gate and reset) - status=%a\n",
    mPlatformUp ? "up" : "not up, see the line above"
    ));

  mUsbDeviceProtocol.Start = MusbDeviceStart;
  mUsbDeviceProtocol.Send  = MusbDeviceSend;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gUsbDeviceProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mUsbDeviceProtocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Musb: cannot publish the USB device protocol: %r\n", Status));
    return Status;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MusbExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &ExitEvent
                  );
  if (EFI_ERROR (Status)) {
    //
    // Losing this only means the timer outlives the boot, which is untidy and
    // not fatal; the protocol is already published.
    //
    DEBUG ((DEBUG_WARN, "Musb: cannot register the ExitBootServices handler: %r\n", Status));
  }

  //
  // Phase 2, immediately after the protocol, and at dispatch rather than at a
  // consumer's convenience: the device half - SRAM, the PHY route and passby
  // words, ISCR, POWER, the FIFO map, endpoint 0, the masked interrupts, the DMA
  // engine off - and the poll timer.  This is what the reference's
  // SunxiMusbDeviceDxe does from its own entry point, and it is the one
  // structural difference the port had left: with it, D+ is pulled up and the
  // port is being serviced before BDS, and the fastboot application's Start()
  // arrives as it does on the reference - after the device is already answering.
  //
  // The bulk endpoints are the transport's, and the transport has not spoken yet,
  // so the FIFO map is written for the layout it declares
  // (MusbAssumeTransportEndpoints) and Start() reprograms it if it declares
  // others.  A failure here is logged and the protocol stays published: a device
  // that cannot start is still one Start() can retry.
  //
  MusbAssumeTransportEndpoints (&mBulkIn, &mBulkOut);

  Status = MusbDeviceBringUp ("dispatch");
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Musb: dispatch: the device half did not come up (%r); the protocol is published "
      "and Start() retries whatever is not up\n",
      Status
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_INFO,
    "Musb: dispatch: the device is up at DXE dispatch - D+ is pulled up and the port is "
    "serviced before BDS, as the reference's self-starting device driver leaves it; "
    "Start() adds only the transport's descriptors\n"
    ));

  return EFI_SUCCESS;
}