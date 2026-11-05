/** @file
  eMMC host driver for the Allwinner A733 (sun60iw2p1).

  The driver brings each controller the board declares up far enough to identify
  the card on it and then implements EFI_SD_MMC_PASS_THRU_PROTOCOL, one handle
  per controller: MdeModulePkg's EmmcDxe, PartitionDxe and FatPkg build the
  block, partition and file-system layers on top of it, which is what gives the
  Shell its fs0:.

  Which controllers exist is board data (Library/A733SmhcBoardLib.h and the
  library that answers A733SmhcBoardControllers), not a build switch: this driver
  has no controller it knows by name.  Everything it needs about one - base,
  interrupt, CCU module and clock register, pad function, card type - is carried
  in A733_SMHC_PRIVATE, filled from the table entry the entry point is bringing
  up.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_SMHC_DXE_H__
#define A733_SMHC_DXE_H__

#include <Uefi.h>

#include <Protocol/SdMmcPassThru.h>

#include <Library/A733CcuLib.h>
#include <Library/A733GpioLib.h>
#include <Library/A733SmhcBoardLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>

#include <A733Smhc.h>

//
// The diagnostic reporting switch.
//
// The calibration probes and the sweeps' per-tap lines measure exactly what they
// always measured - this macro changes no decision, no register write and no
// command - but their lines reach the console only when a build asks for them
// with PcdA733SmhcDumpEnable, the same switch the per-command dump uses.  It
// takes DEBUG's own argument tuple, so a gated line is written exactly like an
// unconditional one and keeps its level.
//
// Not behind it: the lines that say what was negotiated - the card is ready, the
// bus width, the mode and the taps and rate it ended on, a step that failed and
// was reverted, a sweep that could not measure.  Those are what explains a
// failure from a log someone else collected.
//
#define SMHC_DIAG(Expression)                          \
  do {                                                 \
    if (PcdGetBool (PcdA733SmhcDumpEnable)) {          \
      DEBUG (Expression);                              \
    }                                                  \
  } while (FALSE)

//
// the A733 user manual, SMHC_NDMA_MODE_CTL (0x7C).  The driver never writes it - it
// moves every byte on the IDMAC - but it is the register the manual lists that
// could plausibly select a per-controller DMA mode, and it is one of the few
// registers this driver never touches.  It is printed at bring-up and on a
// failed transfer so a value that differs between two controllers can be seen
// rather than guessed at.
//
#define A733_SMHC_NDMA_MODE_CTL  0x07CU

//
// The controller version at or above which this family writes an IDMAC
// descriptor address as a word address (the vendor's rule; the manual's own
// descriptor tables agree for this part).  The driver applies that convention
// unconditionally, so the version is printed once per boot next to a verdict on
// whether the convention is the right one for the controller in use.
//
#define A733_SMHC_VERSION_WORD_ADDRESS  0x50300U

//
// One slot, slot 0: the board has one eMMC and the controller has no second
// card.  GetNextSlot reports this slot once and EFI_NOT_FOUND after it.
//
#define A733_SMHC_SLOT       0U
#define A733_SMHC_SLOT_MAX   1U

//
// How many controllers this driver can hold an interrupt registration for.  It
// is a bound on the driver's own tables, not on the board's: the interrupt
// protocol hands its handler no context, so each controller's interrupt has to
// be found by its source in a table of this size, and a board whose table names
// more controllers than this is refused one registration at a time rather than
// silently sharing a handler.  This SoC has four SMHC controllers (SPI 193..196),
// so four is the natural ceiling; nothing here depends on it being exactly that.
//
#define A733_SMHC_MAX_CONTROLLERS  4U

// CMD8's 512-byte EXT_CSD read, and the eMMC's data block: this stack moves
// the device in 512-byte blocks (EmmcDxe fixes Media->BlockSize at 0x200 and
// the FAT driver uses it for everything).
#define A733_SMHC_BLOCK_SIZE  0x200U

//
// A single command's data transfer is capped here and a larger request is split
// into several commands.  The controller itself has no such limit - SMHC_BYTCNT
// is 32 bits, so a UINT32 length always fits - but one command holds the bus
// until the whole byte count has moved, and the descriptor ring below is sized
// to this cap.  1 MiB is the size the FAT driver's own I/O stays under in
// practice, so an ordinary read or write is one command.
//
#define A733_SMHC_MAX_TRANSFER_BYTES  (1024U * 1024U)

//
// The data phase runs on the controller's own DMA controller, so a transfer is
// split into descriptors rather than moved by the CPU, and two pool sizes
// follow from A733_SMHC_MAX_TRANSFER_BYTES.
//
// One descriptor covers A733_SMHC_DMA_CHUNK_BYTES, and the size field of DES1 is
// not as wide on every controller of this family: the manual's table 11-5 gives
// SMHC0-2 bits 12:0 (mask 0x1FFF) but its table 11-6 gives SMHC3 bits 11:0
// (mask 0xFFF).  A 4 KB descriptor writes 4096, which does not fit the narrower
// field, and the manual's own rule for that field says a zero size is ignored -
// so on SMHC3 a whole 4 KB transfer became one ignored descriptor, the card kept
// sending into the FIFO, and the read failed at its end bit with the IDMAC
// reporting no error at all.
//
// 2 KB is therefore the chunk for *every* controller, not a value chosen per
// controller: 2048 fits the narrowest field in the family (SMHC3's 12 bits, whose
// ceiling is 4095), so one compile-time constant is correct everywhere the
// driver can be pointed, and the ring's size stays a compile-time constant too.
// It is also a whole multiple of the 16-DWORD (64 byte) burst SMHC_FIFOTH asks
// for, and it is the largest power of two the narrowest field holds (the ring's
// arithmetic needs a power of two for A733_SMHC_DMA_DESC_COUNT to be exact).
//
// A733_SMHC_DES_SIZE_LIMIT names that narrowest field, and the assertion in
// A733SmhcHci.c holds the chunk against it.
//
// The pool also holds the bounce buffer: a caller's buffer that the descriptor
// address field cannot reach, or that a read could not invalidate without
// touching a neighbour's cache line, is moved through it instead.  One 1 MiB
// transfer is the most a single command can move, so one bounce buffer of that
// size serves every command.
//
#define A733_SMHC_DES_SIZE_LIMIT   0x0FFFU
#define A733_SMHC_DMA_CHUNK_BYTES  SIZE_2KB
#define A733_SMHC_DMA_DESC_COUNT   (A733_SMHC_MAX_TRANSFER_BYTES / A733_SMHC_DMA_CHUNK_BYTES)

//
// The descriptor address fields are 32 bits wide and hold word addresses, so a
// buffer the hardware can reach is one whose last byte stays below 4 GB.  The
// pool is allocated with this as its ceiling, and a caller's buffer above it is
// bounced through the pool.  (SIZE_4GB - 1, because the allocator's ceiling is
// the address of the allocation's last byte.)
//
#define A733_SMHC_DMA_POOL_LIMIT  (SIZE_4GB - 1U)

// Command and data timeouts.  The device's own timeout (Packet->Timeout) wins
// when it is larger; these are the floors for the driver's own commands and for
// a packet that asks for nothing.
#define A733_SMHC_CMD_TIMEOUT_US   1000000U
#define A733_SMHC_DATA_TIMEOUT_US  5000000U
//
// How long a data command waits for the card to release its busy line before
// the next command may be issued.  The observed release is tens to hundreds of
// microseconds at HS400 (a 500 us blind delay was already marginal, 1 ms
// reliable), so 2 ms is the safety net; a card that needs longer is reported
// rather than waited for.
//
#define A733_SMHC_DATA_IDLE_TIMEOUT_US  2000U
#define A733_SMHC_RESET_TIMEOUT_US 100000U

//
// Card detect is a de-bounced input, so the first question about it is asked
// with a step of its own and a budget: see SmhcCardWaitPresent.
//
#define A733_SMHC_CD_SETTLE_STEP_US  10000U
#define A733_SMHC_CD_SETTLE_US       100000U

//
// Card clock.  Three facts settle how this driver clocks the card, and all of
// them come from how the vendor's own driver for this controller family clocks
// it:
//
//   - SMHC_CLKDIV's field is not the card clock's scaler.  The vendor keeps it
//     at 0 for a single-data-rate bus and at 1 for a double-data-rate one, and
//     takes the *card* clock from the module clock the CCU provides: its
//     module clock is twice the card clock for SDR and four times it for DDR,
//     so the card clock is the module clock divided by 2 * (field + 1).  A
//     module clock far above the card clock is therefore not a configuration
//     this hardware is used in - and a *data* transfer is where that shows: the
//     board's first data read (CMD8, 512 bytes) came back with a data end-bit
//     error and a *valid* CRC when it ran on the identification divider (field
//     59, a module clock 120 times the card clock), and the identical read at
//     the data divider succeeded both before and after that.  Commands carry no
//     data and are happy on any divider, which is why identification may keep
//     using the large one.
//   - The module clock is what decides the bus speed, and the CCU can give
//     SMHC2 a parent far faster than HOSC: PERI0_800M (nominal 800 MHz) with
//     M = N = 4 is a 50 MHz module clock, which reaches a 50 MHz SDR bus; for
//     the 100 MB/s DDR52 bus the module clock is doubled and the divider set to
//     1 (M = 8, i.e. a 100 MHz module clock, 50 MHz on the card), which is the
//     same recipe the mainline driver uses for MMC DDR52.  HOSC with M = 1 and
//     both dividers at 1 is the 12 MHz data path the driver was verified at,
//     kept as the baseline every other measurement is read against.
//   - The vendor's own factors (2x the card clock for SDR, 4x for DDR) are its
//     compensation for a per-clock timing-mode switch it flips *in the CCU*;
//     this driver never touches that switch, so here the module clock is the
//     CCU's rate and SMHC_CLKDIV is a plain (field + 1) divider.  The board
//     measurement agrees: PERI0's at-speed bus moves 40 MB/s, which a 25 MHz
//     card clock could not do.
//   - A plan therefore carries *two* controller dividers, because the two
//     phases run under different limits: the bring-up divider covers the first
//     data read and the CMD6 switches, which happen while the card is still in
//     compatibility timing (whose limit is 26 MHz); the data divider is the
//     at-speed one, used once the card is in High Speed timing (limit 52 MHz).
//     Both are small - 0 or 1 - so every data transfer this driver makes runs
//     at a module-to-card ratio the hardware is designed for.
//
// The plans are named here and their CCU details live in A733SmhcDxe.c
// (mA733SmhcMmcClockPlans and mA733SmhcSdClockPlans), because A733_CCU_SRC is a
// CCU type this header does not include.  The two tables hold the same plan
// indices over different parents: the MMC profile's fast parent is PERI0_800M,
// which SMHC2/SMHC3 can reach, and the SD profile's is PERI0_400M, which is the
// fastest parent SMHC0/SMHC1 have at all - a plan naming PERI0_800M for those two
// modules is refused by the CCU.  A controller's profile comes from its board
// table entry (A733_SMHC_CLOCK_PROFILE), and everything that indexes a table -
// SmhcSelectClockPlan, SmhcClockPlanName and the bring-up divider it records -
// reads the one belonging to the controller it is working on.
// SmhcSelectClockPlan applies one to the hardware and records what it actually
// got: the module clock the library reports for the register as it reads back,
// and the three controller dividers computed from it (the identification one,
// the bring-up one and the at-speed one).
//
#define A733_SMHC_PLAN_HOSC       0U   // HOSC 24 MHz / 1     -> 24 MHz module: bring-up and data /2 = 12 MHz
#define A733_SMHC_PLAN_PERI0      1U   // PERI0_800M / 4 / 1  -> 200 MHz module: bring-up /4 = 25 MHz, data /2 = 50 MHz
#define A733_SMHC_PLAN_PERI0_DDR  2U   // PERI0_800M / 4 / 1  -> 200 MHz module: data /2 = 50 MHz (DDR)
#define A733_SMHC_PLAN_PERI0_HS200 3U  // PERI0_800M / 1 / 1  -> 800 MHz module: data /4 = 200 MHz (HS200)
// HS400 is the one plan whose divider is 0: the manual requires SMHC_CTRL's
// CCLK_DIV field to be cleared when HS400_MD_EN is set, so the card clock is the
// module clock halved - 400 MHz / 2 = 200 MHz - rather than the plan's divider.
#define A733_SMHC_PLAN_PERI0_HS400 4U  // PERI0_800M / 2 / 1  -> 400 MHz module: data /2, divider field 0 = 200 MHz (HS400)
#define A733_SMHC_PLAN_MAX        5U

// The plan the driver starts on: PERI0_800M / 4 / 1, the fast plan this pass is
// about.  A733_SMHC_PLAN_HOSC reproduces the 12 MHz data path the driver was
// verified at, with all of the measurement in place - it is how the baseline is
// re-measured - and it is a clean baseline: the DDR and HS200 steps belong to the
// plan named below, so a HOSC build reports its SDR rate and stops there.
// PROBE: set FALSE to run the HS200 attempt from a clean SDR card.
#define A733_SMHC_DDR_ENABLED  TRUE

//
// HS400 reads at 334 MB/s here, and writes do not work in it: the first write
// this port makes on boot (the GPT repair) ends in a device error after 512
// bytes, so the block devices appear and the file systems do not mount.  The
// step is kept, and kept off, until its write side is calibrated - HS200 gives
// 237 MB/s with a fully working file system, which is the bus to ship.
//
#define A733_SMHC_HS400_ENABLED  TRUE

#define A733_SMHC_PLAN_DEFAULT  A733_SMHC_PLAN_PERI0

//
// The DDR step: it runs on the fast SDR plan (A733_SMHC_PLAN_DDR_FROM) and
// moves the module clock to A733_SMHC_PLAN_DDR for the DDR phase.  DDR turns a
// 50 MHz SDR bus into the 100 MB/s one the DDR52 mode is for (104 MB/s at the
// spec's 52 MHz).  On the baseline plan the same step would give a 24 MB/s bus,
// which is a different question and one no boot asks; naming the plans here keeps
// that a single decision instead of a condition inside the DDR step.
//
#define A733_SMHC_PLAN_DDR_FROM  A733_SMHC_PLAN_PERI0
#define A733_SMHC_PLAN_DDR       A733_SMHC_PLAN_PERI0_DDR

//
// The HS200 step: it runs on the same fast SDR plan, after the DDR step has
// failed or been skipped, and moves the module clock to A733_SMHC_PLAN_HS200.
// The field divides by 2 x (n + 1) on this part (measured: 200 MHz module with
// n = 1 is a 50 MHz bus), so 800 MHz with n = 1 is a 200 MHz card clock - HS200's
// own limit, and single-edge, so the bus is 8-bit SDR throughout.
//
#define A733_SMHC_PLAN_HS200  A733_SMHC_PLAN_PERI0_HS200

//
// The HS400 step runs on a plan of its own, because the manual names two fields
// HS400 must have in a state no other mode uses: SMHC_CTRL's CCLK_DIV must be
// cleared and its DDR_MOD_SEL must be clear as well, which makes the card clock
// the module clock halved.  A 400 MHz module with the divider field at 0 is
// therefore the 200 MHz HS400 runs at - the same card clock HS200 gets from
// 800 MHz with the field at 1.
//
#define A733_SMHC_PLAN_HS400  A733_SMHC_PLAN_PERI0_HS400

//
// HS400's two delay lines at that clock, as the vendor's own driver leaves them
// before its calibration: DS_DL 0x83 (software value enabled, tap 3) for the data
// strobe and SAMP_DL 0x86 (enabled, tap 6) for the command line.  The data
// strobe's tap is what this driver sweeps; the command line's is the vendor's
// initial value, kept as one constant so a recovery can put it back.
//
// How many times the HS400 step re-reads at its chosen strobe tap before it
// gives up on the mode: the first read can land while the delay register and the
// clock it belongs to are still settling.
#define A733_SMHC_HS400_VERIFY_ATTEMPTS  3U
#define A733_SMHC_HS400_DS_TAPS   3U
// Measured on this board: HS400 writes pass with the data output phase at 90
// degrees (SMHC_DRV_DL bit17 clear, bit16 set) and the controller sampling the
// card's CRC-status token at tap 4.  The vendor's own entry values (tap 6, data
// 180 degrees) write nowhere here.  A733_SMHC_HS400_CMD_TAPS is what the HS400
// step enters with; the write sweep still runs behind it and only costs time
// when this pair stops working.
#define A733_SMHC_HS400_CMD_TAPS  4U

//
// The write side of HS400, which no read-side sweep calibrates: in a write the
// host drives DATA and the card answers with the CRC-status token alone, so the
// strobe tap (DS_DL) is not in that path at all.  What a write depends on is the
// data lines' output phase - SMHC_DRV_DL bit 17, with bit 16 held - and the point
// the controller samples the token at, which at HS400 is SMHC_SAMP_DL, the command
// line's tap.  The write sweep therefore tries the manual's table 11-9 values for
// that phase (bit 17 set, then clear) and, at each, SAMP_DL over its whole range.
//
#define A733_SMHC_HS400_WRITE_DRV_DL_COUNT  2U

//
// A setting is only worth a line once the bus under it is known to carry data:
// the first failed write leaves the card in the receive state, so the sweep
// recovers the card and proves the bus before it tries the next one (see
// A733_SMHC_HS400_RECOVER_POLLS), and it stops rather than report settings it
// could not measure.  Two further dimensions exist and are only swept when
// nothing in DRV_DL x SAMP_DL writes: NTSR's 1x/2x mode bit, whose inherited
// value puts a 400 MHz module against a 200 MHz card clock, and CSDC's token.

//
// How long a write's own completion is awaited before it is read back: the data
// phase ends when the last block's CRC-status token has been accepted, not when
// the card has programmed the blocks, so CMD13's READY_FOR_DATA is polled until a
// budget runs out.  A 4 KiB write programs for a few milliseconds and a slow one
// for tens, so the budget is deliberately far above that.  Of the two limits, the
// deadline is the one that decides: the attempt count only stops a card that
// answers in microseconds from spinning longer than the deadline allows.
//
#define A733_SMHC_HS400_WRITE_READY_BUDGET_US  200000U
#define A733_SMHC_HS400_WRITE_STATUS_POLLS     200U
#define A733_SMHC_HS400_WRITE_STATUS_POLL_US   1000U

//
// The wait for the card to come back from the write sweep.  The sweep's last act
// is a 4 KiB write-back, and a card programming that write stops answering
// commands for as long as it needs; the sweep's own 200 ms readiness poll steps
// over a timeout by design, so the card can still be busy when the mode step
// returns and the block layer's first read (the partition scan) then gets a
// response timeout that has nothing to do with the bus.  This is the same CMD13
// readiness poll with a budget wide enough for the slowest programming seen on
// this board, and it returns the moment the card answers from TRAN with
// READY_FOR_DATA set, so a card that is already idle costs one command.
//
#define A733_SMHC_CARD_READY_POLLS      10000U
#define A733_SMHC_CARD_READY_POLL_US    1000U

//
// One readiness CMD13's own budget.  The command is a few bytes on an idle bus at
// 200 MHz, so 10 ms is thousands of times what an answer needs, while a card that
// is holding DATA0 low answers with a response timeout instead: the whole point of
// a short budget is that waiting for a busy card costs the 1 ms spacing and not
// the driver's general one-second command timeout.
//
#define A733_SMHC_HS400_WRITE_STATUS_CMD_US  10000U

//
// How many times the read-back after a completed write is attempted before the
// setting is called bad.  A read sent while the card is still programming fails
// within microseconds with a response timeout - the card is busy, not the tap
// wrong - so the first failure is not evidence.
//
#define A733_SMHC_HS400_WRITE_READBACK_TRIES  3U

//
// The sweep's whole budget.  33 settings of a 4 KiB write plus a read-back cost
// well under a second; this bounds the pathological case - a card that never
// becomes ready under any setting - at a few seconds of boot instead of minutes.
// When it runs out the sweep stops where it is and keeps the setting it entered
// with, which is a statement about the budget and not about the mode.
//
#define A733_SMHC_HS400_WRITE_SWEEP_BUDGET_US  5000000U

//
// How a write's readiness wait ended, printed on the attempt's own line so that
// one line tells the three cases apart: CMD13 said READY_FOR_DATA; CMD13 answered
// but never said it within the budget; or CMD13 stopped answering altogether.
//
#define A733_SMHC_WAIT_END_NOT_RUN   0U
#define A733_SMHC_WAIT_END_READY     1U
#define A733_SMHC_WAIT_END_BUDGET    2U
#define A733_SMHC_WAIT_END_DEAD_BUS  3U

//
// CMD13's R1 status word, as the card lays it out: bits 12:9 are CURRENT_STATE
// (0 idle, 1 ready, 2 ident, 3 stby, 4 trans, 5 data, 6 rcv, 7 prg), bit 8 is
// READY_FOR_DATA and bit 15 is the error bit.  The write test reads it to tell a
// card that took the write from one that never started it.
//
#define A733_SMHC_R1_READY_FOR_DATA  BIT8
#define A733_SMHC_R1_STATE_SHIFT     9U
#define A733_SMHC_R1_ERROR           BIT15
#define A733_SMHC_R1_STATE_TRAN      4U

//
// Recovering the card between write attempts.  A write the card never finished
// leaves it in CURRENT_STATE 6 (receiving) - the state the write test's own CMD13
// printed.  Only CMD12 ends a transfer: every other command is illegal in that
// state and comes back as a response timeout (rint 00000104, RTO+CC) in a few
// microseconds, which is what 63 of the write sweep's 64 settings were measured
// on.  So: CMD12, then CMD13 until the card is in TRAN and ready, then a read of
// LBA 0 to prove the bus, before the next setting may be tried at all.
//
#define A733_SMHC_HS400_RECOVER_POLLS    50U
#define A733_SMHC_HS400_RECOVER_POLL_US  1000U

// The prove read: one block of LBA 0 - the block the write test writes, and the
// only block this driver ever writes.
#define A733_SMHC_HS400_RECOVER_PROBE_BYTES  A733_SMHC_BLOCK_SIZE

//
// CSDC's whole field, swept as a last dimension only if the write sweep's two
// real dimensions are empty: its low bits are the CRC-status token value the
// controller detects after a write.  The vendor programs 6 for HS400 and 3 for
// every other mode and 6 is what this port enters with, unverified on this board.
//
#define A733_SMHC_HS400_CSDC_COUNT  8U

// The share of the 8-bit double-edge ceiling for the reported clock that an HS400
// measurement has to reach to be called healthy in the log; below it the bus is
// reported as below expectation, and the step stays taken because the read-back
// and the tap are what decide the card's state.
#define A733_SMHC_HS400_HEALTHY_PERCENT  60U

//
// HS400's own HS_TIMING value, named here beside the HS200 one for the same
// reason: the driver's files are where these switches are made.  BUS_WIDTH stays
// at the DDR width (6), so HS400's card side is timing only.
//
#define A733_SMHC_HS_TIMING_HS400  A733_SMHC_HS_TIMING_HS400_VALUE

// fOD: the eMMC spec's limit for the identification phase, which runs before
// the card has an address and answers with its slowest output timing.  The
// controller divider that reaches it is computed from the module clock.
#define A733_SMHC_IDENT_KHZ  400U

// Nominal module rates of the plans, used when the library cannot name the rate
// behind a plan's parent (it reports 0 for that, never a guess) - the plan's own
// arithmetic then has to stand in, and the log says which one was used.
#define A733_SMHC_PLAN_HOSC_HZ        24000000U
#define A733_SMHC_PLAN_PERI0_HZ       200000000U
#define A733_SMHC_PLAN_PERI0_DDR_HZ   200000000U
#define A733_SMHC_PLAN_PERI0_HS200_HZ 800000000U

//
// The SD profile's numbers, for the same reason: its fast parent is PERI0_400M,
// so its SDR/DDR plans name the 200 MHz module clock that parent divided by 2
// gives, and the two high-rate rows - which an SD card never enters, they exist
// so the plan indices stay the same size - name the 400 MHz the parent reaches
// undivided.
//
#define A733_SMHC_PLAN_PERI0_SD_HZ      200000000U
#define A733_SMHC_PLAN_PERI0_SD_FAST_HZ 400000000U

//
// HS200's own HS_TIMING value.  It lives here rather than with the other two in
// A733Smhc.h only because this pass was limited to the driver's files; it belongs
// beside A733_SMHC_HS_TIMING_HIGH_SPEED and _DDR52 when that header is next open.
// HS200 keeps the single-data-rate 8-bit bus width, so EXT_CSD[183] stays 2.
//
#define A733_SMHC_HS_TIMING_HS200  A733_SMHC_HS_TIMING_HS200_VALUE

// The share of the 8-bit ceiling for the reported clock that an HS200 measurement
// has to reach to be called healthy in the log.  Below it the bus is reported as
// below expectation - the step stays taken, because the read-back and the tap are
// what decide the card's state, and a slow-but-working bus is still a working one.
#define A733_SMHC_HS200_HEALTHY_PERCENT  60U

//
// The host's receive sample delay: SMHC_SAMP_DL (0x144) bits 5:0 are the delay
// in taps, bit 7 enables the software value (without it the controller uses its
// own), and bits 13:8 are read-only reserved - the register's own default is
// 0x0000_2000, so boot0's 0x20A0 is "enabled, 32 taps" and not 160.  (The
// register offsets themselves are in A733Smhc.h; these field definitions belong
// there too and can move when that header is next open.)
//
#define A733_SMHC_SAMP_DL_SW_EN      BIT7
#define A733_SMHC_SAMP_DL_SW_MASK    0x3FU
// The delay field is 6 bits wide, so the taps run 0..63.
#define A733_SMHC_SAMP_DELAY_TAPS    64U

// How far apart the taps the sample-delay sweep tries are.  Step 2 over the
// field's 64 taps is 32 attempts: coarse enough that a sweep costs milliseconds,
// fine enough that the centre of the passing window is a real tap and not an
// artefact of the grid.
#define A733_SMHC_SAMP_DELAY_STEP  2U

// The budget for one sweep attempt's block read.  A wrong tap normally fails on
// the data CRC or end-bit error the controller latches, which ends the wait at
// once; this only bounds the case where it latches nothing, so that 32 attempts
// cannot spend 32 full data timeouts.  A block needs about 100 us of wire time
// at 50 MHz DDR plus the card's access time, so 50 ms never cuts a good tap
// short.
// The sweep probes with a multi-block transfer: a single block passes at
// almost any tap and calibrates the wrong thing.
#define A733_SMHC_SAMP_DELAY_PROBE_BYTES  (8U * 512U)

#define A733_SMHC_SAMP_DELAY_ATTEMPT_US  50000U

//
// The throughput measurement reads this many 512-byte blocks from LBA 0: 256
// KiB, which is one command at the controller's byte count and long enough that
// the per-command overhead is a small part of what it times.  It is measured
// twice per rate, at this size and at A733_SMHC_THROUGHPUT_SMALL_BLOCKS: the
// fixed cost a single transfer carries - this platform's debug output alone is
// about 40 ms of console time per command - is the same for both, so only the
// *difference* between the two times is the bus.  The gate judges that marginal
// rate; the absolute pair stays in the log for the record.
//
#define A733_SMHC_THROUGHPUT_BLOCKS        512U
#define A733_SMHC_THROUGHPUT_SMALL_BLOCKS  (A733_SMHC_THROUGHPUT_BLOCKS / 8U)

// A plan whose *marginal* SDR rate measures meaningfully above the 8-bit ceiling
// for its at-speed card clock is not running at the rate the plan claims, and the
// DDR step will not build on it; below this fraction of the same ceiling the
// plan's rate did not materialise on the bus at all.  The ceiling is a
// theoretical maximum, so the upper bound only has to tolerate the measurement's
// own rounding (a healthy bus measures below its ceiling, never above it), while
// the lower bound only has to catch a bus that is nowhere near the plan's rate.
// The band is *not* widened to make a plan pass: a plan that is genuinely wrong
// still fails.  See SmhcCardCheckSdrRate.
#define A733_SMHC_SDR_FLOOR_PERCENT  30U
#define A733_SMHC_SDR_CEIL_PERCENT   110U

//
// The identification phase is bounded by wall-clock time, not by the state of
// one OCR bit: the eMMC spec allows the device up to a second to finish its
// power-up, and while it is still busy it answers CMD1 with bit 31 set and
// refuses CMD2.  CMD0, CMD1 and CMD2 are therefore re-issued as one round until
// CMD2 answers or A733_SMHC_IDENT_BUDGET_US has passed, with
// A733_SMHC_IDENT_ROUND_DELAY_US between rounds, and CMD2 is attempted on every
// round whatever the OCR said.  (A round also spends A733_SMHC_CMD0_SETTLE_US
// waiting for the card to act on CMD0.)
//
// Each command of a round is given at most A733_SMHC_IDENT_CMD_TIMEOUT_US of the
// phase's budget, which is what makes the budget buy rounds rather than one
// command: a command that never completes would otherwise spend the whole second
// on its own and end the phase after a single round.  The bound is generous
// against the controller's own response timeout at fOD - 255 card clocks, about
// 640 us at 400 kHz - so it only ever bites on a controller that has stopped
// reporting, which is exactly when it should.  With the interrupt working a
// command ends in a fraction of it, so the round spacing dominates.
//
#define A733_SMHC_IDENT_BUDGET_US      1000000U
#define A733_SMHC_IDENT_CMD_TIMEOUT_US 5000U
#define A733_SMHC_IDENT_ROUNDS         100U
#define A733_SMHC_IDENT_ROUND_DELAY_US 10000U

//
// CMD0 puts the card in idle with an initialization sequence of 74 clocks, which
// is 185 us at 400 kHz; the settling below is the extra the card gets to process
// the reset before the next command is aimed at it.
//
// A failed command can also leave the card busy and the controller's command FSM
// parked mid-command (it reads back as FSM_STA = "CMD path wait NCC"), which is
// why every round of the identification loop after a failure starts from a
// freshly initialized controller, and why a round is entered from CMD0 rather
// than carried on from where the failure left off.
//
#define A733_SMHC_CMD0_SETTLE_US  5000U

/**
  One command, in the shape both the identification sequence and the pass-thru
  front end build and hand to the register layer.

  CommandType and ResponseType are the EFI_SD_MMC_* enumerations, so a caller's
  packet maps across without a translation table.  A command with Length == 0
  carries no data; otherwise Buffer/Length/Read describe the transfer, and
  BlockSize is the SMHC_BLKSIZ value (the whole transfer length for a
  single-block command, 512 for a multi-block one).
**/
typedef struct {
  UINT8      Index;
  UINT32     Argument;
  UINT32     CommandType;
  UINT32     ResponseType;
  VOID       *Buffer;
  UINT32     Length;
  UINT32     BlockSize;
  BOOLEAN    Read;
  BOOLEAN    AutoStop;
  BOOLEAN    SendInitSeq;
  BOOLEAN    PrgClk;
  BOOLEAN    Abort;
  //
  // DIAGNOSTIC ONLY - remove once the 136-bit response question is settled.
  // NoCrcCheck drops CHK_RESP_CRC from a long response; ForcePreOver adds
  // WAIT_PRE_OVER to a command that carries no data.
  //
  BOOLEAN    NoCrcCheck;
  BOOLEAN    NoLongResp;
  BOOLEAN    ForcePreOver;
  // RESP0..3 of the command's answer, as the controller lays them out: word 0
  // holds response bits 31:0 and word 3 bits 127:96, which is the order
  // EmmcDxe's EFI_SD_MMC_STATUS_BLOCK consumer expects.
  UINT32     Response[4];
} A733_SMHC_COMMAND;

/**
  Driver state, one instance for each controller the board declares.
**/
typedef struct {
  //
  // The controller this instance drives, as the board's table describes it: the
  // name every log line about it carries, its register window, the interrupt it
  // raises, the CCU module and clock register it hangs off, the pad function its
  // pins are muxed to, and what kind of card it carries.  Nothing here is a
  // build-time constant - a board with two controllers produces two of these,
  // differing in exactly these fields.
  //
  CONST CHAR8                    *Name;
  UINTN                          Base;
  UINT32                         Spi;
  UINT32                         CcuModule;
  UINT32                         CcuClkReg;
  UINT32                         PadFunction;
  UINT32                         MaxBusWidth;
  BOOLEAN                        Removable;

  //
  // What BuildDevicePath reports, and therefore which of MdeModulePkg's card
  // drivers claims this handle: A733SmhcCardEmmc answers MSG_EMMC_DP and
  // A733SmhcCardSd answers MSG_SD_DP.  It is per handle, never per driver, which
  // is why the driver publishes one handle per controller.
  //
  A733_SMHC_CARD_TYPE            CardType;

  //
  // Which of the driver's clock plan tables this controller's plans come from:
  // the CCU does not offer every SMHC module the same parents (SMHC0/SMHC1 stop
  // at PERI0_400M), so the tables are per family and this selects one.  It is
  // read by everything that indexes a table, which is why it is filled in before
  // the first SmhcSelectClockPlan call.
  //
  A733_SMHC_CLOCK_PROFILE        ClockProfile;

  EFI_SD_MMC_PASS_THRU_PROTOCOL  PassThru;

  // The card's relative address.  For an eMMC it is taken from CMD3's answer
  // (A733SmhcCard.c assigns it); for an SD card it is taken from the CMD7 the
  // block layer sends, because SdDxe assigns the address itself and the host
  // only learns it there.
  UINT32                         Rca;

  // What the card ended up with: 8, 4 or 1, and whether HS_TIMING is set.
  UINT32                         BusWidth;
  BOOLEAN                        HighSpeed;

  //
  // Whether an SD slot's one-time bus setup - ACMD6's width switch and CMD6's
  // timing switch - has run.  It cannot run during identification: ACMD6 is
  // addressed to the card's RCA, and the RCA is SdDxe's to assign, so the switch
  // waits for the first data command after SdDxe's CMD7 and is made once per
  // controller whatever its outcome (see SmhcSdPrepareDataCommand).
  //
  BOOLEAN                        SdSpeedDone;

  //
  // The clock plan the module is on, the module clock it produced, and the three
  // controller dividers computed from that: the identification one (data-less
  // commands), the bring-up one (the first data read and the CMD6 switches,
  // while the card is still in compatibility timing) and the at-speed one (the
  // throughput phase, once the card is in High Speed timing).
  // SmhcSelectClockPlan fills these in from what the CCU register reads back and
  // what the library reports for it; every card rate the driver uses afterwards
  // comes from them, and the divider a phase runs on is also what a controller
  // reset has to re-deliver.
  //
  UINT32                         ClockPlan;
  UINT32                         ModuleClockHz;
  UINT32                         IdentDivisor;
  UINT32                         BringUpDivisor;
  UINT32                         DataDivisor;

  //
  // Whether the controller's DDR mode bit is set, i.e. whether the card is in
  // one of EXT_CSD[183]'s DDR bus widths.  It is tracked separately from
  // BusWidth because it is not a width: the CTYPE field keeps the 8-bit
  // encoding and DDR changes the timing of the same lines.
  //
  BOOLEAN                        Ddr;

  //
  // Whether the controller is meant to be carrying the DDR52 *host-side* timing
  // registers (NTSR 1x, both DRV_DL output phases at 180 degrees, DS_DL 0x80).
  // It is the counterpart of Ddr and the two are set together - the only time
  // they differ is the DDR register control run in A733SmhcCard.c, which drives
  // those registers with the card still in SDR.  The recovery path re-applies
  // the bus through SmhcSetDdrMode, and a failed transfer inside a sweep is
  // recovered before the next tap is tried, so this is what keeps the register
  // set under test from being silently replaced mid-sweep.
  //
  BOOLEAN                        HostDdrTiming;

  //
  // The HS400 counterparts: HostHs400Timing says the controller is carrying the
  // HS400 host-side set (EDSD bit 31, CSDC 6, the sample FIFO in circuit and the
  // swept DS_DL tap), and StrobeDelay is the DS_DL tap last programmed - the
  // number a controller reset has to re-apply, because a soft reset takes EDSD,
  // CSDC, SFC and DS_DL with it and the sweep's next attempt would then be
  // measured through a different sampler than the one it chose its tap for.
  //
  BOOLEAN                        HostHs400Timing;
  UINT32                         StrobeDelay;

  // EXT_CSD[185] as the card last read back, and the controller's DDR bit as
  // SMHC_CTRL last read back: the DDR step's evidence and the recovery path's.
  UINT8                          HsTiming;
  BOOLEAN                        DdrModeBit;

  //
  // Whether the card ended up in HS200 timing, and the sample-delay tap the last
  // sweep chose.  The entry point's tail reports both, because they are the state
  // the FAT traffic runs in and the number a later boot is read against.
  //
  BOOLEAN                        Hs200;
  BOOLEAN                        Hs400;
  UINT32                         SampleDelay;

  //
  // Set while a burst of commands is expected to fail on purpose - the DDR
  // sample-delay sweep's per-tap reads - so the per-failure decoding stays off
  // the console and 32 attempts cost one line rather than 32 commands' worth.
  // It suppresses logging only; nothing about the command path changes.
  //
  BOOLEAN                        Quiet;

  // The SMHC_CLKDIV n field the card clock was last given.  Kept because a
  // controller reset can take the card clock with it, and the recovery path
  // after a failed transfer has to deliver the same clock again.
  UINT32                         ClockDivisor;

  // Whether a SET_BLOCK_COUNT (CMD23) armed the card with a block count for the
  // command about to be sent.  It decides whether the controller still has to
  // end a multi-block transfer itself with CMD12; see SmhcPassThru.
  BOOLEAN                        CountArmed;

  // CSD structure 3 means the card addresses data in 512-byte sectors, so a
  // data command's argument is a sector number.  This driver selects sector
  // mode in CMD1 and checks the CSD to confirm the device honours it; the
  // split path needs it to advance a command's argument.
  BOOLEAN                        SectorAddressing;

  //
  // The CID an SD card answered CMD2 with during bring-up.  CMD10 (SEND_CID)
  // is the same register addressed to the card's RCA, and this card sometimes
  // ignores it; keeping the copy CMD2 returned lets a discovery use the value
  // the card has already given the driver instead of failing over a redundant
  // read.
  //
  UINT32                         SdCid[4];
  BOOLEAN                        SdCidValid;

  //
  // The interrupt is what decides that a command finished: the handler in
  // A733SmhcHci.c signals CompletionEvent, and records in IntStatus the raw
  // SMHC_RINTSTS bits it has seen since the command was issued and in IdstStatus
  // the SMHC_IDST bits, so the command engine never has to read the controller
  // to find out whether it succeeded.  Both are cleared before each command.
  //
  // InterruptCount is how often the handler has run at all, which is the number
  // that says whether the platform is delivering the controller's interrupt:
  // every completion log line carries it, and a line that reports a completion
  // with a count that has not moved is a completion the driver had to poll.
  //
  EFI_EVENT                      CompletionEvent;
  UINT32                         IntStatus;
  UINT32                         IdstStatus;
  UINT32                         InterruptCount;

  //
  // Set when a data phase failed in a way an error recovery had to reset the
  // controller for.  The controller is not trusted again until it has been
  // re-initialized and a command has since completed; see SmhcSendCommand.
  //
  BOOLEAN                        DataFault;

  //
  // The IDMAC descriptor ring and the bounce buffer, one allocation below 4 GB
  // that a transfer reuses rather than allocates (A733_SMHC_DMA_STATE in
  // A733SmhcHci.c, which owns the type).
  //
  VOID                           *Dma;
} A733_SMHC_PRIVATE;

/**
  A microsecond budget, built from the platform's free-running counter.

  The counter is the generic timer on this platform (ArmArchTimerLib), which
  counts at 24 MHz; the budget is compared as elapsed ticks over the counter's
  frequency so no fixed loop count has to be guessed for a bus that runs at
  12 MHz today and something else tomorrow.  The identification loop gives one
  of these to the card's power-up, which is a wall-clock bound rather than a
  count of tries.
**/
typedef struct {
  UINT64    Start;
  UINT64    TicksPerMicro;
  UINT64    BudgetUs;
} A733_SMHC_DEADLINE;

//
// A733SmhcHci.c: registers, controller reset, clock, bus width, the interrupt
// that reports completion, and the command engine (command register, IDMAC
// descriptor ring, data movement, cache maintenance).
//
UINT32
SmhcRead (
  IN UINTN   Base,
  IN UINT32  Offset
  );

VOID
SmhcWrite (
  IN UINTN   Base,
  IN UINT32  Offset,
  IN UINT32  Value
  );

VOID
SmhcArmDeadline (
  OUT A733_SMHC_DEADLINE  *Deadline,
  IN  UINT64              TimeoutUs
  );

BOOLEAN
SmhcDeadlineExpired (
  IN A733_SMHC_DEADLINE  *Deadline
  );

UINT64
SmhcDeadlineRemainingUs (
  IN A733_SMHC_DEADLINE  *Deadline
  );

EFI_STATUS
SmhcInitController (
  IN UINTN  Base
  );

VOID
SmhcSetClock (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             Divisor
  );

VOID
SmhcSetBusWidth (
  IN UINTN   Base,
  IN UINT32  Width
  );

/**
  Set or clear the controller's DDR mode bit (SMHC_CTRL bit 10).

  The card and the controller have to agree about this, and the card side is a
  CMD6 write of EXT_CSD[183]'s DDR bus width with EXT_CSD[185] set to 2.  The
  bit changes how the controller drives and samples DATA only - the command and
  response line stays single data rate - which is what lets the driver switch
  the two sides in either order without a data command in between; see the DDR
  step in A733SmhcCard.c for the order it uses and why.

  The state is remembered in the driver: a controller reset writes SMHC_CTRL
  back to its initial value and takes the bit with it, so SmhcRecoverController
  has to put it back along with the bus width and the card clock.

  @param[in,out] Private  Driver state.
  @param[in]     Enable   TRUE to run the data bus double data rate.
**/
VOID
SmhcSetDdrMode (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     BOOLEAN            Enable
  );

/**
  Program the host's receive sample delay (SMHC_SAMP_DL).

  The register's bits 5:0 are the delay in taps and bit 7 enables the software
  value; its bits 13:8 are read-only, so the register is read-modify-written the
  way the vendor's driver for this family does and whatever they carry is left
  alone.  The delay belongs to the card *and* the board's routing, which is why
  the DDR step sweeps it rather than trusting a value.

  @param[in] Base   Controller base.
  @param[in] Delay  Delay in taps, A733_SMHC_SAMP_DELAY_TAPS wide.
**/
VOID
SmhcSetSampleDelay (
  IN UINTN   Base,
  IN UINT32  Delay
  );

/**
  Program the host's data-strobe delay (SMHC_DS_DL) for HS400.

  The register is laid out like SMHC_SAMP_DL - bits 5:0 the delay in taps, bit 7
  enables the software value - so it is read-modify-written the same way.  HS400
  is the only mode whose data is sampled against the card's strobe, and its tap is
  a property of the card and the board's routing, which is why the HS400 step
  sweeps it instead of trusting the vendor's initial value.

  @param[in] Base   Controller base.
  @param[in] Taps   Delay in taps, 0..63.
**/
VOID
SmhcSetStrobeDelay (
  IN UINTN   Base,
  IN UINT32  Taps
  );

/**
  Enter or leave the controller's HS400 mode.

  SMHC_EDSD bit 31 is the data path's HS400 mode, SMHC_CSDC carries the
  CRC-status token that mode expects (6, against 3 otherwise), and HS400 samples
  its data through the sample FIFO rather than around it, so SMHC_SFC bit 0 is
  cleared.  With Enable the swept strobe tap in Private->StrobeDelay and the
  command line's tap (the vendor's initial value) are programmed with the rest;
  without it only the two mode fields are put back, and the caller restores the
  delay registers themselves.

  The state is remembered in Private->HostHs400Timing so that the recovery after
  a failed transfer re-applies this set: a soft reset takes all four registers
  back to their defaults, and a sweep whose later taps ran without them would
  report an empty window for a bus whose tap was never tried.

  @param[in,out] Private  Driver state.
  @param[in]     Enable   TRUE to enter HS400 mode, FALSE to leave it.
**/
VOID
SmhcSetHs400Mode (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     BOOLEAN            Enable
  );

/**
  Turn the controller's sample-FIFO bypass on or off (SMHC_SFC bit 0).

  Set for the duration of a delay-line sweep and cleared for normal traffic, the
  way the vendor's driver for this IP does it: the sweep measures the bus without
  the sample FIFO in circuit, and the read after the bit is cleared is what says
  whether the tap it chose also works through it.

  @param[in] Base    Controller base.
  @param[in] Bypass  TRUE to bypass the sample FIFO, FALSE to use it.
**/
VOID
SmhcSetSampleFifoBypass (
  IN UINTN    Base,
  IN BOOLEAN  Bypass
  );

BOOLEAN
SmhcCardPresent (
  IN UINTN  Base
  );

BOOLEAN
SmhcCardWaitPresent (
  IN UINTN   Base,
  IN UINT64  TimeoutUs
  );

EFI_STATUS
SmhcPrepareController (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             Divisor
  );

UINT32
SmhcClockKhz (
  IN A733_SMHC_PRIVATE  *Private
  );

EFI_STATUS
SmhcWaitCardIdle (
  IN UINTN   Base,
  IN UINT64  TimeoutUs
  );

VOID
SmhcDumpState (
  IN A733_SMHC_PRIVATE  *Private
  );

/**
  Report the interrupt enable state the controller and the GIC hold, once the
  controller has been initialized: the controller's global enable and interrupt
  mask, the distributor's enable/pending/active/group/route words for SMHC2's
  SPI, and how often the handler has run so far.

  @param[in] Private  Driver state; the controller must have been initialized.
  @param[in] When     Where in the driver this is being asked.
**/
VOID
SmhcReportInterruptState (
  IN A733_SMHC_PRIVATE  *Private,
  IN CONST CHAR8        *When
  );

/**
  Register the controller's interrupt handler and enable the interrupt.

  The platform's ArmGicDxe produces both interrupt protocols; this driver
  prefers the second (which carries the trigger type) and falls back to the
  first, and reports the one it got.  Registration happens once per controller,
  before that controller's pass-thru protocol is installed, because from the
  first command on the interrupt is what reports completion.

  The protocol passes no context to a handler, so each registration is remembered
  in the driver's own table, keyed by the source the handler is called with: the
  handler looks its controller up by Source and returns immediately for a source
  it did not register.  A source already in the table is refused, and so is a
  table that is full (A733_SMHC_MAX_CONTROLLERS).

  @param[in] Private  Driver state; its completion event must already exist.

  @retval EFI_SUCCESS            The handler is registered and the interrupt is
                                 enabled.
  @retval EFI_ALREADY_STARTED    The source this controller would register is
                                 already registered for another controller.
  @retval EFI_OUT_OF_RESOURCES   A733_SMHC_MAX_CONTROLLERS registrations exist.
  @retval other                  No interrupt protocol, or the platform refused
                                 the source.
**/
EFI_STATUS
SmhcInstallInterrupt (
  IN A733_SMHC_PRIVATE  *Private
  );

/**
  Unregister a controller's interrupt handler and disable the interrupt.

  Called on every failure path of the entry point, so a driver that does not
  publish a protocol does not leave a handler behind for an interrupt nothing
  will ever clear.  The registration is found by the controller it was made for,
  so tearing one controller down leaves the others' handlers alone.

  @param[in] Private  Driver state whose registration is to be removed.
**/
VOID
SmhcUninstallInterrupt (
  IN A733_SMHC_PRIVATE  *Private
  );

/**
  Allocate the descriptor ring and the bounce buffer of Private->Dma.

  @param[in] Private  Driver state.

  @retval EFI_SUCCESS           The pool is below 4 GB and DWORD aligned.
  @retval EFI_OUT_OF_RESOURCES  No room below 4 GB, or no room for the state.
**/
EFI_STATUS
SmhcDmaAllocatePool (
  IN A733_SMHC_PRIVATE  *Private
  );

VOID
SmhcDmaFreePool (
  IN A733_SMHC_PRIVATE  *Private
  );

EFI_STATUS
SmhcSendCommand (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN OUT A733_SMHC_COMMAND  *Command,
  IN     UINT64             TimeoutUs
  );

//
// A733SmhcDxe.c: the clock plan the module runs on, the throughput measurement
// every step of the speed-up is read from, and the calibration line that reports
// it with the CCU register behind it.
//
/**
  Apply a clock plan to the CCU and record what the hardware and the library
  actually say about it.

  The plan names a parent and a module divider (see mA733SmhcMmcClockPlans and
  mA733SmhcSdClockPlans, the table being the one its controller's profile selects);
  this writes them, reads the CLK_REG back, and works out the two controller
  dividers the plan needs from the module clock:

  - the identification divider, computed so the card clock lands at or below fOD
    (400 kHz) rather than written as a literal, since the module clock is what a
    plan is allowed to change;
  - the data divider, which the plan states (0 = the full module clock).

  The module clock is taken from A733CcuGetModuleClockHz, which decodes the
  register that was just written; when that returns 0 - the library declines to
  guess a rate it cannot verify - the plan's nominal rate is used instead and the
  log says so.  Both the raw register and the reported rate are printed, which is
  what lets a measurement be checked against the number the CCU claims.

  @param[in,out] Private  Driver state; the clock fields are filled in.
  @param[in]     Plan     A plan index, A733_SMHC_PLAN_*.

  @retval EFI_SUCCESS  The plan is applied and the dividers are computed.
  @retval other        The CCU refused the plan; the driver state is unchanged.
**/
EFI_STATUS
SmhcSelectClockPlan (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             Plan
  );

/**
  Name a clock plan, for the log.

  The names differ between the profiles (the MMC table's fast plan is
  PERI0_800M /4, the SD table's is PERI0_400M /2), so the profile is part of the
  question and not decoration: a caller naming a plan for a log line has to say
  which controller's table it is reading.

  @param[in] Profile  The controller's clock profile.
  @param[in] Plan     A733_SMHC_PLAN_*.

  @return The plan's name, or "-" when Plan is out of range.
**/
CONST CHAR8 *
SmhcClockPlanName (
  IN A733_SMHC_CLOCK_PROFILE  Profile,
  IN UINT32                   Plan
  );

/**
  Read a number of blocks from LBA 0 and time it.

  The read goes through the driver's own command path - CMD23 to bound it, then
  CMD18, the same two commands EmmcDxe sends - into a page-aligned buffer below
  4 GB, so the descriptors describe it directly and the number is the bus's rate
  rather than one result of a bounce through the DMA pool.  The timer is the
  platform counter (TimerLib), which is the same clock the driver's deadlines
  use, so the elapsed figure and every timeout in the driver are the same
  measurement.

  @param[in]  Private     Driver state; the card must be selected and ready.
  @param[in]  Blocks      512-byte blocks to read, at most the descriptor ring's
                          own transfer limit.
  @param[out] ElapsedUs   Receives the elapsed microseconds.
  @param[out] KibPerSec   Receives the measured rate in KiB/s.

  @retval EFI_SUCCESS           The read completed; both outputs are set.
  @retval EFI_OUT_OF_RESOURCES  No buffer below 4 GB was available.
  @retval other                 The read failed; the outputs are 0.
**/
EFI_STATUS
SmhcMeasureThroughput (
  IN  A733_SMHC_PRIVATE  *Private,
  IN  UINT32             Blocks,
  OUT UINT32             *ElapsedUs,
  OUT UINT32             *KibPerSec
  );

/**
  A rate measurement: one transfer of each of the two sizes, and the rate their
  difference gives.

  A single transfer carries a fixed cost that has nothing to do with the bus -
  on this platform the driver's own per-command debug output is about 40 ms of
  115200-baud console time - so the rate that describes the bus is the
  *marginal* one: the bytes the larger transfer adds over the smaller, divided by
  the time it adds.  Both absolute figures are kept for the record.
**/
typedef struct {
  UINT32     SmallKib;             // The small read's size in KiB.
  UINT32     SmallUs;
  UINT32     SmallKibPerSec;
  UINT32     BigKib;               // The large read's size in KiB.
  UINT32     BigUs;
  UINT32     BigKibPerSec;
  UINT32     MarginalKibPerSec;    // The bus's rate, 0 when it cannot be formed.
  BOOLEAN    MarginalValid;        // TRUE when the two times can be told apart.
} A733_SMHC_RATE;

/**
  Log one throughput measurement, in the form every step is reported in:

    Smhc: throughput 256 KiB in 21000 us = 12190 KiB/s at 12000 kHz 8-bit SDR

  The rate the line states is the card clock the driver believes it is running -
  SMHC_CLKDIV read back against the module clock the plan selected - and the mode
  is what the card is in: SDR, or DDR once the DDR step has taken and been read
  back.  Separating the measurement from the line lets a caller act on the number
  (the DDR step is gated on it) while still reporting it.

  @param[in] Private     Driver state; the card must be selected and ready.
  @param[in] Kib         The size the line states, in KiB.
  @param[in] ElapsedUs   Elapsed microseconds of the measurement.
  @param[in] KibPerSec   The measured rate.
**/
VOID
SmhcLogThroughput (
  IN A733_SMHC_PRIVATE  *Private,
  IN UINT32             Kib,
  IN UINT32             ElapsedUs,
  IN UINT32             KibPerSec
  );

/**
  Measure the bus at both sizes, log the three rates, and return them.

  The two absolute lines are the record; the marginal one is what the DDR gate
  judges, because it is the only one of the three that describes the bus rather
  than the bus plus the driver's own per-transfer cost.

  @param[in]  Private  Driver state; the card must be selected and ready.
  @param[out] Rate     Receives both measurements and the marginal rate.

  @retval EFI_SUCCESS  Both reads completed and Rate is filled in.
  @retval other        The first read that failed; Rate's times are then 0.
**/
EFI_STATUS
SmhcMeasureRate (
  IN  A733_SMHC_PRIVATE  *Private,
  OUT A733_SMHC_RATE     *Rate
  );

/**
  Measure the bus at both sizes and log the three lines.

  The convenience wrapper around SmhcMeasureRate for the callers that only want
  the report (the entry point's tail).

  @param[in] Private  Driver state; the card must be selected and ready.

  @retval EFI_SUCCESS  Both reads completed and the lines were logged.
  @retval other        A read failed; the failure was logged instead.
**/
EFI_STATUS
SmhcReportThroughput (
  IN A733_SMHC_PRIVATE  *Private
  );

/**
  Print the phase trace of the last two commands, in microseconds, one line each.

  Instrumentation, not part of the driver's behaviour: it is called after a
  measurement has stopped its own timer, so the console time the printing costs
  never lands inside a measured window.  The phases are the steps SmhcSendCommand
  goes through, and the last phase is the one the driver's own per-command debug
  output lands in - which is where a fixed per-transfer cost shows up.
**/
VOID
SmhcPhaseReport (
  VOID
  );

//
// A733SmhcCard.c: the card's side - identification and whatever a device reset
// has to put back.  A733SmhcSd.c: the same question for a card slot, plus the
// bus the block driver above it never asks for.
//
EFI_STATUS
SmhcCardInit (
  IN OUT A733_SMHC_PRIVATE  *Private
  );

/**
  Identify the card in a controller whose board entry says it is an SD slot.

  A733SmhcCard.c identifies an eMMC and nothing else, so an SD controller must not
  be handed to it: the eMMC path would send CMD1/CMD2 to a card that answers
  CMD8/CMD55/ACMD41 at best, and every step of the eMMC ladder - the CMD6
  switches, the DDR and HS200 attempts - is meaningless on a slot whose card may
  not even be there.

  This is the SD physical layer's own sequence (CMD0, CMD8, CMD55/ACMD41, CMD58,
  CMD2) and it stops at CMD2, which leaves the card in ident: the block driver
  above - MdeModulePkg's SdDxe - assigns the card's relative address itself with
  CMD3, so a host that had also sent CMD3 would be addressing a card the block
  driver never learns.  The width and the timing are set later for the same
  reason, in front of the first data command (SmhcSdPrepareDataCommand).

  The entry point treats any failure here like any other controller with no
  usable card: it releases what it took, publishes no handle for it, and carries
  on - so an empty slot costs boot time and nothing else.

  @param[in,out] Private  Driver state; Rca is cleared and the bus-width/timing
                          state is set to what identification leaves, with the
                          switch owed to the first data command.

  @retval EFI_SUCCESS   The card is identified and left in the ident state.
  @retval EFI_NO_MEDIA  No card, no answer, or a card this board cannot supply
                        the voltage for.
  @retval other         The CCU refused the SD profile's plan, or the controller
                        did not come out of reset.
**/
EFI_STATUS
SmhcCardInitSd (
  IN OUT A733_SMHC_PRIVATE  *Private
  );

/**
  Switch an SD card and its controller to the widest, fastest bus a slot can
  carry, once.

  SdDxe sends no CMD55, ACMD6 or CMD6, so without this the card would run on one
  data line in compatibility timing for the whole boot.  ACMD6 is addressed to
  the card's RCA, which only exists after SdDxe's own CMD3, so the switch cannot
  be part of identification; the command path calls this in front of every data
  command and the driver's latch makes the switch happen exactly once.

  Every step is best-effort: a card that refuses a step keeps the bus it had, the
  data command that triggered the switch is still sent, and one line reports the
  clock, the width and whether CMD6 answered.

  @param[in,out] Private  Driver state; Rca must hold the address the block
                          driver assigned, and BusWidth, HighSpeed and
                          SdSpeedDone are filled in.
**/
VOID
SmhcSdPrepareDataCommand (
  IN OUT A733_SMHC_PRIVATE  *Private
  );

EFI_STATUS
SmhcCardReset (
  IN OUT A733_SMHC_PRIVATE  *Private
  );

/**
  Put the card in an SD slot back to the state the block driver left it in.

  A device reset takes the card's address, width and timing with it.  This runs
  the slot's identification again, asks for the same relative address the block
  driver addresses the card by, selects the card, and re-applies the data bus -
  which is the state the caller of ResetDevice carries on from.  An SD card has
  no EXT_CSD, so nothing here reads one.

  @param[in,out] Private  Driver state; Rca, BusWidth and HighSpeed are rebuilt.

  @retval EFI_SUCCESS   The card is identified, addressed, selected and back on
                        its data bus.
  @retval EFI_NO_MEDIA  No card in the slot, or it did not identify.
  @retval other         A command of the recovery failed.
**/
EFI_STATUS
SmhcCardResetSd (
  IN OUT A733_SMHC_PRIVATE  *Private
  );

#endif