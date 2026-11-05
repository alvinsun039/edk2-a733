/** @file
  The A733's system DMA controller (DMAC), for drivers that need one peripheral
  transfer moved without the CPU reading a FIFO port by hand.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Manual: chapter 13 "DMA" (13.1 "DMA Controller (DMAC)").  Section references
  below are to A733_UserManual_V0.92.

  Two controllers, one block each, and the register file is the same in both:

    DMAC0  0x04601000..0x04602FFF   (13.1.6 "Register List", row DMA0)
    DMAC1  0x04024000..0x04025FFF   (13.1.6 "Register List", row DMA1)

  13.1.1 describes two blocks of sixteen channels each, one in the CPUX domain
  and one in the CPUS domain; the DRQ table this package uses (13.1.4.1,
  table 13-2, whose title scopes it to the CPUX domain) is the CPUX one and is
  the only DRQ table the chapter carries, so A733_DMAC0_BASE is the base this
  library uses (A733_DMAC_BASE in A733DmacLib.c - one line, if a measurement
  says the USB0 endpoints are served by the other block instead).  The init line
  prints both blocks' security words, so a boot log says which of the two answers
  at all.

  Provenance, stated plainly.  The register map, the descriptor layout and the
  programming order are the manual's chapter 13 (this is the one part of the
  package that needs no cross-check against another firmware: every offset and
  field below is written down there, and each is cited where it is defined).  The
  one thing the manual cannot say is whether this die exposes the block to
  normal-world software, and it does not: a first transfer attempt left SEC
  reading 0x00000000 where its documented reset value is 0x0000ffff, with
  AUTO_GATE and STA reading zero beside it, i.e. no channel of either controller
  can be programmed from EL2/EL1 as this platform boots.  That was measured on
  the board, not derived; the arm path exists here because it is correct for a
  DMAC, and it has never moved a byte on this SoC.  Opening the block is a
  secure-world (BL31) change - the security word is the switch - and until then
  the only caller this library had (the MUSB device driver's bulk OUT drain) has
  been removed and the library is built but unused.
**/

#ifndef A733_DMAC_H__
#define A733_DMAC_H__

#include <Uefi.h>

//
// Manual 13.1.6: the two blocks, and the span the platform's page tables must
// cover (8 KB each; the highest register named in the chapter is the per-channel
// descriptor-high word at +0x013C + 0x40*15 = +0x04FC, i.e. the first page - the
// second is covered because the manual reserves the block to 0x2000).
//
#define A733_DMAC0_BASE        0x04601000UL
#define A733_DMAC1_BASE        0x04024000UL
#define A733_DMAC_WINDOW_SIZE  0x2000U

//
// The controller's own registers (manual 13.1.6/13.1.7).  Only the ones this
// library reads or writes are named.
//
//   SEC       0x0020  per-channel secure/not-secure, default 0x0000_FFFF
//                     (all 16 channels non-secure) - the boot value that lets a
//                     normal-world driver use the block at all (13.1.7.1).
//   AUTO_GATE 0x0028  clock auto gating; the manual's note on this register
//                     (13.1.7.2) asks for bit 2 to be set at init time.
//   STA       0x0030  bit n = channel n busy (13.1.7.3).
//   IRQ_CPU_EN  0x0034 / IRQ_CPUS_EN 0x0038 - which field a channel's interrupt
//                     is routed to; untouched here (this driver polls).
//
#define A733_DMAC_SEC_REG          0x0020U
#define A733_DMAC_AUTO_GATE_REG    0x0028U
#define A733_DMAC_STA_REG          0x0030U
#define A733_DMAC_IRQ_CPU_EN_REG   0x0034U
#define A733_DMAC_IRQ_CPUS_EN_REG  0x0038U

//
// Per-channel registers: 0x0100 + 0x40 * n (manual 13.1.6, and each of
// 13.1.7.6..13.1.7.19).  The manual spells the same addresses out per register
// as a base plus a per-channel offset; the value added to the block base here is
// that channel block plus the register offset named below.
//
#define A733_DMAC_CHANNEL_BASE    0x0100U
#define A733_DMAC_CHANNEL_STRIDE  0x0040U

#define A733_DMAC_EN(ch)          (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0000U)
#define A733_DMAC_PAU(ch)         (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0004U)
#define A733_DMAC_DESC_ADDR(ch)   (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0008U)
#define A733_DMAC_CFG(ch)         (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x000CU)
#define A733_DMAC_CUR_SRC(ch)     (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0010U)
#define A733_DMAC_CUR_DEST(ch)    (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0014U)
#define A733_DMAC_BCNT_LEFT(ch)   (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0018U)
#define A733_DMAC_PARA(ch)        (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x001CU)
#define A733_DMAC_MODE(ch)        (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0028U)
#define A733_DMAC_FDESC_ADDR(ch)  (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x002CU)
#define A733_DMAC_PKG_NUM(ch)     (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0030U)
#define A733_DMAC_IRQ_EN(ch)      (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0034U)
#define A733_DMAC_IRQ_PEND(ch)    (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x0038U)
#define A733_DMAC_DESC_HIG(ch)    (A733_DMAC_CHANNEL_BASE + A733_DMAC_CHANNEL_STRIDE * (ch) + 0x003CU)

//
// DMA_EN_REG bit 0 (13.1.7.6): channel enable.  The manual's programming
// sequence uses a channel being disabled as its test for an idle channel
// (13.1.5.2 step 2), which is how the status bit above and this one are used
// together here.
//
#define A733_DMAC_EN_ENABLE  0x1U
#define A733_DMAC_EN_MASK    0x1U

//
// DMA_IRQ_PEND_REG (13.1.7.18), write-one-to-clear, one bit per event: half
// package, package end, queue end, timeout.  A single-package descriptor that
// has moved all of its bytes raises package end; queue end follows it because
// the descriptor's link word is the end-of-chain value.
//
#define A733_DMAC_IRQ_HALF_PKG  BIT0
#define A733_DMAC_IRQ_PKG_END   BIT1
#define A733_DMAC_IRQ_QUEUE_END BIT2
#define A733_DMAC_IRQ_TIMEOUT   BIT3
#define A733_DMAC_IRQ_ALL       (A733_DMAC_IRQ_HALF_PKG | A733_DMAC_IRQ_PKG_END | \
                                 A733_DMAC_IRQ_QUEUE_END | A733_DMAC_IRQ_TIMEOUT)

//
// DMA_CFG_REG (13.1.7.9), which is also the first word of the descriptor
// (13.1.4.2 describes the descriptor's configuration word as this register's
// contents).  Both directions carry the same five fields:
//
//   [ 5: 0] source DRQ type        [21:16] destination DRQ type
//   [ 7: 6] source block length    [23:22] destination block length
//   [    8] source address mode    [   24] destination address mode
//   [10: 9] source data width      [26:25] destination data width
//   [   30] BMODE_SEL (read-only: the mode the channel is in)
//
#define A733_DMAC_CFG_SRC_DRQ(drq)    ((UINT32)((drq) & 0x3FU))
#define A733_DMAC_CFG_SRC_BLOCK(b)    (((UINT32)(b) & 0x3U) << 6)
#define A733_DMAC_CFG_SRC_IO_MODE     BIT8
#define A733_DMAC_CFG_SRC_WIDTH(w)    (((UINT32)(w) & 0x3U) << 9)
#define A733_DMAC_CFG_DST_DRQ(drq)    (((UINT32)(drq) & 0x3FU) << 16)
#define A733_DMAC_CFG_DST_BLOCK(b)    (((UINT32)(b) & 0x3U) << 22)
#define A733_DMAC_CFG_DST_IO_MODE     BIT24
#define A733_DMAC_CFG_DST_WIDTH(w)    (((UINT32)(w) & 0x3U) << 25)

//
// Data width and block-length encodings (13.1.7.9): the two-bit width field
// names 8, 16, 32 and 64 bits for 0..3, and the two-bit block field names 1, 4, 8
// and 16 beats per DRQ request for 0..3.
//
#define A733_DMAC_WIDTH_8    0U
#define A733_DMAC_WIDTH_16   1U
#define A733_DMAC_WIDTH_32   2U
#define A733_DMAC_WIDTH_64   3U

#define A733_DMAC_BLOCK_1    0U
#define A733_DMAC_BLOCK_4    1U
#define A733_DMAC_BLOCK_8    2U
#define A733_DMAC_BLOCK_16   3U

//
// The end-of-chain value of a descriptor's link word: 13.1.4.2 gives
// 0xFFFFF800 in the link-and-parameter word as the value that makes the
// controller stop after the package it has just moved instead of fetching
// another descriptor.  The Parameter word this library writes is zero - no
// timeout, no IO-speed acceleration, no address bits above 32, no wait cycles -
// because every field of the table the manual gives for it (13-3) may be left
// at zero, and because the addresses are the descriptor's own 32-bit words.
//
#define A733_DMAC_LINK_END   0xFFFFF800U
#define A733_DMAC_PARA_NONE  0x00000000U

//
// DRQ types this package names (13.1.4.1, table 13-2 "DMA DRQ Types in CPUX
// domain"): port 0 is SRAM, port 1 is DRAM - the two memory types, used for the
// side of a transfer that is memory - and ports 27..34 are the USB0 device
// endpoints, port 27 being EP1 and port 28 EP2:
//
//   port27 USB0_DEVICE_EP1   port28 USB0_DEVICE_EP2   port29 USB0_DEVICE_EP3
//   port30 USB0_DEVICE_EP4   port31 USB0_DEVICE_EP5   port32 USB0_DEVICE_EP6
//   port33 USB0_DEVICE_EP7   port34 USB0_DEVICE_EP8
//
#define A733_DMAC_DRQ_SRAM  0U
#define A733_DMAC_DRQ_DRAM  1U
#define A733_DMAC_DRQ_USB0_DEVICE_EP(Ep)  (26U + (UINT32)(Ep))

//
// One DMA descriptor (13.1.4.2, words in this order; the field layout of word 0
// is DMA_CFG_REG's and of word 4 is table 13-3's).  The controller fetches it
// from DRAM at the address the channel's DESC_ADDR register names, so the manual
// requires the structure to be word-aligned (13.1.4.7: an unaligned address is
// one the controller will not recognise as a descriptor) and it has to sit inside
// the low 4 GB, because the descriptor's address words are 32 bits and the bits
// above bit 31 live in the Parameter word this library leaves at zero.
//
#define A733_DMAC_DESC_WORDS  6U
//
// 64 bytes, i.e. one cache line on this port: the controller reads only the six
// words above (24 bytes is all the manual defines), and the padding is what lets
// a library clean one channel's descriptor without the cache operation straddling
// a neighbour's.  It is storage, not part of the format.
//
typedef struct {
  UINT32  Config;       // word 0: as A733_DMAC_CFG_REG
  UINT32  Source;       // word 1: source address, low 32 bits
  UINT32  Destination;  // word 2: destination address, low 32 bits
  UINT32  Count;        // word 3: byte counter, up to 2^25-1 (13.1.4.2)
  UINT32  Parameter;    // word 4: table 13-3; A733_DMAC_PARA_NONE
  UINT32  Link;         // word 5: next descriptor, A733_DMAC_LINK_END = last
  UINT8   Pad[64U - A733_DMAC_DESC_WORDS * sizeof (UINT32)];
} A733_DMAC_DESC;
STATIC_ASSERT (sizeof (A733_DMAC_DESC) == 64, "one DMAC descriptor per cache line");

//
// What a caller programs into one channel: a single-package transfer, i.e. one
// descriptor with no chain behind it.
//
typedef struct {
  UINT32   Source;
  UINT32   Destination;
  UINT32   Length;            // bytes; must be a whole number of the widest beat
  UINT8    SourceDrq;         // A733_DMAC_DRQ_*
  UINT8    DestinationDrq;
  BOOLEAN  SourceIoMode;      // TRUE: a peripheral address that does not advance
  BOOLEAN  DestinationIoMode;
  UINT8    SourceWidth;       // A733_DMAC_WIDTH_*
  UINT8    DestinationWidth;
  UINT8    SourceBlock;       // A733_DMAC_BLOCK_*
  UINT8    DestinationBlock;
} A733_DMAC_TRANSFER;

//
// Channel registers as they are, decoded, for a log line.
//
typedef struct {
  UINT32  Security;
  UINT32  AutoGate;
  UINT32  Busy;          // DMA_STA_REG
  UINT32  Enable;        // this channel's DMA_EN_REG
  UINT32  Pending;       // this channel's DMA_IRQ_PEND_REG
  UINT32  Residual;      // this channel's DMA_BCNT_LEFT_REG
  UINT32  Source;        // DMA_CUR_SRC_REG
  UINT32  Destination;   // DMA_CUR_DEST_REG
  UINT32  PackageCount;  // DMA_PKG_NUM_REG
  UINT32  DescriptorConfig;  // word 0 of the descriptor last programmed here
  BOOLEAN Initialised;
} A733_DMAC_STATE;

#define A733_DMAC_CHANNELS   16U
#define A733_DMAC_NO_CHANNEL 0xFFU

/**
  Bring the controller up: de-assert its reset and open its bus clock gate in the
  CCU, then set the clock-gating bit the manual requires before any channel is
  programmed.

  Manual 13.1.5.1 covers the block's clock control and gives the order: the
  reset is de-asserted first and the AHB gating bit opened after it.  The gate is
  DMA0_BGR_REG, CCU offset 0x0704, bit 0 gating and bit 16 reset (4.1.6.85) - the
  CCU does have a DMAC gate, so this library opens it.  DMA0 is clocked from
  AHB_CLK (4.1.6.85), i.e. there is no divider or parent to program.

  Idempotent: the second and later calls do nothing.

  What this cannot tell you is whether the block is there: an address with no
  slave behind it reads as zeros and a channel whose registers do not hold a
  value never completes, so the reading that settles it is the caller's first
  transfer (the init line prints the security word, whose reset value is
  0x0000_FFFF, for exactly that comparison).

  @retval EFI_SUCCESS  The CCU sequence ran and the block's registers are mapped.
  @retval Others       From the CCU library.
**/
EFI_STATUS
EFIAPI
A733DmacInit (
  VOID
  );

/**
  @retval TRUE  A733DmacInit has run and the block answered.
**/
BOOLEAN
EFIAPI
A733DmacIsInitialised (
  VOID
  );

/**
  Take a channel that is neither enabled nor busy: the manual's programming
  sequence tests an idle channel by reading its enable bit (13.1.5.2 step 2), and
  the busy bit is checked with it.

  @param[out] Channel  The channel's index, 0..15.

  @retval EFI_SUCCESS            A channel is held by the caller.
  @retval EFI_NOT_READY          The block is not initialised.
  @retval EFI_OUT_OF_RESOURCES   Every channel is enabled, busy or held.
**/
EFI_STATUS
EFIAPI
A733DmacAcquireChannel (
  OUT UINT8  *Channel
  );

/**
  Give a channel back.  The channel is stopped first, so a caller that gives up
  on a transfer mid-flight cannot leave it writing into memory.

  @param[in] Channel  A channel this library handed out; any other value is
                      ignored.
**/
VOID
EFIAPI
A733DmacReleaseChannel (
  IN UINT8  Channel
  );

/**
  Program one channel with a single-package transfer and start it.

  The order is 13.1.5.2's: the descriptor is written to memory first (steps 3-4),
  its address goes into DESC_ADDR, and the channel is enabled last (step 5).  The
  descriptor's link word is A733_DMAC_LINK_END, so the queue ends after this one
  package and the channel closes itself.

  @param[in] Channel   A channel from A733DmacAcquireChannel.
  @param[in] Transfer  The transfer.  Both addresses and the descriptor must be
                       below 4 GB; a length of zero or one that overflows the
                       manual's 2^25-1 byte counter is refused.

  @retval EFI_SUCCESS            The channel is enabled and the descriptor is its
                                 current one.
  @retval EFI_INVALID_PARAMETER  A parameter is out of range.
  @retval EFI_NOT_READY          The block is not initialised.
**/
EFI_STATUS
EFIAPI
A733DmacStartTransfer (
  IN UINT8                        Channel,
  IN CONST A733_DMAC_TRANSFER     *Transfer
  );

/**
  Whether the armed package has finished.

  Two readings, either of which is the answer: the package-end (or queue-end)
  pending bit, which the controller sets when the last byte of the package has
  been written; and "the byte counter is spent on a channel that has completed at
  least one package" (DMA_BCNT_LEFT_REG and DMA_PKG_NUM_REG), which covers the
  window before the pending bit is visible.

  @param[in] Channel  The channel.

  @retval TRUE   The package has been moved.
  @retval FALSE  It is still in flight, or nothing is armed.
**/
BOOLEAN
EFIAPI
A733DmacCompleted (
  IN UINT8  Channel
  );

/**
  @retval TRUE  The channel is not busy (DMA_STA_REG bit clear).
**/
BOOLEAN
EFIAPI
A733DmacIdle (
  IN UINT8  Channel
  );

/**
  @param[in] Channel  The channel.

  @return The bytes of the armed package that have not been moved, from
          DMA_BCNT_LEFT_REG (13.1.7.12).  The manual warns that the value is not
          real time, so a caller reads it only once it has established that the
          channel has stopped.
**/
UINT32
EFIAPI
A733DmacResidual (
  IN UINT8  Channel
  );

/**
  Close a channel: disable it and clear its latched events.

  @param[in] Channel  The channel.
**/
VOID
EFIAPI
A733DmacStop (
  IN UINT8  Channel
  );

/**
  Snapshot a channel and the shared registers for a log line.

  @param[in]  Channel  The channel.
  @param[out] State    Filled in.
**/
VOID
EFIAPI
A733DmacGetState (
  IN  UINT8          Channel,
  OUT A733_DMAC_STATE  *State
  );

#endif // A733_DMAC_H__