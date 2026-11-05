/** @file
  SMHC register access and command engine for the Allwinner A733 (sun60iw2p1).

  This is the part of the driver that speaks to the controller: reset, clock,
  bus width, issuing a command through SMHC_CMD, and moving a data command's
  bytes through the controller's internal DMA controller.  Everything above it
  (identification, the pass-thru protocol) works in commands and buffers only.

  Three design points worth knowing before reading the code:

  - A data command's bytes move on the IDMAC, driven by the four-word
    descriptors the manual lays out in 11.3.5.4, from a pool this file allocates
    below 4 GB once and reuses for every transfer.  The CPU neither touches
    SMHC_FIFO nor copies the caller's data except when a bounce is forced - see
    SmhcTransferData for when and why one is.

  - Completion is delivered by the controller's interrupt (the SPI its board
    table entry names: 195 on SMHC2, 196 on SMHC3), not read
    out of SMHC_RINTSTS by the CPU: the handler below records the raw status
    bits and signals an event, and SmhcSendCommand decides a command's outcome
    from that record alone.  The deadline it waits against is only a safety net
    for a lost interrupt, never a second opinion on the result.

  - The platform runs with caches on, so the IDMAC's view of memory has to be
    made to agree with the CPU's: a write is cleaned before the transfer (the
    DMA reads memory) and a read is invalidated after it (the DMA has written
    memory).  Which operation each direction needs is stated where it happens.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Protocol/HardwareInterrupt.h>
#include <Protocol/HardwareInterrupt2.h>

#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/A733CcuLib.h>
#include <A733Gpio.h>

#include "A733SmhcDxe.h"

//
// A poll that finds nothing waits this long before looking again.  The
// shortest thing this driver waits for is a command's answer (tens of
// microseconds), so a longer step would coarsen every timeout in the driver
// without saving anything measurable.  Since the interrupt is what reports
// completion, this step only sets how often the command engine re-reads the
// handler's record while it waits.
//
#define A733_SMHC_POLL_STEP_US  1U

// The clock-change pseudo-command the manual gives as an example (11.3.6.1
// step 3): START | PRG_CLK | WAIT_PRE_OVER.
#define A733_SMHC_CMD_PRG_CLK_VALUE \
  (A733_SMHC_CMD_START | A733_SMHC_CMD_PRG_CLK | A733_SMHC_CMD_WAIT_PRE_OVER)

//
// ===================== TRANSFER PHASE TRACE (INSTRUMENTATION) ===============
//
// Nothing in this block decides anything: it times the steps SmhcSendCommand
// goes through so a fixed cost per transfer can be told from a per-byte one,
// and SmhcPhaseReport (declared in A733SmhcDxe.h) prints the last two commands'
// phases in microseconds.  To remove the instrumentation: delete this block, the
// phase calls at the marked points in SmhcSendCommand, and SmhcPhaseReport's
// declaration and its call in SmhcMeasureRate.
//
// The marks are recorded on every command whatever the build asks for, because
// they are what keeps the command path's own timing identical to a build that
// prints them; SmhcPhasePrint is what PcdA733SmhcDumpEnable silences.
//
// The phases are the *intervals* between the marks, and they are named after
// what runs in them.  "tail" is where the per-command dump and the GIC readback
// print: on a 115200-baud console those ~460 characters are about 40 ms, which
// is by far the largest fixed cost a single transfer carries and the reason the
// absolute rate of a small transfer is not the bus's rate.
//
typedef enum {
  A733_SMHC_PHASE_START = 0,   // Entry to SmhcSendCommand.
  A733_SMHC_PHASE_FSM,         // Fault recovery (if any), then the FSM idle wait.
  A733_SMHC_PHASE_HANDOFF,     // Status clear and event drain at TPL_HIGH_LEVEL.
  A733_SMHC_PHASE_PREP,        // FIFO reset, BLKSIZ/BYTCNT, descriptor ring, cache clean, DMA/IDMAC reset.
  A733_SMHC_PHASE_ISSUE,       // CMDARG and CMD writes.
  A733_SMHC_PHASE_WAIT,        // The transfer itself, until the interrupt reports it.
  A733_SMHC_PHASE_COPYOUT,     // Cache invalidate, bounce copy.
  A733_SMHC_PHASE_TAIL,        // R1b busy wait, per-command dump, GIC readback.
  A733_SMHC_PHASE_END,         // Epilogue: recovery bookkeeping and the return.
  A733_SMHC_PHASE_MAX
} A733_SMHC_PHASE;

typedef struct {
  BOOLEAN    Used;
  UINT8      Index;
  UINT32     Length;
  UINT32     Mark[A733_SMHC_PHASE_MAX];
} A733_SMHC_PHASE_TRACE;

// The last two commands, which for a measurement are its CMD23 and its CMD18.
#define A733_SMHC_PHASE_SLOTS  2
STATIC A733_SMHC_PHASE_TRACE  mSmhcPhaseTrace[A733_SMHC_PHASE_SLOTS];
STATIC UINT32                 mSmhcPhaseSlot;
STATIC A733_SMHC_PHASE_TRACE  *mSmhcPhaseCurrent;

STATIC CONST CHAR8  *mSmhcPhaseName[A733_SMHC_PHASE_MAX] = {
  "", "fsm", "handoff", "prep", "issue", "bus", "copyout", "tail", "epi"
};

/**
  Start a trace for the command about to be sent.

  @param[in] Index   Command index, for the printed line.
  @param[in] Length  Bytes the command carries, 0 for a command without data.
**/
STATIC
VOID
SmhcPhaseBegin (
  IN UINT8   Index,
  IN UINT32  Length
  )
{
  mSmhcPhaseSlot    = (mSmhcPhaseSlot + 1U) % A733_SMHC_PHASE_SLOTS;
  mSmhcPhaseCurrent = &mSmhcPhaseTrace[mSmhcPhaseSlot];

  ZeroMem (mSmhcPhaseCurrent, sizeof (*mSmhcPhaseCurrent));
  mSmhcPhaseCurrent->Used   = TRUE;
  mSmhcPhaseCurrent->Index  = Index;
  mSmhcPhaseCurrent->Length = Length;
  mSmhcPhaseCurrent->Mark[A733_SMHC_PHASE_START] = (UINT32)GetPerformanceCounter ();
}

/**
  Record the end of one phase.

  @param[in] Phase  The phase that has just finished.
**/
STATIC
VOID
SmhcPhaseMark (
  IN A733_SMHC_PHASE  Phase
  )
{
  if (mSmhcPhaseCurrent == NULL) {
    return;
  }

  mSmhcPhaseCurrent->Mark[Phase] = (UINT32)GetPerformanceCounter ();
}

/**
  Print one trace, if it was used at all.

  @param[in] Trace  Trace to print.
**/
STATIC
VOID
SmhcPhasePrint (
  IN A733_SMHC_PHASE_TRACE  *Trace
  )
{
  UINT64   Frequency;
  UINT32   TicksPerMicro;
  UINT32   Phase;
  UINT32   Last;
  UINT32   Us;

  if (!Trace->Used) {
    return;
  }

  if (!PcdGetBool (PcdA733SmhcDumpEnable)) {
    //
    // Reporting only, so it is the switch's business.  The trace is consumed the
    // same way the print path consumes it - once - so nothing about the next
    // command's trace depends on whether this one was printed.
    //
    Trace->Used = FALSE;
    return;
  }

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);

  //
  // A phase whose end was never marked (an early return took the command out of
  // the sequence) is skipped, and the last marked phase ends the trace, so an
  // abandoned command still prints the phases it did reach.
  //
  Last = A733_SMHC_PHASE_START;
  for (Phase = A733_SMHC_PHASE_FSM; Phase < A733_SMHC_PHASE_MAX; Phase++) {
    if (Trace->Mark[Phase] != 0) {
      Last = Phase;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "SmhcDiag: cmd %u (%u bytes) phase us:",
    Trace->Index,
    Trace->Length
    ));

  for (Phase = A733_SMHC_PHASE_FSM; Phase <= Last; Phase++) {
    if (Trace->Mark[Phase] == 0) {
      continue;
    }

    Us = (Trace->Mark[Phase] - Trace->Mark[Phase - 1]) / TicksPerMicro;
    DEBUG ((DEBUG_ERROR, " %a %u,", mSmhcPhaseName[Phase], Us));
  }

  DEBUG ((
    DEBUG_ERROR,
    " total %u\n",
    (Trace->Mark[Last] - Trace->Mark[A733_SMHC_PHASE_START]) / TicksPerMicro
    ));

  //
  // Consumed: a trace is printed once, so a measurement that fails before its
  // report cannot make the next one print the same pair again.
  //
  Trace->Used = FALSE;
}
//
// =================== END OF TRANSFER PHASE TRACE ============================
//

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
  )
{
  UINT32  Slot;

  for (Slot = 0; Slot < A733_SMHC_PHASE_SLOTS; Slot++) {
    SmhcPhasePrint (&mSmhcPhaseTrace[Slot]);
  }
}

//
// The descriptor ring plus the bounce buffer, one page allocation below 4 GB.
// The type is this file's business: the rest of the driver only passes the
// pointer around (A733_SMHC_PRIVATE.Dma).
//
typedef struct {
  EFI_PHYSICAL_ADDRESS    Base;         // The page allocation itself.
  UINTN                   Pages;
  A733_SMHC_DESCRIPTOR    *Descriptors; // A733_SMHC_DMA_DESC_COUNT entries.
  UINT8                   *Bounce;      // A733_SMHC_MAX_TRANSFER_BYTES, cache-line aligned.
} A733_SMHC_DMA_STATE;

//
// GICv3 distributor register offsets, used only by the readback that reports
// where the controller's interrupt is going.  ArmGicLib.h names the same words (ICDDCR,
// ICDISR, ICDISER, ICDSPR, ICDABR, ICDICFR); they are GIC architecture, not
// controller registers, so they stay here rather than in A733Smhc.h.  IROUTER
// is per-interrupt and 64 bits wide, one entry per SPI above the SGI/PPI range.
//
#define A733_GICD_CTLR       0x000U
#define A733_GICD_IGROUPR    0x080U
#define A733_GICD_ISENABLER  0x100U
#define A733_GICD_ISPENDR    0x200U
#define A733_GICD_ISACTIVER  0x300U
#define A733_GICD_ICFGR      0xC00U
#define A733_GICD_IROUTER    0x6000U

//
// What the controller's interrupt looks like in those registers: the words themselves,
// the single bit that is the SPI's, and the trigger type those words encode.
//
typedef struct {
  UINT32     Control;   // GICD_CTLR
  UINT32     Group;     // GICD_IGROUPR,  0 = group 0, 1 = group 1
  UINT32     Enable;    // GICD_ISENABLER
  UINT32     Pending;   // GICD_ISPENDR
  UINT32     Active;    // GICD_ISACTIVER
  UINT32     Config;    // GICD_ICFGR
  UINT64     Route;     // GICD_IROUTER for this SPI
  UINT32     Mask;      // The SPI's bit within the first four
  UINT32     Bits;      // The SPI's two-bit ICFGR field
  BOOLEAN    Level;     // TRUE when those bits say level (not edge)
} A733_SMHC_GIC_STATE;

//
// The three relationships the descriptor ring and the FIFO's burst size have to
// keep with each other, checked where they are written rather than where they
// are used: a chunk must fit A733_SMHC_DES_SIZE_LIMIT, the narrowest
// buffer-size field DES1 has anywhere in this controller family, it must be a
// whole number of the 16-DWORD burst SMHC_FIFOTH asks the controller for, and
// the largest transfer must be a whole number of chunks - which is what makes
// A733_SMHC_DMA_DESC_COUNT the ring's exact size.
//
STATIC_ASSERT (A733_SMHC_DMA_CHUNK_BYTES <= A733_SMHC_DES_SIZE_LIMIT, "A descriptor's buffer-size field cannot hold a whole chunk");
STATIC_ASSERT ((A733_SMHC_DMA_CHUNK_BYTES % A733_SMHC_FIFOTH_BURST_BYTES) == 0, "A chunk is not a whole number of the FIFO's bursts");
STATIC_ASSERT ((A733_SMHC_MAX_TRANSFER_BYTES % A733_SMHC_DMA_CHUNK_BYTES) == 0, "The ring would need a partial descriptor for the largest transfer");

//
// The handler the platform can call has no context argument, so each registered
// controller is remembered here and found again by the source the handler is
// called with.  One entry per controller the board declares, bounded by
// A733_SMHC_MAX_CONTROLLERS: the sources are the controllers' SPIs, which are
// distinct for distinct controllers, so the lookup is unambiguous.
//
// An entry is filled in before its handler is registered and cleared after it is
// removed, and Installed is what makes a half-built entry invisible to the
// handler: a slot that is being registered or torn down must never match a
// source.
//
typedef struct {
  HARDWARE_INTERRUPT_SOURCE          Source;
  A733_SMHC_PRIVATE                  *Private;

  //
  // The protocol the interrupt was registered through.  Only one of the two is
  // non-NULL: the platform's ArmGicDxe produces both, and the driver prefers
  // HardwareInterrupt2 for its trigger-type call.  Each registration carries its
  // own pointer because the handler ends the interrupt through it, and a handler
  // must not use another controller's protocol to end its own source.
  //
  EFI_HARDWARE_INTERRUPT_PROTOCOL    *Interrupt;
  EFI_HARDWARE_INTERRUPT2_PROTOCOL   *Interrupt2;
  BOOLEAN                            Installed;
} A733_SMHC_INTERRUPT_REGISTRATION;

STATIC A733_SMHC_INTERRUPT_REGISTRATION  mSmhcInterrupts[A733_SMHC_MAX_CONTROLLERS];

/**
  The registration a handler call belongs to.

  The platform passes the source it was registered for and nothing else, so this
  is how one handler serves several controllers.  It is a linear scan of at most
  A733_SMHC_MAX_CONTROLLERS entries: the table is tiny and fixed-size, and a scan
  has no state to get out of step with the registrations themselves.

  A source that is not in the table is not this driver's to handle - it belongs to
  another driver's registration, or to a slot that has already been torn down -
  and the caller returns without touching any controller.  That is safe in exactly
  the way reading the wrong instance would not be: nothing is read, cleared or
  signalled, so the interrupt is left to whoever does own it, and a stale source
  can never be answered with another controller's status.

  @param[in] Source  The source the handler was called with.

  @return Its registration, or NULL when the driver has none for that source.
**/
STATIC
A733_SMHC_INTERRUPT_REGISTRATION *
SmhcFindInterruptRegistration (
  IN HARDWARE_INTERRUPT_SOURCE  Source
  )
{
  UINTN  Index;

  for (Index = 0; Index < A733_SMHC_MAX_CONTROLLERS; Index++) {
    if (mSmhcInterrupts[Index].Installed && (mSmhcInterrupts[Index].Source == Source)) {
      return &mSmhcInterrupts[Index];
    }
  }

  return NULL;
}

// Defined below; SmhcDumpState names the failing bits of RINTSTS through it.
STATIC
VOID
SmhcDumpInterruptBits (
  IN UINT32  IntStatus
  );

// Defined below; the same for the IDMAC's own status word, and the recovery the
// data path runs when a transfer fails.
STATIC
VOID
SmhcDumpIdmaBits (
  IN UINT32  IdmaStatus
  );

STATIC
VOID
SmhcResetDataPath (
  IN UINTN  Base
  );

/**
  Read a controller register.

  @param[in] Base    Controller base, e.g. A733_SMHC_BASE.
  @param[in] Offset  Register offset, A733_SMHC_*.

  @return The register's value.
**/
UINT32
SmhcRead (
  IN UINTN   Base,
  IN UINT32  Offset
  )
{
  return MmioRead32 (Base + Offset);
}

/**
  Write a controller register.

  @param[in] Base    Controller base, e.g. A733_SMHC_BASE.
  @param[in] Offset  Register offset, A733_SMHC_*.
  @param[in] Value   Value to write.
**/
VOID
SmhcWrite (
  IN UINTN   Base,
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (Base + Offset, Value);
}

/**
  Start a microsecond budget.

  @param[out] Deadline   Budget to arm.
  @param[in]  TimeoutUs  Length of the budget in microseconds.  A zero budget
                         still allows one pass, so a caller that asks for
                         nothing gets a look at the hardware rather than a
                         timeout before the first read.
**/
VOID
SmhcArmDeadline (
  OUT A733_SMHC_DEADLINE  *Deadline,
  IN  UINT64              TimeoutUs
  )
{
  UINT64  Frequency;

  Frequency = GetPerformanceCounterProperties (NULL, NULL);

  Deadline->Start         = GetPerformanceCounter ();
  Deadline->TicksPerMicro = (Frequency == 0) ? 1 : Frequency / 1000000U;
  Deadline->BudgetUs      = TimeoutUs;
}

/**
  Report whether a budget has run out.

  @param[in] Deadline  Budget to check.

  @retval TRUE   The budget is spent.
  @retval FALSE  There is time left.
**/
BOOLEAN
SmhcDeadlineExpired (
  IN A733_SMHC_DEADLINE  *Deadline
  )
{
  UINT64  Elapsed;

  if (Deadline->BudgetUs == 0) {
    return TRUE;
  }

  Elapsed = GetPerformanceCounter () - Deadline->Start;

  return (Elapsed / Deadline->TicksPerMicro) >= Deadline->BudgetUs;
}

/**
  How much of a budget is left, in microseconds.

  The identification loop hands a slice of its budget to each command it sends,
  so that a single slow command cannot outlive the loop that bounds it.  A
  spent budget reports 0, which every caller of SmhcSendCommand reads as
  "nothing left to wait for".

  @param[in] Deadline  Budget to measure.

  @return Microseconds remaining, 0 once the budget is spent.
**/
UINT64
SmhcDeadlineRemainingUs (
  IN A733_SMHC_DEADLINE  *Deadline
  )
{
  UINT64  Elapsed;

  if (Deadline->BudgetUs == 0) {
    return 0;
  }

  Elapsed = (GetPerformanceCounter () - Deadline->Start) / Deadline->TicksPerMicro;

  return (Elapsed >= Deadline->BudgetUs) ? 0 : (Deadline->BudgetUs - Elapsed);
}

/**
  Wait until every bit of a mask is clear in a register, or the budget runs out.

  @param[in]  Base       Controller base.
  @param[in]  Offset     Register offset.
  @param[in]  Mask       Bits to wait for the hardware to clear.
  @param[in]  TimeoutUs  Budget in microseconds.

  @retval EFI_SUCCESS  The mask is clear.
  @retval EFI_TIMEOUT  The budget ran out first.
**/
STATIC
EFI_STATUS
SmhcWaitClear (
  IN UINTN   Base,
  IN UINT32  Offset,
  IN UINT32  Mask,
  IN UINT64  TimeoutUs
  )
{
  A733_SMHC_DEADLINE  Deadline;
  UINT32              Current;

  SmhcArmDeadline (&Deadline, TimeoutUs);

  do {
    Current = SmhcRead (Base, Offset);
    if ((Current & Mask) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (A733_SMHC_POLL_STEP_US);
  } while (!SmhcDeadlineExpired (&Deadline));

  return EFI_TIMEOUT;
}

/**
  Throw away whatever the FIFO holds and wait for the controller to finish.

  Bytes left over from a failed transfer would otherwise be read back as the
  beginning of the next one, and there is no way to tell the two apart from the
  FIFO alone.

  @param[in] Base  Controller base.

  @retval EFI_SUCCESS  The FIFO is empty.
  @retval EFI_TIMEOUT  The reset did not self-clear.
**/
STATIC
EFI_STATUS
SmhcFifoReset (
  IN UINTN  Base
  )
{
  UINT32  Value;

  Value  = SmhcRead (Base, A733_SMHC_CTRL);
  Value |= A733_SMHC_CTRL_FIFO_RST;
  SmhcWrite (Base, A733_SMHC_CTRL, Value);

  return SmhcWaitClear (Base, A733_SMHC_CTRL, A733_SMHC_CTRL_FIFO_RST, A733_SMHC_RESET_TIMEOUT_US);
}

/**
  Bring the controller's own registers to the state this driver expects.

  Called once, before the first command, and again after a card reset.  It does
  not touch the card clock: SMHC_CLKDIV is left to SmhcSetClock, because the
  manual's programming sequence opens the card clock as a separate step
  (11.3.6.1).

  @param[in] Base  Controller base.

  @retval EFI_SUCCESS  The controller is idle and configured.
  @retval EFI_TIMEOUT  A reset bit never self-cleared.
**/
EFI_STATUS
SmhcInitController (
  IN UINTN  Base
  )
{
  UINT32  Value;

  //
  // Software reset first, then the FIFO: both bits self-clear and the manual
  // warns that no command may be written while a reset is in progress.  The
  // IDMAC is reset with them, so nothing about the previous transfer - a
  // descriptor it still owned, a status bit it had not cleared - survives this.
  //
  Value  = SmhcRead (Base, A733_SMHC_CTRL);
  Value |= A733_SMHC_CTRL_SOFT_RST;
  SmhcWrite (Base, A733_SMHC_CTRL, Value);

  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_CTRL, A733_SMHC_CTRL_SOFT_RST, A733_SMHC_RESET_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "Smhc: controller soft reset did not complete\n"));
    return EFI_TIMEOUT;
  }

  if (EFI_ERROR (SmhcFifoReset (Base))) {
    DEBUG ((DEBUG_ERROR, "Smhc: FIFO reset did not complete\n"));
    return EFI_TIMEOUT;
  }

  SmhcWrite (Base, A733_SMHC_DMAC, A733_SMHC_DMAC_IDMAC_RST);
  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_DMAC, A733_SMHC_DMAC_IDMAC_RST, A733_SMHC_RESET_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "Smhc: IDMAC reset did not complete\n"));
    return EFI_TIMEOUT;
  }

  //
  // Global control: the DMA engine on, the interrupt output on, and the FIFO
  // left on the DMA bus.  FIFO_AC_MOD (bit 31) must be clear because the card's
  // data does not pass through the AHB side of the FIFO any more - the IDMAC
  // moves it - and DMA_ENB (bit 5) is what puts the FIFO on that bus.
  //
  // Written as a whole rather than read-modify-write, deliberately: this is
  // also the state the identification sequence was brought up on (see
  // SmhcCardIdentify), and boot0 leaves a bit 29 here that the manual's table
  // does not name.  Only the bits named by the manual are programmed.
  //
  SmhcWrite (Base, A733_SMHC_CTRL, A733_SMHC_CTRL_DMA_ENB | A733_SMHC_CTRL_INT_ENB);

  //
  // Unmask the interrupts this driver's completion model needs: command
  // complete, data transfer complete, auto-command done, and the error bits -
  // which include the ones the wait uses to notice a failure early instead of
  // waiting out the deadline.  With the mask clear, the controller would never
  // raise the line and every command would end in a timeout.
  //
  SmhcWrite (Base, A733_SMHC_INTMASK, A733_SMHC_INTMASK_VALUE);

  //
  // Timeouts.  Both fields count card clock periods (SMHC_CTRL bits 12/11 are
  // left at their reset value, one period each), so neither needs revisiting
  // when the card clock changes:
  //
  //   response timeout 0xff = 255 card clocks.  The eMMC spec bounds a card's
  //     response at 64 clocks (NCR), so 255 is about four times the bound at
  //     any rate: 637 us at 400 kHz, 21 us at 12 MHz.
  //   data timeout 0xffffff = 16.7 million card clocks, the field's maximum and
  //     the manual's reset value: 42 s at 400 kHz, 0.33 s at 50 MHz.  For a
  //     multiple-block read the manual defines this as the gap between blocks,
  //     not the whole transfer, so it only has to cover the card's access time.
  //     An 8-bit DDR bus halves it (see A733_SMHC_DDR_DTO_HALVES), which is
  //     still thousands of times the card's access time at any clock this
  //     driver uses; the DDR step reports the resulting bound rather than
  //     changing a field that is already at its maximum.
  //
  SmhcWrite (
    Base,
    A733_SMHC_TMOUT,
    (A733_SMHC_TMOUT_DTO_MAX << A733_SMHC_TMOUT_DTO_SHIFT) | A733_SMHC_TMOUT_RTO_MAX
    );

  //
  // FIFO water level.  These are the manual's recommended values for this 256
  // entry FIFO, and the burst size they encode (3 = 16 DWORDs = 64 bytes) is
  // the unit every descriptor buffer this driver builds is a multiple of.
  //
  SmhcWrite (Base, A733_SMHC_FIFOTH, A733_SMHC_FIFOTH_VALUE);

  //
  // Card threshold, the vendor's own value: 512 bytes with both directions
  // enabled.  Written once here rather than per transfer because it is a
  // property of the FIFO and the card's access time, not of a mode.
  //
  SmhcWrite (Base, A733_SMHC_THLD, A733_SMHC_THLD_VALUE);

  //
  // The vendor's core_init writes 3 here, which the manual documents as the
  // CRC-status token the controller expects after a write (0x6 is HS400).
  //
  SmhcWrite (Base, A733_SMHC_CSDC, 3);

  //
  // One bit to start with: the card answers CMD0/CMD1 on DATA0 alone, and the
  // width only widens once the card has been told to.
  //
  SmhcWrite (Base, A733_SMHC_CTYPE, A733_SMHC_BUS_1BIT);

  //
  // The card's hardware reset line is left in active (non-reset) mode, which is
  // the register's reset value: 0 holds the card in pre-idle and it would have
  // to be identified all over again (11.3.8.42).  The register is written only
  // when it is not already there, so a previous stage that left the line
  // asserted is put right without this driver ever writing the bit that asserts
  // it.
  //
  if ((SmhcRead (Base, A733_SMHC_HWRST) & A733_SMHC_HWRST_ACTIVE) == 0) {
    SmhcWrite (Base, A733_SMHC_HWRST, A733_SMHC_HWRST_ACTIVE);
  }

  return EFI_SUCCESS;
}

/**
  Bring the controller to the state a card identification starts from.

  The soft reset throws away everything a previous attempt left behind,
  including a command FSM parked mid-command by a response timeout, which is
  why a retry after a failure goes through here rather than just re-sending
  CMD0.  The card clock and the bus width are part of the state, so both are
  set here too: identification runs at fOD on one bit.

  @param[in,out] Private  Driver state.
  @param[in]     Divisor  SMHC_CLKDIV's n field for the card clock.

  @retval EFI_SUCCESS  The controller is idle at the requested clock.
  @retval EFI_TIMEOUT  A reset bit never self-cleared.
**/
EFI_STATUS
SmhcPrepareController (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             Divisor
  )
{
  EFI_STATUS  Status;

  Status = SmhcInitController (Private->Base);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SmhcSetClock (Private, Divisor);
  SmhcSetBusWidth (Private->Base, A733_SMHC_BUS_1BIT);

  return EFI_SUCCESS;
}

/**
  Set the card clock divider and deliver the new clock.

  The controller has no clock-update bit: the manual's sequence is to write
  SMHC_CLKDIV and then issue the clock-change command, whose example argument
  is 0x80202000 (START | PRG_CLK | WAIT_PRE_OVER).  DATA0 is masked across the
  change because it is the card's busy line, and a card holding it low would
  otherwise stall the switch.

  The divider is also remembered in the driver state: a controller reset can
  take the card clock with it, and the recovery path has to deliver the same
  clock again, which means knowing which one the card was last given.

  @param[in,out] Private  Driver state.
  @param[in]     Divisor  SMHC_CLKDIV's field: the module clock is divided by
                          Divisor + 1, so 0 is the full module clock.
**/
VOID
SmhcSetClock (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             Divisor
  )
{
  A733_SMHC_DEADLINE  Deadline;
  UINTN               Base;
  UINT32              Value;

  Base = Private->Base;

  Private->ClockDivisor = Divisor;

  Value  = SmhcRead (Base, A733_SMHC_CLKDIV);
  Value &= ~A733_SMHC_CLKDIV_CCLK_DIV_MASK;
  Value |= (Divisor & A733_SMHC_CLKDIV_CCLK_DIV_MASK);
  Value |= A733_SMHC_CLKDIV_CCLK_ENB | A733_SMHC_CLKDIV_MASK_DAT0;
  SmhcWrite (Base, A733_SMHC_CLKDIV, Value);

  //
  // The clock-change command sends nothing on the wire, so it has no response
  // and no command-complete of its own; the observable end is the start bit
  // clearing itself, which is also the manual's rule for writing the next
  // command.  A failure here is reported but not fatal: the divider is already
  // programmed, and every later command is checked on its own.
  //
  SmhcWrite (Base, A733_SMHC_CMD, A733_SMHC_CMD_PRG_CLK_VALUE);
  SmhcArmDeadline (&Deadline, A733_SMHC_CMD_TIMEOUT_US);
  do {
    Value = SmhcRead (Base, A733_SMHC_CMD);
    if ((Value & A733_SMHC_CMD_START) == 0) {
      break;
    }

    MicroSecondDelay (A733_SMHC_POLL_STEP_US);
  } while (!SmhcDeadlineExpired (&Deadline));

  if ((Value & A733_SMHC_CMD_START) != 0) {
    DEBUG ((DEBUG_ERROR, "Smhc: clock change command did not complete\n"));
  }

  //
  // Unmask DATA0 now that the clock has moved.
  //
  Value  = SmhcRead (Base, A733_SMHC_CLKDIV);
  Value &= ~A733_SMHC_CLKDIV_MASK_DAT0;
  SmhcWrite (Base, A733_SMHC_CLKDIV, Value);
}

/**
  Set the bus width the controller drives.

  @param[in] Base   Controller base.
  @param[in] Width  A733_SMHC_BUS_1BIT, _4BIT or _8BIT.
**/
VOID
SmhcSetBusWidth (
  IN UINTN   Base,
  IN UINT32  Width
  )
{
  SmhcWrite (Base, A733_SMHC_CTYPE, Width);
}

/**
  Set or clear the controller's DDR mode bit and the host-side timing that goes
  with it.

  One bit is the mode itself, but a DDR52 bus needs the rest of the host's
  timing registers set for it too, and this is the one place that owns them
  because the recovery path re-applies the driver's idea of the bus through here
  (SmhcRecoverController) after every failed transfer.  The card's side of the
  same decision is a CMD6 write this driver makes separately, and the vendor's
  order is card first, these registers after (see the DDR step in
  A733SmhcCard.c):

  - SMHC_NTSR bit 31 = 0, the 1x timing mode.  Its reset value is 2x, and the
    delay line calibrated below is the 1x one.
  - SMHC_DRV_DL bits 16|17: 180-degree output phase on both the command and the
    data lines.  The reset value sets the command bit alone.
  - SMHC_DS_DL = 0x80: the data-strobe delay with its software value enabled and
    tap 0, which is what the vendor programs for every mode but HS400.
  - SMHC_CSDC = 3: the CRC-status token the controller expects after a write in
    a non-HS400 mode.

  SMHC_SAMP_DL is deliberately not touched: it is the swept value and belongs to
  the sweep (A733SmhcCard.c).  DS_DL has no such sweep here, so the DDR step
  records it before the switch and puts it back on the way out.

  Which register set is written is Private->HostDdrTiming, not Enable: the two are
  set together for a real DDR bus, and they differ only for the DDR register
  control run, which holds the card in SDR and therefore wants the register set
  without the mode bit.  Enable is only the SMHC_CTRL bit.

  @param[in,out] Private  Driver state.
  @param[in]     Enable   TRUE to run the data bus double data rate.
**/
VOID
SmhcSetDdrMode (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     BOOLEAN            Enable
  )
{
  UINT32  Value;

  //
  // The state of all four delay/timing registers the switch below writes, before
  // it writes them: this is the one part of the switch an earlier build never
  // touched for DDR.  The line is suppressed while Quiet is set, which is the
  // sweep's own recoveries - they run this function once per attempt.
  //
  if (!Private->Quiet) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: timing registers before DDR=%u host=%u: ntsr %08x drv_dl %08x samp_dl %08x ds_dl %08x sfc %08x csdc %08x\n",
      (UINT32)Enable,
      (UINT32)Private->HostDdrTiming,
      SmhcRead (Private->Base, A733_SMHC_NTSR),
      SmhcRead (Private->Base, A733_SMHC_DRV_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SFC),
      SmhcRead (Private->Base, A733_SMHC_CSDC)
      ));
  }

  //
  // NTSR is a read-modify-write of bit 31, never a whole-register write: the
  // value boot0 leaves there (0x81710000) carries the rest of the timing
  // configuration, and the vendor clears the 2x-mode bit the same way
  // (sunxi-mmc-v4p5x.c "enable 1x mode", u-boot's "Clear NTSR bit 31").  Writing
  // a bare 0 or 0x80000000 threw those bits away, which is what a DDR data phase
  // that never produced a valid end bit looked like.
  //
  Value = SmhcRead (Private->Base, A733_SMHC_NTSR);
  if (Private->HostDdrTiming) {
    Value &= ~A733_SMHC_NTSR_2X_TIMING_MODE;
  } else {
    Value |= A733_SMHC_NTSR_2X_TIMING_MODE;
  }

  SmhcWrite (Private->Base, A733_SMHC_NTSR, Value);

  if (Private->HostDdrTiming) {
    SmhcWrite (Private->Base, A733_SMHC_DRV_DL, A733_SMHC_DRV_DL_DDR);
    SmhcWrite (Private->Base, A733_SMHC_DS_DL, A733_SMHC_DS_DL_DDR52);
    SmhcWrite (Private->Base, A733_SMHC_CSDC, A733_SMHC_CSDC_NORMAL);
  } else {
    //
    // The single-data-rate state this driver has always left behind on the way
    // out of a DDR attempt, unchanged: the manual's reset values for NTSR and
    // DRV_DL, and CSDC at the non-HS400 token.  DS_DL is not written here - the
    // DDR step restores the value it recorded, since only that step ever
    // changes it.
    //
    SmhcWrite (Private->Base, A733_SMHC_DRV_DL, A733_SMHC_DRV_DL_RESET);
    SmhcWrite (Private->Base, A733_SMHC_CSDC, A733_SMHC_CSDC_NORMAL);
  }

  Value = SmhcRead (Private->Base, A733_SMHC_CTRL);
  if (Enable) {
    Value |= A733_SMHC_CTRL_DDR_MODE;
  } else {
    Value &= ~A733_SMHC_CTRL_DDR_MODE;
  }

  SmhcWrite (Private->Base, A733_SMHC_CTRL, Value);

  //
  // What the bit reads back as, which is the host's side of the mode and not
  // the card's: Private->Ddr is set by the card layer once the card has been
  // switched and the switch read back, and it is that one the log reports.
  //
  Value               = SmhcRead (Private->Base, A733_SMHC_CTRL);
  Private->DdrModeBit = (Value & A733_SMHC_CTRL_DDR_MODE) != 0;

  if (!Private->Quiet) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: timing registers after DDR=%u host=%u: ntsr %08x drv_dl %08x samp_dl %08x ds_dl %08x sfc %08x csdc %08x ctrl %08x\n",
      (UINT32)Enable,
      (UINT32)Private->HostDdrTiming,
      SmhcRead (Private->Base, A733_SMHC_NTSR),
      SmhcRead (Private->Base, A733_SMHC_DRV_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SFC),
      SmhcRead (Private->Base, A733_SMHC_CSDC),
      SmhcRead (Private->Base, A733_SMHC_CTRL)
      ));
  }
}

/**
  Turn the controller's sample-FIFO bypass on or off (SMHC_SFC bit 0).

  The vendor driver sets the bypass while it calibrates a delay line and clears
  it for normal traffic, so the tap it picks is measured with the same sampler
  the traffic will use only if the bypass is cleared again before the transfer
  that proves it.  That is exactly what the DDR step does, and the read it makes
  after clearing the bit is the one that says whether the two agree.

  @param[in] Base    Controller base.
  @param[in] Bypass  TRUE to bypass the sample FIFO, FALSE to use it.
**/
VOID
SmhcSetSampleFifoBypass (
  IN UINTN    Base,
  IN BOOLEAN  Bypass
  )
{
  UINT32  Value;

  Value = SmhcRead (Base, A733_SMHC_SFC);
  if (Bypass) {
    Value |= A733_SMHC_SFC_BYPASS;
  } else {
    Value &= ~A733_SMHC_SFC_BYPASS;
  }

  SmhcWrite (Base, A733_SMHC_SFC, Value);
}

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
  )
{
  UINT32  Value;

  Value  = SmhcRead (Base, A733_SMHC_SAMP_DL);
  Value &= ~A733_SMHC_SAMP_DL_SW_MASK;
  Value |= Delay & A733_SMHC_SAMP_DL_SW_MASK;
  Value |= A733_SMHC_SAMP_DL_SW_EN;
  SmhcWrite (Base, A733_SMHC_SAMP_DL, Value);
}

/**
  Program the host's data-strobe delay (SMHC_DS_DL) for HS400.

  Laid out like SMHC_SAMP_DL - bits 5:0 taps, bit 7 the software-value enable - so
  it is read-modify-written the same way and whatever the register carries above
  the enable is left alone.

  @param[in] Base   Controller base.
  @param[in] Taps   Delay in taps, 0..63.
**/
VOID
SmhcSetStrobeDelay (
  IN UINTN   Base,
  IN UINT32  Taps
  )
{
  UINT32  Value;

  Value  = SmhcRead (Base, A733_SMHC_DS_DL);
  Value &= ~A733_SMHC_DS_DL_SW_MASK;
  Value |= Taps & A733_SMHC_DS_DL_SW_MASK;
  Value |= A733_SMHC_DS_DL_SW_EN;
  SmhcWrite (Base, A733_SMHC_DS_DL, Value);
}

/**
  Enter or leave the controller's HS400 mode.

  Three register fields are the mode - EDSD bit 31, CSDC's CRC-status token and
  SFC's sample-FIFO bypass - and two delay lines belong to it: DS_DL, which is
  swept (Private->StrobeDelay), and SAMP_DL, which at HS400 stays on the command
  line and is also the point the CRC-status token is sampled at
  (Private->SampleDelay: the vendor's initial value on entry, the write sweep's
  choice afterwards).

  The state is remembered in Private->HostHs400Timing because the recovery after a
  failed transfer re-applies the driver's idea of the bus through here: a soft
  reset takes EDSD, CSDC, SFC and both delays back to their defaults, and a sweep
  whose later taps ran without them would report an empty window for a bus whose
  tap was never tried.

  @param[in,out] Private  Driver state.
  @param[in]     Enable   TRUE to enter HS400 mode, FALSE to leave it.
**/
VOID
SmhcSetHs400Mode (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     BOOLEAN            Enable
  )
{
  UINT32  Value;

  if (!Private->Quiet) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 mode %u: edsd %08x csdc %08x sfc %08x ds_dl %08x samp_dl %08x ntsr %08x drv_dl %08x ctrl %08x\n",
      (UINT32)Enable,
      SmhcRead (Private->Base, A733_SMHC_EDSD),
      SmhcRead (Private->Base, A733_SMHC_CSDC),
      SmhcRead (Private->Base, A733_SMHC_SFC),
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_NTSR),
      SmhcRead (Private->Base, A733_SMHC_DRV_DL),
      SmhcRead (Private->Base, A733_SMHC_CTRL)
      ));
  }

  if (Enable) {
    SmhcWrite (Private->Base, A733_SMHC_EDSD, SmhcRead (Private->Base, A733_SMHC_EDSD) | A733_SMHC_EDSD_HS400_MD_EN);
    SmhcWrite (Private->Base, A733_SMHC_CSDC, A733_SMHC_CSDC_HS400);

    //
    // HS400's data path runs through the sample FIFO, so the bypass is cleared
    // here rather than set: it is the opposite of the DDR and HS200 sweeps, and
    // getting it wrong is invisible until the data is compared.
    //
    SmhcSetSampleFifoBypass (Private->Base, FALSE);
    SmhcSetStrobeDelay (Private->Base, Private->StrobeDelay);

    //
    // The command line's tap, which at HS400 is also the point the controller
    // samples the CRC-status token at - so it is the one delay a write depends
    // on.  The caller sets Private->SampleDelay to the vendor's initial value
    // when it enters the mode, and the write sweep replaces it with whatever
    // setting it proved; programming it from the state field, the way DS_DL is
    // programmed, is what lets that setting survive the reset a failed transfer
    // causes.
    //
    SmhcSetSampleDelay (Private->Base, Private->SampleDelay);

    //
    // The data output phase, which only a write depends on: the card samples what
    // the host drives, and at 200 MHz DDR it accepts that with the data phase at
    // 90 degrees (bit 17 clear) and rejects it at 180 (bit 17 set, which is what
    // the DDR step leaves for DDR52).  Reads never notice - they sample with the
    // card's own strobe - which is why this looked like a sampling problem for so
    // long.  Measured here: this bit, and the command-line tap above, are the whole
    // difference between a write that programs and one the card rejects.
    //
    Value = SmhcRead (Private->Base, A733_SMHC_DRV_DL) & ~A733_SMHC_DRV_DL_DAT_PH180;
    SmhcWrite (Private->Base, A733_SMHC_DRV_DL, Value);

    //
    // the A733 user manual, SMHC_CTRL: "Although eMMC's HS400 speed mode is 8-bit
    // DDR, this field should be cleared when HS400_MD_EN is set."  The mode is
    // DDR at the card and a strobe-sampled bus at the host, and the host's DDR
    // select is what HS400 mode replaces - leaving it set is what makes every
    // command complete while the data phase frames wrongly.
    //
    Value = SmhcRead (Private->Base, A733_SMHC_CTRL) & ~A733_SMHC_CTRL_DDR_MODE;
    SmhcWrite (Private->Base, A733_SMHC_CTRL, Value);
    Private->DdrModeBit = FALSE;
  } else {
    SmhcWrite (Private->Base, A733_SMHC_EDSD, SmhcRead (Private->Base, A733_SMHC_EDSD) & ~A733_SMHC_EDSD_HS400_MD_EN);
    SmhcWrite (Private->Base, A733_SMHC_CSDC, A733_SMHC_CSDC_NORMAL);
  }

  Private->HostHs400Timing = Enable;

  if (!Private->Quiet) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 mode %u: edsd %08x (md en %u) csdc %08x sfc %08x ds_dl %08x samp_dl %08x ntsr %08x ctrl %08x\n",
      (UINT32)Enable,
      SmhcRead (Private->Base, A733_SMHC_EDSD),
      (UINT32)((SmhcRead (Private->Base, A733_SMHC_EDSD) & A733_SMHC_EDSD_HS400_MD_EN) != 0),
      SmhcRead (Private->Base, A733_SMHC_CSDC),
      SmhcRead (Private->Base, A733_SMHC_SFC),
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_NTSR),
      SmhcRead (Private->Base, A733_SMHC_CTRL)
      ));
  }
}

/**
  Report whether a card is in the slot.

  The controller has no card-detect pin: SMHC_STATUS bit 8 is the level of
  DATA[3].  On this board that is a weak signal - every data line is pulled up,
  so an empty slot is not guaranteed to read low - which is why identification
  failure is what the driver really trusts.  See SmhcCardInit.

  @param[in] Base  Controller base.

  @retval TRUE   DATA[3] reads high.
  @retval FALSE  DATA[3] reads low, so there is certainly no card.
**/
BOOLEAN
SmhcCardPresent (
  IN UINTN  Base
  )
{
  return (SmhcRead (Base, A733_SMHC_STATUS) & A733_SMHC_STATUS_CARD_PRESENT) != 0;
}

/**
  The card clock SMHC_CLKDIV currently produces, in kHz.

  Read from the register rather than remembered, so a dump states the rate the
  controller is actually running at even when the rate changes mid-sequence.
  The arithmetic follows the field's meaning the vendor's own driver depends on
  (see A733_SMHC_CLKDIV in A733Smhc.h): the card clock is the module clock
  divided by field + 1.

  The module clock is asked for the module that belongs to *this* controller
  (Private->CcuModule, from the board's table), never a fixed one: asking for
  SMHC2's clock while the driver drives SMHC3 reports SMHC2's 24 MHz default for a
  200 MHz module, which is how this driver once made SMHC3 look like a 24 MHz bus
  while it was running at 50.

  @param[in] Private  Driver state; the controller must have been brought up.

  @return The rate in kHz, or 0 when the card clock is off.
**/
UINT32
SmhcClockKhz (
  IN A733_SMHC_PRIVATE  *Private
  )
{
  UINT32  Value;
  UINT32  Divisor;

  Value = SmhcRead (Private->Base, A733_SMHC_CLKDIV);
  if ((Value & A733_SMHC_CLKDIV_CCLK_ENB) == 0) {
    return 0;
  }

  //
  // The field divides by 2 x (n + 1), which the board settled by measurement:
  // a 24 MHz module with n = 1 moved 5.5 MB/s (a 6 MHz bus, not 12), a 50 MHz
  // module with n = 0 moved 22.6 MB/s (25 MHz), and a 200 MHz module with
  // n = 1 moved 44.4 MB/s - 96 % of what 50 MHz x 8 bit can carry.
  //
  Divisor = ((Value & A733_SMHC_CLKDIV_CCLK_DIV_MASK) + 1) * 2;

  return (UINT32)(A733CcuGetModuleClockHz (Private->CcuModule) / Divisor / 1000U);
}

/**
  Wait for the card's busy line to be released.

  STATUS bit 9 is DATA[0] inverted, i.e. low while the card is busy.  R1b
  commands are only half finished when their response arrives, and a card that
  was left busy by an earlier command cannot be addressed again until it lets go.

  @param[in] Base       Controller base.
  @param[in] TimeoutUs  Budget.

  @retval EFI_SUCCESS  The card is idle.
  @retval EFI_TIMEOUT  It was still busy when the budget ran out; the caller
                       decides whether that is fatal.
**/
EFI_STATUS
SmhcWaitCardIdle (
  IN UINTN   Base,
  IN UINT64  TimeoutUs
  )
{
  return SmhcWaitClear (Base, A733_SMHC_STATUS, A733_SMHC_STATUS_CARD_BUSY, TimeoutUs);
}

/**
  Print what the controller looks like right now.

  Unconditional and at DEBUG_ERROR on purpose: this is the line a failed
  identification leaves behind, and it is the difference between "the card did
  not answer" and knowing which side of the bus did not.  The IDMAC's own
  registers are part of that picture: its status tells a descriptor or bus
  failure apart from a card failure.

  @param[in] Private  Driver state.
**/
VOID
SmhcDumpState (
  IN A733_SMHC_PRIVATE  *Private
  )
{
  UINTN   Base;
  UINT32  IntStatus;
  UINT32  IdmaStatus;

  Base = Private->Base;

  IntStatus  = SmhcRead (Base, A733_SMHC_RINTSTS);
  IdmaStatus = SmhcRead (Base, A733_SMHC_IDST);

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: state ctrl %08x clkdiv %08x (n=%u, %u kHz) hwrst %08x ctype %u status %08x rint %08x (fsm %u)\n",
    SmhcRead (Base, A733_SMHC_CTRL),
    SmhcRead (Base, A733_SMHC_CLKDIV),
    SmhcRead (Base, A733_SMHC_CLKDIV) & A733_SMHC_CLKDIV_CCLK_DIV_MASK,
    SmhcClockKhz (Private),
    SmhcRead (Base, A733_SMHC_HWRST),
    SmhcRead (Base, A733_SMHC_CTYPE) & 0x3U,
    SmhcRead (Base, A733_SMHC_STATUS),
    IntStatus,
    (SmhcRead (Base, A733_SMHC_STATUS) & A733_SMHC_STATUS_FSM_STA_MASK) >> 4
    ));

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: idmac dmac %08x dlba %08x idst %08x idie %08x idsts %u\n",
    SmhcRead (Base, A733_SMHC_DMAC),
    SmhcRead (Base, A733_SMHC_DLBA),
    IdmaStatus,
    SmhcRead (Base, A733_SMHC_IDIE),
    (IdmaStatus & 0xF8000U) >> 13
    ));

  SmhcDumpInterruptBits (IntStatus);
  SmhcDumpIdmaBits (IdmaStatus);
}

/**
  Wait for the card-detect level to read high.

  SMHC_CTRL's de-bounce is on (it is the register's reset value), so the level
  bit 8 reports is not trustworthy in the first moments after the controller
  comes out of reset.  A driver that read it once and believed it would report
  an empty slot on a board with a card in it, so the first card-detect question
  is asked here, over a budget.

  @param[in] Base       Controller base.
  @param[in] TimeoutUs  How long to keep asking.

  @retval TRUE   DATA[3] read high within the budget.
  @retval FALSE  It read low for the whole budget.
**/
BOOLEAN
SmhcCardWaitPresent (
  IN UINTN   Base,
  IN UINT64  TimeoutUs
  )
{
  A733_SMHC_DEADLINE  Deadline;

  SmhcArmDeadline (&Deadline, TimeoutUs);

  do {
    if (SmhcCardPresent (Base)) {
      return TRUE;
    }

    MicroSecondDelay (A733_SMHC_CD_SETTLE_STEP_US);
  } while (!SmhcDeadlineExpired (&Deadline));

  return FALSE;
}

/**
  Assemble SMHC_CMD for a command.

  @param[in] Command  Command to encode.

  @return The value to write to SMHC_CMD.
**/
STATIC
UINT32
SmhcCommandRegister (
  IN CONST A733_SMHC_COMMAND  *Command
  )
{
  UINT32  Value;

  Value = A733_SMHC_CMD_START | (Command->Index & A733_SMHC_CMD_IDX_MASK);

  if (Command->PrgClk) {
    //
    // The clock-change command carries no command index on the wire.
    //
    return A733_SMHC_CMD_PRG_CLK_VALUE;
  }

  if (Command->SendInitSeq) {
    // 74 clocks of the initialization sequence, which CMD0 needs.
    Value |= A733_SMHC_CMD_SEND_INIT_SEQ;
  }

  if (Command->Abort) {
    Value |= A733_SMHC_CMD_STOP_ABT_CMD;
  }

  // DIAGNOSTIC ONLY.
  if (Command->ForcePreOver) {
    Value |= A733_SMHC_CMD_WAIT_PRE_OVER;
  }

  if (Command->AutoStop) {
    //
    // The controller sends CMD12 once the byte count has moved, which is what
    // ends a streaming multi-block transfer.
    //
    Value |= A733_SMHC_CMD_STOP_CMD_FLAG;
  }

  if (Command->Length != 0) {
    Value |= A733_SMHC_CMD_DATA_TRANS | A733_SMHC_CMD_WAIT_PRE_OVER;
    if (!Command->Read) {
      Value |= A733_SMHC_CMD_TRANS_DIR;
    }
  }

  if (Command->CommandType != SdMmcCommandTypeBc) {
    Value |= A733_SMHC_CMD_RESP_RCV;

    switch (Command->ResponseType) {
      case SdMmcResponseTypeR2:
        //
        // The 136-bit response carries its own CRC7 past the 128 bits this
        // driver keeps, so the controller checks it and the driver reads all
        // four response words.
        //
        if (!Command->NoLongResp) {
          Value |= A733_SMHC_CMD_LONG_RESP;
        } else if (!Command->NoCrcCheck) {
          Value |= A733_SMHC_CMD_CHK_RESP_CRC;
        }

        if (!Command->NoCrcCheck) {
          Value |= A733_SMHC_CMD_CHK_RESP_CRC;
        }

        break;

      case SdMmcResponseTypeR3:
      case SdMmcResponseTypeR4:
        //
        // R3 (OCR) and R4 have no CRC7 at all, so there is nothing to check.
        //
        break;

      case SdMmcResponseTypeR1b:
      case SdMmcResponseTypeR5b:
        Value |= A733_SMHC_CMD_CHK_RESP_CRC |
                 A733_SMHC_CMD_R1B_RESP |
                 A733_SMHC_CMD_R1B_BUSY_HW_DETECT;
        break;

      default:
        // R1, R5, R6 and R7: a 48-bit response with its CRC7.
        Value |= A733_SMHC_CMD_CHK_RESP_CRC;
        break;
    }
  }

  return Value;
}

/**
  The interrupt bits that make a command with this direction a failure.

  Reading and writing disagree about bit 13: the manual calls it "data start
  error" while reading but "busy signal is cleared after the last block" while
  writing, and a write's last block always releases busy.  Treating it as an
  error in both directions would fail every write this driver ever sends.

  @param[in] Read  TRUE for a read (SMHC_TRANS_DIR is 0), FALSE for a write.

  @return The mask of fatal bits for that direction.
**/
STATIC
UINT32
SmhcErrorMask (
  IN BOOLEAN  Read
  )
{
  return Read ? (A733_SMHC_RINT_ERRORS | A733_SMHC_RINT_DSE_BC) : A733_SMHC_RINT_ERRORS;
}

/**
  Turn SMHC_RINTSTS into an EFI_STATUS.

  A response or data timeout says the card never answered, which a caller can
  reasonably retry; every other error bit says the card or the bus answered
  something wrong, which is a device error.  SMHC_RINT_DSE_BC only counts while
  reading - see SmhcErrorMask.

  @param[in] IntStatus  The value read from SMHC_RINTSTS.
  @param[in] Read       TRUE when the command was reading data.

  @return EFI_SUCCESS, EFI_TIMEOUT or EFI_DEVICE_ERROR.
**/
STATIC
EFI_STATUS
SmhcMapErrors (
  IN UINT32   IntStatus,
  IN BOOLEAN  Read
  )
{
  UINT32  Fatal;

  Fatal = SmhcErrorMask (Read);
  if ((IntStatus & Fatal) == 0) {
    return EFI_SUCCESS;
  }

  if ((IntStatus & (Fatal & ~A733_SMHC_RINT_TIMEOUTS)) != 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_TIMEOUT;
}

/**
  Name the failing bits of SMHC_RINTSTS, one DEBUG line each.

  Called from the PCD-gated per-command dump and unconditionally from
  SmhcDumpState: the bit that failed is the whole diagnosis of a stubborn card,
  and it is not worth a decoded line per command in a normal boot.

  @param[in] IntStatus  The value read from SMHC_RINTSTS.
**/
STATIC
VOID
SmhcDumpInterruptBits (
  IN UINT32  IntStatus
  )
{
  STATIC CONST struct {
    UINT32         Bit;
    CONST CHAR8    *Name;
  }  mBits[] = {
    { A733_SMHC_RINT_RE,              "response error (bit or index or end bit)" },
    { A733_SMHC_RINT_RCE,             "response CRC error" },
    { A733_SMHC_RINT_DCE,             "data CRC error" },
    { A733_SMHC_RINT_RTO_BACK,        "response timeout" },
    { A733_SMHC_RINT_DTO_BDS,         "data timeout / no start bit" },
    { A733_SMHC_RINT_DSTO_VSD,        "data starvation timeout" },
    { A733_SMHC_RINT_FU_FO,           "FIFO underrun/overflow" },
    { A733_SMHC_RINT_CB_IW,           "command busy / illegal write" },
    { A733_SMHC_RINT_DSE_BC,          "data start error" },
    { A733_SMHC_RINT_DEE,             "end bit error" },
    { A733_SMHC_RINT_DATA_WR_BUSY_TO, "write busy timeout" }
  };
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mBits); Index++) {
    if ((IntStatus & mBits[Index].Bit) != 0) {
      DEBUG ((DEBUG_INFO, "Smhc:     %a\n", mBits[Index].Name));
    }
  }
}

/**
  Name the failing bits of SMHC_IDST, one DEBUG line each.

  The IDMAC's status is a different picture from SMHC_RINTSTS: it says whether
  the descriptor ring or the bus failed, as against the card.  On a failed
  transfer that is the first distinction worth having, and the error cause
  field only means anything while the fatal bus error bit is set.

  @param[in] IdmaStatus  The value read from SMHC_IDST.
**/
STATIC
VOID
SmhcDumpIdmaBits (
  IN UINT32  IdmaStatus
  )
{
  STATIC CONST struct {
    UINT32         Bit;
    CONST CHAR8    *Name;
  }  mBits[] = {
    { A733_SMHC_IDST_DES_UNAVL,    "descriptor unavailable (the IDMAC did not own it)" },
    { A733_SMHC_IDST_FATAL_BERR,   "fatal bus error (the IDMAC stopped its bus accesses)" },
    { A733_SMHC_IDST_ERR_FLAG_SUM, "card error summary" },
    { A733_SMHC_IDST_TX_INT,       "transmit complete" },
    { A733_SMHC_IDST_RX_INT,       "receive complete" },
    { A733_SMHC_IDST_NIS,          "normal interrupt summary" },
    { A733_SMHC_IDST_AIS,          "abnormal interrupt summary" }
  };
  UINT32  Cause;
  UINTN   Index;

  DEBUG ((DEBUG_INFO, "Smhc:     idmac idst %08x (state %u)\n", IdmaStatus, (IdmaStatus & 0xF8000U) >> 13));

  for (Index = 0; Index < ARRAY_SIZE (mBits); Index++) {
    if ((IdmaStatus & mBits[Index].Bit) != 0) {
      DEBUG ((DEBUG_INFO, "Smhc:     %a\n", mBits[Index].Name));
    }
  }

  if ((IdmaStatus & A733_SMHC_IDST_FATAL_BERR) != 0) {
    Cause = (IdmaStatus & A733_SMHC_IDST_ERR_STA_MASK) >> A733_SMHC_IDST_ERR_STA_SHIFT;
    DEBUG ((
      DEBUG_INFO,
      "Smhc:     idmac bus error cause %u (%a)\n",
      Cause,
      (Cause == 1) ? "host abort during transmission" :
      ((Cause == 2) ? "host abort during reception" : "reserved")
      ));
  }
}

/**
  The GIC distributor's word for the controller's interrupt, read back.

  These are the registers that decide whether an asserted peripheral line ever
  reaches this core, and they are read here only to be reported: if the
  controller's interrupt is not being delivered, the difference between a line
  that is asserted but disabled, one that is enabled but never asserted, and one
  that is in the wrong group is the whole diagnosis, and none of them can be
  seen from the controller's own registers.

  The offsets are the GICv3 distributor's (ArmGicLib.h names the same words:
  ICDISER for the set-enable register, ICDSPR for set-pending, ICDABR for
  active, ICDISR for the group register and ICDICFR for the configuration).
  One interrupt's bit lives in register index Id/32 at bit Id%32, and its
  two-bit configuration field in ICFGR index Id/16 - so for the controller's SPI
  these are the words at +0x114, +0x214, +0x314, +0x094 and +0x228 if their
  index arithmetic is repeated by hand.

  @param[out] State  Receives the words, and which bit of them is the SPI's.
  @param[in]  Spi    The controller's interrupt, from its table entry.
**/
STATIC
VOID
SmhcReadGicState (
  OUT A733_SMHC_GIC_STATE  *State,
  IN  UINT32               Spi
  )
{
  UINTN  Distributor;
  UINTN  Index;
  UINTN  ConfigIndex;

  ZeroMem (State, sizeof (*State));

  Distributor = (UINTN)PcdGet64 (PcdGicDistributorBase);
  if (Distributor == 0) {
    return;
  }

  Index       = Spi / 32U;
  ConfigIndex = Spi / 16U;

  State->Mask    = (UINT32)(1U << (Spi % 32U));
  State->Control = MmioRead32 (Distributor + A733_GICD_CTLR);
  State->Group   = MmioRead32 (Distributor + A733_GICD_IGROUPR + 4U * Index);
  State->Enable  = MmioRead32 (Distributor + A733_GICD_ISENABLER + 4U * Index);
  State->Pending = MmioRead32 (Distributor + A733_GICD_ISPENDR + 4U * Index);
  State->Active  = MmioRead32 (Distributor + A733_GICD_ISACTIVER + 4U * Index);
  State->Config  = MmioRead32 (Distributor + A733_GICD_ICFGR + 4U * ConfigIndex);
  State->Route   = MmioRead64 (Distributor + A733_GICD_IROUTER + 8U * Spi);

  State->Bits  = State->Config >> (2U * (Spi % 16U));
  State->Level = (State->Bits & 0x2U) == 0;
}

/**
  Report what the GIC holds for the controller's interrupt.

  At DEBUG_ERROR on purpose while this is the open question: the point of the
  readback is that it cannot be missed in a boot log, and it is what says
  whether registration, enabling and the trigger type actually took effect.

  The registers that decide delivery are all here, because an interrupt that is
  registered, enabled and asserted still goes nowhere if the distributor is not
  forwarding group 1, if the SPI belongs to group 0 (where this core's writes to
  its enable bit are ignored), or if its route points at another affinity.  The
  core's own MPIDR is printed with them, since "route points at another core" is
  only answerable against the core that is asking.

  @param[in] Private  Driver state; its Spi is the interrupt being reported.
  @param[in] When     Where in the driver this is being asked.
**/
STATIC
VOID
SmhcReportGicState (
  IN A733_SMHC_PRIVATE  *Private,
  IN CONST CHAR8        *When
  )
{
  A733_SMHC_GIC_STATE  State;
  UINT32               Spi;

  Spi = Private->Spi;

  SmhcReadGicState (&State, Spi);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: GIC SPI %u (%a): CTLR %08x (ARE %u, EnableGrp1NS %u), IGROUPR+%x %08x (group %u), "
    "ISENABLER+%x %08x (enabled %u), ISPENDR+%x %08x (pending %u), ISACTIVER+%x %08x (active %u), "
    "ICFGR+%x %08x (bits %u, %a), IROUTER+%x %08x (route %08x %08x, this core %08x)\n",
    Spi,
    When,
    State.Control,
    (State.Control & 0x10U) != 0,
    (State.Control & 0x2U) != 0,
    A733_GICD_IGROUPR + 4U * (Spi / 32U),
    State.Group,
    (State.Group & State.Mask) != 0,
    A733_GICD_ISENABLER + 4U * (Spi / 32U),
    State.Enable,
    (State.Enable & State.Mask) != 0,
    A733_GICD_ISPENDR + 4U * (Spi / 32U),
    State.Pending,
    (State.Pending & State.Mask) != 0,
    A733_GICD_ISACTIVER + 4U * (Spi / 32U),
    State.Active,
    (State.Active & State.Mask) != 0,
    A733_GICD_ICFGR + 4U * (Spi / 16U),
    State.Config,
    State.Bits,
    State.Level ? "level-high" : "edge-rising",
    A733_GICD_IROUTER + 8U * Spi,
    (UINT32)State.Route,
    (UINT32)(State.Route >> 32),
    (UINT32)ArmReadMpidr ()
    ));
}

/**
  The controller's interrupt: record what happened, tell the command engine.

  Three rules shape this handler.  It records rather than decides: whether a
  command succeeded is worked out from what is left in the driver state here,
  which keeps one place - SmhcSendCommand - responsible for the outcome, and
  means the interrupt and the deadline can never disagree about it.

  It clears exactly the bits it saw, because both status registers are
  write-1-to-clear and a bit left set would hold the level-sensitive line up
  forever; bits it did not see are left alone, so nothing another read is
  looking at goes missing.

  And it does nothing else that can block: it reads two registers, writes two,
  and signals an event.

  @param[in] Source         Interrupt source, which is the controller's SPI.
  @param[in] SystemContext  Unused; no context is exchanged with the handler.
**/
STATIC
VOID
EFIAPI
SmhcInterruptHandler (
  IN HARDWARE_INTERRUPT_SOURCE  Source,
  IN EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  A733_SMHC_INTERRUPT_REGISTRATION  *Registration;
  A733_SMHC_PRIVATE                 *Private;
  UINT32                            IntStatus;
  UINT32                            IdmaStatus;

  //
  // Which controller this call is for is the source's registration; a source the
  // driver did not register is not this driver's interrupt, and returning here
  // without reading or ending anything is what keeps one controller's handler from
  // ever acting on another's registers.
  //
  Registration = SmhcFindInterruptRegistration (Source);
  if ((Registration == NULL) || (Registration->Private == NULL)) {
    return;
  }

  Private = Registration->Private;

  Private->InterruptCount++;

  IntStatus  = SmhcRead (Private->Base, A733_SMHC_RINTSTS);
  IdmaStatus = SmhcRead (Private->Base, A733_SMHC_IDST);

  if (IntStatus != 0) {
    SmhcWrite (Private->Base, A733_SMHC_RINTSTS, IntStatus);
    Private->IntStatus |= IntStatus;
  }

  if ((IdmaStatus & A733_SMHC_IDST_CLEAR_MASK) != 0) {
    SmhcWrite (Private->Base, A733_SMHC_IDST, IdmaStatus & A733_SMHC_IDST_CLEAR_MASK);
    Private->IdstStatus |= IdmaStatus;
  }

  //
  // The signal carries no result: it only wakes the waiter up to look at the
  // record above.  A signal with nothing behind it is possible and harmless.
  //
  gBS->SignalEvent (Private->CompletionEvent);

  //
  // ArmGicDxe dispatches to a registered handler and does NOT end the
  // interrupt itself - it only writes the EOI for a spurious one.  A handler
  // that returns without this leaves the interrupt active in the distributor,
  // and a level-sensitive source that stays active is never delivered again:
  // that is one interrupt and then silence, which is exactly what this driver
  // saw with SPI 195 before this call was here.
  //
  if (Registration->Interrupt2 != NULL) {
    Registration->Interrupt2->EndOfInterrupt (Registration->Interrupt2, Source);
  } else if (Registration->Interrupt != NULL) {
    Registration->Interrupt->EndOfInterrupt (Registration->Interrupt, Source);
  }
}

/**
  Register the controller's interrupt handler and enable the interrupt.

  The platform's ArmGicDxe produces both interrupt protocols.  The second is
  preferred because it also carries the trigger type, and the controller's
  interrupt is a level: its source is a set of latched status bits, so the line
  stays asserted until this driver's handler clears them.

  The registration is what the handler finds its controller by, so it is filled
  in here: an entry of mSmhcInterrupts keyed by the controller's SPI.  A source
  that is already registered is refused rather than overwritten - a second
  registration for one source would make the handler's lookup ambiguous - and so is
  a table with no free entry.

  @param[in] Private  Driver state; its completion event must already exist.

  @retval EFI_SUCCESS           The handler is registered and the source enabled.
  @retval EFI_NOT_FOUND         The platform has no interrupt protocol.
  @retval EFI_ALREADY_STARTED   This controller's source is already registered.
  @retval EFI_OUT_OF_RESOURCES  A733_SMHC_MAX_CONTROLLERS registrations exist.
  @retval other                 The platform refused the source or the handler.
**/
EFI_STATUS
SmhcInstallInterrupt (
  IN A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS                        Status;
  EFI_STATUS                        TriggerStatus;
  EFI_STATUS                        RegisterStatus;
  EFI_STATUS                        EnableStatus;
  A733_SMHC_INTERRUPT_REGISTRATION  *Registration;
  UINTN                             Index;

  //
  // One entry per controller, keyed by the source the handler is called with.  The
  // search for a free one also covers the collision case: the same source twice
  // would make the handler's lookup depend on the order of the table, which is not
  // a question a registration should ever leave open.
  //
  Registration = NULL;
  for (Index = 0; Index < A733_SMHC_MAX_CONTROLLERS; Index++) {
    if (mSmhcInterrupts[Index].Installed &&
        (mSmhcInterrupts[Index].Source == (HARDWARE_INTERRUPT_SOURCE)Private->Spi))
    {
      DEBUG ((
        DEBUG_ERROR,
        "Smhc: %a cannot own the interrupt: SPI %u is already registered for another controller\n",
        Private->Name,
        Private->Spi
        ));
      return EFI_ALREADY_STARTED;
    }

    if (!mSmhcInterrupts[Index].Installed && (Registration == NULL)) {
      Registration = &mSmhcInterrupts[Index];
    }
  }

  if (Registration == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a cannot own the interrupt: all %u registrations are in use\n",
      Private->Name,
      (UINT32)A733_SMHC_MAX_CONTROLLERS
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  Registration->Source     = (HARDWARE_INTERRUPT_SOURCE)Private->Spi;
  Registration->Private    = Private;
  Registration->Interrupt  = NULL;
  Registration->Interrupt2 = NULL;

  TriggerStatus = EFI_NOT_FOUND;

  Status = gBS->LocateProtocol (&gHardwareInterrupt2ProtocolGuid, NULL, (VOID **)&Registration->Interrupt2);
  if (!EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: SPI %u via EFI_HARDWARE_INTERRUPT2_PROTOCOL\n",
      Private->Spi
      ));

    TriggerStatus = Registration->Interrupt2->SetTriggerType (
                                               Registration->Interrupt2,
                                               Registration->Source,
                                               EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH
                                               );
    if (EFI_ERROR (TriggerStatus)) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: the interrupt controller would not set SPI %u to level-high: %r\n",
        Private->Spi,
        TriggerStatus
        ));
    }
  } else {
    Registration->Interrupt2 = NULL;

    Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL, (VOID **)&Registration->Interrupt);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "Smhc: no hardware interrupt protocol on this platform; the data path needs one, so this driver cannot run\n"
        ));
      return Status;
    }

    DEBUG ((
      DEBUG_INFO,
      "Smhc: SPI %u via EFI_HARDWARE_INTERRUPT_PROTOCOL (no trigger-type call)\n",
      Private->Spi
      ));
  }

  if (Registration->Interrupt2 != NULL) {
    RegisterStatus = Registration->Interrupt2->RegisterInterruptSource (Registration->Interrupt2, Registration->Source, SmhcInterruptHandler);
  } else {
    RegisterStatus = Registration->Interrupt->RegisterInterruptSource (Registration->Interrupt, Registration->Source, SmhcInterruptHandler);
  }

  if (EFI_ERROR (RegisterStatus)) {
    DEBUG ((DEBUG_ERROR, "Smhc: SPI %u cannot be registered: %r\n", Private->Spi, RegisterStatus));
    Registration->Private = NULL;
    return RegisterStatus;
  }

  //
  // From here the handler is live, so the driver's record of that is set before
  // the next step can fail: unregistering has to be possible from that point on,
  // or a handler would be left behind for a driver that is about to be freed.
  //
  Registration->Installed = TRUE;

  //
  // Registering a handler already enables the source, but enabling it here
  // again is what makes the driver's dependence on it explicit and survives a
  // platform whose register call does not.
  //
  if (Registration->Interrupt2 != NULL) {
    EnableStatus = Registration->Interrupt2->EnableInterruptSource (Registration->Interrupt2, Registration->Source);
  } else {
    EnableStatus = Registration->Interrupt->EnableInterruptSource (Registration->Interrupt, Registration->Source);
  }

  //
  // The three calls' results are reported together with the distributor's state
  // they are supposed to have changed: a call that returned an error, a source
  // left disabled in the distributor, a line that is asserted but never
  // delivered and a line that is never asserted are four different faults, and
  // this line tells them apart.  A healthy boot prints it at DEBUG_INFO only,
  // so it costs nothing unless "the interrupt never arrives".  The
  // controller's own interrupt enable and mask are not read here - the
  // controller has not been initialized yet at this point - but just before the
  // first command, by SmhcReportInterruptState.
  //
  DEBUG ((
    DEBUG_INFO,
    "Smhc: interrupt setup: SetTriggerType %r, RegisterInterruptSource %r, EnableInterruptSource %r\n",
    TriggerStatus,
    RegisterStatus,
    EnableStatus
    ));

  SmhcReportGicState (Private, "after EnableInterruptSource");

  //
  // BISECTION (interrupt delivery): the distributor shows this SPI in group 0
  // with group-0 delivery disabled, so the line can never reach the NS CPU
  // interface.  A Non-secure write to IGROUPR is only defined to move the bit
  // from 0 to 1 (a Group-0 interrupt may or may not be assignable from NS, it
  // is the implementation's rule); try it and read the bit back - if it
  // sticks, the interrupt is deliverable without touching BL31.
  //
  {
    UINTN   Distributor;
    UINT32  GroupReg;
    UINT32  Bit;
    UINT32  Before;
    UINT32  After;

    Distributor = (UINTN)PcdGet64 (PcdGicDistributorBase);
    GroupReg    = A733_GICD_IGROUPR + 4U * (Private->Spi / 32U);
    Bit         = 1U << (Private->Spi % 32U);
    Before      = MmioRead32 (Distributor + GroupReg);
    MmioWrite32 (Distributor + GroupReg, Before | Bit);
    After       = MmioRead32 (Distributor + GroupReg);
    DEBUG ((
      DEBUG_INFO,
      "Smhc: IGROUPR+%x was 0x%08x, wrote bit %u, now 0x%08x (bit %a)\n",
      GroupReg,
      Before,
      (UINT32)(Private->Spi % 32U),
      After,
      ((After & Bit) != 0) ? "set: group 1 NS, interrupt deliverable" : "still clear: the SPI stays in group 0"
      ));
  }

  if (EFI_ERROR (EnableStatus)) {
    DEBUG ((DEBUG_ERROR, "Smhc: SPI %u cannot be enabled: %r\n", Private->Spi, EnableStatus));
    SmhcUninstallInterrupt (Private);
    return EnableStatus;
  }

  return EFI_SUCCESS;
}

/**
  Report the interrupt enable state the controller and the GIC are left in,
  which is only meaningful once the controller has been initialized.

  Both masks have to be right for an interrupt to happen at all, and they are
  set in two different places - the controller's global interrupt enable and its
  interrupt mask in SmhcInitController, the distributor's enable in the
  platform's GIC - so this says what each of them actually holds at the moment a
  command is about to be sent.  The GIC's own readback goes with it, since a
  source that is enabled inside the controller still goes nowhere if the
  distributor is not passing it on.

  @param[in] Private  Driver state; the controller must have been initialized.
  @param[in] When     Where in the driver this is being asked.
**/
VOID
SmhcReportInterruptState (
  IN A733_SMHC_PRIVATE  *Private,
  IN CONST CHAR8        *When
  )
{
  UINT32  Control;
  UINT32  Mask;

  Control = SmhcRead (Private->Base, A733_SMHC_CTRL);
  Mask    = SmhcRead (Private->Base, A733_SMHC_INTMASK);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: interrupt state %a: SMHC_CTRL %08x (DMA_ENB %u, INT_ENB %u, FIFO_AC_MOD %u), "
    "SMHC_INTMASK %08x (CC %u, DTC %u, errors %08x), irq count %u\n",
    When,
    Control,
    (Control & A733_SMHC_CTRL_DMA_ENB) != 0,
    (Control & A733_SMHC_CTRL_INT_ENB) != 0,
    (Control & A733_SMHC_CTRL_FIFO_AC_MOD) != 0,
    Mask,
    (Mask & A733_SMHC_RINT_CC) != 0,
    (Mask & A733_SMHC_RINT_DTC) != 0,
    Mask & A733_SMHC_RINT_ERRORS,
    Private->InterruptCount
    ));

  SmhcReportGicState (Private, When);
}

/**
  Unregister a controller's interrupt handler and disable the interrupt.

  Every failure path of the entry point goes through here, so a driver that
  publishes no protocol leaves no handler behind for an interrupt nothing will
  ever clear.  The registration is found by the controller it was made for, so
  tearing one controller down leaves the others' handlers exactly as they were.

  @param[in] Private  Driver state whose registration is to be removed.
**/
VOID
SmhcUninstallInterrupt (
  IN A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_INTERRUPT_REGISTRATION  *Registration;
  UINTN                             Index;

  Registration = NULL;
  for (Index = 0; Index < A733_SMHC_MAX_CONTROLLERS; Index++) {
    if (mSmhcInterrupts[Index].Installed && (mSmhcInterrupts[Index].Private == Private)) {
      Registration = &mSmhcInterrupts[Index];
      break;
    }
  }

  if (Registration == NULL) {
    return;
  }

  if (Registration->Interrupt2 != NULL) {
    Registration->Interrupt2->RegisterInterruptSource (Registration->Interrupt2, Registration->Source, NULL);
    Registration->Interrupt2->DisableInterruptSource (Registration->Interrupt2, Registration->Source);
  } else if (Registration->Interrupt != NULL) {
    Registration->Interrupt->RegisterInterruptSource (Registration->Interrupt, Registration->Source, NULL);
    Registration->Interrupt->DisableInterruptSource (Registration->Interrupt, Registration->Source);
  }

  //
  // The entry goes last, so the handler's lookup cannot see a half-cleared
  // registration: a source that arrives while the calls above are running still
  // finds its own instance, which is still valid here.
  //
  Registration->Installed  = FALSE;
  Registration->Private    = NULL;
  Registration->Interrupt  = NULL;
  Registration->Interrupt2 = NULL;
}

/**
  Allocate the descriptor ring and the bounce buffer.

  One page allocation serves every transfer the driver ever makes: the ring is
  A733_SMHC_DMA_DESC_COUNT four-word descriptors and the bounce buffer is the
  largest transfer a single command can move.  Both are used in place and reused
  for the next command, so a transfer allocates nothing.

  The allocation is made below 4 GB with AllocateMaxAddress because the
  descriptor address fields are 32 bits wide: the board has 8 GB of DRAM from 0,
  so an ordinary pool allocation can land where the controller cannot reach it.
  A page allocation is also cache-line aligned, which is what lets the ring and
  the bounce buffer be cleaned or invalidated without either touching the
  other's lines.

  Why not EmbeddedPkg's DmaLib (NonCoherentDmaLib): it would replace the four
  cache calls this file makes - and nothing else.  DmaAllocateBuffer gives no
  guarantee that the buffer is addressable by a 32-bit word-address descriptor,
  which is the constraint that shapes this pool in the first place, and the
  bounce decision below is that same constraint applied per transfer.  Swapping
  the four calls for map/unmap pairs around them would add plumbing to the path
  that carries the fastest mode this controller has while removing none of the
  address logic, so the cache calls stay, kept byte-identical to what that
  library does: write back before the device reads, invalidate after it writes,
  rounded out to cache lines.

  @param[in] Private  Driver state; Private->Dma is filled in.

  @retval EFI_SUCCESS           The pool is ready.
  @retval EFI_OUT_OF_RESOURCES  No room for the state, or none below 4 GB.
**/
EFI_STATUS
SmhcDmaAllocatePool (
  IN A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_DMA_STATE   *Dma;
  EFI_PHYSICAL_ADDRESS  Address;
  EFI_STATUS            Status;
  UINTN                 DescriptorBytes;
  UINTN                 BounceOffset;
  UINTN                 Line;
  UINTN                 Bytes;

  Dma = AllocateZeroPool (sizeof (*Dma));
  if (Dma == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Line            = ArmDataCacheLineLength ();
  DescriptorBytes = A733_SMHC_DMA_DESC_COUNT * sizeof (A733_SMHC_DESCRIPTOR);
  BounceOffset    = ALIGN_VALUE (DescriptorBytes, Line);
  Bytes           = BounceOffset + A733_SMHC_MAX_TRANSFER_BYTES;

  Address = A733_SMHC_DMA_POOL_LIMIT;
  Status  = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, EFI_SIZE_TO_PAGES (Bytes), &Address);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: cannot allocate %u bytes of DMA pool below 4 GB: %r\n",
      (UINT32)Bytes,
      Status
      ));
    FreePool (Dma);
    return Status;
  }

  Dma->Base        = Address;
  Dma->Pages       = EFI_SIZE_TO_PAGES (Bytes);
  Dma->Descriptors = (A733_SMHC_DESCRIPTOR *)(UINTN)Address;
  Dma->Bounce      = (UINT8 *)(UINTN)Address + BounceOffset;

  //
  // AllocatePages hands back whatever the pages held before, and a descriptor
  // whose ownership bit happened to be set would be a descriptor the IDMAC
  // could walk into before this driver has filled the ring in.  Nothing uses
  // more than the descriptors a transfer fills, but zeroing the ring costs one
  // 4 KB write and removes that question entirely.
  //
  ZeroMem (Dma->Descriptors, DescriptorBytes);

  //
  // The ring has to be word aligned because a descriptor's own address is
  // decoded from a word address, and the bounce buffer has to be line aligned
  // because a read into it is invalidated by cache line.  Both follow from the
  // page allocation and the offset arithmetic above.
  //
  ASSERT (((UINTN)Dma->Descriptors & 0x3U) == 0);
  ASSERT (((UINTN)Dma->Bounce & (Line - 1U)) == 0);

  Private->Dma = Dma;

  DEBUG ((
    DEBUG_INFO,
    "Smhc: DMA pool at %08x: %u descriptors (%u bytes each), bounce buffer %u bytes at %08x\n",
    (UINT32)Address,
    A733_SMHC_DMA_DESC_COUNT,
    A733_SMHC_DMA_CHUNK_BYTES,
    A733_SMHC_MAX_TRANSFER_BYTES,
    (UINT32)(UINTN)Dma->Bounce
    ));

  return EFI_SUCCESS;
}

/**
  Release the descriptor ring and the bounce buffer.

  @param[in] Private  Driver state.
**/
VOID
SmhcDmaFreePool (
  IN A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_DMA_STATE  *Dma;

  Dma = Private->Dma;
  if (Dma == NULL) {
    return;
  }

  FreePages ((VOID *)(UINTN)Dma->Base, Dma->Pages);
  FreePool (Dma);
  Private->Dma = NULL;
}

/**
  Fill the descriptor ring in for one transfer.

  One descriptor per A733_SMHC_DMA_CHUNK_BYTES of the buffer, in order, each
  owned by the IDMAC and chained to the next; the first and the last are
  flagged, which is what bounds the transfer, and only the last one allows the
  IDMAC to raise its interrupt - so a transfer reports itself once, when its
  data has moved.

  @param[in] Dma     The pool.
  @param[in] Buffer  Physical address the IDMAC moves the bytes to or from.
  @param[in] Length  Byte count, already known to fit the pool.

  @return The number of descriptors used.
**/
STATIC
UINTN
SmhcBuildDescriptors (
  IN A733_SMHC_DMA_STATE  *Dma,
  IN UINTN                Buffer,
  IN UINT32               Length
  )
{
  A733_SMHC_DESCRIPTOR  *Descriptor;
  UINT32                Control;
  UINT32                Chunk;
  UINT32                Offset;
  UINTN                 Count;
  UINTN                 Index;

  ASSERT (Length != 0);
  ASSERT (Length <= A733_SMHC_MAX_TRANSFER_BYTES);
  ASSERT ((Buffer & 0x3U) == 0);

  Count = (Length + A733_SMHC_DMA_CHUNK_BYTES - 1) / A733_SMHC_DMA_CHUNK_BYTES;

  for (Index = 0; Index < Count; Index++) {
    Offset = (UINT32)(Index * A733_SMHC_DMA_CHUNK_BYTES);
    Chunk  = MIN ((UINT32)A733_SMHC_DMA_CHUNK_BYTES, Length - Offset);

    Control = A733_SMHC_DES_CHAIN | A733_SMHC_DES_OWN;
    if (Index == 0) {
      Control |= A733_SMHC_DES_FIRST;
    }

    if (Index == (Count - 1)) {
      Control |= A733_SMHC_DES_LAST;
    } else {
      Control |= A733_SMHC_DES_NO_INT;
    }

    Descriptor          = &Dma->Descriptors[Index];
    Descriptor->Control = Control;
    Descriptor->Size    = Chunk;
    Descriptor->Buffer  = A733_SMHC_DMA_WORD_ADDRESS (Buffer + Offset);
    Descriptor->Next    = (Index == (Count - 1))
                          ? 0
                          : A733_SMHC_DMA_WORD_ADDRESS (&Dma->Descriptors[Index + 1]);
  }

  return Count;
}

/**
  Decide whether a command's buffer can be described to the IDMAC directly.

  Three conditions force a bounce, and the reason is reported so a log says
  which one it was: the descriptor's address half cannot hold an address whose
  last byte is not below 4 GB, a word address cannot describe a buffer that is
  not DWORD aligned, and a read that does not fill whole cache lines could not
  be invalidated afterwards without discarding the neighbours sharing its first
  and last lines.  (A write only has to be cleaned before the transfer, which
  is safe on whole lines, so it is moved directly whenever its address fits.)

  @param[in]  Command  Command whose Buffer/Length/Read to test.
  @param[in]  Line     Cache line length in bytes.
  @param[out] Reason   Set to why a bounce is needed, or NULL when it is not.

  @retval TRUE   The transfer has to go through the pool's bounce buffer.
  @retval FALSE  The IDMAC can move the caller's buffer itself.
**/
STATIC
BOOLEAN
SmhcBufferNeedsBounce (
  IN  A733_SMHC_COMMAND  *Command,
  IN  UINTN              Line,
  OUT CONST CHAR8        **Reason
  )
{
  UINTN  Source;

  Source = (UINTN)Command->Buffer;

  if ((UINT64)Source + Command->Length > (UINT64)SIZE_4GB) {
    *Reason = "beyond the 32-bit descriptor address";
    return TRUE;
  }

  if ((Source & 0x3U) != 0) {
    *Reason = "not DWORD aligned";
    return TRUE;
  }

  if (Command->Read &&
      (((Source & (Line - 1U)) != 0) || ((Command->Length & (Line - 1U)) != 0)))
  {
    *Reason = "read range would share a cache line with its neighbours";
    return TRUE;
  }

  *Reason = NULL;
  return FALSE;
}

/**
  Hand a data command's transfer to the IDMAC.

  Runs after SMHC_BLKSIZ and SMHC_BYTCNT and before SMHC_CMD, in the order the
  manual's own programming steps give (11.3.6.2 step 1/2): reset the DMA
  engine, enable it, point it at the descriptor list, then let the command move
  the data it describes.

  Which buffer the hardware moves is decided here, and it is not always the
  caller's:

  - A buffer whose last byte is not below 4 GB cannot be described at all, and
    one that is not DWORD aligned cannot be described in a word address; either
    is bounced through the pool.
  - A read whose range does not fill whole cache lines cannot be invalidated
    without discarding whatever shares its first and last lines, so it is
    bounced as well.  (A write's range only has to be cleaned, which is safe on
    whole lines, so it is moved directly whenever the address fits.)

  The bounce costs one CopyMem per transfer and only for buffers that would
  otherwise be wrong; every transfer this stack sends in practice comes from a
  page or pool allocation and is DWORD aligned.

  @param[in,out] Private  Driver state.
  @param[in,out] Command  Command whose Buffer/Length/Read to set up.

  @retval EFI_SUCCESS       The IDMAC is ready for the command about to be sent.
  @retval EFI_TIMEOUT       A reset bit did not self-clear.
  @retval EFI_UNSUPPORTED   The transfer is larger than the descriptor ring.
**/
STATIC
EFI_STATUS
SmhcDmaStart (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN OUT A733_SMHC_COMMAND  *Command
  )
{
  A733_SMHC_DMA_STATE  *Dma;
  UINTN                Base;
  UINTN                Source;
  UINTN                Target;
  UINTN                Line;
  UINTN                Count;
  BOOLEAN              Bounced;
  CONST CHAR8          *Reason;

  Base   = Private->Base;
  Dma    = (A733_SMHC_DMA_STATE *)Private->Dma;
  Source = (UINTN)Command->Buffer;
  Line   = ArmDataCacheLineLength ();

  if (Command->Length > A733_SMHC_MAX_TRANSFER_BYTES) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: cmd %u moves %u bytes, more than the %u-byte descriptor ring holds\n",
      Command->Index,
      Command->Length,
      A733_SMHC_MAX_TRANSFER_BYTES
      ));
    return EFI_UNSUPPORTED;
  }

  Reason  = NULL;
  Bounced = SmhcBufferNeedsBounce (Command, Line, &Reason);

  if (Bounced) {
    Target = (UINTN)Dma->Bounce;

    //
    // Only one direction needs the copy up front.  A read fills the bounce
    // buffer from the card and the caller's buffer afterwards, so nothing is
    // copied yet; a write is the caller's data, and the IDMAC has to be given
    // it before the command starts.
    //
    if (!Command->Read) {
      CopyMem (Dma->Bounce, Command->Buffer, Command->Length);
    }

    if (PcdGetBool (PcdA733SmhcDumpEnable)) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: cmd %u: %u bytes bounce through the DMA pool (%a)\n",
        Command->Index,
        Command->Length,
        Reason
        ));
    }
  } else {
    Target = Source;
  }

  Count = SmhcBuildDescriptors (Dma, Target, Command->Length);

  //
  // The data range is cleaned before the transfer in both directions, for two
  // different reasons that happen to need the same operation.  On a write the
  // IDMAC reads the caller's data, so the CPU's copy has to reach memory first.
  // On a read nothing has to reach memory, but leaving a dirty line under a
  // range the card is about to overwrite would let the cache evict that stale
  // line mid-transfer, straight over the card's data - so the range is written
  // back and left clean, and the invalidate after the transfer then has nothing
  // to lose.  The descriptor list needs the same treatment as a write: the
  // IDMAC is about to read what this driver just wrote there.
  //
  WriteBackDataCacheRange (Dma->Descriptors, Count * sizeof (A733_SMHC_DESCRIPTOR));
  WriteBackDataCacheRange ((VOID *)Target, Command->Length);

  //
  // Reset the DMA engine around the reconfiguration, so a descriptor the IDMAC
  // still owned from a previous transfer cannot be walked into, then enable it
  // with fixed bursts as the manual's steps do.
  //
  SmhcWrite (Base, A733_SMHC_CTRL, SmhcRead (Base, A733_SMHC_CTRL) | A733_SMHC_CTRL_DMA_RST);
  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_CTRL, A733_SMHC_CTRL_DMA_RST, A733_SMHC_RESET_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "Smhc: DMA reset did not self-clear before cmd %u\n", Command->Index));
    return EFI_TIMEOUT;
  }

  SmhcWrite (Base, A733_SMHC_DMAC, A733_SMHC_DMAC_IDMAC_RST);
  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_DMAC, A733_SMHC_DMAC_IDMAC_RST, A733_SMHC_RESET_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "Smhc: IDMAC reset did not self-clear before cmd %u\n", Command->Index));
    return EFI_TIMEOUT;
  }

  //
  // The descriptor list base is written before the IDMAC is enabled, not after
  // as the manual's step 1/2 order reads: an IDMAC that was enabled first could
  // fetch a descriptor from wherever DLBA still pointed, and there is no reason
  // to leave it a window to do that.
  //
  SmhcWrite (Base, A733_SMHC_DLBA, A733_SMHC_DMA_WORD_ADDRESS (Dma->Descriptors));
  SmhcWrite (Base, A733_SMHC_IDIE, A733_SMHC_IDIE_VALUE);
  SmhcWrite (Base, A733_SMHC_FIFOTH, A733_SMHC_FIFOTH_VALUE);
  SmhcWrite (Base, A733_SMHC_DMAC, A733_SMHC_DMAC_VALUE);

  return EFI_SUCCESS;
}

/**
  Print everything a failed data transfer left behind, while it is still there.

  Called from SmhcDmaFinish before SmhcResetDataPath, because the reset wipes
  exactly the evidence this exists to collect.  Three groups, and each answers a
  different question:

  - The register values the transfer was programmed with and still reads back:
    SMHC_CMD (both the word this driver wrote, with START set, and the live
    register, whose START has cleared), SMHC_BLKSIZ, SMHC_BYTCNT, SMHC_CTRL and
    SMHC_CLKDIV.  A multi-block transfer that is programmed with the wrong block
    size, byte count, direction, DATA_TRANS or STOP_CMD_FLAG shows up here, and
    so does a CTRL whose DMA_ENB has gone away or whose FIFO_AC_MOD is on.

  - The IDMAC's descriptor ring: how many descriptors the transfer used, how many
    the IDMAC still owns (OWN set - a transfer it stopped walking) and how many
    it flagged (ERROR), then the words themselves for the first and last few.
    DES1/DES2/DES3 are what a wrong address convention would corrupt.

  - The address convention itself: SMHC_DLBA read back against the descriptor
    pool shifted as the driver writes it, and the first descriptor's buffer
    address against the buffer the transfer actually used.  A word/byte address
    mismatch makes those two disagree, which is otherwise invisible.

  Nothing here decides anything; it only reports.  The caller prints it under the
  same Quiet gate as the rest of the failure decoding.

  @param[in] Private  Driver state.
  @param[in] Command  The command whose data phase failed.
  @param[in] Target   The buffer the descriptors describe (the caller's, or the
                      bounce buffer), as SmhcDmaFinish computed it.
**/
STATIC
VOID
SmhcDumpFailedTransfer (
  IN A733_SMHC_PRIVATE  *Private,
  IN A733_SMHC_COMMAND  *Command,
  IN UINTN              Target
  )
{
  A733_SMHC_DMA_STATE   *Dma;
  A733_SMHC_DESCRIPTOR  *Descriptor;
  UINTN                 Base;
  UINTN                 Index;
  UINTN                 Count;
  UINT32                Owned;
  UINT32                Errored;
  UINT32                Dlba;
  UINT32                ExpectedDlba;
  UINT32                Ctrl;
  //
  // The first four and the last four descriptors are printed when the ring is
  // longer than that: the head says whether the IDMAC started the list and the
  // tail says how far it got, and a 1 MiB transfer's 256 descriptors are far too
  // many for a console line each.
  //
  UINTN                 Head = 4;
  UINTN                 Tail = 4;

  Base  = Private->Base;
  Dma   = (A733_SMHC_DMA_STATE *)Private->Dma;
  Count = (Command->Length + A733_SMHC_DMA_CHUNK_BYTES - 1) / A733_SMHC_DMA_CHUNK_BYTES;

  //
  // What the transfer was asked to do, and what the controller still says about
  // it.  The programmed CMD word is recomputed rather than remembered: it is the
  // same function SmhcSendCommand wrote through, so the two cannot disagree about
  // what was sent, only about the START bit the hardware has cleared.
  //
  Dlba = SmhcRead (Base, A733_SMHC_DLBA);
  Ctrl = SmhcRead (Base, A733_SMHC_CTRL);

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: cmd %u programmed: cmd %08x blksiz %08x bytecnt %08x ctrl %08x clkdiv %08x\n",
    Command->Index,
    SmhcCommandRegister (Command),
    SmhcRead (Base, A733_SMHC_BLKSIZ),
    SmhcRead (Base, A733_SMHC_BYTCNT),
    Ctrl,
    SmhcRead (Base, A733_SMHC_CLKDIV)
    ));

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: cmd %u read back now: cmd %08x status %08x dmac %08x dlba %08x ndma_ctl %08x\n",
    Command->Index,
    SmhcRead (Base, A733_SMHC_CMD),
    SmhcRead (Base, A733_SMHC_STATUS),
    SmhcRead (Base, A733_SMHC_DMAC),
    Dlba,
    SmhcRead (Base, A733_SMHC_NDMA_MODE_CTL)
    ));

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: cmd %u ctrl bits: DMA_ENB %u, FIFO_AC_MOD %u (0 is what the DMA bus needs), INT_ENB %u, DDR %u\n",
    Command->Index,
    (Ctrl & A733_SMHC_CTRL_DMA_ENB) ? 1U : 0U,
    (Ctrl & A733_SMHC_CTRL_FIFO_AC_MOD) ? 1U : 0U,
    (Ctrl & A733_SMHC_CTRL_INT_ENB) ? 1U : 0U,
    (Ctrl & A733_SMHC_CTRL_DDR_MODE) ? 1U : 0U
    ));

  //
  // The descriptor ring's state, and then the words.  Count is at least 1 for a
  // data command, so the loops are safe.
  //
  Owned   = 0;
  Errored = 0;
  for (Index = 0; Index < Count; Index++) {
    if ((Dma->Descriptors[Index].Control & A733_SMHC_DES_OWN) != 0) {
      Owned++;
    }

    if ((Dma->Descriptors[Index].Control & A733_SMHC_DES_ERROR) != 0) {
      Errored++;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: cmd %u %u descriptors, up to %u bytes each (the driver's DES1 mask %08x): "
    "%u still owned by the IDMAC, %u with the error flag\n",
    Command->Index,
    (UINT32)Count,
    A733_SMHC_DMA_CHUNK_BYTES,
    A733_SMHC_DES_SIZE_MASK,
    Owned,
    Errored
    ));

  for (Index = 0; Index < Count; Index++) {
    if ((Count > (Head + Tail)) && (Index >= Head) && (Index < (Count - Tail))) {
      continue;
    }

    Descriptor = &Dma->Descriptors[Index];
    DEBUG ((
      DEBUG_ERROR,
      "Smhc:   des[%u] ctrl %08x size %u buffer %08x next %08x\n",
      (UINT32)Index,
      Descriptor->Control,
      Descriptor->Size,
      Descriptor->Buffer,
      Descriptor->Next
      ));
  }

  //
  // The address convention, stated as the comparison it is: DLBA has to be the
  // descriptor pool's address shifted the way the driver shifted it, and the
  // first descriptor's buffer has to be the transfer buffer shifted the same
  // way.  If either disagrees, the shift is wrong for this controller and every
  // descriptor fetch is aimed at the wrong memory.
  //
  ExpectedDlba = A733_SMHC_DMA_WORD_ADDRESS (Dma->Descriptors);

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: cmd %u descriptor addresses: pool %08x, DLBA %08x, pool >> %u = %08x (%a); first buffer %08x, target %08x, target >> %u = %08x (%a)\n",
    Command->Index,
    (UINT32)(UINTN)Dma->Descriptors,
    Dlba,
    A733_SMHC_DMA_ADDRESS_SHIFT,
    ExpectedDlba,
    (Dlba == ExpectedDlba) ? "match" : "MISMATCH",
    Dma->Descriptors[0].Buffer,
    (UINT32)Target,
    A733_SMHC_DMA_ADDRESS_SHIFT,
    (UINT32)((UINT64)Target >> A733_SMHC_DMA_ADDRESS_SHIFT),
    (Dma->Descriptors[0].Buffer == (UINT32)((UINT64)Target >> A733_SMHC_DMA_ADDRESS_SHIFT)) ? "match" : "MISMATCH"
    ));
}

/**
  Take the transfer back from the IDMAC and make memory agree with it again.

  Two things have to happen after the controller says the byte count has moved,
  and both are about caches rather than the bus:

  - A read's data was written to memory by the IDMAC, so the CPU's copy of that
    range is stale and has to be invalidated before anything reads it - the
    bounce buffer when the transfer was bounced, the caller's buffer otherwise.
    A write needs nothing here: the card only ever read memory.
  - The descriptor list was written by the IDMAC too (it clears each descriptor's
    ownership bit and can set its error flag), so it is invalidated as well
    before the last descriptor is read back for the failure log.

  A failed transfer then goes through SmhcResetDataPath: the controller's state
  after a DMA failure is unknown - an IDMAC stopped mid-descriptor, a half-full
  FIFO, a command FSM parked on a transfer that never finished - and the next
  command is not sent until the driver has re-initialized it.

  @param[in] Private  Driver state.
  @param[in] Command  The command that was sent.
  @param[in] Status   What SmhcSendCommand decided about it.
**/
STATIC
VOID
SmhcDmaFinish (
  IN A733_SMHC_PRIVATE  *Private,
  IN A733_SMHC_COMMAND  *Command,
  IN EFI_STATUS         Status
  )
{
  A733_SMHC_DMA_STATE   *Dma;
  A733_SMHC_DESCRIPTOR  *Last;
  UINTN                 Base;
  UINTN                 Target;
  UINTN                 Line;
  UINTN                 Count;
  BOOLEAN               Bounced;
  CONST CHAR8           *Reason;

  Base    = Private->Base;
  Dma     = (A733_SMHC_DMA_STATE *)Private->Dma;
  Line    = ArmDataCacheLineLength ();
  Bounced = SmhcBufferNeedsBounce (Command, Line, &Reason);
  Target  = Bounced ? (UINTN)Dma->Bounce : (UINTN)Command->Buffer;
  Count   = (Command->Length + A733_SMHC_DMA_CHUNK_BYTES - 1) / A733_SMHC_DMA_CHUNK_BYTES;

  if (Command->Read) {
    InvalidateDataCacheRange ((VOID *)Target, Command->Length);

    if (Bounced) {
      CopyMem (Command->Buffer, Dma->Bounce, Command->Length);
    }
  }

  InvalidateDataCacheRange (Dma->Descriptors, Count * sizeof (A733_SMHC_DESCRIPTOR));

  Last = &Dma->Descriptors[Count - 1];

  if (EFI_ERROR (Status)) {
    //
    // Quiet is set while a burst of commands is expected to fail on purpose (the
    // sample-delay sweep's taps), so the decoding below is one line per sweep
    // rather than one long line per attempt.  Nothing that decides anything is
    // suppressed, only the printing.
    //
    if (!Private->Quiet) {
      DEBUG ((
        DEBUG_ERROR,
        "Smhc: cmd %u data phase failed: %r, %u bytes %a, last descriptor %08x\n",
        Command->Index,
        Status,
        Command->Length,
        Command->Read ? "read" : "written",
        Last->Control
        ));

      if ((Last->Control & A733_SMHC_DES_ERROR) != 0) {
        DEBUG ((DEBUG_ERROR, "Smhc: the IDMAC set the error flag on the last descriptor\n"));
      }

      //
      // Everything the failed transfer left in the controller, printed here
      // because SmhcResetDataPath below is about to wipe it (see
      // SmhcDumpFailedTransfer).  Unconditional on a real failure, not behind the
      // per-command dump PCD: this is the diagnosis, not a command trace.
      //
      SmhcDumpFailedTransfer (Private, Command, Target);
    }

    SmhcResetDataPath (Base);
    Private->DataFault = TRUE;
  }
}

/**
  Report whether the interrupt's record already answers for a command.

  "Answered" means an error bit the caller treats as fatal, the completion bit
  the command is shaped to expect, or an IDMAC failure - the three things that
  end a wait.  Which of them it is stays the caller's decision; this only says
  whether there is anything to decide on yet.

  @param[in] Private    Driver state.
  @param[in] WaitMask   Bits of SMHC_RINTSTS that mean "this is done".
  @param[in] ErrorMask  Bits of SMHC_RINTSTS that mean "this failed".

  @retval TRUE   The record holds an answer.
  @retval FALSE  Nothing has been reported yet.
**/
STATIC
BOOLEAN
SmhcCompletionReported (
  IN A733_SMHC_PRIVATE  *Private,
  IN UINT32             WaitMask,
  IN UINT32             ErrorMask
  )
{
  return ((Private->IntStatus & ErrorMask) != 0) ||
         ((Private->IntStatus & WaitMask) == WaitMask) ||
         ((Private->IdstStatus & A733_SMHC_IDST_DMA_FATAL) != 0);
}

/**
  Wait for the controller's completion to be reported.

  Nothing is read from the controller on the normal path: the handler has
  already recorded the raw status bits, and the loop below only waits for that
  record to contain an error bit or the completion bit the command is waiting
  on.

  When the deadline arrives first, the interrupt has not reported this command,
  and the controller is read once to find out whether the command finished
  anyway.  That read goes into the same record the handler fills and nothing
  else: the decision that follows it is the caller's single decision, made from
  the same status bits it would have used had the interrupt arrived, so the poll
  is a fallback inside one mechanism and not a second one that could disagree
  with it.  It is reported at DEBUG_ERROR because a completion that had to be
  polled means the platform is not delivering this controller's interrupt, which
  is a firmware defect and not a card one.

  @param[in,out] Private    Driver state; IntStatus and IdstStatus are read.
  @param[in]     WaitMask   Bits of SMHC_RINTSTS that mean "this is done".
  @param[in]     ErrorMask  Bits of SMHC_RINTSTS that mean "this failed".
  @param[in]     TimeoutUs  Budget.
**/
STATIC
VOID
SmhcWaitCompletion (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             WaitMask,
  IN     UINT32             ErrorMask,
  IN     UINT64             TimeoutUs
  )
{
  A733_SMHC_DEADLINE  Deadline;
  UINT32              Polled;

  SmhcArmDeadline (&Deadline, TimeoutUs);

  for ( ; ;) {
    if (SmhcCompletionReported (Private, WaitMask, ErrorMask)) {
      return;
    }

    if (SmhcDeadlineExpired (&Deadline)) {
      break;
    }

    //
    // Read the controller's own status on every pass: this board's SMHC
    // interrupt is never delivered (the handler records nothing, irq count
    // stays 0), and a status record that only changes on interrupt leaves the
    // wait blind until the deadline - a transfer that completed right after
    // the command is then only found by the single post-deadline poll, and a
    // caller whose budget is shorter than the command's own latency reads a
    // completed command as a timeout.  Polling here makes the wait independent
    // of the interrupt path; a working handler still wins the race through the
    // event check below, and both paths only ever merge status into the same
    // record and clear what they read.
    //
    Polled = SmhcRead (Private->Base, A733_SMHC_RINTSTS);
    if (Polled != 0) {
      Private->IntStatus |= Polled;
      SmhcWrite (Private->Base, A733_SMHC_RINTSTS, Polled);
    }

    //
    // A signalled event means the handler has just recorded more status, so
    // look at it again rather than sleeping; otherwise this step decides how
    // often the record is re-read while the interrupt is on its way.
    //
    if (gBS->CheckEvent (Private->CompletionEvent) == EFI_SUCCESS) {
      continue;
    }

    MicroSecondDelay (A733_SMHC_POLL_STEP_US);
  }

  //
  // The deadline came first.  Take what the controller is holding and then
  // clear it: this interrupt is level-sensitive, so leaving a bit set keeps the
  // line asserted, which parks the interrupt as pending in the distributor and
  // stops every later one from being delivered.  The caller's decision is made
  // from what was read here.
  //
  Polled = SmhcRead (Private->Base, A733_SMHC_RINTSTS);
  Private->IntStatus |= Polled;
  if (Polled != 0) {
    SmhcWrite (Private->Base, A733_SMHC_RINTSTS, Polled);
  }

  //
  // Say which of the two answers the poll produced, with the same wording the
  // completion case needs: a command the poll found finished is the firmware
  // defect this fallback exists to expose, and one it found failed says the
  // interrupt was missing for that too.
  //
  if ((Private->IntStatus & WaitMask) == WaitMask) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: completion polled, interrupt did not arrive (rint %08x, irq count %u)\n",
      Polled,
      Private->InterruptCount
      ));
  } else if (SmhcCompletionReported (Private, WaitMask, ErrorMask)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: failure polled, interrupt did not arrive (rint %08x, irq count %u)\n",
      Polled,
      Private->InterruptCount
      ));
  }
}

/**
  Put the data path back into a state a command can be sent from.

  Runs after a data phase failed: the DMA engine, the FIFO and the controller
  are each reset, and both status registers are cleared, so nothing about the
  failed transfer is left for the next command to trip over.  The controller is
  left reset and unconfigured on purpose - SmhcRecoverController is what
  configures it again, and until a command has been through there the driver
  treats the controller as unusable (Private->DataFault).

  @param[in] Base  Controller base.
**/
STATIC
VOID
SmhcResetDataPath (
  IN UINTN  Base
  )
{
  SmhcWrite (Base, A733_SMHC_CTRL, SmhcRead (Base, A733_SMHC_CTRL) | A733_SMHC_CTRL_DMA_RST);
  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_CTRL, A733_SMHC_CTRL_DMA_RST, A733_SMHC_RESET_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "Smhc: DMA reset did not self-clear during recovery\n"));
  }

  SmhcWrite (Base, A733_SMHC_DMAC, A733_SMHC_DMAC_IDMAC_RST);
  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_DMAC, A733_SMHC_DMAC_IDMAC_RST, A733_SMHC_RESET_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "Smhc: IDMAC reset did not self-clear during recovery\n"));
  }

  if (EFI_ERROR (SmhcFifoReset (Base))) {
    DEBUG ((DEBUG_ERROR, "Smhc: FIFO reset did not self-clear during recovery\n"));
  }

  SmhcWrite (Base, A733_SMHC_CTRL, SmhcRead (Base, A733_SMHC_CTRL) | A733_SMHC_CTRL_SOFT_RST);
  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_CTRL, A733_SMHC_CTRL_SOFT_RST, A733_SMHC_RESET_TIMEOUT_US))) {
    DEBUG ((DEBUG_ERROR, "Smhc: controller soft reset did not self-clear during recovery\n"));
  }

  SmhcWrite (Base, A733_SMHC_RINTSTS, 0xFFFFFFFFU);
  SmhcWrite (Base, A733_SMHC_IDST, A733_SMHC_IDST_CLEAR_MASK);
}

/**
  Configure a reset controller again for the card state the driver holds.

  A controller reset takes the global control, the interrupt mask, the FIFO
  water level, the timeouts and the bus width with it, and can take the card
  clock as well, so all of them are put back: the width the card is actually
  using, the DDR mode bit when the card is in a DDR timing, and the same divider
  SMHC_CLKDIV was last given, delivered through the clock-change command again
  because writing the divider alone does not hand it to the card.

  An HS400 bus needs more than the DDR bit: EDSD, CSDC, SFC and the swept DS_DL
  tap are all reset with the controller, and a sweep whose later taps ran without
  them would be measured through a different sampler than the one it is choosing
  a tap for.  Private->HostHs400Timing is what says so, and it is re-applied after
  the DDR set because SmhcSetDdrMode writes CSDC and DS_DL itself.

  @param[in,out] Private  Driver state.

  @retval EFI_SUCCESS  The controller is configured as it was.
  @retval other        A reset bit never self-cleared.
**/
STATIC
EFI_STATUS
SmhcRecoverController (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS  Status;

  Status = SmhcInitController (Private->Base);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SmhcSetBusWidth (Private->Base, Private->BusWidth);
  SmhcSetDdrMode (Private, Private->Ddr);

  if (Private->HostHs400Timing) {
    SmhcSetHs400Mode (Private, TRUE);
  }

  SmhcSetClock (Private, Private->ClockDivisor);

  return EFI_SUCCESS;
}

/**
  Send one command and, when it has data, have the IDMAC move that data.

  A command's outcome is decided in one place and from one source: the raw
  status bits the interrupt handler recorded, with one fallback - if the wait
  reaches its deadline without the interrupt having reported anything, the
  controller's status is read once and added to that same record before the
  decision is made (see SmhcWaitCompletion).  Either way, what the command ended
  on is read afterwards, from the record, so a command that succeeded, one the
  card refused and one that timed out are told apart by the same evidence every
  time, and a lost interrupt shows up as a DEBUG_ERROR line rather than as a
  wrong result.

  Completion the command asks for differs by its shape: a command without data
  ends at command-complete, a data command at data-transfer-complete, and an
  R1b command only once the card has released the busy line.  The first two are
  interrupt-driven (with the polled fallback); the last is a level in
  SMHC_STATUS that the manual has an interrupt bit for (R1B_BUSY_CLR) which this
  driver does not enable - CMD6 and CMD7 are what identification stands on, and
  changing how they are waited on without hardware to check it against is not
  worth the risk.

  @param[in,out] Private    Driver state.
  @param[in,out] Command    Command to send; its Response is filled in when the
                            command has an answer.
  @param[in]     TimeoutUs  Budget for the command and its data.

  @retval EFI_SUCCESS       The command was sent and answered.
  @retval EFI_TIMEOUT       The controller or the card did not answer in time.
  @retval EFI_DEVICE_ERROR  The card, the bus or the IDMAC reported an error.
  @retval EFI_UNSUPPORTED   The transfer is larger than the descriptor ring.
**/
EFI_STATUS
SmhcSendCommand (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN OUT A733_SMHC_COMMAND  *Command,
  IN     UINT64             TimeoutUs
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT32      IntStatus;
  UINT32      IdmaStatus;
  UINT32      WaitMask;
  UINT32      Errors;
  EFI_TPL     OldTpl;
  BOOLEAN     Read;

  mSmhcPhaseCurrent = NULL;

  if ((Private == NULL) || (Command == NULL) ||
      ((Command->Length != 0) && (Command->Buffer == NULL)))
  {
    return EFI_INVALID_PARAMETER;
  }

  SmhcPhaseBegin (Command->Index, Command->Length);

  Base = Private->Base;
  Read = (Command->Length != 0) && Command->Read;

  //
  // A data transfer on the identification divider is the configuration the
  // board showed this controller cannot do: the module clock is then far above
  // the card clock, and a 512-byte read came back with a data end-bit error and
  // a valid CRC on it (see A733SmhcDxe.h).  The divider is a level, not a
  // completion, so it is read here when the phase is about to start; a plan
  // whose identification divider is the bring-up one (the baseline plan) is
  // fine, which is why the check compares the two.
  //
  if ((Command->Length != 0) &&
      (Private->IdentDivisor != Private->BringUpDivisor) &&
      ((SmhcRead (Base, A733_SMHC_CLKDIV) & A733_SMHC_CLKDIV_CCLK_DIV_MASK) == Private->IdentDivisor))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: cmd %u carries %u bytes while the card clock is still on the identification divider %u; "
      "a data phase on that divider mis-frames on this hardware (this is a driver ordering fault)\n",
      Command->Index,
      Command->Length,
      Private->IdentDivisor
      ));
  }

  //
  // A controller that a failed data phase was reset out of is not trusted here
  // until it has been configured again and a command has since completed: the
  // flag stays set until the end of this function.
  //
  if (Private->DataFault) {
    Status = SmhcRecoverController (Private);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Errors = SmhcErrorMask (Read);
  if (Command->Length != 0) {
    //
    // A response timeout on a data command is not the transfer's verdict.  This
    // controller raises RTO before the command's own completion is reported
    // (the vendor driver's answer is to keep waiting for COMMAND_DONE when it
    // sees RTO on its own), and aborting the data phase on it throws away a
    // transfer the card did accept.  The wait below is therefore left to the
    // data-complete bit and the deadline; SmhcMapErrors still reports a genuine
    // one when the data never moves.
    //
    Errors &= ~A733_SMHC_RINT_RTO_BACK;
  }

  //
  // The previous command must have left the command FSM idle.  With R1b busy
  // detection or an auto CMD12 that outlives the interrupt which reported the
  // command as complete, the FSM is still finishing when the driver gets here.
  // This is a level in SMHC_STATUS, not a completion, so it stays a bounded
  // read of the register.
  //
  if (EFI_ERROR (SmhcWaitClear (Base, A733_SMHC_STATUS, A733_SMHC_STATUS_FSM_BUSY, TimeoutUs))) {
    if (!Private->Quiet) {
      DEBUG ((DEBUG_ERROR, "Smhc: command FSM is still busy before cmd %u\n", Command->Index));
    }

    SmhcPhaseMark (A733_SMHC_PHASE_FSM);
    SmhcPhaseMark (A733_SMHC_PHASE_END);
    return EFI_TIMEOUT;
  }

  SmhcPhaseMark (A733_SMHC_PHASE_FSM);

  //
  // Both status registers are write-1-to-clear and sticky, so the previous
  // command's bits go before this one starts, or they would be read as this
  // one's result.  The record the handler accumulates goes with them, and so
  // does a signal it may have left behind.
  //
  // The interrupt is masked across this, the one place in the driver where it
  // has to be.  A handler that had already read the old status when the clear
  // below ran would write that status back and accumulate it as this command's,
  // and a stale command-complete bit read as this command's would have it look
  // finished before it was sent.  Raising to TPL_HIGH_LEVEL closes that window:
  // no interrupt can start while the TPL holds, and one already running has
  // finished before this code resumes.  Nothing is lost by the masking - the
  // controller latches its status and the GIC keeps the source pending.
  //
  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  SmhcWrite (Base, A733_SMHC_RINTSTS, 0xFFFFFFFFU);
  SmhcWrite (Base, A733_SMHC_IDST, A733_SMHC_IDST_CLEAR_MASK);
  Private->IntStatus  = 0;
  Private->IdstStatus = 0;
  gBS->CheckEvent (Private->CompletionEvent);
  gBS->RestoreTPL (OldTpl);

  SmhcPhaseMark (A733_SMHC_PHASE_HANDOFF);

  if (Command->Length != 0) {
    if (EFI_ERROR (SmhcFifoReset (Base))) {
      DEBUG ((DEBUG_ERROR, "Smhc: FIFO reset failed before cmd %u\n", Command->Index));
      SmhcPhaseMark (A733_SMHC_PHASE_PREP);
      SmhcPhaseMark (A733_SMHC_PHASE_END);
      return EFI_TIMEOUT;
    }

    SmhcWrite (Base, A733_SMHC_BLKSIZ, Command->BlockSize & A733_SMHC_BLKSIZ_MASK);
    SmhcWrite (Base, A733_SMHC_BYTCNT, Command->Length);

    Status = SmhcDmaStart (Private, Command);
    if (EFI_ERROR (Status)) {
      SmhcResetDataPath (Base);
      Private->DataFault = TRUE;
      SmhcPhaseMark (A733_SMHC_PHASE_PREP);
      SmhcPhaseMark (A733_SMHC_PHASE_END);
      return Status;
    }
  }

  SmhcPhaseMark (A733_SMHC_PHASE_PREP);

  SmhcWrite (Base, A733_SMHC_CMDARG, Command->Argument);
  SmhcWrite (Base, A733_SMHC_CMD, SmhcCommandRegister (Command));

  SmhcPhaseMark (A733_SMHC_PHASE_ISSUE);

  //
  // A data command is done when the byte count has moved; a command without
  // data is done at command-complete.  Either wait also ends on any fatal
  // interrupt bit, so a failure is seen as soon as the controller reports it
  // instead of after the whole budget.
  //
  WaitMask = (Command->Length != 0) ? A733_SMHC_RINT_DTC : A733_SMHC_RINT_CC;
  SmhcWaitCompletion (Private, WaitMask, Errors, TimeoutUs);

  SmhcPhaseMark (A733_SMHC_PHASE_WAIT);

  IntStatus  = Private->IntStatus;
  IdmaStatus = Private->IdstStatus;

  //
  // The data phase's completion bit outranks a response-timeout bit: a card
  // cannot have completed the data without having accepted the command, so RTO
  // next to DTC is the controller reporting the response stage late, not a
  // failed transfer.
  //
  if ((Command->Length != 0) && ((IntStatus & WaitMask) == WaitMask)) {
    Status = SmhcMapErrors (IntStatus & ~A733_SMHC_RINT_RTO_BACK, Read);
  } else {
    Status = SmhcMapErrors (IntStatus, Read);
  }

  //
  // The IDMAC's own failure bits are the one thing SMHC_RINTSTS cannot report:
  // a descriptor it could not fetch, or a bus error that stopped it.  A data
  // phase that failed there has not failed at the card's side at all.
  //
  if (!EFI_ERROR (Status) && ((IdmaStatus & A733_SMHC_IDST_DMA_FATAL) != 0)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status) && ((IntStatus & WaitMask) != WaitMask)) {
    Status = EFI_TIMEOUT;
  }

  if (!EFI_ERROR (Status) && ((IntStatus & A733_SMHC_RINT_CC) != 0)) {
    Command->Response[0] = SmhcRead (Base, A733_SMHC_RESP0);
    Command->Response[1] = SmhcRead (Base, A733_SMHC_RESP1);
    Command->Response[2] = SmhcRead (Base, A733_SMHC_RESP2);
    Command->Response[3] = SmhcRead (Base, A733_SMHC_RESP3);
  }

  if (Command->Length != 0) {
    SmhcDmaFinish (Private, Command, Status);
  }

  SmhcPhaseMark (A733_SMHC_PHASE_COPYOUT);

  //
  // An R1b command is not finished when its response arrives: the card is still
  // holding DATA0 low while it works.  CMD6 (SWITCH) is the one this driver
  // cares about; it moves the card's bus width and timing and can hold the line
  // for as long as the card needs.
  //
  if (!EFI_ERROR (Status) &&
      ((Command->ResponseType == SdMmcResponseTypeR1b) || (Command->ResponseType == SdMmcResponseTypeR5b)))
  {
    Status = SmhcWaitClear (Base, A733_SMHC_STATUS, A733_SMHC_STATUS_CARD_BUSY, TimeoutUs);
  }

  if (!Private->Quiet && PcdGetBool (PcdA733SmhcDumpEnable)) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: cmd %u arg %08x clk %u kHz width %u rint %08x idst %08x status %08x "
      "blksiz %08x bytecnt %08x cmd %08x dmac %08x "
      "resp %08x %08x %08x %08x %r (irq count %u)\n",
      Command->Index,
      Command->Argument,
      SmhcClockKhz (Private),
      SmhcRead (Base, A733_SMHC_CTYPE) & 0x3U,
      IntStatus,
      IdmaStatus,
      SmhcRead (Base, A733_SMHC_STATUS),
      SmhcRead (Base, A733_SMHC_BLKSIZ),
      SmhcRead (Base, A733_SMHC_BYTCNT),
      SmhcRead (Base, A733_SMHC_CMD),
      SmhcRead (Base, A733_SMHC_DMAC),
      Command->Response[0],
      Command->Response[1],
      Command->Response[2],
      Command->Response[3],
      Status,
      Private->InterruptCount
      ));

    //
    // Where an interrupt this command completed with has got to, if it got as
    // far as the distributor at all: the controller's line is asserted while its
    // latched status is set, so if the handler is not running but run it still
    // should be, this is the register that says so.
    //
    SmhcReportGicState (Private, "after this command");
  }

  //
  // The bits behind a failure are decoded whenever a data command fails, whether
  // or not the per-command dump is on: the IDMAC's own status is what tells a
  // DMA failure from a card failure, and a data command that failed is the one
  // case where that distinction is the whole diagnosis.  A command without data
  // that failed is only worth decoding when the dump asks for it.
  //
  if (!Private->Quiet &&
      EFI_ERROR (Status) &&
      ((Command->Length != 0) || PcdGetBool (PcdA733SmhcDumpEnable)))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: failed cmd %u arg %08x len %u clk %u rint %08x idst %08x status %08x %r\n",
      Command->Index,
      Command->Argument,
      Command->Length,
      SmhcClockKhz (Private),
      IntStatus,
      IdmaStatus,
      SmhcRead (Base, A733_SMHC_STATUS),
      Status
      ));
    SmhcDumpInterruptBits (IntStatus);
    SmhcDumpIdmaBits (IdmaStatus);
  }

  SmhcPhaseMark (A733_SMHC_PHASE_TAIL);

  //
  // After a data transfer, wait for the card to release its busy line before
  // returning.  The failure this prevents: back-to-back after a completed
  // read, the next command - the CMD23 that arms the following transfer -
  // comes back as a response timeout (RINTSTS CC|RTO_BACK) with STATUS.CARD_BUSY
  // set, i.e. the card was still holding DATA0 low and a busy card does not
  // answer commands.  The first victim is the partition scan's second read (the
  // GPT entry array), which then leaves fastboot without a partition table.
  //
  // DTC means the host has the last block, not that the card has let go of the
  // line, and this board's HS400 timing is marginal (the mode's own report says
  // 75% of the bus's ceiling).  A blind delay was the first fix (100 us fails
  // every cold boot, 500 us marginal, 1 ms reliable, 2 ms clean); waiting for
  // the actual condition is the correct form: an idle card returns at once, a
  // busy one is waited for exactly as long as it needs, and a wait that runs
  // out of budget is reported instead of being guessed at.
  //
  if (Command->Length != 0) {
    UINT64  IdleStart;
    UINT64  IdleUs;
    UINT64  Frequency;
    UINT32  TicksPerMicro;
    EFI_STATUS  IdleStatus;

    Frequency     = GetPerformanceCounterProperties (NULL, NULL);
    TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);
    IdleStart     = GetPerformanceCounter ();
    IdleStatus    = SmhcWaitCardIdle (Base, A733_SMHC_DATA_IDLE_TIMEOUT_US);
    IdleUs        = (GetPerformanceCounter () - IdleStart) / TicksPerMicro;

    if (EFI_ERROR (IdleStatus)) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: the card did not release its busy line within %u us after cmd %u (%u bytes); rint %08x status %08x - the next command may time out\n",
        (UINT32)A733_SMHC_DATA_IDLE_TIMEOUT_US,
        Command->Index,
        Command->Length,
        Private->IntStatus,
        SmhcRead (Base, A733_SMHC_STATUS)
        ));
    } else if (IdleUs >= 200) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: the card held its busy line for %u us after cmd %u (%u bytes)\n",
        (UINT32)IdleUs,
        Command->Index,
        Command->Length
        ));
    }
  }

  //
  // A command that completed is what makes the controller usable again after a
  // data path failure; the flag is the driver's record of that, and of the
  // recovery SmhcSendCommand ran above.
  //
  if (!EFI_ERROR (Status) && Private->DataFault) {
    if (!Private->Quiet) {
      DEBUG ((DEBUG_INFO, "Smhc: the data path recovered on cmd %u\n", Command->Index));
    }

    Private->DataFault = FALSE;
  }

  SmhcPhaseMark (A733_SMHC_PHASE_END);

  return Status;
}