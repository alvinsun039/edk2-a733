/** @file
  SMHC (SD/MMC host controller) register map for the Allwinner A733
  (sun60iw2p1).

  Every offset and field below is from the A733 user manual's SMHC chapter
  (11.3.7 register table, 11.3.8 register descriptions).  The manual states
  that SMHC1, SMHC2 and SMHC3 have the same registers as SMHC0, and the board's
  eMMC is on SMHC2.

  Two things the manual spells out that a reader of SDHCI would otherwise
  guess at: there is no card-detect register (SMHC_STATUS bit 8 samples
  DATA[3]), and there is no clock-update bit (the card clock is changed by
  writing SMHC_CLKDIV and then delivering it with SMHC_CMD bit 21, PRG_CLK -
  the manual's own example argument is 0x80202000).

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_SMHC_H__
#define A733_SMHC_H__

#include <Uefi.h>

//
// Manual: SMHC chapter, base addresses.  Each controller owns 4 KB and the
// four are contiguous.
//
#define A733_SMHC0_BASE  0x04020000UL
#define A733_SMHC1_BASE  0x04021000UL
//
// Driven through SMHC2.  The PC pads carry both the SDC2 and the SDC3 function
// (manual PCn_SELECT: 0011 = SDC2, 0100 = SDC3), so which controller reaches the
// card is a pad-mux decision rather than a wiring one: the Tina board file
// enables sdc2@0x04022000 and the AOSP image whose boot log shows HS400 at
// 200 MHz runs sdc3@0x04023000, on the same card.  This port uses SMHC2, which
// is the one its identification and data path were verified on; the SMHC3
// experiment is recorded in A733-EMMC-BRINGUP.md.
//
#define A733_SMHC2_BASE  0x04022000UL
#define A733_SMHC3_BASE  0x04023000UL

//
// The span the platform's page tables give the controllers.  One region
// covers SMHC0..SMHC3, so a driver for any of them finds its registers
// mapped; the A733 has no other peripheral in 0x04020000..0x0402FFFF.
//
#define A733_SMHC_WINDOW_BASE  A733_SMHC0_BASE
#define A733_SMHC_WINDOW_SIZE  SIZE_64KB

//
// the A733 user manual, register offsets.  Offsets are relative to a controller's
// base.  SMHC_FIFO is the data port; every other register is 32 bits wide and
// 4-byte aligned.
//
#define A733_SMHC_CTRL     0x000U
#define A733_SMHC_CLKDIV   0x004U
#define A733_SMHC_TMOUT    0x008U
#define A733_SMHC_CTYPE    0x00CU
#define A733_SMHC_BLKSIZ   0x010U
#define A733_SMHC_BYTCNT   0x014U
#define A733_SMHC_CMD      0x018U
#define A733_SMHC_CMDARG   0x01CU
#define A733_SMHC_RESP0    0x020U
#define A733_SMHC_RESP1    0x024U
#define A733_SMHC_RESP2    0x028U
#define A733_SMHC_RESP3    0x02CU
#define A733_SMHC_INTMASK  0x030U
#define A733_SMHC_MINTSTS  0x034U
#define A733_SMHC_RINTSTS  0x038U
#define A733_SMHC_STATUS   0x03CU
#define A733_SMHC_FIFOTH   0x040U
#define A733_SMHC_THLD     0x100U
#define A733_SMHC_HWRST    0x078U
#define A733_SMHC_CSDC     0x054U
//
// Timing/sampling controls.  Their reset values are the ones the manual
// documents; boot0 leaves them tuned for whatever mode it used (SDR52 here),
// and a mode with an edge on both halves of the clock samples differently.
//
#define A733_SMHC_NTSR       0x05CU
#define A733_SMHC_SFC        0x104U
#define A733_SMHC_DRV_DL     0x140U
#define A733_SMHC_SAMP_DL    0x144U
#define A733_SMHC_DS_DL      0x148U

//
// EDSD, the extended-mode register.  The manual's table does not reach it; the
// vendor's driver for this IP calls it SMHC_EDSD and uses its bit 31 as the
// HS400 mode enable, which is the only field of it this driver programs.  It is
// a mode bit of the data path, not of the card: the card's side of HS400 is
// EXT_CSD[185] = 3, and the two have to be set together.
//
#define A733_SMHC_EDSD               0x10CU
#define A733_SMHC_EDSD_HS400_MD_EN   BIT31
//
// NTSR MODE_SEL chooses how many clock phases the controller's own timing
// engine uses: 1 is the 2x mode (the register's reset value), 0 the 1x mode.
// The vendor's driver for this IP clears the bit on *every* clock set, before
// programming the divider, because the delay lines below are calibrated for 1x;
// a DDR52 bus on a 2x-mode controller samples at a rate none of them describe.
//
#define A733_SMHC_NTSR_MODE_SEL          BIT31
// bit 31 selects the sample clock mode (manual table 11-9: "1x mode" with the
// bit clear, "2x mode" with it set); the rest of the register is timing
// configuration that belongs to whoever programmed it, so the bit is always
// changed with a read-modify-write, never by writing a whole value.
#define A733_SMHC_NTSR_2X_TIMING_MODE     BIT31
#define A733_SMHC_NTSR_1X                0x00000000U
#define A733_SMHC_NTSR_RESET             0x80000000U
//
// DRV_DL: bit 16 selects the 180-degree phase for the command line's output,
// bit 17 the same for the data lines' (clear is 90 degrees).  The reset value
// sets the command bit alone.  The vendor programs both for an 8-bit DDR bus
// and for the SDR modes at or above 50 MHz.
//
#define A733_SMHC_DRV_DL_CMD_PH180        BIT16
#define A733_SMHC_DRV_DL_DAT_PH180        BIT17
#define A733_SMHC_DRV_DL_RESET            0x00010000U
#define A733_SMHC_DRV_DL_DDR              (A733_SMHC_DRV_DL_CMD_PH180 | A733_SMHC_DRV_DL_DAT_PH180)
//
// DS_DL: the data-strobe delay line, laid out like SAMP_DL (bits 5:0 taps, bit
// 7 the software enable).  It is HS400's strobe tap; every other mode programs
// it with the software value enabled and tap 0, which is 0x80.
//
// HS400 samples DATA against the card's strobe instead of against the host's own
// clock, so this is the register its calibration sweeps - SAMP_DL stays on the
// command line in that mode, which is the opposite of HS200.
//
#define A733_SMHC_DS_DL_SW_EN             BIT7
#define A733_SMHC_DS_DL_SW_MASK           0x3FU
#define A733_SMHC_DS_DL_DDR52             0x00000080U
#define A733_SMHC_SAMP_DL_RESET          0x00002000U
//
// SFC: bit 0 bypasses the controller's sample FIFO, so a read is sampled straight
// off the bus.  The vendor sets it for the duration of a delay-line calibration
// and clears it when normal traffic resumes - except for HS400, whose data path
// runs through that FIFO, so the bit stays clear while an HS400 tap is swept.
//
#define A733_SMHC_SFC_BYPASS              BIT0
#define A733_SMHC_CSDC_NORMAL            3U
//
// The CRC-status token the controller expects after a write in HS400.  CSDC's low
// field is the value the controller detects, and the vendor programs 6 there for
// HS400 and 3 for every other mode.
//
#define A733_SMHC_CSDC_HS400             6U
#define A733_SMHC_FIFO     0x200U

//
// The manual's register table stops at the command-queue block, so this one is
// not in it: the vendor's own driver for this SoC reads a 24-bit controller
// version at 0x300 (REG_SMCV), and that version is what decides whether the
// descriptor addresses below are word addresses or byte addresses.  The driver
// only prints it, as evidence for the next run; see A733_SMHC_DMA_ADDRESS_SHIFT.
//
#define A733_SMHC_VERSION      0x300U
#define A733_SMHC_VERSION_MASK 0xFFFFFFU

//
// the A733 user manual, SMHC_CTRL (global control).  Reset value 0x0000_0100, i.e.
// card-detect de-bounce is on and nothing else is.
//
#define A733_SMHC_CTRL_FIFO_AC_MOD  BIT31
// 1: the FIFO is reached over the AHB bus (CPU PIO).  0: over the DMA bus.
#define A733_SMHC_CTRL_TIME_UNIT_CMD BIT12
//
// DDR_MOD_SEL: the data bus runs double data rate, so the controller drives and
// samples DATA on both clock edges.  Set only while the card is in one of the
// DDR bus widths of EXT_CSD[183] (5 or 6), which is a card-side switch this
// driver makes with CMD6; SMHC_CTYPE itself only encodes 1/4/8 bits.
//
// The manual notes the data timeout is halved in an 8-bit DDR bus: the same
// SMHC_TMOUT field counts half as many card clocks' worth of wall-clock time.
// See A733_SMHC_DDR_DTO_HALVES.
//
#define A733_SMHC_CTRL_DDR_MODE     BIT10
#define A733_SMHC_CTRL_CD_DBC_ENB   BIT8
#define A733_SMHC_CTRL_DMA_ENB      BIT5
#define A733_SMHC_CTRL_INT_ENB      BIT4
#define A733_SMHC_CTRL_DMA_RST      BIT2
#define A733_SMHC_CTRL_FIFO_RST     BIT1
#define A733_SMHC_CTRL_SOFT_RST     BIT0

//
// the A733 user manual, SMHC_CLKDIV.  The field is a divider and the card clock is
// the module clock divided by (field + 1): field 0 is the full module clock,
// which is the only reading that fits the vendor's own driver - it never writes
// this register at all and divides the module clock in the CCU instead, so the
// reset value 0 has to mean "no division".  The manual's own text describes the
// field as "the source clock divided by 2*n", which is the other reading and
// the one that makes field 0 meaningless; the silicon follows the vendor.
//
#define A733_SMHC_CLKDIV_MASK_DAT0     BIT31
// 1: turn the card clock off while the FSM is idle.  0: always on.
#define A733_SMHC_CLKDIV_CCLK_CTRL     BIT17
#define A733_SMHC_CLKDIV_CCLK_ENB      BIT16
#define A733_SMHC_CLKDIV_CCLK_DIV_MASK 0x000000FFU

//
// the A733 user manual, SMHC_TMOUT.  Both limits count card clock periods (unit
// selected by SMHC_CTRL bits 12/11, which are left at 1 period).  The manual's
// reset value is 0xFFFF_FF40: data 0xffffff, response 0x40.
//
#define A733_SMHC_TMOUT_DTO_SHIFT 8U
#define A733_SMHC_TMOUT_DTO_MAX   0xFFFFFFU
#define A733_SMHC_TMOUT_RTO_MAX   0xFFU

//
// the A733 user manual, SMHC_CTYPE (bus width).  This is the controller's own
// encoding of a width, not EXT_CSD[183]'s: the card's DDR widths (5 and 6)
// change the *timing* of the bus, not how many lines it uses, so the controller
// takes an 8-bit DDR bus as A733_SMHC_BUS_8BIT here plus SMHC_CTRL.DDR_MOD_SEL.
//
#define A733_SMHC_BUS_1BIT  0U
#define A733_SMHC_BUS_4BIT  1U
#define A733_SMHC_BUS_8BIT  2U

//
// the A733 user manual /6, SMHC_BLKSIZ and SMHC_BYTCNT.  BYTCNT counts bytes and
// must be an integer multiple of BLKSIZ; BLKSIZ is 16 bits wide, BYTCNT 32.
//
#define A733_SMHC_BLKSIZ_MASK  0xFFFFU

//
// the A733 user manual, SMHC_CMD.  Writing START issues the command; the bit clears
// itself when the command has been sent.
//
#define A733_SMHC_CMD_START               BIT31
#define A733_SMHC_CMD_VOL_SW              BIT28
#define A733_SMHC_CMD_BOOT_MOD_MASK       (0x3U << 24)
#define A733_SMHC_CMD_PRG_CLK             BIT21
#define A733_SMHC_CMD_R1B_BUSY_HW_DETECT  BIT17
#define A733_SMHC_CMD_R1B_RESP            BIT16
#define A733_SMHC_CMD_SEND_INIT_SEQ       BIT15
#define A733_SMHC_CMD_STOP_ABT_CMD        BIT14
#define A733_SMHC_CMD_WAIT_PRE_OVER       BIT13
#define A733_SMHC_CMD_STOP_CMD_FLAG       BIT12
#define A733_SMHC_CMD_TRANS_MODE          BIT11
#define A733_SMHC_CMD_TRANS_DIR           BIT10
#define A733_SMHC_CMD_DATA_TRANS          BIT9
#define A733_SMHC_CMD_CHK_RESP_CRC        BIT8
#define A733_SMHC_CMD_LONG_RESP           BIT7
#define A733_SMHC_CMD_RESP_RCV            BIT6
#define A733_SMHC_CMD_IDX_MASK            0x3FU

//
// the A733 user manual, SMHC_RINTSTS (raw interrupt status, write 1 to clear).
// Bit 0 does not exist: the manual's table starts at bit 1 and says so on
// purpose, which is why a generous "clear everything" write spells out
// 0xFFFFFFFF rather than relying on a reserved bit being harmless.
// The odd 0x01 in the low half is BIT1, RE.
//
#define A733_SMHC_RINT_RE                 BIT1
#define A733_SMHC_RINT_CC                 BIT2
#define A733_SMHC_RINT_DTC                BIT3
#define A733_SMHC_RINT_DTR                BIT4
#define A733_SMHC_RINT_DRR                BIT5
#define A733_SMHC_RINT_RCE                BIT6
#define A733_SMHC_RINT_DCE                BIT7
#define A733_SMHC_RINT_RTO_BACK           BIT8
#define A733_SMHC_RINT_DTO_BDS            BIT9
#define A733_SMHC_RINT_DSTO_VSD           BIT10
#define A733_SMHC_RINT_FU_FO              BIT11
#define A733_SMHC_RINT_CB_IW              BIT12
#define A733_SMHC_RINT_DSE_BC             BIT13
#define A733_SMHC_RINT_ACD                BIT14
#define A733_SMHC_RINT_DEE                BIT15
#define A733_SMHC_RINT_SDIO               BIT16
#define A733_SMHC_RINT_R1B_BUSY_CLR       BIT17
#define A733_SMHC_RINT_DATA_WR_BUSY_TO    BIT18
#define A733_SMHC_RINT_CARD_INSERT        BIT30
#define A733_SMHC_RINT_CARD_REMOVAL       BIT31

//
// Every bit that means the command or its data failed.  DSE_BC is deliberately
// absent: on a write the manual defines it as "busy signal is cleared after the
// last block", which is a normal end-of-write event, and it is only an error
// while reading.  The driver adds it back for reads.
//
#define A733_SMHC_RINT_ERRORS                                                      \
  (A733_SMHC_RINT_RE | A733_SMHC_RINT_RCE | A733_SMHC_RINT_DCE |                  \
   A733_SMHC_RINT_RTO_BACK | A733_SMHC_RINT_DTO_BDS | A733_SMHC_RINT_DSTO_VSD |   \
   A733_SMHC_RINT_FU_FO | A733_SMHC_RINT_CB_IW | A733_SMHC_RINT_DEE |             \
   A733_SMHC_RINT_DATA_WR_BUSY_TO)

//
// The error bits that mean "the bus did not answer in time" rather than "the
// card answered something wrong": a caller can tell the two apart.
//
#define A733_SMHC_RINT_TIMEOUTS                                                    \
  (A733_SMHC_RINT_RTO_BACK | A733_SMHC_RINT_DTO_BDS | A733_SMHC_RINT_DSTO_VSD |   \
   A733_SMHC_RINT_DATA_WR_BUSY_TO)

//
// the A733 user manual, SMHC_STATUS.  Reset value 0x0000_0006.
//
#define A733_SMHC_STATUS_DMA_REQ            BIT31
#define A733_SMHC_STATUS_FIFO_LEVEL_MASK    0x0FFE0000U
#define A733_SMHC_STATUS_FIFO_LEVEL_SHIFT   17U
#define A733_SMHC_STATUS_RESP_IDX_MASK      0x0001F800U
#define A733_SMHC_STATUS_RESP_IDX_SHIFT     11U
#define A733_SMHC_STATUS_FSM_BUSY           BIT10
#define A733_SMHC_STATUS_CARD_BUSY          BIT9
#define A733_SMHC_STATUS_CARD_PRESENT       BIT8
#define A733_SMHC_STATUS_FSM_STA_MASK       0x000000F0U
#define A733_SMHC_STATUS_FIFO_FULL          BIT3
#define A733_SMHC_STATUS_FIFO_EMPTY         BIT2
#define A733_SMHC_STATUS_FIFO_TX_LEVEL      BIT1
#define A733_SMHC_STATUS_FIFO_RX_LEVEL      BIT0

//
// the A733 user manual, SMHC_FIFOTH.  The manual recommends burst size 3 (16
// DWORDs), RX trigger 15 and TX trigger 240 for a 256-entry, 1 KB FIFO, which
// is also the value the vendor tree programs.
//
#define A733_SMHC_FIFOTH_VALUE  ((3U << 28) | (15U << 16) | 240U)

// the A733 user manual, the FIFO is 256 entries of 32 bits ("FIFO_DEPTH = 256").
#define A733_SMHC_FIFO_DEPTH_WORDS  256U

//
// the A733 user manual, fixes that threshold arithmetically: the burst size must be a
// sub-multiple of (RX_TL + 1) and of (FIFO_DEPTH - TX_TL).  A733_SMHC_FIFOTH_VALUE
// is (3 << 28) | (15 << 16) | 240, i.e. burst 3 = 16 DWORDs = 64 bytes, and
// 16 is exactly both (RX_TL + 1) and (256 - TX_TL).  Every DMA chunk this
// driver builds (see A733_SMHC_DMA_CHUNK_BYTES) is a whole multiple of it.
//
#define A733_SMHC_FIFOTH_BURST_BYTES  (16U * 4U)

//
// the A733 user manual, SMHC_THLD: the card threshold the controller waits for
// before it starts a transfer.  The vendor's own init writes 512 in the size
// field with both enable bits, for every mode (u-boot sunxi_mmc_host_tm4.c:547,
// tm5:529).  Without it a write at 400 MB/s runs the FIFO dry before the card
// is ready - the underrun this port hit as an HS400 write failure.
//
#define A733_SMHC_THLD_SIZE_SHIFT  16U
#define A733_SMHC_THLD_RD_ENB      BIT0
#define A733_SMHC_THLD_WR_ENB      BIT2
#define A733_SMHC_THLD_VALUE \
  ((512U << A733_SMHC_THLD_SIZE_SHIFT) | A733_SMHC_THLD_WR_ENB | A733_SMHC_THLD_RD_ENB)

//
// the A733 user manual ..28: the internal DMA controller's own registers.  They are
// separate from the card-visible ones above: SMHC_IDST reports what the IDMAC
// did, and SMHC_IDIE enables the IDMAC's own interrupt bits.
//
#define A733_SMHC_DMAC  0x080U
#define A733_SMHC_DLBA  0x084U
#define A733_SMHC_IDST  0x088U
#define A733_SMHC_IDIE  0x08CU

//
// the A733 user manual, SMHC_DMAC.  DES_LOAD_CTRL makes the IDMAC re-fetch a
// descriptor whose valid bit was clear; FIX_BUST_CTRL restricts the AHB master
// to SINGLE/INCR4/INCR8; IDMAC_ENB is the block's enable and IDMAC_RST resets
// it (the manual's own programming steps write 0x82 for "enabled, fixed burst").
//
#define A733_SMHC_DMAC_DES_LOAD_CTRL  BIT31
#define A733_SMHC_DMAC_IDMAC_ENB      BIT7
#define A733_SMHC_DMAC_FIX_BUST_CTRL  BIT1
#define A733_SMHC_DMAC_IDMAC_RST      BIT0
#define A733_SMHC_DMAC_VALUE \
  (A733_SMHC_DMAC_FIX_BUST_CTRL | A733_SMHC_DMAC_IDMAC_ENB)

//
// the A733 user manual /27 and tables 11-4/11-7/11-8: the descriptor list base, a
// descriptor's buffer address and its "next descriptor" pointer are written as
// *word* addresses - the byte address shifted right by two - which is also how
// the vendor's driver for this controller programs them once the controller
// version is 0x50300 or later (the A733's is).  Every address this driver uses
// comes from the pool in A733SmhcHci.c and is therefore both below 4 GB and
// DWORD aligned, so the shift loses nothing.
//
#define A733_SMHC_DMA_ADDRESS_SHIFT  2U
#define A733_SMHC_DMA_WORD_ADDRESS(Address) \
  ((UINT32)((UINT64)(UINTN)(Address) >> A733_SMHC_DMA_ADDRESS_SHIFT))

//
// the A733 user manual, SMHC_IDST.  TX_INT/RX_INT say a descriptor's data moved,
// FATAL_BERR_INT and DES_UNAVL_INT say the IDMAC itself failed (the first has
// its cause encoded in DMAC_ERR_STA), and ERR_FLAG_SUM mirrors the card-side
// error bits of SMHC_RINTSTS.  The manual clears the block with 0x337.
//
#define A733_SMHC_IDST_AIS            BIT9
#define A733_SMHC_IDST_NIS            BIT8
#define A733_SMHC_IDST_ERR_FLAG_SUM   BIT5
#define A733_SMHC_IDST_DES_UNAVL      BIT4
#define A733_SMHC_IDST_FATAL_BERR     BIT2
#define A733_SMHC_IDST_RX_INT         BIT1
#define A733_SMHC_IDST_TX_INT         BIT0
#define A733_SMHC_IDST_ERR_STA_MASK   (0x7U << 10)
#define A733_SMHC_IDST_ERR_STA_SHIFT  10U
#define A733_SMHC_IDST_CLEAR_MASK \
  (A733_SMHC_IDST_AIS | A733_SMHC_IDST_NIS | A733_SMHC_IDST_ERR_FLAG_SUM |      \
   A733_SMHC_IDST_DES_UNAVL | A733_SMHC_IDST_FATAL_BERR | A733_SMHC_IDST_RX_INT | \
   A733_SMHC_IDST_TX_INT)

//
// The two bits that mean the DMA transport itself failed, as opposed to the
// card answering something wrong: a descriptor the IDMAC could not fetch, or a
// bus error that made it stop.  These are what tells a DMA failure from a card
// failure and they are the only SMHC_IDST bits that decide a transfer's outcome.
//
#define A733_SMHC_IDST_DMA_FATAL \
  (A733_SMHC_IDST_FATAL_BERR | A733_SMHC_IDST_DES_UNAVL)

//
// the A733 user manual, SMHC_IDIE: one enable per SMHC_IDST bit.  TX_INT for a
// write, RX_INT for a read, and the three failure summaries: this driver wants
// the IDMAC to speak up exactly when its descriptor-level work has an answer.
//
#define A733_SMHC_IDIE_TX_INT_ENB     BIT0
#define A733_SMHC_IDIE_RX_INT_ENB     BIT1
#define A733_SMHC_IDIE_FERR_INT_ENB   BIT2
#define A733_SMHC_IDIE_DES_UNAVL_ENB  BIT4
#define A733_SMHC_IDIE_ERR_SUM_ENB    BIT5
#define A733_SMHC_IDIE_VALUE \
  (A733_SMHC_IDIE_TX_INT_ENB | A733_SMHC_IDIE_RX_INT_ENB |                       \
   A733_SMHC_IDIE_FERR_INT_ENB | A733_SMHC_IDIE_DES_UNAVL_ENB |                  \
   A733_SMHC_IDIE_ERR_SUM_ENB)

//
// the A733 user manual, and table 11-3: an IDMAC descriptor is four 32-bit words.
// DES0 is control and status, DES1 the buffer's size in bytes (a multiple of
// four, 13 bits wide on SMHC0-2), DES2 the buffer's own word address, DES3 the
// next descriptor's word address.
//
// The manual names bits 31 (ownership), 30 (error), 4 (chain, "must be set 1"),
// 3 (first), 2 (last) and 1 (no interrupt on completion); bit 5 is listed as
// not used, so it is left clear here.  The two vendor drivers for this
// controller disagree about bit 5 - Linux's leaves it clear, one older u-boot
// sets it on the final descriptor - and the manual's own table does not name
// it, so this driver follows the manual and the current vendor driver.
//
#define A733_SMHC_DES_OWN       BIT31
#define A733_SMHC_DES_ERROR     BIT30
#define A733_SMHC_DES_CHAIN     BIT4
#define A733_SMHC_DES_FIRST     BIT3
#define A733_SMHC_DES_LAST      BIT2
#define A733_SMHC_DES_NO_INT    BIT1

// The buffer-size field of DES1 on SMHC0-2 (SMHC3's is a bit narrower).
#define A733_SMHC_DES_SIZE_MASK  0x1FFFU

/**
  One IDMAC descriptor, as the manual's figure 11-10 lays it out: four words,
  DES0 first.  The struct is what the hardware reads, so the field order is the
  register order, and every instance lives in a pool the driver allocates below
  4 GB (see A733SmhcHci.c).
**/
typedef struct {
  UINT32    Control;    // DES0
  UINT32    Size;       // DES1, bytes
  UINT32    Buffer;     // DES2, word address
  UINT32    Next;       // DES3, word address, 0 on the last descriptor
} A733_SMHC_DESCRIPTOR;

// the A733 user manual, SMHC_INTMASK: one enable per SMHC_RINTSTS bit.  This is the
// set the manual's own initialisation asks for (0xFFCE, "the common interrupts
// and the interrupts caused by error/anomaly") plus bit 18, the write-busy
// timeout, which SMHC_RINT_ERRORS already treats as fatal and which 0xFFCE
// leaves out: 0x0004_FFCE.
#define A733_SMHC_INTMASK_VALUE                                                    \
  (A733_SMHC_RINT_CC | A733_SMHC_RINT_DTC | A733_SMHC_RINT_ACD |                  \
   A733_SMHC_RINT_ERRORS | A733_SMHC_RINT_DSE_BC)

//
// SMHC2's interrupt as the GIC sees it: SPI 163, i.e. interrupt ID 163 (the
// platform's own DTB and the manual's interrupt table both put the SMHC2
// controller there).
//
#define A733_SMHC2_SPI  195U

//
// the A733 user manual, SMHC_HWRST (default 1).  0 drives the card's hardware
// reset line and puts the card in pre-idle; 1 is normal operation.
//
#define A733_SMHC_HWRST_RESET  0U
#define A733_SMHC_HWRST_ACTIVE BIT0

//
// eMMC command indices this driver issues itself or recognises.  The manual's
// SMHC chapter writes them into SMHC_CMD's index field, and the identification
// order is the one the eMMC spec's 6.4.1 puts the device through.
//
#define A733_SMHC_CMD_GO_IDLE_STATE      0U
#define A733_SMHC_CMD_SEND_OP_COND       1U
#define A733_SMHC_CMD_ALL_SEND_CID       2U
#define A733_SMHC_CMD_SET_RELATIVE_ADDR  3U
#define A733_SMHC_CMD_SWITCH             6U
#define A733_SMHC_CMD_SELECT_CARD        7U
#define A733_SMHC_CMD_SEND_EXT_CSD       8U
#define A733_SMHC_CMD_SEND_CSD           9U
#define A733_SMHC_CMD_SEND_CID           10U
#define A733_SMHC_CMD_STOP_TRANSMISSION  12U
#define A733_SMHC_CMD_SEND_STATUS  13U
#define A733_SMHC_CMD_SET_BLOCKLEN       16U
#define A733_SMHC_CMD_READ_SINGLE_BLOCK  17U
#define A733_SMHC_CMD_READ_MULTIPLE      18U
#define A733_SMHC_CMD_SET_BLOCK_COUNT    23U
#define A733_SMHC_CMD_WRITE_SINGLE_BLOCK 24U
#define A733_SMHC_CMD_WRITE_MULTIPLE     25U

//
// CMD6's argument is (value << 24) | (EXT_CSD index << 16) | (access << 8) |
// command set; access 0x03 writes one byte of EXT_CSD.
//
#define A733_SMHC_SWITCH_ACCESS_MASK  0x03U

//
// EXT_CSD byte offsets this driver programs (eMMC spec 6.99; the same layout as
// MdePkg's EMMC_EXT_CSD, which names each byte in its comments).
//
#define A733_SMHC_EXT_CSD_BUS_WIDTH   183U
#define A733_SMHC_EXT_CSD_HS_TIMING   185U
#define A733_SMHC_EXT_CSD_DEVICE_TYPE 196U
#define A733_SMHC_EXT_CSD_SIZE        512U

//
// EXT_CSD[183] BUS_WIDTH, which is the card's field - the first three values
// happen to match SMHC_CTYPE (A733_SMHC_BUS_*), the DDR ones do not.  The DDR
// widths are the single-data-rate widths with the bus timing doubled; the card
// expects HS_TIMING = 2 with them.
//
#define A733_SMHC_EXT_CSD_WIDTH_1BIT      0U
#define A733_SMHC_EXT_CSD_WIDTH_4BIT      1U
#define A733_SMHC_EXT_CSD_WIDTH_8BIT      2U
#define A733_SMHC_EXT_CSD_WIDTH_4BIT_DDR  5U
#define A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR  6U

//
// EXT_CSD[196] DEVICE_TYPE bits, as the in-tree SDHCI driver reads them:
// bit 0 legacy 26 MHz, bit 1 High Speed (26/52 MHz), bit 2 DDR52 at 52 MHz,
// bit 3 DDR52 at 52 MHz with 1.2 V signalling, bits 4-5 HS200, bits 6-7 HS400.
// Bit 2 is the DDR mode this board can use - the same 3.3 V signalling the rest
// of the identification runs at; bit 3's lower signalling voltage is not
// something this board can supply.
//
#define A733_SMHC_DEVICE_TYPE_HIGH_SPEED  BIT1
#define A733_SMHC_DEVICE_TYPE_DDR52       BIT2
//
// Bits 4 and 5 are HS200 at 1.8 V and at 1.2 V, and they are not
// interchangeable: a card that lists only bit 5 asks for a VCCQ this board does
// not supply (VCC-PC is CLDO5 at 1.8 V, see the PMIC note), so the HS200 step is
// gated on bit 4 and skips a 1.2 V-only card with a line that says so.  This
// card's EXT_CSD[196] reads 0x57: bit 4 and bit 6 set, which is HS200 and HS400
// both at 1.8 V.
//
#define A733_SMHC_DEVICE_TYPE_HS200_1V8   BIT4
#define A733_SMHC_DEVICE_TYPE_HS200_1V2   BIT5
//
// Bit 6 is HS400 at 1.8 V - the fastest mode the card names, and the one the
// ladder above SDR tries first.
//
#define A733_SMHC_DEVICE_TYPE_HS400       BIT6

//
// EXT_CSD[185] HS_TIMING.  The encoding is not one code per speed mode: High
// Speed covers both SDR52 and DDR52, and it is EXT_CSD[183] (BUS_WIDTH 2 versus
// 6) that says which.  HS200 and HS400 are the next two codes.
//
//   0 = backwards compatible   1 = High Speed (SDR52, or DDR52 at width 5/6)
//   2 = HS200                  3 = HS400
//
// This is the encoding the vendor driver for this part uses (u-boot2018
// include/mmc.h: EXT_CSD_TIMING_HS 1, _HS200 2, _HS400 3) and the one the card
// itself follows.  Writing 2 for DDR52 asked the card for HS200 while the host
// switched to DDR framing, and writing 3 for HS200 asked it for HS400 - which is
// why both modes failed on a bus whose reads were otherwise correct.
//
#define A733_SMHC_HS_TIMING_HIGH_SPEED  1U
#define A733_SMHC_HS_TIMING_DDR52       1U
#define A733_SMHC_HS_TIMING_HS200_VALUE 2U
#define A733_SMHC_HS_TIMING_HS400_VALUE 3U

//
// The manual's own note against an 8-bit DDR bus: the data timeout is halved.
// SMHC_TMOUT's field counts card clock periods, and DDR moves two of them per
// clock, so the same 0xFFFFFF buys half the wall-clock time an SDR bus would
// get from it.  The driver prints the resulting bound (see the DDR step in
// A733SmhcCard.c) rather than changing the field: it is already at its maximum,
// and even halved it covers the card's own access time several thousand times
// over at any clock this driver uses.
//
#define A733_SMHC_DDR_DTO_HALVES  2U

//
// OCR bits: bit 30 puts the device in sector addressing (the eMMC equivalent of
// high capacity), bit 31 is "power-up still in progress" and reads 1 until the
// device is ready.  The voltage window the argument offers is the card's whole
// 2.7-3.6 V range; the board supplies 3.3 V and cannot switch it.
//
// Bit 31 is what the identification loop deliberately does *not* gate on: the
// board showed a 64 GB device answering CMD1 with an OCR whose bit 30 was clear,
// which no such device can report correctly, so the captured word is not
// trustworthy enough to end the sequence on.  The loop asks CMD2 instead, which
// is the device's own statement that it is ready, and only reports this bit as
// evidence (see SmhcCardIdentifyRound).
//
#define A733_SMHC_OCR_SECTOR_MODE  BIT30
//
// OCR bit 31 is the power-up status, and like SD its polarity is the opposite
// of its name: 0 while the device is still in its power-up cycle, 1 once that
// cycle is over and the rest of the OCR is meaningful.  A7A hardware answers
// 0x00FF8080 mid-cycle and 0xC0FF8080 afterwards.  Identification may not send
// CMD2 until this bit is set - a device that is still powering up answers CMD1
// and ignores everything else, which is exactly how this driver first failed.
//
#define A733_SMHC_OCR_POWER_UP_DONE BIT31
#define A733_SMHC_OCR_POLL_US       200000U
#define A733_SMHC_OCR_ARG          0x40FF8000U

#endif