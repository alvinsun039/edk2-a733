/** @file
  SPI0 master and the 25-series command set, for the A733's SPI NOR.

  Written from the A733 user manual's SPI chapter (18.8: the controller's
  register list and descriptions, its sample-mode table, and the "CPU or DMA
  transfer" programming sequence of 18.8.6.1) and from the standard 25-series
  flash command set.  No vendor SPI driver was carried over: the register
  access, the FIFO handling and the chip-select sequencing below are this
  file's own, and the only board knowledge in it is the four pads and which
  chip select the flash answers on.

  Provenance, stated plainly because four values in here are not what the
  manual alone would give:

  - the pads carry SPI0 as pin function 5, where the manual's port table
    lists the alternatives in a different order and reads as 3;
  - CCR (0x24) = 2, SAMP_DL (0x28) = 0x5000 and FCR (0x18) = 0x00200140 are
    the live values this controller needs, and the manual describes the fields
    without giving a working combination;
  - a command is two bursts, not one full-duplex burst (below).

  All four were established by taking the manual's reading as a hypothesis,
  watching it fail on this board, and cross-checking the result against a
  different firmware running on the same board - a source of *facts* about
  this silicon, not of code.  The implementation here was written against
  those facts; nothing was copied from that firmware or from any vendor
  driver, and the failure that led to each value is recorded with it so the
  next reader can re-derive it rather than trust it.

  How a command is put on the bus.  The manual's sequence programs the
  transmit counters for one burst and the receive counters for another, never
  both at once, and this controller needs it that way: a single full-duplex
  burst - TX FIFO filled, MBC/MTC/BCC.STC all set to the length, XCH, then RX
  drained after the same TC - runs to completion (TC is set) and brings back
  nothing at all.  So each command is two bursts:

  1. A transmit burst shifts the command byte, the address and any dummy or
     data bytes out: MBC = MTC = BCC.STC = the burst length, BCC.DBC = 0, the
     bytes written to SPI_TXD one at a time, SPI_TCR.XCH to start it, TC to
     finish it.
  2. A receive burst shifts the part's answer in: MBC = the receive length,
     MTC = BCC.STC = BCC.DBC = 0, nothing written to the TX FIFO, XCH, TC, and
     only then is SPI_RXD drained, one byte at a time.  With no transmit
     counter set the controller has nothing to send and simply clocks the
     part's output in.

  The chip select is taken low by software (SPI_TCR.SS_OWNER set, SS_LEVEL
  cleared), not by the burst engine, so one command is one select/exchange/
  deselect sequence whatever its length, and both of its bursts sit inside the
  same chip-select window: the flash sees one uninterrupted command, and a
  read longer than the FIFO is a series of bursts inside that window.

  The FIFOs are 64 entries of 8 bits, so SPI_TXD and SPI_RXD are accessed at
  byte width and no burst is longer than 64 bytes.  Every burst starts by
  resetting both FIFOs and clearing the status bits, so a burst cannot inherit
  state from the one before it, and SPI_TCR.DHB is set so the controller
  discards the burst rather than hashing it.

  The clock is SYS_CLK24M, one of the two sources that needs no PLL to be
  running, divided by 1, with the part driven in SPI mode 0 (CPOL = CPHA = 0).
  24 MHz is the manual's own upper bound for normal sampling, so the rate and
  the part agree by construction, and no measurement of a PLL that this port
  cannot verify is involved.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733CcuLib.h>
#include <Library/A733GpioLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include <A733Ccu.h>
#include <A733Gpio.h>
#include <A733Spi.h>

#include "A733SpiNor.h"

//
// The controller's FIFOs hold 64 entries of 8 bits (manual 18.8.1), so that is
// the most one burst can carry.
//
#define A733_SPI_CHUNK_BYTES  64U

//
// SPI_MBC, SPI_MTC.MWTC and SPI_BCC.STC/DBC are burst counters 13 bits wide
// (manual 18.8.8.14..8.16), so a length is masked to that width before it is
// written.  A burst here is 64 bytes at most, so the mask never bites; it is
// there so the field's width is stated where it is written rather than assumed.
//
#define A733_SPI_BURST_COUNT_MASK  0x1FFFU

//
// Polling interval and the three time budgets.  The part's own maxima are a
// 3 ms page program, a 400 ms sector erase and a 2 s block erase; each budget
// below is that maximum with an order of magnitude of slack, because a timeout
// here costs a boot with RAM-only variables rather than a retry.
//
#define A733_SPI_POLL_US             10U
#define A733_SPI_RESET_TIMEOUT_US    10000U
#define A733_SPI_BURST_TIMEOUT_US    1000000U
#define A733_SPI_PROGRAM_TIMEOUT_US  100000U
#define A733_SPI_ERASE_TIMEOUT_US    4000000U

#define A733_SPI_SPINS(TimeoutUs)  ((TimeoutUs) / A733_SPI_POLL_US)

//
// Manual: SPI chapter, 25-series command set.  Only what a variable store
// needs: identify, read, program, erase, and the status register that says
// when the part has finished.
//
#define A733_SPI_NOR_OP_WREN    0x06U  // write enable
#define A733_SPI_NOR_OP_RDSR    0x05U  // read status register 1
#define A733_SPI_NOR_OP_READ    0x03U  // read data, 24-bit address
#define A733_SPI_NOR_OP_PP      0x02U  // page program, 24-bit address
#define A733_SPI_NOR_OP_SE      0x20U  // sector erase, 4 KB, 24-bit address
#define A733_SPI_NOR_OP_BE      0xD8U  // block erase, 64 KB, 24-bit address
#define A733_SPI_NOR_OP_RDID    0x9FU  // JEDEC identification
#define A733_SPI_NOR_OP_SFDP    0x5AU  // SFDP parameter area

//
// The part's capacity, from the JEDEC identification.  Zero until
// A733SpiNorSetCapacity has been told, which is what makes every command
// refuse to run against a part nobody measured.
//
STATIC UINTN  mNorCapacity;

//
// Erase instructions the part advertises through SFDP.  The JEDEC defaults
// are what nearly every part uses; the probe below replaces them when the
// flash says otherwise.
//
STATIC UINT8  mEraseSectorOp = A733_SPI_NOR_OP_SE;
STATIC UINT8  mEraseBlockOp  = A733_SPI_NOR_OP_BE;

//
// Manual: GPIO chapter, port C multiplex.  PC2/PC3/PC4/PC12 carry SPI0 on
// function 3 (NAND is function 2 on each of them, SPIF0 function 4), which is
// the same selector the eMMC driver's board table calls ALT3 for SDC2.
//
#define A733_SPI0_PAD_FUNCTION  A733_GPIO_FUNC_ALT5
// The lowest drive code.  This flash is a single load on a short net and the
// pads were not measured here, so the value is the conservative one.
#define A733_SPI0_PAD_DRIVE     1U

typedef struct {
  UINT32         Pin;
  CONST CHAR8    *Signal;
  A733_GPIO_PULL Pull;
} A733_SPI_PAD;

//
// PC3 is the chip select and idles high, so it keeps its pull-up; the other
// three are driven by the controller for the whole of a command and are left
// without a pull.
//
STATIC CONST A733_SPI_PAD  mA733Spi0Pads[] = {
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 2),  "MOSI", A733_GPIO_PULL_DISABLE },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 3),  "CS0",  A733_GPIO_PULL_UP      },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 4),  "MISO", A733_GPIO_PULL_DISABLE },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 12), "CLK",  A733_GPIO_PULL_DISABLE },
};

/**
  How one exchange turned out, burst by burst.

  Diagnostic only: the command layer takes the EFI_STATUS and the bytes, and
  never looks at this.  It exists because "the id is wrong" and "no burst
  completed" are different faults, and the log has to be able to tell them
  apart without a rebuild.
**/
typedef struct {
  BOOLEAN  TxComplete;  ///< The transmit burst reported TC.
  BOOLEAN  RxComplete;  ///< The receive burst reported TC.
  UINTN    RxBytes;     ///< Bytes drained from the RX FIFO.
} A733_SPI_TRACE;

/**
  Wait for the controller's FIFO reset bits to clear.

  @retval EFI_SUCCESS      Both resets completed.
  @retval EFI_DEVICE_ERROR The controller never reported completion.
**/
STATIC
EFI_STATUS
SpiWaitFifoReset (
  VOID
  )
{
  UINTN  Spins;

  for (Spins = 0; Spins < A733_SPI_SPINS (A733_SPI_RESET_TIMEOUT_US); Spins++) {
    if ((MmioRead32 (A733_SPI0_BASE + A733_SPI_FCR) &
         (A733_SPI_FCR_TF_RST | A733_SPI_FCR_RF_RST)) == 0)
    {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (A733_SPI_POLL_US);
  }

  return EFI_DEVICE_ERROR;
}

/**
  Reset both FIFOs and drop whatever status the previous burst left behind.

  @retval EFI_SUCCESS      The controller is ready for a burst.
  @retval EFI_DEVICE_ERROR A FIFO reset never completed.
**/
STATIC
EFI_STATUS
SpiResetBurst (
  VOID
  )
{
  EFI_STATUS  Status;

  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_FCR,
    MmioRead32 (A733_SPI0_BASE + A733_SPI_FCR) |
    A733_SPI_FCR_TF_RST | A733_SPI_FCR_RF_RST
    );
  Status = SpiWaitFifoReset ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNor: FIFO reset never completed\n"));
    return Status;
  }

  //
  // Drop any completion or error a previous burst left behind: every one of
  // these bits is write-1-to-clear (18.8.8.4).
  //
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_ISR,
    A733_SPI_ISR_TC | A733_SPI_ISR_ERRORS | A733_SPI_ISR_SSI
    );

  return EFI_SUCCESS;
}

/**
  Start a burst whose counters the caller has already programmed, and wait for
  the controller to report it complete.

  SPI_TCR.DHB is set for every burst: the hash engine has no part in a NOR
  command and the manual has the burst discarded rather than hashed.

  @param[in]  Length    Byte count of the burst, for the log only.
  @param[out] Complete  OPTIONAL: set TRUE when SPI_ISR.TC was seen.

  @retval EFI_SUCCESS      SPI_ISR.TC was seen.
  @retval EFI_TIMEOUT      It never came within the burst budget.
  @retval EFI_DEVICE_ERROR A FIFO error condition ended the burst.
**/
STATIC
EFI_STATUS
SpiRunBurst (
  IN  UINTN    Length,
  OUT BOOLEAN  *Complete OPTIONAL
  )
{
  UINT32  Value;
  UINTN   Spins;

  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_TCR,
    MmioRead32 (A733_SPI0_BASE + A733_SPI_TCR) |
    A733_SPI_TCR_DHB | A733_SPI_TCR_XCH
    );

  for (Spins = 0; ; Spins++) {
    Value = MmioRead32 (A733_SPI0_BASE + A733_SPI_ISR);
    if ((Value & A733_SPI_ISR_TC) != 0) {
      break;
    }

    if ((Value & A733_SPI_ISR_ERRORS) != 0) {
      DEBUG ((
        DEBUG_ERROR,
        "SpiNor: FIFO error 0x%x on a %u-byte burst\n",
        Value & A733_SPI_ISR_ERRORS,
        (UINT32)Length
        ));
      return EFI_DEVICE_ERROR;
    }

    if (Spins >= A733_SPI_SPINS (A733_SPI_BURST_TIMEOUT_US)) {
      DEBUG ((
        DEBUG_ERROR,
        "SpiNor: %u-byte burst did not complete, ISR 0x%x\n",
        (UINT32)Length,
        Value
        ));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (A733_SPI_POLL_US);
  }

  if (Complete != NULL) {
    *Complete = TRUE;
  }

  return EFI_SUCCESS;
}

/**
  Shift one burst out, at most 64 bytes.

  The counters are the manual's transmit-burst ones (18.8.8.14..8.16):
  MTC.MWTC says how many bytes come out of the FIFO, MBC how many bursts the
  exchange is, and BCC.STC how many of those are single-mode transmits.  With
  BCC.DBC left at 0 no dummy burst is inserted, so all three are the length.

  The caller owns the chip select; this function only moves bytes.  Both FIFOs
  are reset first, so this is safe to call after a burst that failed.

  @param[in]  Tx        Bytes to send; not NULL.
  @param[in]  Length    1..A733_SPI_CHUNK_BYTES.
  @param[out] Complete  OPTIONAL: set TRUE when the burst reported TC.

  @retval EFI_SUCCESS            The burst was shifted out.
  @retval EFI_INVALID_PARAMETER  The length or the buffer is out of range.
  @retval other                  See SpiRunBurst.
**/
STATIC
EFI_STATUS
SpiTxBurst (
  IN  CONST UINT8  *Tx,
  IN  UINTN        Length,
  OUT BOOLEAN      *Complete OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  if ((Tx == NULL) || (Length == 0) || (Length > A733_SPI_CHUNK_BYTES)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = SpiResetBurst ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Length; Index++) {
    MmioWrite8 (A733_SPI0_BASE + A733_SPI_TXD, Tx[Index]);
  }

  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_MTC,
    (UINT32)Length & A733_SPI_BURST_COUNT_MASK
    );
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_BCC,
    (UINT32)Length & A733_SPI_BURST_COUNT_MASK
    );
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_MBC,
    (UINT32)Length & A733_SPI_BURST_COUNT_MASK
    );

  return SpiRunBurst (Length, Complete);
}

/**
  Shift one burst in, at most 64 bytes, and drain it from the RX FIFO.

  The counters are zero where a transmit burst's are not: MTC = 0 and
  BCC.STC = 0 say the controller has nothing of its own to send, so it spends
  the burst clocking MISO into the FIFO, and MBC alone says how many bytes
  that is.  Nothing is written to SPI_TXD - filling it here would turn the
  burst back into the full-duplex one that returns nothing.

  @param[out] Rx        Bytes received; not NULL.
  @param[in]  Length    1..A733_SPI_CHUNK_BYTES.
  @param[out] Read      OPTIONAL: advanced by Length on success.
  @param[out] Complete  OPTIONAL: set TRUE when the burst reported TC.

  @retval EFI_SUCCESS            The burst was shifted in and drained.
  @retval EFI_INVALID_PARAMETER  The length or the buffer is out of range.
  @retval other                  See SpiRunBurst.
**/
STATIC
EFI_STATUS
SpiRxBurst (
  OUT UINT8    *Rx,
  IN  UINTN    Length,
  OUT UINTN    *Read OPTIONAL,
  OUT BOOLEAN  *Complete OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  if ((Rx == NULL) || (Length == 0) || (Length > A733_SPI_CHUNK_BYTES)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = SpiResetBurst ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (A733_SPI0_BASE + A733_SPI_MTC, 0);
  MmioWrite32 (A733_SPI0_BASE + A733_SPI_BCC, 0);
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_MBC,
    (UINT32)Length & A733_SPI_BURST_COUNT_MASK
    );

  Status = SpiRunBurst (Length, Complete);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The burst is over, so the FIFO holds exactly this burst's bytes and is
  // being drained rather than raced.
  //
  for (Index = 0; Index < Length; Index++) {
    Rx[Index] = MmioRead8 (A733_SPI0_BASE + A733_SPI_RXD);
  }

  if (Read != NULL) {
    *Read += Length;
  }

  return EFI_SUCCESS;
}

/**
  Carry out one command's exchange with the chip select already asserted.

  Transmit first, then receive: one or more transmit bursts for the command
  byte, the address and any data, followed by one or more receive bursts for
  the part's answer.  Neither side is ever both at once, which is the manual's
  two-burst sequence and the only shape this controller returns data on.

  @param[in]  Tx        Bytes to send, or NULL for a receive-only exchange.
  @param[in]  TxLength  Byte count to send, possibly zero.
  @param[out] Rx        Bytes received, or NULL for a transmit-only exchange.
  @param[in]  RxLength  Byte count to receive, possibly zero.
  @param[out] Trace     OPTIONAL: per-burst outcome, for the diagnostic.

  @retval EFI_SUCCESS            The whole exchange was shifted.
  @retval EFI_INVALID_PARAMETER  A length and its buffer disagree.
  @retval other                  A burst failed; see SpiRunBurst.
**/
STATIC
EFI_STATUS
SpiExchange (
  IN  CONST UINT8     *Tx,
  IN  UINTN           TxLength,
  OUT UINT8           *Rx,
  IN  UINTN           RxLength,
  OUT A733_SPI_TRACE  *Trace OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINTN       Chunk;

  if ((RxLength != 0) && (Rx == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Trace != NULL) {
    ZeroMem (Trace, sizeof (*Trace));
  }

  while (TxLength > 0) {
    Chunk = MIN (TxLength, (UINTN)A733_SPI_CHUNK_BYTES);
    Status = SpiTxBurst (
               Tx,
               Chunk,
               (Trace == NULL) ? NULL : &Trace->TxComplete
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Tx       += Chunk;
    TxLength -= Chunk;
  }

  while (RxLength > 0) {
    Chunk = MIN (RxLength, (UINTN)A733_SPI_CHUNK_BYTES);
    Status = SpiRxBurst (
               Rx,
               Chunk,
               (Trace == NULL) ? NULL : &Trace->RxBytes,
               (Trace == NULL) ? NULL : &Trace->RxComplete
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Rx       += Chunk;
    RxLength -= Chunk;
  }

  return EFI_SUCCESS;
}

/**
  Assert the chip select: software control, level low.
**/
STATIC
VOID
SpiSelect (
  VOID
  )
{
  UINT32  Tcr;

  Tcr = MmioRead32 (A733_SPI0_BASE + A733_SPI_TCR);
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_TCR,
    (Tcr | A733_SPI_TCR_SS_OWNER) & ~(UINT32)A733_SPI_TCR_SS_LEVEL
    );
}

/**
  De-assert the chip select: software control, level high.
**/
STATIC
VOID
SpiDeselect (
  VOID
  )
{
  UINT32  Tcr;

  Tcr = MmioRead32 (A733_SPI0_BASE + A733_SPI_TCR);
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_TCR,
    Tcr | A733_SPI_TCR_SS_OWNER | A733_SPI_TCR_SS_LEVEL
    );
}

/**
  Read status register 1.

  @param[out] Status  The register.

  @retval EFI_SUCCESS  The register was read.
  @retval other        The burst failed.
**/
STATIC
EFI_STATUS
SpiNorReadStatus (
  OUT UINT8  *Status
  )
{
  CONST UINT8  Tx  = A733_SPI_NOR_OP_RDSR;
  UINT8        Rx;
  EFI_STATUS   Result;

  SpiSelect ();
  Result = SpiExchange (&Tx, 1, &Rx, 1, NULL);
  SpiDeselect ();
  if (EFI_ERROR (Result)) {
    return Result;
  }

  *Status = Rx;
  return EFI_SUCCESS;
}

/**
  Poll the part until it is no longer busy.

  @param[in] TimeoutUs  Budget for the whole operation.

  @retval EFI_SUCCESS      The part is idle.
  @retval EFI_TIMEOUT      WIP was still set when the budget ran out.
  @retval EFI_DEVICE_ERROR The status register could not be read.
**/
STATIC
EFI_STATUS
SpiNorWaitReady (
  IN UINTN  TimeoutUs
  )
{
  EFI_STATUS  Status;
  UINT8       Sr;
  UINTN       Spins;

  for (Spins = 0; Spins < A733_SPI_SPINS (TimeoutUs); Spins++) {
    Status = SpiNorReadStatus (&Sr);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Sr & A733_SPI_NOR_SR_WIP) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (A733_SPI_POLL_US);
  }

  DEBUG ((DEBUG_ERROR, "SpiNor: still busy after %u us\n", (UINT32)TimeoutUs));
  return EFI_TIMEOUT;
}

/**
  Issue write enable and require the part to acknowledge it.

  A 25-series part clears WEL on every completed program or erase and on any
  write-protected operation, so a WEL that does not read back means the next
  program or erase would silently do nothing.  Catching it here keeps a
  write-protected part from looking like a store that is merely stale.

  @retval EFI_SUCCESS      Write enable is set.
  @retval EFI_DEVICE_ERROR WEL did not read back, or the burst failed.
**/
STATIC
EFI_STATUS
SpiNorWriteEnable (
  VOID
  )
{
  CONST UINT8  Tx    = A733_SPI_NOR_OP_WREN;
  EFI_STATUS   Status;
  UINT8        Sr;

  SpiSelect ();
  Status = SpiExchange (&Tx, 1, NULL, 0, NULL);
  SpiDeselect ();
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = SpiNorReadStatus (&Sr);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  if ((Sr & A733_SPI_NOR_SR_WEL) == 0) {
    DEBUG ((DEBUG_ERROR, "SpiNor: write enable refused, SR 0x%02x\n", Sr));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Send a command byte and a 24-bit address, then de-assert the chip select.

  @param[in] Op      Command byte.
  @param[in] Address Byte offset in the part.

  @retval EFI_SUCCESS  The address phase was shifted.
  @retval other        A burst failed.
**/
STATIC
EFI_STATUS
SpiNorSendAddress (
  IN UINT8  Op,
  IN UINTN  Address
  )
{
  UINT8       Cmd[4];
  EFI_STATUS  Status;

  Cmd[0] = Op;
  Cmd[1] = (UINT8)(Address >> 16);
  Cmd[2] = (UINT8)(Address >> 8);
  Cmd[3] = (UINT8)Address;

  SpiSelect ();
  Status = SpiExchange (Cmd, sizeof (Cmd), NULL, 0, NULL);
  SpiDeselect ();

  return Status;
}

/**
  Bring SPI0 up: CCU clock and gate, then the pads, then the controller.

  The order is the manual's (18.8.6.1): clock, reset, pins, sample mode, SPI
  mode, interrupts off, enable.  The CCU calls are made in the same order the
  eMMC driver uses - parent and dividers first, gate and reset second - so that
  the gate is opened onto a clock that is already programmed.

  @retval EFI_SUCCESS            SPI0 is up.
  @retval EFI_UNSUPPORTED        The CCU does not model a clock for SPI0.
  @retval other                  A pad or the module gate could not be set.
**/
EFI_STATUS
EFIAPI
A733SpiNorInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  UINT32      Tcr;

  Status = A733CcuSetModuleClock (
             A733_CCU_MODULE_SPI0,
             A733_CCU_SRC_SYS_CLK24M,
             1
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNor: SPI0 clock: %r\n", Status));
    return Status;
  }

  Status = A733CcuEnableModule (A733_CCU_MODULE_SPI0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SpiNor: SPI0 gate: %r\n", Status));
    return Status;
  }

  for (Index = 0; Index < ARRAY_SIZE (mA733Spi0Pads); Index++) {
    Status = A733GpioSetFunction (
               mA733Spi0Pads[Index].Pin,
               A733_SPI0_PAD_FUNCTION
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "SpiNor: pin %a cannot be muxed to SPI0: %r\n",
        mA733Spi0Pads[Index].Signal,
        Status
        ));
      return Status;
    }

    Status = A733GpioSetPull (
               mA733Spi0Pads[Index].Pin,
               mA733Spi0Pads[Index].Pull
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "SpiNor: pin %a pull: %r\n",
        mA733Spi0Pads[Index].Signal,
        Status
        ));
      return Status;
    }

    Status = A733GpioSetDrive (mA733Spi0Pads[Index].Pin, A733_SPI0_PAD_DRIVE);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "SpiNor: pin %a drive: %r\n",
        mA733Spi0Pads[Index].Signal,
        Status
        ));
      return Status;
    }
  }

  //
  // Reset the whole controller first (GCR.SRST is write-1 and clears itself
  // when the reset completes), so nothing programmed below can be discarded by
  // it: the reset puts every register back to its default.
  //
  MmioWrite32 (A733_SPI0_BASE + A733_SPI_GCR, A733_SPI_GCR_SRST);
  for (Index = 0; Index < A733_SPI_SPINS (A733_SPI_RESET_TIMEOUT_US); Index++) {
    if ((MmioRead32 (A733_SPI0_BASE + A733_SPI_GCR) & A733_SPI_GCR_SRST) == 0) {
      break;
    }

    MicroSecondDelay (A733_SPI_POLL_US);
  }

  if (Index >= A733_SPI_SPINS (A733_SPI_RESET_TIMEOUT_US)) {
    DEBUG ((DEBUG_ERROR, "SpiNor: controller reset never completed\n"));
    return EFI_DEVICE_ERROR;
  }

  //
  // Master mode, old sample timing (GCR.MODE_SEL cleared, the table of
  // 18.8.6.3).  Transmit pause is left off on purpose: it would stop the
  // shifter when the RX FIFO fills, and this driver drains the FIFO only after
  // the burst reports complete, so a full FIFO must not stop it.
  //
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_GCR,
    A733_SPI_GCR_MODE | A733_SPI_GCR_EN
    );

  //
  // SPI mode 0 (CPOL = CPHA = 0), chip select active low, software controlled
  // and de-asserted, chip select 0, SSCTL cleared so the pin stays low between
  // the two bursts of a command, MSB first, the burst discarded rather than
  // hashed, and the sample-delay bits SDM, SDC and SDC1 left clear (their
  // reset value).  That is the reset value of SPI_TCR with the software chip
  // select, the polarity and DHB added, and it is the shape a 25-series part
  // answers.
  //
  Tcr = A733_SPI_TCR_SPOL | A733_SPI_TCR_SS_OWNER | A733_SPI_TCR_SS_LEVEL |
        A733_SPI_TCR_DHB;
  MmioWrite32 (A733_SPI0_BASE + A733_SPI_TCR, Tcr);

  //
  // The controller's own clock divider, sample delay and FIFO thresholds.  The
  // module clock being on is not the same as the part seeing a clock: CCR is
  // what divides the module clock down to SPI_CLK, and a reset value of zero
  // leaves the shifter idle however correct everything else is.
  //
  MmioWrite32 (A733_SPI0_BASE + A733_SPI_CCR, A733_SPI_CCR_VALUE);
  MmioWrite32 (A733_SPI0_BASE + A733_SPI_SAMP_DL, A733_SPI_SAMP_DL_VALUE);
  MmioWrite32 (A733_SPI0_BASE + A733_SPI_FCR, A733_SPI_FCR_VALUE);

  // Nothing here is interrupt driven.
  MmioWrite32 (A733_SPI0_BASE + A733_SPI_IER, 0);

  return EFI_SUCCESS;
}

/**
  Read the JEDEC identification (0x9F).

  @param[out] Id  Manufacturer, memory type, capacity.

  @retval EFI_SUCCESS  The three bytes were returned.
  @retval other        The burst failed.
**/
EFI_STATUS
EFIAPI
A733SpiNorReadId (
  OUT UINT8  Id[3]
  )
{
  CONST UINT8  Tx = A733_SPI_NOR_OP_RDID;
  UINT8        Rx[3];
  EFI_STATUS   Status;

  SpiSelect ();
  Status = SpiExchange (&Tx, 1, Rx, sizeof (Rx), NULL);
  SpiDeselect ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (Id, Rx, sizeof (Rx));
  return EFI_SUCCESS;
}

/**
  DIAGNOSTIC: why is the identification still wrong?

  With the send-then-receive exchange in place this should never print, so it
  is kept for the case where the id is still not the part's.  It repeats the
  identification read over the four SPI modes x the two sample-data
  selections, and for each one it prints what the exchange's two bursts
  actually did - whether the transmit burst reported TC, whether the receive
  burst did, and how many bytes were drained from the RX FIFO - next to the
  three id bytes and the controller's status.  A burst that never completed, a
  burst that completed with an empty FIFO (0 of 3 bytes) and a burst that
  brought back the wrong bytes are three different faults, and the log says
  which one it is.

  The MISO pad's own level is printed first, so an unpowered or unpopulated
  part can be told apart from a controller that never shifted.

  @param[in] Why  Short tag naming the caller.
**/
VOID
EFIAPI
A733SpiNorDiag (
  IN CONST CHAR8  *Why
  )
{
  CONST UINT8  Tx[4] = { A733_SPI_NOR_OP_RDID, 0, 0, 0 };
  UINT8        Rx[4];
  UINT32       Tcr;
  UINT32       Gcr;
  UINTN        Mode;
  UINTN        Sample;
  EFI_STATUS   Status;

  //
  // DIAGNOSTIC: ask the pin library what it thinks each pad is, rather than
  // reading a register by hand - the CFG registers are four bytes apart, and a
  // hand-computed offset reads some other block entirely.
  //
  DEBUG ((
    DEBUG_ERROR,
    "SpiNor diag (%a): MOSI PC2 fn %u, CS0 PC3 fn %u, MISO PC4 fn %u, CLK PC12 fn %u (want %u)\n",
    Why,
    A733GpioGetFunction (A733_GPIO_PIN (A733_GPIO_BANK_C, 2)),
    A733GpioGetFunction (A733_GPIO_PIN (A733_GPIO_BANK_C, 3)),
    A733GpioGetFunction (A733_GPIO_PIN (A733_GPIO_BANK_C, 4)),
    A733GpioGetFunction (A733_GPIO_PIN (A733_GPIO_BANK_C, 12)),
    A733_SPI0_PAD_FUNCTION
    ));

  Gcr = MmioRead32 (A733_SPI0_BASE + A733_SPI_GCR);
  Tcr = MmioRead32 (A733_SPI0_BASE + A733_SPI_TCR);
  DEBUG ((
    DEBUG_ERROR,
    "SpiNor diag: GCR %08x TCR %08x ISR %08x FSR %08x\n",
    Gcr,
    Tcr,
    MmioRead32 (A733_SPI0_BASE + A733_SPI_ISR),
    MmioRead32 (A733_SPI0_BASE + A733_SPI_FSR)
    ));

  for (Mode = 0; Mode < 4; Mode++) {
    for (Sample = 0; Sample < 2; Sample++) {
      Tcr &= ~(UINT32)(A733_SPI_TCR_CPOL | A733_SPI_TCR_CPHA | A733_SPI_TCR_SDM);
      if ((Mode & 2U) != 0) {
        Tcr |= A733_SPI_TCR_CPOL;
      }
      if ((Mode & 1U) != 0) {
        Tcr |= A733_SPI_TCR_CPHA;
      }
      if (Sample != 0) {
        Tcr |= A733_SPI_TCR_SDM;
      }

      MmioWrite32 (A733_SPI0_BASE + A733_SPI_TCR, Tcr);

      ZeroMem (Rx, sizeof (Rx));
      SpiSelect ();
      Status = SpiExchange (Tx, 1, Rx, 3, NULL);
      SpiDeselect ();

      DEBUG ((
        DEBUG_ERROR,
        "SpiNor diag: mode %u sample %u: %r id %02x %02x %02x ISR %08x\n",
        (UINT32)Mode,
        (UINT32)Sample,
        Status,
        Rx[0],
        Rx[1],
        Rx[2],
        MmioRead32 (A733_SPI0_BASE + A733_SPI_ISR)
        ));
    }
  }

  //
  // Leave the controller in the shipping configuration whatever the walk did.
  //
  MmioWrite32 (
    A733_SPI0_BASE + A733_SPI_TCR,
    A733_SPI_TCR_SPOL | A733_SPI_TCR_SS_OWNER | A733_SPI_TCR_SS_LEVEL |
    A733_SPI_TCR_DHB
    );
}

/**
  Tell the command layer how large the part is.

  @param[in] CapacityBytes  Part size, a power of two.

  @retval EFI_SUCCESS            The capacity was accepted.
  @retval EFI_INVALID_PARAMETER  It is zero, not a power of two, or smaller
                                 than one window.
**/
EFI_STATUS
EFIAPI
A733SpiNorSetCapacity (
  IN UINTN  CapacityBytes
  )
{
  if ((CapacityBytes < A733_SPI_NOR_WINDOW_SIZE) ||
      ((CapacityBytes & (CapacityBytes - 1)) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  mNorCapacity = CapacityBytes;
  return EFI_SUCCESS;
}

/**
  Read from the part with 0x03.

  The command byte and the 24-bit address go out in a transmit burst, and the
  data comes back in receive bursts, all with the chip select held: the part
  keeps streaming while it stays selected, so one 0x03 covers the whole range
  however many 64-byte bursts it takes.

  @param[in]  Offset  Byte offset in the part.
  @param[out] Buffer  Destination.
  @param[in]  Length  Byte count.

  @retval EFI_SUCCESS            The bytes were read.
  @retval EFI_INVALID_PARAMETER  The range is past the end of the part, or its
                                 capacity has not been set.
  @retval other                  A burst failed.
**/
EFI_STATUS
EFIAPI
A733SpiNorRead (
  IN  UINTN  Offset,
  OUT VOID   *Buffer,
  IN  UINTN  Length
  )
{
  UINT8       Cmd[4];
  EFI_STATUS  Status;

  if ((Buffer == NULL) || (Length == 0) ||
      (Offset + Length > mNorCapacity))
  {
    return EFI_INVALID_PARAMETER;
  }

  //
  // One command per 4 KB sector, each inside its own chip-select window.  A
  // single 0x03 stream of 192 KB does not survive: the transfer, which is all
  // the part needs to do, gets about 68 KB in and the rest of the buffer comes
  // back as a repeating four-byte pattern the flash never contained.  Restarting
  // the command at every sector boundary is what the part's own reference
  // waveform shows for long reads and it keeps every exchange short enough that
  // nothing has to stay in step across a FIFO reset.
  //
  while (Length > 0) {
    UINTN  Chunk;

    Chunk = A733_SPI_CHUNK_BYTES;
    if (Chunk > Length) {
      Chunk = Length;
    }

    Cmd[0] = A733_SPI_NOR_OP_READ;
    Cmd[1] = (UINT8)(Offset >> 16);
    Cmd[2] = (UINT8)(Offset >> 8);
    Cmd[3] = (UINT8)Offset;

    SpiSelect ();
    Status = SpiExchange (Cmd, sizeof (Cmd), Buffer, Chunk, NULL);
    SpiDeselect ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset += Chunk;
    Buffer  = (UINT8 *)Buffer + Chunk;
    Length -= Chunk;
  }

  return EFI_SUCCESS;
}

/**
  Program the part with 0x02, one page at a time.

  @param[in] Offset  Byte offset in the part.
  @param[in] Buffer  Source.
  @param[in] Length  Byte count.

  @retval EFI_SUCCESS            The bytes were programmed.
  @retval EFI_INVALID_PARAMETER  The range is past the end of the part.
  @retval EFI_DEVICE_ERROR       Write enable was refused, a program did not
                                 complete, or a burst failed.
**/
EFI_STATUS
EFIAPI
A733SpiNorProgram (
  IN UINTN        Offset,
  IN CONST VOID   *Buffer,
  IN UINTN        Length
  )
{
  CONST UINT8  *Data;
  UINT8        Cmd[4];
  EFI_STATUS   Status;
  UINTN        Chunk;
  UINTN        PageRoom;
  UINTN        Remaining;

  if ((Buffer == NULL) || (Length == 0) ||
      (Offset + Length > mNorCapacity))
  {
    return EFI_INVALID_PARAMETER;
  }

  Data      = Buffer;
  Remaining = Length;
  while (Remaining > 0) {
    //
    // A page program cannot cross a page boundary: the address wraps inside
    // the part and the first bytes are overwritten again.
    //
    PageRoom = A733_SPI_NOR_PAGE_BYTES - (Offset % A733_SPI_NOR_PAGE_BYTES);
    Chunk    = MIN (Remaining, PageRoom);

    Status = SpiNorWriteEnable ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Cmd[0] = A733_SPI_NOR_OP_PP;
    Cmd[1] = (UINT8)(Offset >> 16);
    Cmd[2] = (UINT8)(Offset >> 8);
    Cmd[3] = (UINT8)Offset;

    SpiSelect ();
    Status = SpiExchange (Cmd, sizeof (Cmd), NULL, 0, NULL);
    if (!EFI_ERROR (Status)) {
      Status = SpiExchange (Data, Chunk, NULL, 0, NULL);
    }

    SpiDeselect ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SpiNorWaitReady (A733_SPI_PROGRAM_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "SpiNor: program at 0x%06x did not complete: %r\n",
        (UINT32)Offset,
        Status
        ));
      return Status;
    }

    Offset    += Chunk;
    Data      += Chunk;
    Remaining -= Chunk;
  }

  return EFI_SUCCESS;
}

/**
  Verify that a range reads back erased.

  @param[in] Offset  Byte offset in the part.
  @param[in] Length  Byte count.

  @retval EFI_SUCCESS      Every byte is 0xFF.
  @retval EFI_DEVICE_ERROR A byte is not, at the offset the log names.
**/
STATIC
EFI_STATUS
SpiNorVerifyErased (
  IN UINTN  Offset,
  IN UINTN  Length
  )
{
  EFI_STATUS  Status;
  UINT8       Buffer[64];
  UINTN       Chunk;
  UINTN       Index;

  while (Length > 0) {
    Chunk  = MIN (Length, (UINTN)sizeof (Buffer));
    Status = A733SpiNorRead (Offset, Buffer, Chunk);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    for (Index = 0; Index < Chunk; Index++) {
      if (Buffer[Index] != 0xFF) {
        DEBUG ((
          DEBUG_ERROR,
          "SpiNor: offset 0x%06x reads 0x%02x after erase\n",
          (UINT32)(Offset + Index),
          Buffer[Index]
          ));
        return EFI_DEVICE_ERROR;
      }
    }

    Offset += Chunk;
    Length -= Chunk;
  }

  return EFI_SUCCESS;
}

/**
  Erase a range with 0xD8 where a whole 64 KB block is covered and 0x20
  otherwise.

  @param[in] Offset  Byte offset in the part.
  @param[in] Length  Byte count.

  @retval EFI_SUCCESS            The range is erased.
  @retval EFI_INVALID_PARAMETER  The range or the alignment is wrong.
  @retval EFI_DEVICE_ERROR       Write enable was refused, an erase did not
                                 complete, or a unit did not read back erased.
**/
EFI_STATUS
EFIAPI
A733SpiNorErase (
  IN UINTN  Offset,
  IN UINTN  Length
  )
{
  EFI_STATUS  Status;
  UINT8       Op;
  UINTN       Unit;
  UINTN       Remaining;

  if ((Length == 0) || ((Offset % A733_SPI_NOR_SECTOR_BYTES) != 0) ||
      ((Length % A733_SPI_NOR_SECTOR_BYTES) != 0) ||
      (Offset + Length > mNorCapacity))
  {
    return EFI_INVALID_PARAMETER;
  }

  Remaining = Length;
  while (Remaining > 0) {
    if (((Offset % A733_SPI_NOR_BLOCK_BYTES) == 0) &&
        (Remaining >= A733_SPI_NOR_BLOCK_BYTES))
    {
      Op   = mEraseBlockOp;
      Unit = A733_SPI_NOR_BLOCK_BYTES;
    } else {
      Op   = mEraseSectorOp;
      Unit = A733_SPI_NOR_SECTOR_BYTES;
    }

    Status = SpiNorWriteEnable ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = SpiNorSendAddress (Op, Offset);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    Status = SpiNorWaitReady (A733_SPI_ERASE_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "SpiNor: erase at 0x%06x (%u bytes) did not complete: %r\n",
        (UINT32)Offset,
        (UINT32)Unit,
        Status
        ));
      return EFI_DEVICE_ERROR;
    }

    Status = SpiNorVerifyErased (Offset, Unit);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset    += Unit;
    Remaining -= Unit;
  }

  return EFI_SUCCESS;
}

/**
  Read the JEDEC Serial Flash Discoverable Parameter area (0x5A).

  The command takes a 24-bit address and then eight dummy clocks before the
  data; the fifth byte of the transmit burst is those clocks.

  @param[in]  Address  Byte offset into the SFDP area.
  @param[out] Buffer   Destination.
  @param[in]  Length   Byte count.

  @retval EFI_SUCCESS  The bytes were read.
  @retval other        A burst failed.
**/
STATIC
EFI_STATUS
SpiNorSfdpRead (
  IN  UINT32  Address,
  OUT VOID    *Buffer,
  IN  UINTN   Length
  )
{
  UINT8       Cmd[5];
  EFI_STATUS  Status;

  Cmd[0] = A733_SPI_NOR_OP_SFDP;
  Cmd[1] = (UINT8)(Address >> 16);
  Cmd[2] = (UINT8)(Address >> 8);
  Cmd[3] = (UINT8)Address;
  Cmd[4] = 0x00;                       // eight dummy clocks

  SpiSelect ();
  Status = SpiExchange (Cmd, sizeof (Cmd), Buffer, Length, NULL);
  SpiDeselect ();

  return Status;
}

/**
  Ask the part for its size and erase instructions (JESD216 SFDP).

  Reads the SFDP header, walks the parameter headers to the JEDEC basic
  flash parameter table, and takes the density and the 4 KB/64 KB erase
  instructions from it.  Anything malformed leaves the compile-time
  defaults in place and reports failure, so the caller can fall back to
  the JEDEC ID capacity byte.

  @param[out] CapacityBytes  The die size SFDP reports.

  @retval EFI_SUCCESS      The table was found and parsed.
  @retval EFI_NOT_FOUND    No usable basic flash parameter table.
  @retval other            A bus transaction failed.
**/
EFI_STATUS
EFIAPI
A733SpiNorSfdpInfo (
  OUT UINTN  *CapacityBytes
  )
{
  EFI_STATUS  Status;
  UINT8       Header[8];
  UINT8       Param[8];
  UINT8       Bfpt[64];
  UINTN       Count;
  UINTN       Index;
  UINTN       Length;
  UINTN       Pointer;
  UINT32      Density;
  UINTN       Erase;

  if (CapacityBytes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *CapacityBytes = 0;

  Status = SpiNorSfdpRead (0, Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Header[0] != 'S') || (Header[1] != 'F') || (Header[2] != 'D') ||
      (Header[3] != 'P'))
  {
    return EFI_NOT_FOUND;
  }

  Count = Header[6];
  for (Index = 0; Index < Count; Index++) {
    Status = SpiNorSfdpRead (8 + Index * 8, Param, sizeof (Param));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // JEDEC basic flash parameter table: parameter ID 0xFF00, stored with
    // the LSB first and the MSB in the last byte of the header.
    //
    if ((Param[0] != 0x00) || (Param[7] != 0xFF)) {
      continue;
    }

    Length  = Param[3];
    Pointer = (UINTN)Param[4] | ((UINTN)Param[5] << 8) | ((UINTN)Param[6] << 16);
    if ((Length < 16) || (Pointer == 0)) {
      return EFI_NOT_FOUND;
    }

    if (Length > sizeof (Bfpt)) {
      Length = sizeof (Bfpt);
    }

    Status = SpiNorSfdpRead ((UINT32)Pointer, Bfpt, Length);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // DWORD 2: density in bits, minus one.  0xFFFFFFFF means "unknown".
    //
    Density = (UINT32)Bfpt[4] | ((UINT32)Bfpt[5] << 8) |
              ((UINT32)Bfpt[6] << 16) | ((UINT32)Bfpt[7] << 24);
    if (Density == 0xFFFFFFFF) {
      return EFI_NOT_FOUND;
    }

    *CapacityBytes = (UINTN)(Density + 1) / 8;

    //
    // DWORDs 8 and 9: four erase types, each a size (log2 bytes) and an
    // instruction.  Take the 4 KB and 64 KB entries when the part has them.
    //
    for (Erase = 0; Erase < 4; Erase++) {
      UINT8  Size  = Bfpt[28 + Erase * 2];
      UINT8  Instr = Bfpt[29 + Erase * 2];

      if (Size == 12) {
        mEraseSectorOp = Instr;
      } else if (Size == 16) {
        mEraseBlockOp = Instr;
      }
    }

    DEBUG ((
      DEBUG_INFO,
      "SpiNor: SFDP %u.%u, density %u bits -> %u bytes, "
      "erase 4K %02x / 64K %02x\n",
      Header[5],
      Header[4],
      Density + 1,
      (UINT32)*CapacityBytes,
      mEraseSectorOp,
      mEraseBlockOp
      ));

    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}
