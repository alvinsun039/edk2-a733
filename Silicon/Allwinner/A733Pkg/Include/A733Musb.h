/** @file
  Mentor MUSB device-core register map, as the A733 (sun60iw2p1) implements it.

  Manual: 18.5 "USB2.0 DRD" is the whole of what the A733 user manual says about
  this core - 18.5.1 lists what it can do (bi-directional endpoint 0, nine IN
  and nine OUT endpoints including EP0, up to 8 KB + 64 B of endpoint FIFO, soft
  connect/disconnect, an internal DMA controller, and one 8 KB SRAM and one PHY
  shared with the host controller) and 18.5.2 draws the block diagram that shows
  the device controller, the host controller and PHY0 hanging off the same
  UTMI and SRAM.  It publishes no register list: the offsets and bit positions
  below are the Mentor MUSB register model - the same indexed-endpoint core
  that Linux's "musb" and U-Boot's "musb-new" program - laid out the way this
  particular instantiation decodes it.

  Provenance, stated plainly because this core's window is not the classic
  MUSB one: the layout below differs from the original Inventra map in where
  the two halves of the register file sit.  Reading from 0x00 are the
  per-endpoint FIFO data ports (four bytes each, one port per endpoint number,
  so EP0 is +0x00 and EP2 is +0x08); the common and indexed control registers
  start at +0x40 and +0x80 instead of +0x00 and +0x10.  That placement, and the
  wrapper registers at +0x400 (ISCR, PHYCTL, SEL), are the one part of this
  header the manual does not describe; they are marked "not in the manual"
  where they appear, and they match the register state a device-mode driver
  known to enumerate on this board programs.  Everything read or written
  through those offsets was checked against this silicon by watching the
  endpoint state change, not assumed from another SoC.

  What is *not* here, deliberately: the second half of the OTG wrapper.  The
  window at 0x04100400 is the PHY control block the board's device tree names
  "phy_ctrl", and its +0x20 word carries the route select that the FES
  (flashing) path owns.  A733MusbDeviceDxe writes exactly one bit of it - the
  "MUSB is on the pads" select, BIT0 - and nothing else; the rest of that
  register stays with the flashing path.  See A733MusbDeviceDxe.c.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_MUSB_H__
#define A733_MUSB_H__

#include <Uefi.h>

//
// Manual: memory map, USB0 window 0x04100000---0x041FFFFF.  The MUSB core is
// the first block of it, immediately below EHCI0 at +0x1000 (18.5.2's block
// diagram: the device controller and the host controller of one USB2.0 DRD
// port sit side by side in the same window and share PHY0 and the FIFO SRAM).
//
#define A733_MUSB_BASE  0x04100000U

//
// The core's own window: 0x00---0x9F of registers, then the Allwinner wrapper
// from +0x400.  One 4 KB page covers every register this driver touches, and
// the platform's MMU map already covers 64 KB of the window.
//
#define A733_MUSB_WINDOW_SIZE  0x1000U

//
// Per-endpoint FIFO data ports.  The port is 32 bits wide and one endpoint
// number owns exactly one port, whichever direction that endpoint runs: EP0's
// control FIFO at +0x00, EP1 (the bulk IN endpoint) at +0x04, EP2 (the bulk OUT
// endpoint) at +0x08.  A write fills the IN side, a read empties the OUT side.
//
#define A733_MUSB_FIFO_PORT(Ep)  (0x00U + 4U * (Ep))

//
// Common registers (not endpoint-indexed).
//
#define A733_MUSB_POWER_REG    0x40U
#define A733_MUSB_DEVCTL_REG   0x41U
#define A733_MUSB_INDEX_REG    0x42U
#define A733_MUSB_VEND0_REG    0x43U   // Allwinner vendor hook: PIO/DMA select
#define A733_MUSB_INTRTX_REG   0x44U   // 16 bits, one bit per IN endpoint
#define A733_MUSB_INTRRX_REG   0x46U   // 16 bits, one bit per OUT endpoint
#define A733_MUSB_INTRTXE_REG  0x48U
#define A733_MUSB_INTRRXE_REG  0x4AU
#define A733_MUSB_INTRUSB_REG  0x4CU
#define A733_MUSB_INTRUSBE_REG 0x50U
#define A733_MUSB_FRAME_REG    0x54U

//
// POWER (common).  HSENAB lets the core answer the host's high-speed chirp;
// SOFTCONN is the soft-connect the manual's feature list names - with it clear
// the core does not drive the D+ pull-up and the host cannot see the device;
// ISOUPDATE selects isochronous mode and must be 0 for the bulk/control
// traffic this driver serves.
//
// Bit 1 is SUSPENDM.  The A733 host firmware's device bring-up sets it in the
// same write that sets HSENAB and SOFTCONN, and this driver does the same: one
// POWER write, never cleared.  An earlier version of this header asserted the
// opposite - that the enumerating stack never sets bit 1 and that a core told to
// suspend accepts a SETUP and cannot answer it - which was inferred from a port
// that never enumerated for an unrelated reason (it was brought up with the
// wrong half of USB0 in charge).  The reference write is the measured one.
//
#define A733_MUSB_POWER_ISOUPDATE  BIT7
#define A733_MUSB_POWER_SOFTCONN   BIT6
#define A733_MUSB_POWER_HSENAB     BIT5
#define A733_MUSB_POWER_HSMODE     BIT4   // read-only: the core is running at HS
#define A733_MUSB_POWER_RESET      BIT3
#define A733_MUSB_POWER_RESUME     BIT2
#define A733_MUSB_POWER_SUSPEND    BIT1   // SUSPENDM in musb_regs.h
#define A733_MUSB_POWER_ENSUSPEND  BIT0

//
// DEVCTL (common).  The bit assignments are the MUSB core's own, the ones
// Linux's "musb" and U-Boot's "musb-new" decode (musb_regs.h, MUSB_DEVCTL_*),
// and they are not the OTG session-control byte another core's DEVCTL would
// put here:
//
//   bit 7  BDEVICE   status: 0 = A device (the host side of the cable),
//                    1 = B device.  A peripheral on a B-side connector reads
//                    this as 1; it is not the role select, and a log reader
//                    who takes it for one draws the wrong conclusion from a
//                    perfectly healthy byte.
//   bit 6  FSDEV     status: the core is running as a full-speed device
//   bit 5  LSDEV     status: low-speed device (host side, an LS peripheral)
//   bit 4:3 VBUS     what the PHY's VBUS comparator sees: 0 below VBUS-valid,
//                    1 above VBUS-valid, 2 above session-end, 3 above
//                    VBus-valid
//   bit 2  HOSTMODE  the role select: 1 = the core is running as a host, which
//                    means it never drives the D+ pull-up and never answers
//                    SETUP.  This is the bit a peripheral clears.
//   bit 1  HOSTREQ   the A-device's host request; cleared here, meaningless in
//                    peripheral mode
//   bit 0  SESSION   a session is in progress - VBUS is present - which is
//                    what permits the core to drive its D+ pull-up
//
// So 0x80 reads "B device, VBUS below valid, no session": a core that is
// already in peripheral mode and does not see VBUS.  It does not read "host
// mode"; that is 0x04.
//
#define A733_MUSB_DEVCTL_SESSION      BIT0
#define A733_MUSB_DEVCTL_HOSTREQ      BIT1
#define A733_MUSB_DEVCTL_HOSTMODE     BIT2
#define A733_MUSB_DEVCTL_VBUS_SHIFT   3U
#define A733_MUSB_DEVCTL_VBUS_MASK    0x3U
#define A733_MUSB_DEVCTL_LSDEV        BIT5
#define A733_MUSB_DEVCTL_FSDEV        BIT6
#define A733_MUSB_DEVCTL_BDEVICE      BIT7

//
// INTRUSB: the bus-level events.  They latch whether or not INTRUSBE unmasks
// them, which is what lets this driver poll instead of taking an interrupt.
//
#define A733_MUSB_INTRUSB_SUSPEND     BIT0
#define A733_MUSB_INTRUSB_RESUME      BIT1
#define A733_MUSB_INTRUSB_RESET       BIT2
#define A733_MUSB_INTRUSB_SOF         BIT3
#define A733_MUSB_INTRUSB_CONNECT     BIT4
#define A733_MUSB_INTRUSB_DISCONNECT  BIT5
#define A733_MUSB_INTRUSB_SESSREQ     BIT6
#define A733_MUSB_INTRUSB_VBUSERROR   BIT7
// Write-1-to-clear bits of INTRUSB that this driver acknowledges.
#define A733_MUSB_INTRUSB_W1C  (A733_MUSB_INTRUSB_SUSPEND | A733_MUSB_INTRUSB_RESUME | \
                                A733_MUSB_INTRUSB_RESET |   A733_MUSB_INTRUSB_SOF |    \
                                A733_MUSB_INTRUSB_CONNECT | A733_MUSB_INTRUSB_DISCONNECT | \
                                A733_MUSB_INTRUSB_SESSREQ | A733_MUSB_INTRUSB_VBUSERROR)

//
// Endpoint-indexed registers, selected by INDEX.  EP0 answers TXMAXP/CSR0 with
// the control-endpoint meaning of each bit, the other endpoints answer with the
// bulk/interrupt meaning; the addresses are the same either way.
//
#define A733_MUSB_TXMAXP_REG     0x80U
#define A733_MUSB_TXCSR_REG      0x82U   // CSR0 when INDEX is 0
#define A733_MUSB_RXMAXP_REG     0x84U   // no meaning at INDEX 0 (EP0 is fixed 64 B)
#define A733_MUSB_RXCSR_REG      0x86U
#define A733_MUSB_RXCOUNT_REG    0x88U   // COUNT0 at INDEX 0; 16 bits wide on this core
#define A733_MUSB_TXTYPE_REG     0x8CU
#define A733_MUSB_TXINTERVAL_REG 0x8DU
#define A733_MUSB_RXTYPE_REG     0x8EU
#define A733_MUSB_RXINTERVAL_REG 0x8FU
#define A733_MUSB_TXFIFOSZ_REG   0x90U
#define A733_MUSB_TXFIFOADD_REG  0x92U   // 16 bits, in 8-byte units
#define A733_MUSB_RXFIFOSZ_REG   0x94U
#define A733_MUSB_RXFIFOADD_REG  0x96U   // 16 bits, in 8-byte units
#define A733_MUSB_FADDR_REG      0x98U   // the address the core answers to

//
// FADDR: the function address the host assigned, 0---127.  Bit 7 is the
// "update" strobe on cores that have one; this one takes the address directly,
// which is what SET_ADDRESS's completion writes.
//
#define A733_MUSB_FADDR_MASK  0x7FU
//
// FADDR bit 7.  In the Mentor register model this is UPDATE: the core takes the
// address in bits 6:0 when it is written with UPDATE set, and clears the bit
// once the address is in force.  It is written and read back once per
// SET_ADDRESS so the log shows whether this core needs it.
//
#define A733_MUSB_FADDR_UPDATE  0x80U

//
// CSR0, peripheral mode (INDEX = 0, TXCSR address).  The names are the
// peripheral-mode meaning of the MUSB core's CSR0 bits:
//
//   0x0001 RXPKTRDY   a SETUP packet (or OUT data) is in the FIFO
//   0x0002 TXPKTRDY   an IN packet is loaded and not yet taken by the host
//   0x0004 SENTSTALL  this core stalled the endpoint; write 1 to acknowledge
//   0x0008 DATAEND    the data stage is done: arm the status stage
//   0x0010 SETUPEND   a SETUP arrived in the middle of a transfer
//   0x0020 SENDSTALL  stall this endpoint
//   0x0040 SVDRXPKTRDY  acknowledge the packet in the FIFO (clears RXPKTRDY)
//   0x0080 SVDSETUPEND  acknowledge SETUPEND
//   0x0100 FLUSHFIFO    drop whatever is in the FIFO
//
// A request with no data stage is finished with ONE write of
// SVDRXPKTRDY|DATAEND (0x0048): the acknowledge and "the data phase is over"
// go out together and the core answers the host's status IN token with a
// zero-length packet itself.  TXPKTRDY must not be set for that packet - with
// it set the core waits for a data packet that was never loaded, CSR0 keeps
// reading 0x000A, no zero-length packet appears on the wire, and the status
// stage only ends when the host gives up with a bus reset (measured:
// SET_ADDRESS re-sent four times, CSR0.DATAEND stuck at 42 ticks).
//
// The vendor device stack ends the stage exactly that way:
// __USBC_Dev_ep0_ReadDataComplete (usbc_dev.c:130-134) writes
// (1 << SERVICED_RX_PKT_READY) | (1 << DATA_END) and nothing else; for a
// request with no data stage it is reached from sunxi_udc_set_address ->
// USBC_Dev_ReadDataStatus(..., EP0, 1) (usb_base_platform.c:1011-1015), and
// for every EP0 packet the stack reads from __usb_readcomplete
// (usb_base_platform.c:573-589 -> 705).
//
// SVDRXPKTRDY|DATAEND is also a write with bit 0 clear, which is how this core
// clears RXPKTRDY (see the clearing note below), so the acknowledge still
// lands.  A data-IN stage is different: its last packet carries DATAEND
// together with TXPKTRDY, because there a packet really is loaded
// (__USBC_Dev_ep0_WriteDataComplete, usbc_dev.c:141-145).
//
// FLUSHFIFO is 0x0100 here and 0x0008 in TXCSR/RXCSR (see the TXCSR block
// below): the two are different registers' bits and are not
// interchangeable.  Both implementations that enumerate on this board flush
// EP0 with this bit - the vendor device stack writes exactly
// (1 << USBC_BP_CSR0_D_FLUSH_FIFO), BP = 8 - and the flush bit self-clears.
//
// Note the clearing convention, which is this core's and not the classic
// MUSB one: RXPKTRDY, SENTSTALL and OVERRUN are cleared by writing 0 to them
// and a 1 leaves them alone.  That is why an acknowledge reads the register,
// clears the one bit and writes the rest back - writing a bare SENTSTALL
// would clear RXPKTRDY and TXPKTRDY with it.
//
#define A733_CSR0_RXPKTRDY    0x0001U
#define A733_CSR0_TXPKTRDY    0x0002U
#define A733_CSR0_SENTSTALL   0x0004U
#define A733_CSR0_DATAEND     0x0008U
#define A733_CSR0_SETUPEND    0x0010U
#define A733_CSR0_SENDSTALL   0x0020U
#define A733_CSR0_SVDRXPKTRDY 0x0040U
#define A733_CSR0_SVDSETUPEND 0x0080U
#define A733_CSR0_FLUSHFIFO   0x0100U

//
// TXCSR for the bulk IN endpoint.  MODE (bit 13) marks the endpoint as an IN
// one; CLRDATATOG resets the data toggle, which a bus reset requires because
// the host resets its side too.
//
#define A733_TXCSR_TXPKTRDY    0x0001U
#define A733_TXCSR_FIFONOTEMPTY 0x0002U
#define A733_TXCSR_UNDERRUN    0x0004U
#define A733_TXCSR_FLUSHFIFO   0x0008U
#define A733_TXCSR_SENDSTALL   0x0010U
#define A733_TXCSR_SENTSTALL   0x0020U
#define A733_TXCSR_CLRDATATOG  0x0040U
#define A733_TXCSR_MODE        0x2000U

//
// RXCSR for the bulk OUT endpoint.  A completed packet is acknowledged by
// clearing RXPKTRDY (write 0 to it - see the clearing note under CSR0 above),
// and SENTSTALL and OVERRUN go with it; ISO (bit 14) must stay clear or the
// endpoint ACKs packets without putting them in the FIFO.
//
#define A733_RXCSR_RXPKTRDY   0x0001U
#define A733_RXCSR_FIFOFULL   0x0002U
#define A733_RXCSR_OVERRUN    0x0004U
#define A733_RXCSR_DATAERROR  0x0008U
#define A733_RXCSR_FLUSHFIFO  0x0010U
#define A733_RXCSR_SENDSTALL  0x0020U
#define A733_RXCSR_SENTSTALL  0x0040U
#define A733_RXCSR_CLRDATATOG 0x0080U
#define A733_RXCSR_INCOMPRX   0x0100U
#define A733_RXCSR_DISNYET    0x1000U
#define A733_RXCSR_ISO        0x4000U
#define A733_RXCSR_ACK_MASK   (A733_RXCSR_RXPKTRDY | A733_RXCSR_SENTSTALL | A733_RXCSR_OVERRUN)
//
// RXCSR's engine bits, from the vendor's __USBC_Dev_Rx_ConfigEpDma /
// __USBC_Dev_Rx_ClearEpDma (usbc_dev.c): DMA_REQ_EN is the endpoint's request
// line to the engine, DMA_REQ_MODE (mode 0 for bulk OUT here) is left clear, and
// AUTO_CLEAR lets the core acknowledge a drained packet itself - which is what
// makes a transfer with no CPU in it possible at all.  The vendor sets all
// three on both bulk endpoints before arming a channel, and clears them again
// before handing the endpoint back to its FIFO ports.
//
#define A733_RXCSR_DMA_REQ_MODE 0x0800U
#define A733_RXCSR_DMA_REQ_EN   0x2000U
#define A733_RXCSR_AUTOCLEAR    0x8000U
#define A733_RXCSR_DMA_MASK     (A733_RXCSR_AUTOCLEAR | A733_RXCSR_DMA_REQ_EN | A733_RXCSR_DMA_REQ_MODE)

//
// TXFIFOSZ / RXFIFOSZ: FIFO size as 2^(field + 3) bytes, and DPB (bit 4) to
// double-buffer the endpoint.  Both bulk endpoints run one packet deep here; the
// map below says why, and the measurement that decided it is in the driver.
//
#define A733_MUSB_FIFOSZ_DPB       0x10U
#define A733_MUSB_FIFOSZ_SIZE_64   0x03U   // 2^6  =   64 B
#define A733_MUSB_FIFOSZ_SIZE_512  0x06U   // 2^9  =  512 B
#define A733_MUSB_FIFOSZ_SIZE_4096 0x09U   // 2^12 = 4096 B
#define A733_MUSB_FIFOSZ_SIZE_MASK 0x0FU

//
// The FIFO SRAM map this driver programs, in bytes.  The SRAM is the 8 KB the
// manual's feature list gives the endpoint FIFOs, shared with the host
// controller, and each endpoint's buffer is placed by TXFIFOADD/RXFIFOADD in
// 8-byte units:
//
//   EP0  one fixed 64 B buffer at 0    the control endpoint's FIFO (the "+64 B"
//                                     the manual lists on top of the 8 KB).
//                                     Not programmable: INDEX 0 has no
//                                     TXFIFOSZ/RXFIFOSZ - a write to them reads
//                                     back 0x00 - because the core owns this
//                                     FIFO itself, 64 B shared by both
//                                     directions in full-speed mode.
//   EP1  TX 512 B at 1124             bulk IN  (the vendor's address, unchanged)
//   EP2  RX 512 B at 2048             bulk OUT.  The address reserves the 4096 B
//        (4096 B reserved)            an eight-packet FIFO would need; 512 B is
//                                     what RXFIFOSZ selects - see below.
//
// EP1's TX FIFO is where the vendor puts it (usb_base_platform.c:650-663,
// __usb_bulk_ep_reset: bulk_in_addr = 1124), and that address is what reserves
// the bytes 100..1123 below it for the bulk-OUT RX FIFO - i.e. exactly the
// vendor's 512 B doubled by DPB, 1024 B.  A 4096-byte RX FIFO therefore cannot
// live at the vendor's byte 100 alongside it: 100 + 4096 = 4196 runs through
// EP1's 1124..1635 and would have the bulk IN and the bulk OUT endpoints
// writing the same SRAM.  The bulk-OUT FIFO is the one that moves here, to the
// first 8-byte-aligned, power-of-two address above EP1's TX FIFO: bytes
// 2048..6143, with 1636..2047 and 6144..8255 left as slack inside the 8 KB.
// EP1's TX FIFO is the one endpoint a deepening would buy nothing for, so it is
// the one that keeps the reference's address.
//
// Why the bulk-OUT FIFO's address reserves 4096 B, and why the size is 512.
// One 512-byte buffer means at most one packet in the FIFO, so the core answers
// one OUT token per microframe and NAKs the host for the rest of the microframe
// while software drains the packet.  Eight packets of buffering would let eight
// consecutive OUT tokens land back to back and the drain take a whole burst of
// them per interrupt (see MusbBulkOutStage) - that is the shape of Linux's PIO
// path on this core, where musb_gadget.c's rxstate reads MUSB_RXCOUNT and drains
// that many bytes in one musb_read_fifo call, and it is why the vendor's own map
// gives the bulk-OUT FIFO 1024 B (512 doubled by DPB).  It was built here and it
// measured nothing: 3.02-3.16 MB/s against 3.15 with one packet, because the NAK
// was never the limit - the CPU reading 128 FIFO words per packet is - and the
// deeper FIFO only added a 1.2 ms TPL_HIGH hold per burst.  So the address keeps
// the room (moving EP2 was the expensive part and the space costs nothing) and
// RXFIFOSZ selects 512 B; the burst path in the drain stays because it is
// correct for either.
//
// The two vendor addresses are the ones both implementations that enumerate on
// this board use, field for field:
//
//   SunxiMusbDeviceDxe     TXFIFOADD = 1124 >> 3 = 140, RXFIFOADD = 100 >> 3 = 12
//   vendor sunxi_usb       bulk_out_addr = 1124, bulk_in_addr = 100
//                          (usb_base_platform.c; the TX FIFO of the bulk IN
//                          endpoint at 1124 and the RX FIFO of the bulk OUT
//                          endpoint at 100, i.e. OUT below IN)
//
// The register field is in 8-byte units, so the vendor's two byte figures
// truncate: the core ends up with 1120 and 96.  The addresses below are stored
// as bytes and shifted where they are written, so only EP1's is a truncating
// one; EP2's 2048 is already a multiple of 8, which is why the read-back line
// prints the field and the byte address it decodes to - a FIFO-address write
// that does not stick, or that lands somewhere other than intended, is then a
// line in the boot log (MusbLogFifoLayout) rather than a hypothesis.
//
// An earlier version of this driver placed EP1 at byte 128 and EP2 at 1152 -
// its own invention, with no reference behind it - so the vendor's addresses
// are kept where they still work.
//
// The vendor stack also sets DPB (double packet buffer) on both bulk FIFOs;
// SunxiMusbDeviceDxe does not and enumerates, so DPB stays clear here.  A FIFO
// deeper than one packet already gives the flow control DPB would - the core
// holds several packets instead of one - and DPB was measured on this board at
// 3.06 MB/s against 3.18 MB/s without it.
//
#define A733_MUSB_EP0_MAX_PACKET  64U
#define A733_MUSB_BULK_MAX_PACKET 512U
//
// Endpoints this core implements (EP0 plus EP1..EP5): the count the FIFO map
// walks so the endpoints fastboot does not use can be sized to zero, and the
// bound every "select an endpoint" loop in the driver stays inside.
//
#define A733_MUSB_NUM_EPS         6U
//
// EP0 has no programmable FIFO registers (see the map above); the byte 0 is
// recorded for the log's arithmetic only.
//
#define A733_MUSB_EP0_FIFO_BYTE      0U
#define A733_MUSB_EP1_FIFO_BYTE      1124U
#define A733_MUSB_EP2_FIFO_BYTE      2048U
//
// The bulk-OUT RX FIFO's space in bytes - what the address at 2048 reserves, not
// what RXFIFOSZ selects (512 B, one packet).  Three things are written against
// this number rather than against the programmed size: the two STATIC_ASSERTs
// that keep the map from overlapping, the oversized-reading guard in the drain,
// and the burst ceiling.  Using the reserved space for all three is the
// conservative choice: a reading larger than the space the endpoint owns cannot
// be framed at all, whatever size is selected.
//
#define A733_MUSB_BULK_RX_FIFO_BYTES 4096U

//
// The non-indexed window at 0x78---0x7F: the core's capability words and its
// test-mode register (offsets from the vendor usb.h, USBC_REG_o_*).  This
// driver reads them and writes none of them; TMCTL in particular must read 0,
// because a residual TEST_SE0_NAK / TEST_K / TEST_J / FORCE_FS / FORCE_HS left
// by an earlier stage would make the SIE answer no token at all while the core
// still reports SOF - which is one of the two shapes of "no SETUP ever
// arrives".  Nothing in the boot chain is known to clear it.
//
#define A733_MUSB_EPINFO_REG    0x78U   // endpoints implemented
#define A733_MUSB_RAMINFO_REG   0x79U   // FIFO RAM implemented
#define A733_MUSB_LINKINFO_REG  0x7AU   // UTMI/ULPI link width
#define A733_MUSB_VPLEN_REG     0x7BU   // VBUS pulse length
#define A733_MUSB_TMCTL_REG     0x7CU   // Test Mode Control
#define A733_MUSB_TMCTL_FORCE_HOST   BIT7
#define A733_MUSB_TMCTL_FIFO_ACCESS  BIT6
#define A733_MUSB_TMCTL_FORCE_FS     BIT5
#define A733_MUSB_TMCTL_FORCE_HS     BIT4
#define A733_MUSB_TMCTL_TEST_PACKET  BIT3
#define A733_MUSB_TMCTL_TEST_K       BIT2
#define A733_MUSB_TMCTL_TEST_J       BIT1
#define A733_MUSB_TMCTL_TEST_SE0_NAK BIT0

//
// The core's identity words: what it says it is, once, at bring-up.  Unlike the
// capability block above these are not part of the register file this driver
// programs, so they appear here only so the log can decode them.
//
// CONFIGDATA is the one that decides whether the FIFO model in this header is
// even the right one: its MPTXE/MPRXE bits say whether the core has a single
// shared packet buffer per direction (the "soft cone" model) or per-endpoint
// FIFOs, and DYNFIFO_SIZING says whether TXFIFOSZ/RXFIFOSZ are honoured at all.
// A core that read MPTXE = 1 would ignore the layout MusbFifoLayout programs.
//
// Offset: 0x0F, 8 bits.  Stated plainly because three maps disagree about it -
// the classic MUSB HDRC map puts TESTMODE at 0x0f and CONFIGDATA at 0x10, the
// vendor usb.h for this core family (sunxi_usb_src, include/asm/arch-sunxi/
// usb.h) puts CONFIGDATA at 0xc0, and 0x0f is what this driver had been told.
// All three are read-only reads of a live window, so A733MusbDeviceDxe reads
// all three once and prints them side by side; whichever byte decodes as a
// sane configuration is the core's, and one boot settles it.
//
#define A733_MUSB_CONFIGDATA_REG     0x0FU
#define A733_MUSB_CONFIGDATA_MPRXE          BIT7
#define A733_MUSB_CONFIGDATA_MPTXE          BIT6
#define A733_MUSB_CONFIGDATA_BIGENDIAN      BIT5
#define A733_MUSB_CONFIGDATA_HBRXE          BIT4
#define A733_MUSB_CONFIGDATA_HBTXE          BIT3
#define A733_MUSB_CONFIGDATA_DYNFIFO_SIZING BIT2
#define A733_MUSB_CONFIGDATA_SOFTCONE       BIT1
#define A733_MUSB_CONFIGDATA_UTMI_DATAWIDTH BIT0
//
// The two other offsets the maps disagree about, read for the line above only.
//
#define A733_MUSB_CONFIGDATA_ALT_HDRC  0x10U   // classic MUSB map
#define A733_MUSB_CONFIGDATA_ALT_VENDOR 0xC0U  // vendor usb.h, this core family
//
// HWVERS, 16 bits at 0x6C: the Mentor core's version, major in 15:11 and minor
// in 10:0 when the field split is the usual one.  Printed raw, because the
// split is an assumption and the raw word is not.
//
#define A733_MUSB_HWVERS_REG  0x6CU

//
// Allwinner wrapper, at +0x400 of the MUSB window (not in the manual; see the
// file header).  0x400 is ISCR - the OTG control the vendor code calls
// "USBC_ISCR": id/vbus force fields, per-bit pull-up enables and the wake
// latches.  0x410 is PHYCTL - SIDDQ (bit 3, 1 = PHY powered down) and the
// "VBUS valid comes from outside this chip" bit (bit 5) the board needs
// because its only power input is the USB-C connector.  0x420 is the route
// select the FES path owns; it appears here so the driver can print it.
//
#define A733_MUSB_ISCR_REG           0x400U
#define A733_MUSB_PHYCTL_REG         0x410U
#define A733_MUSB_ROUTE_REG          0x420U
#define A733_MUSB_ISCR_VBUS_FORCE_SHIFT  12U
#define A733_MUSB_ISCR_ID_FORCE_SHIFT    14U
#define A733_MUSB_ISCR_FORCE_MASK        0x3U
//
// Each force field is "enable" (high bit) plus the forced value (low bit), so
// 0b10 forces "no VBUS" and 0b11 forces "VBUS valid" (manual 18.4.5.1).  Both
// fields are set to 0b11 once, in the ISCR write of A733MusbDeviceDxe.c's step
// 4, and left there: the enumerating device-mode driver this driver was
// compared against writes exactly that and never needs a session edge, which
// is also why DEVCTL.SESSION reading 0 is logged and not waited on.
//
#define A733_MUSB_ISCR_VBUS_FORCE_LOW    0x2U
#define A733_MUSB_ISCR_VBUS_FORCE_HIGH   0x3U
//
// Pull-up enables.  The manual's U2P0_ISCR table (18.4.5.1) prints bits 17:16
// as reserved; the vendor register model gives them to the ID and D+/D- pull-up
// enables, and the enumerating device-mode driver sets both in the same write
// as the force fields - the D+/D- one being the physical precondition for a
// host to see a device at all.  Value from the comparison, not the manual.
//
#define A733_MUSB_ISCR_DPDM_PULLUP_EN    BIT16
#define A733_MUSB_ISCR_ID_PULLUP_EN      BIT17
//
// ISCR bits 11:10, the manual's "External VBUS Valid Source Select" (§18.4.5.1,
// register U2P0_ISCR at 0x0410400):
//
//   0x  external VBUS valid is detected from the VBUS input
//   10  external VBUS valid is detected from the DP/DM input (line state)
//   11  external VBUS valid is detected from either the VBUS or the DP/DM input
//
// This is a different field from the force pair at 13:12.  The force field
// drives the value the SIE is handed; this one chooses which of the PHY's two
// detectors produces the "valid" the PHY reports in the first place, and it is
// the one that matters on a board whose VBUS pin is not bonded (see
// A733MusbDeviceDxe.c, MusbIscrOn).
//
#define A733_MUSB_ISCR_VBUS_SRC_SHIFT  10U
#define A733_MUSB_ISCR_VBUS_SRC_MASK   0x3U
#define A733_MUSB_ISCR_VBUS_SRC_PAD    0x0U
#define A733_MUSB_ISCR_VBUS_SRC_LINE   0x2U
#define A733_MUSB_ISCR_VBUS_SRC_EITHER 0x3U
//
// Read-only ISCR status, named the way the vendor USB stack names it
// (sunxi usb.h, USBC_BP_ISCR_*): the two detectors' verdicts and the merged
// result the SIE is fed.  The pad's detector and the line-state detector are
// the only two inputs this signal can have, so printing both is what tells a
// dead VBUS pin from a dead PHY.
//
#define A733_MUSB_ISCR_VBUS_VALID_FROM_VBUS BIT29
#define A733_MUSB_ISCR_VBUS_VALID_FROM_DATA BIT30
#define A733_MUSB_ISCR_MERGED_VBUS_STATUS   BIT25
// The wake latches: bits 7:4 are R/W1C in the manual's U2P0_ISCR table - bit 7
// the ID-mult change, bit 6 the VBUS change, bit 5 the ID change, bit 4 the
// DP/DM change.  The A733 host firmware's device bring-up clears the field in
// the word it writes (it does not write 1s back), and this driver matches that
// write bit for bit; an earlier version wrote the latched bits back, which is
// the manual's behaviour for a W1C field but not what the stack that enumerates
// does.  The latched value is printed either way, so the log still shows what the
// PHY reported.
//
#define A733_MUSB_ISCR_WAKE_MASK  (BIT7 | BIT6 | BIT5 | BIT4)
#define A733_MUSB_PHYCTL_SIDDQ        BIT3
//
// PHYCTL bit 1, which the manual's U2P0_PHYCTL table does not name usefully.
// The vendor usb stack's device init clears it after USBC_Dev_ConectSwitch(ON),
// i.e. with the D+ pull-up already up; the A733 host firmware's device bring-up
// (the driver this port is verified against) never writes it at all, and this
// driver now follows the latter - see the note in MusbPhyUp.  The bit is named
// here because the compiled-out PHYCTL sweep still refers to it.
//
// Provenance of the vendor's write: read-modify-write of 0x04100410 in the
// vendor usb stack's device init, executed after USBC_Dev_ConectSwitch(ON) and
// before the driver returns to the host.  Nothing in the vendor source says
// what the bit controls.
//
#define A733_MUSB_PHYCTL_POST_CONNECT_OFF  BIT1
//
// The rest of PHYCTL's VBUS path as the manual describes it (§18.4.5.2, same
// register the SIDDQ/VBUSVLDEXT bits live in).  VBUSVLDEXTSEL is read-only here
// because the manual gives its reset value as 1 ("use the VBUSVLDEXT input to
// assert the D+ pull-up"), which is already the wanted setting; the other three
// are printed so the log can say which of the comparators is powered at all.
//
#define A733_MUSB_PHYCTL_VBUSVLDEXTSEL BIT4
#define A733_MUSB_PHYCTL_VBUSVLDEXT    BIT5
#define A733_MUSB_PHYCTL_DRVVBUS       BIT9
#define A733_MUSB_PHYCTL_OTGDISABLE    BIT10

//
// The core's own bus-master DMA engine, in the same 4 KB window at +0x500 (not
// in the manual).  Everything below is the vendor device stack's model, taken
// from the registers it programs rather than from its prose, and every claim
// names the function it comes from:
//
//   ENABLE   0x500  one bit per channel, bit n = channel n enabled.  Written by
//                  USBC_Dma_Start (usbc_dma.c) before the channel is
//                  programmed and cleared by USBC_Dma_Int_Stop, i.e. it is the
//                  channel's gate and not a completion latch.  The vendor
//                  header (include/asm/arch-sunxi/usb.h) also names this
//                  register USBC_REG_o_DMA_INTE - the same word is the engine's
//                  interrupt enable, so enabling a channel is what lets its
//                  completion reach the controller's interrupt (see below).
//   STATUS   0x504  one bit per channel, RW1C.  USBC_Dma_Int_Query reads it and
//                  USBC_Dma_Int_Clear clears it by writing 0xff, and the vendor
//                  interrupt handler (usb_base_platform.c, sunxi_usb_irq) tests
//                  it as `status & (1 << endpoint_index)`, so a completion for
//                  the endpoint whose number is n arrives as bit n.
//
// Per channel n, at 0x540 + 0x10 * n (16-byte stride, eight channels: the
// vendor's usb_dma_request hands out indices 1..7 of SUNXI_USB_DMA_A50_MAX = 8):
//
//   CONFIG   +0x0   [3:0]  the endpoint the channel serves.  USBC_Dma_Set_
//                          ChannalPara writes `ep_type & 0xf` here and both
//                          call sites pass the endpoint *number* (SUNXI_USB_
//                          BULK_IN_EP_INDEX = 1, ..._OUT_EP_INDEX = 2), so the
//                          field binds a channel to an endpoint number.
//                   [4]    direction: 0 = DRAM to host, 1 = host to DRAM
//                          (USB_DMA_FROM_DRAM_TO_HOST = 0,
//                          USB_DMA_FROM_HOST_TO_DRAM = 1 in the vendor header).
//                          The bulk OUT endpoint is the 1 case.
//                   [30:16] burst/packet length, written from the endpoint's
//                          maximum packet size (USBC_Dma_Set_PktLen, called
//                          with HIGH_SPEED_EP_MAX_PACKET_SIZE = 512), 0x7ff max.
//                   [31]   start.  Set last by USBC_Dma_Start, after ADDR and
//                          COUNT, and the vendor re-arms by writing it again.
//   ADDR     +0x4   32-bit bus address the engine reads or writes (whole words:
//                  USBC_Dma_Start is the vendor's one caller and refuses a
//                  length that is not a multiple of 4).
//   COUNT    +0x8   byte count to move; the same offset reads back as the
//                  residual (the vendor names it USBC_REG_o_DMA_RESU), which is
//                  the only runtime reading of how far a transfer got.
//
// There is no DMA interrupt line of its own: the engine reports through the
// controller's line (GIC SPI 156 / INTID 188 here), which is why the vendor
// reads STATUS at the top of the same handler that services INTRTX/INTRRX and
// clears it at the end of that handler.
//
// Step 9 of the bring-up turns this engine off and nothing turns it back on:
// every byte this driver moves - endpoint 0, bulk IN and bulk OUT - is read from
// or written to the FIFO ports.  The verdict below is why.
//
// Verdict on this die: the window above does not exist here.
//
// Measured.  Arming channel 2 the way USBC_Dma_Start arms it leaves CONFIG, ADDR,
// COUNT and ENABLE all reading 0x00000000, and ADDR/COUNT are the two the vendor
// itself reads back as the residual counter (USBC_REG_o_DMA_RESU is COUNT, read
// by USBC_Dma_Int_Query's caller after every transfer).  A channel that is
// present and programmed cannot read zero there, so those writes are not landing
// in a register file.  Nothing in the packet path notices: an arm returns "not
// armed" and the FIFO path carries the packet, which is why the engine was never
// worth a runtime fallback - the code that would have armed it was removed once
// the measurement was in, and the numbers are in the driver's MusbDmaOff.
//
// Three local sources say the same thing independently:
//
//   - The A733 BROM's FEL stack is the one USB driver on this die that was
//     written for this die.  Over the whole of it (rom192k.bin 0x179F0-0x192FC,
//     NS BROM) it references exactly three wrapper addresses - 0x04100400 ISCR,
//     0x04100410 PHYCTL, 0x04100420 route - and the entire 192 KB image's literal
//     pool contains no 0x041005xx word at all.  FEL moves packets through the
//     FIFO port 512 bytes at a time.
//   - The manual's USB2.0 DRD chapter (18.5) has an overview and a block diagram
//     and no register chapter, and its feature list gives the device side
//     "interface to an external Normal DMA controller for every EP".  The DMA
//     chapter's DRQ table (13.1.4.1, table 13-2) lists ports 27-34 as both source
//     and destination DRQ types: USB0_DEVICE_EP1 ... USB0_DEVICE_EP8.  The bulk
//     OUT endpoint of this die is wired to the SoC's DMAC, not to a DMA block
//     inside this page - which is why the rest of the page reads as nothing.
//   - The CCU gives USB0 three gates and three resets and no fourth: USB0_CLK_REG
//     bit 31 and USB0_BGR_REG bits 24/20/16 with their GATING bits at 8/4/0
//     (manual 4.1.6.195/196) - device, EHCI, OHCI.  There is no clock enable that
//     would make 0x500 start answering, so "it needs a gate" is not an open
//     question.
//
// What that leaves.  A DMA path for bulk OUT on this die is the SoC DMAC through
// DRQ port 28 (USB0_DEVICE_EP2): RXCSR.DMA_REQ_EN as the endpoint's half of the
// handshake, and the FIFO-to-DRAM move described to the DMAC in its own
// descriptor format (manual 13.1: DMAC0 at 0x04601000, its channel registers and
// the six-word descriptor - see A733Dmac.h and A733DmacLib).  That is a second
// driver and a different completion model, not a different offset in this one,
// and both DMACs read as secure on this die: SEC reads zero where its reset value
// is 0x0000ffff, so normal-world software cannot program a transfer until BL31
// opens them.  A733DmacLib is written and builds, and nothing calls it.
//
// The register model above stays as the record of what the vendor stack believes,
// for a die whose core does carry the block.
//
#define A733_MUSB_DMA_ENABLE_REG 0x500U
#define A733_MUSB_DMA_STATUS_REG 0x504U
#define A733_MUSB_DMA_CHANNEL_BASE   0x540U
#define A733_MUSB_DMA_CHANNEL_STRIDE 0x10U
#define A733_MUSB_DMA_CHANNEL_COUNT  8U
//
// The three per-channel words, and the fields of CONFIG.
//
#define A733_MUSB_DMA_CONFIG(Chan)  (A733_MUSB_DMA_CHANNEL_BASE + A733_MUSB_DMA_CHANNEL_STRIDE * (Chan))
#define A733_MUSB_DMA_ADDR(Chan)    (A733_MUSB_DMA_CONFIG (Chan) + 0x04U)
#define A733_MUSB_DMA_COUNT(Chan)   (A733_MUSB_DMA_CONFIG (Chan) + 0x08U)
#define A733_MUSB_DMA_CFG_EP_MASK       0x0000000FU
#define A733_MUSB_DMA_CFG_DIR           BIT4
#define A733_MUSB_DMA_CFG_BURST_SHIFT   16U
#define A733_MUSB_DMA_CFG_BURST_MASK    0x7FFU
#define A733_MUSB_DMA_CFG_START         BIT31
//
// There is no separate engine-wide gate to write: the vendor's
// __USBC_Dev_ConfigDma_Trans sets "PCTL bit 24", and PCTL in its model
// (usb.h: USBC_REG_o_PCTL = 0x0040) is a 32-bit word covering 0x40---0x43, so
// bit 24 is byte 3 bit 0 - VEND0.BUS_SEL, the same select USBC_SelectBus writes
// as a byte.  That is why arming a channel takes VEND0 = BUS_SEL |
// DRQ_SEL_OUT(ep) and nothing else on the wrapper side.
//
// This driver writes the VEND0 *byte* and never the 32-bit word: a 32-bit
// read-modify-write at 0x40 would carry POWER, DEVCTL and INDEX back with it,
// and DEVCTL's HOSTREQ bit and INDEX's endpoint select are not this function's
// to rewrite.
//
// VEND0 selects between the FIFO ports and the engine, and which endpoint's
// request line the engine listens to (USBC_SelectBus, usbc.c): bit 0 is the bus
// select (1 = DMA) and bits 6:1 hold drq_sel, which for an OUT endpoint is
// (2 * ep - 1).  So DMA on bulk OUT EP2 is 0x07 and PIO is 0x00.
//
#define A733_MUSB_VEND0_BUS_SEL         BIT0
#define A733_MUSB_VEND0_DRQ_SEL_SHIFT   1U
#define A733_MUSB_VEND0_DRQ_SEL_OUT(Ep) ((UINT8)(((2U * (Ep)) - 1U) << A733_MUSB_VEND0_DRQ_SEL_SHIFT))

//
// SRAMC (manual: memory map, 0x03000000---0x03000FFF, a 4 KB window the map
// labels SYSCTRL and for which it publishes no register chapter).  The word at
// +0x04 is where the vendor USB stack hands the FIFO SRAM to the device
// controller: bits 1:0 are the FIFO mode field and 0b01 is the 8 KB USB FIFO
// mode.  That write is inside the vendor's `#ifdef CONFIG_ARCH_SUN3IW1P1`
// (usbc.c, USBC_ConfigFIFO_Base()), i.e. it is conditional on an older SoC and
// does not run on A733 at all, so neither the field nor its read-back says
// anything about where this die's FIFO RAM is.  The driver writes and dumps the
// window anyway - it costs one line and a write that has been harmless on every
// boot - but this register is not evidence about the FIFO path.
//
#define A733_SRAMC_BASE        0x03000000U
#define A733_SRAMC_USB_FIFO    0x04U
#define A733_SRAMC_USB_FIFO_MODE_MASK 0x3U
#define A733_SRAMC_USB_FIFO_ON BIT0
#define A733_SRAMC_WINDOW_MMIO SIZE_4KB

#endif