/** @file
  eMMC card identification for the Allwinner A733 (sun60iw2p1).

  Which controller the driver runs this on is board data - see
  Library/A733SmhcBoardLib.h and the A733_SMHC_PRIVATE fields the board's table
  entry fills in.

  EmmcDxe does not identify the card it binds to - it reads the CSD, the CID and
  EXT_CSD and then flips the partition configuration, and it expects all of that
  to work the moment the pass-thru protocol appears.  This file is what makes
  that true: CMD0/1/2/3, CMD9, CMD16 and CMD7 bring the device up on one bit at
  the identification clock, and CMD6 then moves it to the widest bus and the
  fastest timing the card and this controller can both hold without tuning.

  Two things the identification sequence has to respect, and the reason for most
  of what follows:

  - The eMMC spec requires the identification phase to run at fOD (400 kHz or
    less): until it has an address, the card answers with its slowest output
    timing, and a high clock reads that as nothing at all.  The controller's
    divider provides fOD here, computed from whatever module clock the driver's
    clock plan selected (see A733SmhcDxe.h).
  - The spec also allows the device up to a second to finish its power-up, and
    while it is still busy it answers CMD1 with bit 31 set and refuses CMD2.
    The driver therefore re-issues the whole CMD0 -> CMD1 -> CMD2 sequence for
    as long as that second lasts rather than asking once and believing the
    answer: the OCR's own busy bit is exactly what a mid-initialisation device
    reports wrongly, and a card that answers CMD1 but not CMD2 is a card that
    is not ready yet, not a card that is broken.  CMD2 is attempted on every
    round for that reason, and the loop stops on the first CMD2 that answers.

  The bus width and the timing are not taken on trust either.  Every CMD6 is
  followed by a read of EXT_CSD, which is a data command: a bus that is too wide
  for the wiring, or a timing the card is not in, answers with a data CRC error
  or a start-bit error, so the 512-byte read is the test.  The driver falls back
  from 8 to 4 to 1 bit and says which width it kept, and the speed-up beyond
  that is a sequence of measured steps rather than a set of settings applied and
  hoped for:

  1. the SDR stage: EXT_CSD read, bus width, HS_TIMING = 1, all at fOD, then the
     card clock raised and a read back at speed to confirm the timing held;
  2. a throughput measurement of that SDR bus (256 KiB through the driver's own
     command path), which is also what the steps after it are gated on;
  3. the HS400 step: the card through 8-bit DDR to HS_TIMING 3, the host's HS400
     fields with it, its strobe tap swept at 200 MHz, and the mode kept only if a
     read back says the card took it - the fastest rung, tried first;
  4. the HS200 step: the same 200 MHz clock, single edge, when HS400 is not
     offered or does not take;
  5. the DDR step: EXT_CSD[183] = 6 and EXT_CSD[185] = 1 with the controller's
     DDR mode bit, verified by a read back;
  and each of those three is reverted in full if any part of it fails - the card
  must end in a state the FAT path still works in.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733CcuLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <A733Ccu.h>

#include <Library/A733PmicLib.h>

#include "A733SmhcDxe.h"

//
// EXT_CSD as the card sends it: 512 bytes, and this struct names the byte
// offsets this driver reads (eMMC spec 6.99).  Byte-by-byte reads below keep
// the struct free of alignment assumptions.
//
typedef struct {
  UINT8    Reserved0[183];
  UINT8    BusWidth;                 // 183: widths the device supports
  UINT8    Reserved1;                // 184
  UINT8    HsTiming;                 // 185: 0 legacy, 1 High Speed
  UINT8    Reserved2[6];             // 186..191
  UINT8    ExtCsdRev;                // 192
  UINT8    Reserved3[3];             // 193..195
  UINT8    DeviceType;               // 196: speed modes the device runs
  UINT8    Reserved4[15];            // 197..211
  UINT8    SecCount[4];              // 212: device size in 512-byte sectors
  UINT8    Reserved5[A733_SMHC_EXT_CSD_SIZE - 216];
} A733_SMHC_EXT_CSD;

// The register is 512 bytes and CMD8 moves all of it; the struct above has to
// add up to exactly that or the offsets after a mistake would be wrong.
STATIC_ASSERT (sizeof (A733_SMHC_EXT_CSD) == A733_SMHC_EXT_CSD_SIZE, "EXT_CSD struct and its size are out of step");

/**
  Fill in a command that carries no data.

  @param[out] Command       Command to clear and fill in.
  @param[in]  Index         Command index.
  @param[in]  Argument      Command argument.
  @param[in]  CommandType   EFI_SD_MMC_COMMAND_TYPE.
  @param[in]  ResponseType  EFI_SD_MMC_RESPONSE_TYPE; ignored for a broadcast
                            command, which has no answer to type.
**/
STATIC
VOID
SmhcInitCommand (
  OUT A733_SMHC_COMMAND  *Command,
  IN  UINT8              Index,
  IN  UINT32             Argument,
  IN  UINT32             CommandType,
  IN  UINT32             ResponseType
  )
{
  ZeroMem (Command, sizeof (*Command));
  Command->Index        = Index;
  Command->Argument     = Argument;
  Command->CommandType  = CommandType;
  Command->ResponseType = ResponseType;
}

/**
  Send one command of the identification sequence and log a failure.

  @param[in,out] Private    Driver state.
  @param[in,out] Command    Command to send.
  @param[in]     TimeoutUs  Budget.

  @retval EFI_SUCCESS  The command was answered.
  @retval other        The command failed; the caller stops the sequence.
**/
STATIC
EFI_STATUS
SmhcIdentificationCommand (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN OUT A733_SMHC_COMMAND  *Command,
  IN     UINT64             TimeoutUs
  )
{
  EFI_STATUS  Status;

  Status = SmhcSendCommand (Private, Command, TimeoutUs);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: identification command CMD%u arg %08x failed: %r\n",
      Command->Index,
      Command->Argument,
      Status
      ));
  }

  return Status;
}

/**
  Read the 512-byte EXT_CSD register.

  @param[in]  Private  Driver state.
  @param[out] ExtCsd   Buffer for the register.

  @retval EFI_SUCCESS  The register was read.
  @retval other        The command or its data failed.
**/
STATIC
EFI_STATUS
SmhcCardReadExtCsd (
  IN  A733_SMHC_PRIVATE  *Private,
  OUT A733_SMHC_EXT_CSD  *ExtCsd
  )
{
  A733_SMHC_COMMAND  Command;

  ZeroMem (ExtCsd, sizeof (*ExtCsd));

  SmhcInitCommand (&Command, A733_SMHC_CMD_SEND_EXT_CSD, 0, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1);
  Command.Buffer    = ExtCsd;
  Command.Length    = sizeof (*ExtCsd);
  Command.BlockSize = sizeof (*ExtCsd);
  Command.Read      = TRUE;

  return SmhcSendCommand (Private, &Command, A733_SMHC_DATA_TIMEOUT_US);
}

/**
  Program one byte of EXT_CSD with CMD6 (SWITCH).

  CMD6 answers R1b: the card holds DATA0 low while it applies the change, and the
  command is not finished until that line is released.  SmhcSendCommand waits.

  @param[in] Private  Driver state.
  @param[in] Index    EXT_CSD byte offset.
  @param[in] Value    Byte to write.

  @retval EFI_SUCCESS  The card accepted the change.
  @retval other        The command failed.
**/
// DIAGNOSTIC ONLY: the R1 status of the last CMD6 switch, for the probe.
STATIC UINT32  mSmhcSwitchR1;

STATIC
EFI_STATUS
SmhcCardSwitch (
  IN A733_SMHC_PRIVATE  *Private,
  IN UINT32             Index,
  IN UINT32             Value
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;
  UINT32             Argument;

  //
  // CMD6's argument is [31:24] = access (0x03, write byte) | [23:16] = the
  // EXT_CSD byte to write | [15:8] = the value | [7:0] = command set.  Getting
  // the value and the access fields the other way round makes every switch
  // "succeed" while writing nothing, which is exactly how the bus width and
  // the timing mode silently stayed at their power-on values.
  //
  Argument = ((UINT32)A733_SMHC_SWITCH_ACCESS_MASK << 24) |
             ((Index & 0xFFU) << 16) |
             ((Value & 0xFFU) << 8);

  SmhcInitCommand (&Command, A733_SMHC_CMD_SWITCH, Argument, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);

  Status = SmhcIdentificationCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);

  //
  // DIAGNOSTIC ONLY: CMD6's answer is the card's status, and bit 7 is
  // SWITCH_ERROR - a card that refused the change says so here while the command
  // itself still completes.  Kept for the probe to print; remove with it.
  //
  mSmhcSwitchR1 = EFI_ERROR (Status) ? 0U : Command.Response[0];

  return Status;
}

//
// ========================== PROBES (MEASUREMENT ONLY) =======================
//
// Three probes, added to answer why HS200 is refused while all 32 sample-delay
// taps read.  They decide nothing and change nothing: the only write they route
// is EXT_CSD byte 185, through the one helper the six switch sites now share, and
// the extra reads they make *are* the measurement.  To remove them: delete this
// block, the SmhcProbe calls in SmhcCardTryHs200, and the SmhcProbeHsTimingRead
// calls at the switch sites.
//
// Reporting only: every line below is written through SMHC_DIAG, so it costs
// PcdA733SmhcDumpEnable to see it.  The measurements themselves are not behind
// the switch - they run exactly as before - because removing reads from the
// middle of the HS200 attempt would change the sequence a failure is judged on.
// The two probes that only print (SmhcProbeClock, SmhcProbeHsTimingRead) return
// without touching anything when the switch is off.
//
//   1. SmhcProbeClock - the CCU's SMHC2 register, the controller's divider and
//      both rates, before and at the HS200 plan, plus SMHC_CTRL's DDR bit, so a
//      clock that never arrived says so instead of looking like a narrow window.
//   2. SmhcCardSetHsTiming / SmhcProbeHsTimingRead - every write of EXT_CSD[185]
//      with the caller that made it, and every read-back that follows a switch.
//   3. SmhcProbeCapture50 / SmhcProbeExtCsdAtSpeed - the 512-byte EXT_CSD, its
//      byte 185 and its sector count, and the CSD, all captured at 50 MHz; then
//      two EXT_CSD reads and a CSD read at the HS200 plan, compared.  A path that
//      is sampling wrongly keeps a plausible byte or two and loses the capacity.
//
STATIC UINT8    mSmhcProbeExtCsd50[A733_SMHC_EXT_CSD_SIZE];
STATIC UINT32   mSmhcProbeCsd50[4];
STATIC UINT32   mSmhcProbeSectors50;
STATIC BOOLEAN  mSmhcProbeCaptured;

/**
  The capacity field of a CSD response, for the probe's comparison only.

  CSD bits 69:48 hold the density of a device larger than 2 GB in 512 KB units
  less one, and the controller lays the response out with word 3 holding bits
  127:96 (which is why bit 69 is bit 5 of word 2 and bit 63 is bit 31 of word 1).

  @param[in] Csd  Response words, as SmhcSendCommand leaves them.

  @return The raw C_SIZE field.
**/
STATIC
UINT32
SmhcProbeCsdSize (
  IN UINT32  *Csd
  )
{
  return ((Csd[2] & 0x3FU) << 16) | ((Csd[1] >> 16) & 0xFFFFU);
}

/**
  The EXT_CSD sector count, for the probe's comparison only.

  @param[in] ExtCsd  The 512-byte register as read.

  @return Bytes 212..215, little endian.
**/
STATIC
UINT32
SmhcProbeSectorCount (
  IN UINT8  *ExtCsd
  )
{
  return (UINT32)ExtCsd[212] |
         ((UINT32)ExtCsd[213] << 8) |
         ((UINT32)ExtCsd[214] << 16) |
         ((UINT32)ExtCsd[215] << 24);
}

/**
  Probe 1: the clock as the registers and the library see it.

  @param[in] Private  Driver state.
  @param[in] Where    Which point in the flow this is.
**/
STATIC
VOID
SmhcProbeClock (
  IN A733_SMHC_PRIVATE  *Private,
  IN CONST CHAR8        *Where
  )
{
  UINT32  Ccu;
  UINT32  ClkDiv;
  UINT32  Ctrl;

  if (!PcdGetBool (PcdA733SmhcDumpEnable)) {
    return;
  }

  //
  // The register belonging to the controller this instance drives.  Reading
  // SMHC2's pair here said 24 MHz while the card measured a 50 MHz bus, which is
  // how a diagnostic line can argue against the hardware it is there to describe.
  //
  Ccu    = MmioRead32 ((UINTN)PcdGet64 (PcdA733CcuBase) + Private->CcuClkReg);
  ClkDiv = SmhcRead (Private->Base, A733_SMHC_CLKDIV);
  Ctrl   = SmhcRead (Private->Base, A733_SMHC_CTRL);

  DEBUG ((
    DEBUG_ERROR,
    "SmhcProbe: clock %a: ccu %08x (src %u, M %u, N %u, gate %u) ccu_hz %u kHz, clkdiv %08x (div %u, enb %u, mask_dat0 %u), "
    "card %u kHz, module(plan) %u kHz, plan %a, div(state) %u, ctrl %08x (ddr bit %u, state %u)\n",
    Where,
    Ccu,
    (Ccu >> A733_CCU_CLK_SRC_SHIFT) & 0x7U,
    Ccu & A733_CCU_CLK_FACTOR_M_MASK,
    (Ccu & A733_CCU_CLK_FACTOR_N_MASK) >> A733_CCU_CLK_FACTOR_N_SHIFT,
    (Ccu & A733_CCU_CLK_GATING) != 0,
    (UINT32)(A733CcuGetModuleClockHz (Private->CcuModule) / 1000U),
    ClkDiv,
    ClkDiv & A733_SMHC_CLKDIV_CCLK_DIV_MASK,
    (ClkDiv & A733_SMHC_CLKDIV_CCLK_ENB) != 0,
    (ClkDiv & A733_SMHC_CLKDIV_MASK_DAT0) != 0,
    SmhcClockKhz (Private),
    Private->ModuleClockHz / 1000U,
    SmhcClockPlanName (Private->ClockProfile, Private->ClockPlan),
    Private->DataDivisor,
    Ctrl,
    (Ctrl & A733_SMHC_CTRL_DDR_MODE) != 0,
    (UINT32)Private->DdrModeBit
    ));
}

/**
  Probe 2, write side: the one place EXT_CSD byte 185 is written.

  @param[in,out] Private  Driver state.
  @param[in]     Value    The HS_TIMING value to write.
  @param[in]     Tag      Short caller tag for the log.

  @retval EFI_SUCCESS  The card accepted the switch.
  @retval other        The command failed; the caller decides.
**/
STATIC
EFI_STATUS
SmhcCardSetHsTiming (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             Value,
  IN     CONST CHAR8        *Tag
  )
{
  EFI_STATUS  Status;

  SMHC_DIAG ((
    DEBUG_ERROR,
    "SmhcProbe: HS_TIMING <- %u from %a (state HsTiming %u, Ddr %u, ddr bit %u, card %u kHz)\n",
    Value,
    Tag,
    Private->HsTiming,
    (UINT32)Private->Ddr,
    (UINT32)Private->DdrModeBit,
    SmhcClockKhz (Private)
    ));

  Status = SmhcCardSwitch (Private, A733_SMHC_EXT_CSD_HS_TIMING, Value);
  if (!EFI_ERROR (Status)) {
    //
    // CMD6's answer carries the card's status, and bit 7 is SWITCH_ERROR: a card
    // that refused the mode change says so here while the command still
    // completes.  Without this the refusal is only visible in the read-back
    // afterwards, where it cannot be told apart from a read that went wrong.
    //
    SMHC_DIAG ((
      DEBUG_ERROR,
      "SmhcProbe: HS_TIMING <- %u from %a: R1 status %08x, SWITCH_ERROR %u\n",
      Value,
      Tag,
      mSmhcSwitchR1,
      (mSmhcSwitchR1 >> 7) & 1U
      ));
  }

  return Status;
}

/**
  Probe 2, read side: what byte 185 read back as, right after a switch.

  @param[in] Private  Driver state.
  @param[in] Tag      Short tag naming the read.
  @param[in] Wanted   The value that was written.
  @param[in] ExtCsd   The register just read.
**/
STATIC
VOID
SmhcProbeHsTimingRead (
  IN A733_SMHC_PRIVATE  *Private,
  IN CONST CHAR8        *Tag,
  IN UINT32             Wanted,
  IN A733_SMHC_EXT_CSD  *ExtCsd
  )
{
  if (!PcdGetBool (PcdA733SmhcDumpEnable)) {
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "SmhcProbe: HS_TIMING read back %u (wanted %u) %a (state HsTiming %u, Ddr %u, BUS_WIDTH %u, ddr bit %u, card %u kHz)\n",
    ExtCsd->HsTiming,
    Wanted,
    Tag,
    Private->HsTiming,
    (UINT32)Private->Ddr,
    ExtCsd->BusWidth,
    (UINT32)Private->DdrModeBit,
    SmhcClockKhz (Private)
    ));
}

/**
  Probe 3: read the CSD (CMD9) for the probe's comparison.

  This is the driver's own command path and its own 48-bit response shape; the
  command carries no data, so it says whether the card still answers at all, and
  the words it returns are compared with the ones the 50 MHz capture saw.

  @param[in,out] Private  Driver state.
  @param[out]    Csd      Receives the four response words.

  @retval EFI_SUCCESS  The card answered.
  @retval other        The command failed.
**/
STATIC
EFI_STATUS
SmhcProbeReadCsd (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    UINT32             Csd[4]
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;

  SmhcInitCommand (&Command, A733_SMHC_CMD_SEND_CSD, Private->Rca << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR2);

  Status = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  CopyMem (Csd, Command.Response, sizeof (Command.Response));

  return Status;
}

/**
  Probe 3, capture side: the 50 MHz reference, taken before the HS200 switch.

  @param[in,out] Private  Driver state.
**/
STATIC
VOID
SmhcProbeCapture50 (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_EXT_CSD  ExtCsd;
  UINT32             Csd[4];
  EFI_STATUS         Status;

  mSmhcProbeCaptured = FALSE;

  Status = SmhcCardReadExtCsd (Private, &ExtCsd);
  if (EFI_ERROR (Status)) {
    SMHC_DIAG ((DEBUG_ERROR, "SmhcProbe: the 50 MHz reference EXT_CSD read failed: %r\n", Status));
    return;
  }

  CopyMem (mSmhcProbeExtCsd50, &ExtCsd, sizeof (ExtCsd));

  Status = SmhcProbeReadCsd (Private, Csd);
  if (EFI_ERROR (Status)) {
    SMHC_DIAG ((DEBUG_ERROR, "SmhcProbe: the 50 MHz reference CSD read failed: %r\n", Status));
    return;
  }

  CopyMem (mSmhcProbeCsd50, Csd, sizeof (Csd));
  mSmhcProbeSectors50 = SmhcProbeSectorCount (mSmhcProbeExtCsd50);
  mSmhcProbeCaptured  = TRUE;

  SMHC_DIAG ((
    DEBUG_ERROR,
    "SmhcProbe: 50 MHz reference: ext_csd[185] %u [183] %u [196] %02x [192] %u, %u sectors, c_size %u, CSD %08x %08x %08x %08x\n",
    ExtCsd.HsTiming,
    ExtCsd.BusWidth,
    ExtCsd.DeviceType,
    ExtCsd.ExtCsdRev,
    mSmhcProbeSectors50,
    SmhcProbeCsdSize (Csd),
    Csd[0],
    Csd[1],
    Csd[2],
    Csd[3]
    ));
}

/**
  Probe 3, compare side: the EXT_CSD twice and the CSD once, at the HS200 plan and
  the chosen tap, against the 50 MHz reference and against each other.

  @param[in,out] Private  Driver state.
**/
STATIC
VOID
SmhcProbeExtCsdAtSpeed (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_EXT_CSD  First;
  A733_SMHC_EXT_CSD  Second;
  UINT8              *FirstBytes;
  UINT8              *SecondBytes;
  UINT32             Csd[4];
  UINT32             RefDiff;
  UINT32             RefFirst;
  UINT32             SelfDiff;
  UINT32             SelfFirst;
  UINT32             Index;
  UINT32             TicksPerMicro;
  UINT32             FirstUs;
  UINT32             SecondUs;
  UINT64             Frequency;
  UINT64             Start;
  EFI_STATUS         Status;

  if (!mSmhcProbeCaptured) {
    SMHC_DIAG ((DEBUG_ERROR, "SmhcProbe: no 50 MHz reference was captured, so nothing can be compared\n"));
    mSmhcProbeCaptured = FALSE;
    return;
  }

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);

  //
  // The two reads are also timed: 1 KiB of data takes about 40 us on a real
  // 200 MHz bus and about 165 us on 50 MHz, so the pair says whether the clock
  // arrived at all - which no amount of window shape can say by itself.  (A read
  // that fails prints its own line after the timing, so its figure includes that;
  // the status is in the same probe line.)
  //
  Start  = GetPerformanceCounter ();
  Status = SmhcCardReadExtCsd (Private, &First);
  FirstUs = (UINT32)((GetPerformanceCounter () - Start) / TicksPerMicro);
  if (EFI_ERROR (Status)) {
    SMHC_DIAG ((DEBUG_ERROR, "SmhcProbe: the first at-speed EXT_CSD read failed: %r (took %u us)\n", Status, FirstUs));
    return;
  }

  Start   = GetPerformanceCounter ();
  Status  = SmhcCardReadExtCsd (Private, &Second);
  SecondUs = (UINT32)((GetPerformanceCounter () - Start) / TicksPerMicro);
  if (EFI_ERROR (Status)) {
    SMHC_DIAG ((DEBUG_ERROR, "SmhcProbe: the second at-speed EXT_CSD read failed: %r (took %u us)\n", Status, SecondUs));
    return;
  }

  FirstBytes  = (UINT8 *)&First;
  SecondBytes = (UINT8 *)&Second;

  RefDiff  = 0;
  RefFirst = 0;
  SelfDiff = 0;
  SelfFirst = 0;

  for (Index = 0; Index < A733_SMHC_EXT_CSD_SIZE; Index++) {
    if (FirstBytes[Index] != mSmhcProbeExtCsd50[Index]) {
      if (RefDiff == 0) {
        RefFirst = Index;
      }

      RefDiff++;
    }

    if (FirstBytes[Index] != SecondBytes[Index]) {
      if (SelfDiff == 0) {
        SelfFirst = Index;
      }

      SelfDiff++;
    }
  }

  //
  // The word that answers "is this the same card": the capacity the register
  // carries.  Byte 185 can come back plausible while the block behind it is not,
  // so both are printed with it.
  //
  ZeroMem (Csd, sizeof (Csd));
  Status = SmhcProbeReadCsd (Private, Csd);

  SMHC_DIAG ((
    DEBUG_ERROR,
    "SmhcProbe: at speed (%u kHz, div %u, tap %u): ext_csd[185] %u [183] %u [196] %02x [192] %u, %u sectors (50 MHz %u)"
    ", c_size %u (50 MHz %u)%a, CSD %08x %08x %08x %08x, %u/%u bytes differ from the 50 MHz copy (first at %u), "
    "%u/%u between the two at-speed reads (first at %u), reads took %u and %u us%a\n",
    SmhcClockKhz (Private),
    Private->DataDivisor,
    Private->SampleDelay,
    Second.HsTiming,
    Second.BusWidth,
    Second.DeviceType,
    Second.ExtCsdRev,
    SmhcProbeSectorCount (SecondBytes),
    mSmhcProbeSectors50,
    SmhcProbeCsdSize (Csd),
    SmhcProbeCsdSize (mSmhcProbeCsd50),
    EFI_ERROR (Status) ? ", CSD read failed" : "",
    Csd[0],
    Csd[1],
    Csd[2],
    Csd[3],
    RefDiff,
    (UINT32)A733_SMHC_EXT_CSD_SIZE,
    RefFirst,
    SelfDiff,
    (UINT32)A733_SMHC_EXT_CSD_SIZE,
    SelfFirst,
    FirstUs,
    SecondUs,
    (SelfDiff == 0) ? "" : " (unstable)"
    ));
}
//
// ======================= END OF PROBES ======================================
//

/**
  Move the card and the controller to one bus width, then check the bus by
  reading EXT_CSD over it.

  EXT_CSD[183] takes 0 for 1 bit, 1 for 4 and 2 for 8, which is the same
  encoding SMHC_CTYPE uses (A733_SMHC_BUS_*), so one value serves both.

  @param[in]  Private  Driver state.
  @param[in]  Width    A733_SMHC_BUS_8BIT, _4BIT or _1BIT.
  @param[out] ExtCsd   Receives the EXT_CSD read that proves the width works.

  @retval EFI_SUCCESS  The width works.
  @retval other        The width does not work: a command or the read over it
                       failed.
**/
STATIC
EFI_STATUS
SmhcCardTryBusWidth (
  IN  A733_SMHC_PRIVATE  *Private,
  IN  UINT32             Width,
  OUT A733_SMHC_EXT_CSD  *ExtCsd
  )
{
  EFI_STATUS  Status;

  Status = SmhcCardSwitch (Private, A733_SMHC_EXT_CSD_BUS_WIDTH, Width);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The controller has to follow the card: the card drives the extra data
  // lines from the next command on, so the controller must be listening to
  // them when that command's data arrives.
  //
  SmhcSetBusWidth (Private->Base, Width);
  Private->BusWidth = Width;

  return SmhcCardReadExtCsd (Private, ExtCsd);
}

/**
  Wait for the card's busy line before aiming a command at it.

  Not every command needs this, but CMD2 and CMD3 are the ones that come after a
  card that may still be working (CMD1's power-up, and the address assignment),
  and a card that is holding DATA0 low cannot answer.  A timeout here is
  reported and then the command is sent anyway: the command's own result is the
  more useful half of the log, and the pair says whether the busy line was real.

  @param[in] Private  Driver state.
  @param[in] Name     Command name, for the debug line.
**/
STATIC
VOID
SmhcCardWaitIdle (
  IN A733_SMHC_PRIVATE  *Private,
  IN CONST CHAR8        *Name
  )
{
  if (EFI_ERROR (SmhcWaitCardIdle (Private->Base, A733_SMHC_CMD_TIMEOUT_US))) {
    DEBUG (
      (DEBUG_ERROR,
       "Smhc: %a: the card was still busy (STATUS %08x) after %u us; sending it anyway\n",
       Name,
       SmhcRead (Private->Base, A733_SMHC_STATUS),
       A733_SMHC_CMD_TIMEOUT_US)
      );
  }
}

/**
  DIAGNOSTIC: ask the card for its status word at one relative address.

  Only reaches the log when identification has given up: CMD13 is what tells an
  idle card from an addressed one, and the address boot0 left behind from one RCA
  to another.
**/
STATIC
VOID
SmhcDiagCurrentState (
  IN A733_SMHC_PRIVATE  *Private,
  IN CONST CHAR8        *When,
  IN UINT32             Rca
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;
  UINT32             Word;

  SmhcInitCommand (&Command, 13, Rca << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  Word   = Command.Response[0];

  //
  // CMD13's status word: bits 12:9 are CURRENT_STATE (0 idle, 1 ready,
  // 2 ident, 3 stby, 4 trans, 5 data, 6 rcv, 7 prg), bit 8 is READY_FOR_DATA.
  //
  DEBUG ((
    DEBUG_ERROR,
    "SmhcDiag: %a CMD13 rca %u: %r rint %08x status %08x word %08x state %u ready %u\n",
    When,
    Rca,
    Status,
    SmhcRead (Private->Base, A733_SMHC_RINTSTS),
    SmhcRead (Private->Base, A733_SMHC_STATUS),
    Word,
    (Word >> 9) & 0xFU,
    (Word >> 8) & 0x1U
    ));
}

/**
  One round of the identification sequence: CMD0, then CMD1, then CMD2.

  Everything the sequence can learn about the device is learned afresh here, so
  that a device which was not ready for the last round gets a complete attempt at
  this one: CMD0 puts it back in idle with 74 clocks of initialization, CMD1
  collects its OCR (which says whether it has left power-up), and CMD2 is
  attempted whether or not that OCR bit looked clear, because a device that is
  still busy is exactly the one whose OCR bit is not to be trusted.

  Each command gets at most what is left of the phase's budget, so a slow
  command cannot outlive the loop that bounds the phase.

  @param[in,out] Private   Driver state.
  @param[in,out] Budget    The identification phase's wall-clock budget.
  @param[in]     Round     Round number, reported in the log line.
  @param[out]    Ocr       Receives what CMD1 answered, 0 when it did not answer.
  @param[out]    Cid       Receives CMD2's four response words when it answered.
  @param[out]    Answered  Receives whether CMD1 answered at all this round.

  @retval EFI_SUCCESS  CMD2 answered: the device has given its CID.
  @retval other        The round did not get that far; the caller decides.
**/
STATIC
EFI_STATUS
SmhcCardIdentifyRound (
  IN OUT A733_SMHC_PRIVATE   *Private,
  IN OUT A733_SMHC_DEADLINE  *Budget,
  IN     UINT32              Round,
  OUT    UINT32              *Ocr,
  OUT    UINT32              Cid[4],
  OUT    BOOLEAN             *Answered
  )
{
  EFI_STATUS         Status;
  A733_SMHC_COMMAND  Command;
  UINT64             Slice;
  UINT32             RemainingUs;

  *Ocr      = 0;
  *Answered = FALSE;

  //
  // Every command of a round is given a slice of the phase's remaining budget
  // and no more (A733_SMHC_IDENT_CMD_TIMEOUT_US): a command that never reports
  // anything would otherwise spend the whole second on its own and end the phase
  // after a single round, which is not what a bounded phase is for.
  //
  //
  // CMD0 with the initialization sequence: 74 clocks on the command line while
  // the card is idle, which is what puts it in the idle state from whatever
  // state it was left in.  The vendor tree sends argument 0 (not 0xFFFFFFFA)
  // and lets the controller generate the clocks through SEND_INIT_SEQ.
  //
  //
  // CMD0 is sent once, on the first round only.  Repeating it every round
  // would push the card back to idle just as it was making progress through its
  // power-up, which is the one thing a retry loop must not do; the rounds after
  // the first are there to give the device time, not to reset it again.
  //
  if (Round == 1) {
    SmhcInitCommand (&Command, A733_SMHC_CMD_GO_IDLE_STATE, 0, SdMmcCommandTypeBc, SdMmcResponseTypeR1);
    Command.SendInitSeq = TRUE;

    Slice  = SmhcDeadlineRemainingUs (Budget);
    Status = SmhcSendCommand (Private, &Command, MIN (Slice, A733_SMHC_IDENT_CMD_TIMEOUT_US));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // 74 clocks is 185 us at 400 kHz; the rest of the settling below is the
    // card's own time to act on the reset before the next command is aimed at
    // it.
    //
    MicroSecondDelay (A733_SMHC_CMD0_SETTLE_US);
  }

  //
  // CMD1: a device that is present answers with its OCR, whether or not it has
  // finished powering up.  No answer at all is the signature of a device that
  // is not there, which is worth one DEBUG_INFO line and not a line per round.
  //
  SmhcInitCommand (
    &Command,
    A733_SMHC_CMD_SEND_OP_COND,
    A733_SMHC_OCR_ARG,
    SdMmcCommandTypeBcr,
    SdMmcResponseTypeR3
    );

  //
  // Poll CMD1 until the device reports that its power-up cycle is over (OCR
  // bit 31 set).  Sending CMD2 before that is what made identification fail
  // with a response timeout while CMD1 answered perfectly: a device still
  // powering up takes CMD1 and ignores the rest.
  //
  for (RemainingUs = A733_SMHC_OCR_POLL_US; RemainingUs > 0; RemainingUs -= 100U) {
    Slice  = SmhcDeadlineRemainingUs (Budget);
    Status = SmhcSendCommand (Private, &Command, MIN (Slice, A733_SMHC_IDENT_CMD_TIMEOUT_US));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_INFO, "Smhc: ident round %u: no answer to CMD1 (%r)\n", Round, Status));
      return Status;
    }

    *Ocr      = Command.Response[0];
    *Answered = TRUE;
    if ((*Ocr & A733_SMHC_OCR_POWER_UP_DONE) != 0) {
      break;
    }

    MicroSecondDelay (100);
  }

  if ((*Ocr & A733_SMHC_OCR_POWER_UP_DONE) == 0) {
    DEBUG ((DEBUG_INFO, "Smhc: ident round %u: still powering up (OCR %08x)\n", Round, *Ocr));
    return EFI_NOT_READY;
  }

  //
  // CMD2 asks every device on the bus for its CID, and is what this phase is
  // really waiting for: a device that answers it is out of the busy phase and
  // identified.  It is the identity of the device, so the driver reads it for
  // the operator to see, and addresses the device by CMD3's address afterwards.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_ALL_SEND_CID, 0, SdMmcCommandTypeBcr, SdMmcResponseTypeR2);

  Slice  = SmhcDeadlineRemainingUs (Budget);
  Status = SmhcSendCommand (Private, &Command, MIN (Slice, A733_SMHC_IDENT_CMD_TIMEOUT_US));
  CopyMem (Cid, Command.Response, sizeof (Command.Response));

  if (PcdGetBool (PcdA733SmhcDumpEnable)) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: ident round %u: OCR %08x, CMD2 %r (rint %08x, irq count %u)\n",
      Round,
      *Ocr,
      Status,
      Private->IntStatus,
      Private->InterruptCount
      ));
  }

  return Status;
}

/**
  DIAGNOSTIC: print everything identification can see, once it has given up.

  Two things make this worth its command traffic when the phase fails and not
  worth it when it succeeds (which is why none of it runs on the success path):
  it says whether CMD0 really did put the card back in idle, by asking CMD13 at
  every plausible relative address, and it tries the four framings of a 136-bit
  response for CMD2, since "one of the DMA/response flag combinations is the one
  the controller wants" is a question a successful run never has to answer.
**/
STATIC
VOID
SmhcCardDiagnoseIdentification (
  IN A733_SMHC_PRIVATE  *Private,
  IN UINT32             Ocr,
  IN UINT32             Rounds
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;
  UINT32             Retry;
  UINT32             Variant;
  UINT32             RintBefore;

  DEBUG ((
    DEBUG_ERROR,
    "SmhcDiag: identification gave up after %u rounds; last OCR %08x, card clock %u kHz, irq count %u\n",
    Rounds,
    Ocr,
    SmhcClockKhz (Private),
    Private->InterruptCount
    ));

  SmhcDiagCurrentState (Private, "after the rounds", 1);
  SmhcDiagCurrentState (Private, "after the rounds", 0);

  //
  // DIAGNOSTIC: the addresses a card could still be answering at.  Every one of
  // these timing out says CMD0 did reset the card (it is no longer addressed),
  // which is the difference between "the card never got identified" and "the card
  // ignored the reset".
  //
  for (Retry = 0; Retry < 16; Retry++) {
    SmhcInitCommand (&Command, 13, Retry << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
    Status = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
    if (!EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "SmhcDiag: CMD13 answers for RCA %u: word %08x state %u ready %u\n",
        Retry,
        Command.Response[0],
        (Command.Response[0] >> 9) & 0xFU,
        (Command.Response[0] >> 8) & 0x1U
        ));
    }
  }

  //
  // DIAGNOSTIC: the framings of CMD2's long response, one per plausible flag
  // combination, so a failing run says which one the controller would want.
  //
  for (Variant = 0; Variant < 4; Variant++) {
    SmhcCardWaitIdle (Private, "before the CMD2 variants");
    SmhcInitCommand (&Command, A733_SMHC_CMD_ALL_SEND_CID, 0, SdMmcCommandTypeBcr, SdMmcResponseTypeR2);
    Command.NoCrcCheck   = (BOOLEAN)((Variant == 1) || (Variant == 3));
    Command.NoLongResp   = (BOOLEAN)(Variant == 3);
    Command.ForcePreOver = (BOOLEAN)(Variant == 2);

    RintBefore = SmhcRead (Private->Base, A733_SMHC_RINTSTS);
    Status     = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
    DEBUG ((
      DEBUG_ERROR,
      "SmhcDiag: CMD2 v%u (nocrc %u nolong %u preover %u) %r rint %08x -> %08x status %08x resp %08x %08x %08x %08x\n",
      Variant,
      Command.NoCrcCheck,
      Command.NoLongResp,
      Command.ForcePreOver,
      Status,
      RintBefore,
      SmhcRead (Private->Base, A733_SMHC_RINTSTS),
      SmhcRead (Private->Base, A733_SMHC_STATUS),
      Command.Response[0],
      Command.Response[1],
      Command.Response[2],
      Command.Response[3]
      ));
  }
}

/**
  Bring the device to the state where it has an address and is selected.

  Runs the whole sequence at the identification clock: CMD0 (idle, with the
  initialization sequence), CMD1 and CMD2 as one round repeated until the device
  answers CMD2 or the phase's one-second budget is spent, then CMD3 (address),
  CMD9 (CSD), CMD16 (block length) and CMD7 (select).  A failed round is retried
  from CMD0 with the controller re-initialized, because a response timeout can
  leave the command FSM parked mid-command.  The caller prepares the controller
  once before the first round; every later round starts by preparing it again.

  @param[in,out] Private  Driver state; Rca and SectorAddressing are filled in.
  @param[in,out] Budget   The identification phase's wall-clock budget.

  @retval EFI_SUCCESS       The card is addressed and selected.
  @retval EFI_NO_MEDIA      The device never answered CMD1 at all.
  @retval EFI_DEVICE_ERROR  The device answered CMD1 but never left power-up.
  @retval other             A command of the sequence failed.
**/
STATIC
EFI_STATUS
SmhcCardIdentify (
  IN OUT A733_SMHC_PRIVATE   *Private,
  IN OUT A733_SMHC_DEADLINE  *Budget
  )
{
  EFI_STATUS         Status;
  A733_SMHC_COMMAND  Command;
  UINT32             Cid[4];
  UINT32             Ocr;
  UINT32             Round;
  UINT32             Rounds;
  BOOLEAN            Answered;

  Cid[0]   = 0;
  Cid[1]   = 0;
  Cid[2]   = 0;
  Cid[3]   = 0;
  Ocr      = 0;
  Answered = FALSE;
  Status   = EFI_TIMEOUT;

  for (Round = 1; Round <= A733_SMHC_IDENT_ROUNDS; Round++) {
    Rounds = Round;

    //
    // The first round runs on the controller the caller prepared; every later
    // one starts from the same state, because the round before it failed
    // somewhere and may have left the command FSM parked.  A controller that
    // will not come back does not end the phase either: the round runs against
    // whatever state it is in, fails, and the next round tries again until the
    // budget or the round ceiling is what stops the loop.
    //
    if (Round != 1) {
      if (EFI_ERROR (SmhcPrepareController (Private, Private->IdentDivisor))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the controller did not come back for round %u\n", Round));
      }
    }

    Status = SmhcCardIdentifyRound (Private, Budget, Round, &Ocr, Cid, &Answered);

    if (!EFI_ERROR (Status)) {
      break;
    }

    if (SmhcDeadlineExpired (Budget)) {
      break;
    }

    MicroSecondDelay (A733_SMHC_IDENT_ROUND_DELAY_US);
  }

  if (EFI_ERROR (Status)) {
    SmhcCardDiagnoseIdentification (Private, Ocr, Rounds);

    if (!Answered) {
      //
      // CMD1 never answered once in the whole phase, which is what a slot with
      // no device in it looks like; the identity of the failure is worth more
      // than the last command's error code.
      //
      return EFI_NO_MEDIA;
    }

    return Status;
  }

  if (PcdGetBool (PcdA733SmhcDumpEnable)) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: CID %08x %08x %08x %08x (after %u rounds)\n",
      Cid[0],
      Cid[1],
      Cid[2],
      Cid[3],
      Round
      ));
  }

  if ((Ocr & A733_SMHC_OCR_SECTOR_MODE) == 0) {
    //
    // The device did not take the sector-mode request.  This is a warning and
    // not a refusal: how a data command's argument is read is decided by the
    // card's CSD (read below), which is the same field EmmcDxe reads, so the
    // two layers address the same blocks either way.
    //
    DEBUG ((
      DEBUG_INFO,
      "Smhc: device did not report sector addressing (OCR %08x); following the CSD instead\n",
      Ocr
      ));
  }

  //
  // CMD3 hands the device an address.  The driver asks for address 1 and then
  // uses the address the device answered with, which its response carries in
  // the top half.
  //
  SmhcCardWaitIdle (Private, "before CMD3");
  SmhcInitCommand (&Command, A733_SMHC_CMD_SET_RELATIVE_ADDR, 1U << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR6);
  Status = SmhcIdentificationCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->Rca = (Command.Response[0] >> 16) & 0xFFFFU;

  //
  // The address the driver uses has to be the one EmmcDxe will use: it
  // addresses the device as slot + 1 (see its EmmcIdentification), so a card
  // that answered anything but 1 for the 1 it was given would be addressed two
  // different ways by the two layers.  The lower layer yields here.
  //
  if (Private->Rca != 1) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: card answered RCA %u for the requested 1; using 1\n",
      Private->Rca
      ));
    Private->Rca = 1;
  }

  //
  // CMD9 reads the CSD.  The CSD structure field is how the device addresses
  // data: structure 3 is sector addressing, and EmmcDxe reads the same field to
  // decide whether a data command's argument is a sector number or a byte
  // offset.  The split path in SmhcPassThru needs the same answer.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_SEND_CSD, Private->Rca << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR2);
  Status = SmhcIdentificationCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->SectorAddressing = ((Command.Response[3] >> 30) & 0x3U) == 0x3U;

  if (PcdGetBool (PcdA733SmhcDumpEnable)) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: CSD %08x %08x %08x %08x (structure %u)\n",
      Command.Response[0],
      Command.Response[1],
      Command.Response[2],
      Command.Response[3],
      (Command.Response[3] >> 30) & 0x3U
      ));
  }

  //
  // CMD7 selects the device.  R1b: the device is busy for a while after being
  // selected, and no command may be sent until it releases the line - which
  // SmhcSendCommand waits for, so the clock may be raised as soon as this
  // returns.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_SELECT_CARD, Private->Rca << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
  Status = SmhcIdentificationCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // CMD16 sets the block length, and is only legal once the device is selected:
  // in the Stand-by state it is ignored, which is what a timeout on it means.
  // An eMMC device is 512 bytes per block whatever it is told and this stack
  // uses nothing else, so the command is here for completeness and its failure
  // is not fatal.
  //
  SmhcInitCommand (
    &Command,
    A733_SMHC_CMD_SET_BLOCKLEN,
    A733_SMHC_BLOCK_SIZE,
    SdMmcCommandTypeAc,
    SdMmcResponseTypeR1
    );
  if (EFI_ERROR (SmhcIdentificationCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US))) {
    DEBUG ((DEBUG_INFO, "Smhc: CMD16 did not answer; the device is still usable at 512 bytes per block\n"));
  }

  return EFI_SUCCESS;
}

/**
  Put the card into High Speed SDR timing and read back what it reports.

  CMD6 with EXT_CSD[185] = 1 is the switch; the read of EXT_CSD afterwards is
  both the proof that the card is still answering on the bus and the value the
  caller reports.  The switch is retried once, because a card that ignored the
  first CMD6 would otherwise be left in compatibility timing while the driver
  clocked it at 50 MHz, which is the one combination this pass cannot survive.

  @param[in,out] Private  Driver state; HighSpeed is filled in.
  @param[out]    ExtCsd   Receives the EXT_CSD read that follows the switch.
  @param[in]     Khz      Card clock in kHz, for the log.

  @retval EFI_SUCCESS  The card reports HS_TIMING = 1 at this clock.
  @retval other        The switch or its verifying read failed.
**/
STATIC
EFI_STATUS
SmhcCardSetHighSpeed (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    A733_SMHC_EXT_CSD  *ExtCsd,
  IN     UINT32             Khz
  )
{
  EFI_STATUS  Status;
  UINT32      Attempt;

  Private->HighSpeed = FALSE;

  for (Attempt = 0; Attempt < 2; Attempt++) {
    Status = SmhcCardSetHsTiming (Private, A733_SMHC_HS_TIMING_HIGH_SPEED, "sdrmode");
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: switching to High Speed failed: %r\n", Status));
      return Status;
    }

    Status = SmhcCardReadExtCsd (Private, ExtCsd);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the read after the High Speed switch failed: %r\n", Status));
      return Status;
    }

    SmhcProbeHsTimingRead (Private, "after the sdrmode switch", A733_SMHC_HS_TIMING_HIGH_SPEED, ExtCsd);

    if (ExtCsd->HsTiming == A733_SMHC_HS_TIMING_HIGH_SPEED) {
      Private->HsTiming  = ExtCsd->HsTiming;
      Private->HighSpeed = TRUE;

      DEBUG ((
        DEBUG_INFO,
        "Smhc: SDR timing: EXT_CSD[185] HS_TIMING %u and EXT_CSD[183] BUS_WIDTH %u read back at %u kHz\n",
        ExtCsd->HsTiming,
        ExtCsd->BusWidth,
        Khz
        ));

      return EFI_SUCCESS;
    }

    DEBUG ((
      DEBUG_ERROR,
      "Smhc: EXT_CSD[185] reads back %u for the %u that was written at %u kHz%a\n",
      ExtCsd->HsTiming,
      A733_SMHC_HS_TIMING_HIGH_SPEED,
      Khz,
      (Attempt == 0) ? "; retrying the switch" : ", so the card is not in the timing this pass needs"
      ));
  }

  return EFI_DEVICE_ERROR;
}

/**
  Measure 256 KiB at the 12 MHz plan, then put the driver's own plan back.

  The plan the driver runs on is a rate the CCU claims; the rate the card bus
  actually ran at is what a measurement shows.  Both are worth having from one
  boot, and the HOSC plan is the one rate in the whole picture that is known
  exactly (a 24 MHz oscillator divided by two), so the two numbers together say
  what the fast plan really is: their ratio is the CCU divider's arithmetic
  checked against the silicon.

  Only the module clock and the controller divider change here - the card stays
  in the SDR timing it was already switched to - so this is the same kind of
  clock change the identification-to-data step makes, done twice more.  The
  card's own state is verified with a real data command after the plan is put
  back, and a restore that does not take is reported rather than left to fail
  later: the caller drops to the HOSC plan and skips the DDR step in that case.

  @param[in,out] Private  Driver state; ClockPlan and the clock fields follow the
                         plan that ends up in use.

  @retval EFI_SUCCESS  The baseline was measured and the plan was restored.
  @retval other        The plan could not be put back; the caller must not build
                       on the fast plan.
**/
STATIC
EFI_STATUS
SmhcCardMeasureBaseline (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS         Status;
  A733_SMHC_EXT_CSD  ExtCsd;
  UINT32             Plan;

  Plan = Private->ClockPlan;

  Status = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HOSC);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot switch to the HOSC plan for the baseline measurement: %r\n", Status));
    return Status;
  }

  SmhcSetClock (Private, Private->DataDivisor);
  SmhcReportThroughput (Private);

  Status = SmhcSelectClockPlan (Private, Plan);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot put the %a plan back: %r\n", SmhcClockPlanName (Private->ClockProfile, Plan), Status));
    return Status;
  }

  SmhcSetClock (Private, Private->DataDivisor);

  //
  // The clock change itself is not the proof that the card is still there: a
  // read over the restored bus is.
  //
  Status = SmhcCardReadExtCsd (Private, &ExtCsd);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: the %a plan did not read back after being restored: %r\n",
      SmhcClockPlanName (Private->ClockProfile, Plan),
      Status
      ));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "Smhc: back on the %a plan: %u kHz, EXT_CSD[185] %u, EXT_CSD[183] %u read back\n",
    SmhcClockPlanName (Private->ClockProfile, Plan),
    SmhcClockKhz (Private),
    ExtCsd.HsTiming,
    ExtCsd.BusWidth
    ));

  return EFI_SUCCESS;
}

/**
  Decide whether the SDR bus the driver just measured is the bus the clock plan
  claims, and get the card and the driver onto a plan that works when it is not.

  The number this judges is the *marginal* rate: what the larger of the two
  measured transfers added over the smaller.  A single transfer carries a fixed
  cost that has nothing to do with the bus - the driver's own per-command debug
  output alone is about 40 ms of console time, i.e. eight times a 256 KiB transfer
  at 50 MHz - and an absolute figure is dominated by it.  Judging the marginal
  rate is not the same as widening the band: the band is unchanged, so a plan
  that is genuinely wrong still fails, it is only the fixed cost that no longer
  counts against it.

  Two things can disagree with the plan, and both are answered by moving to the
  one plan whose rate is known: a rate meaningfully above the plan's own ceiling
  for the bus width in use means the card clock is not the rate the CCU and the
  divider give (a divider that did not take, or a parent the library mis-names),
  and a rate below a third of that ceiling means the plan's rate did not
  materialise on the bus at all.  Neither is something to build a DDR step on,
  and neither is something to leave the card on, since the 12 MHz plan is the
  rate the data path was verified at.

  A rate that is merely lower than the ceiling is not a fault: the ceiling is a
  theoretical maximum, and the per-command and per-block overheads of a real
  transfer can only ever take the measurement further below it.

  @param[in,out] Private       Driver state.
  @param[in]     Rate          The two measurements just taken.
  @param[out]    Proven        TRUE only when the marginal rate is inside the
                               band, i.e. only when the plan's rate is shown to
                               be real and the DDR step may be built on it.

  @retval EFI_SUCCESS  The plan is kept: either its rate is proven, or the
                       marginal rate could not be formed at all (in which case
                       nothing is proven and nothing is reverted either).
  @retval other        The driver has moved to the HOSC plan (or could not), and
                       the caller must not build on the fast plan.  The error is
                       returned even when the revert itself measured cleanly,
                       because that measurement describes the fallback and not
                       the plan under test.
**/
STATIC
EFI_STATUS
SmhcCardCheckSdrRate (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     A733_SMHC_RATE     *Rate,
  OUT    BOOLEAN            *Proven
  )
{
  EFI_STATUS   Status;
  UINT32       CeilingKib;
  UINT32       OtherKib;
  UINT32       FloorKib;
  UINT32       AboveKib;
  UINT32       Bits;
  UINT32       AtSpeedHz;
  CONST CHAR8  *Why;

  *Proven = FALSE;

  //
  // The ceiling is the bus's own: an SDR bus moves one bit per line per clock,
  // so it is the *at-speed* card clock times the width, in KiB/s.  The width in
  // use is the one the card ended up on, which is not always the widest - a card
  // that had to fall back to 4 bits has half the ceiling of an 8-bit one, and
  // that is not the clock's fault.  The other reading of the divider field
  // (module / 2 / (div + 1)) is printed with it so the log still shows what the
  // rate would be if the field turned out to divide twice.
  //
  Bits       = (Private->BusWidth == A733_SMHC_BUS_8BIT) ? 8U :
               ((Private->BusWidth == A733_SMHC_BUS_4BIT) ? 4U : 1U);
  AtSpeedHz  = Private->ModuleClockHz / (Private->DataDivisor + 1U);
  CeilingKib = (UINT32)(((UINT64)AtSpeedHz * Bits) / (8U * 1024U));
  OtherKib   = (UINT32)(((UINT64)(Private->ModuleClockHz / (2U * (Private->DataDivisor + 1U))) * Bits) / (8U * 1024U));
  FloorKib   = (CeilingKib * A733_SMHC_SDR_FLOOR_PERCENT) / 100U;
  AboveKib   = (CeilingKib * A733_SMHC_SDR_CEIL_PERCENT) / 100U;

  Why = NULL;
  if (!Rate->MarginalValid) {
    Why = "the two transfers cannot be told apart";
  } else if (Rate->MarginalKibPerSec > AboveKib) {
    Why = "the bus measured faster than the plan's card clock allows";
  } else if (Rate->MarginalKibPerSec < FloorKib) {
    Why = "the bus measured far below the plan's rate";
  }

  DEBUG ((
    DEBUG_INFO,
    "Smhc: SDR rate check: %u KiB/s marginal (%u KiB in %u us) and %u KiB/s absolute (%u KiB in %u us), %u-bit bus, "
    "%u kHz card clock (div %u) -> a ceiling of %u KiB/s (%u KiB/s under the field's other reading), accepted between %u and %u%a\n",
    Rate->MarginalKibPerSec,
    Rate->BigKib - Rate->SmallKib,
    Rate->BigUs - Rate->SmallUs,
    Rate->BigKibPerSec,
    Rate->BigKib,
    Rate->BigUs,
    Bits,
    (UINT32)(AtSpeedHz / 1000),
    Private->DataDivisor,
    CeilingKib,
    OtherKib,
    FloorKib,
    AboveKib,
    (Why == NULL) ? "" : ": the plan is not the rate it claims"
    ));

  if (Why == NULL) {
    *Proven = TRUE;
    return EFI_SUCCESS;
  }

  if (!Rate->MarginalValid) {
    //
    // Nothing can be concluded from the numbers, so nothing is changed and
    // nothing is built on them: the plan stays, and the caller is told not to
    // run the DDR step on an unproven plan.
    //
    DEBUG ((DEBUG_ERROR, "Smhc: %a, so the plan is kept and no DDR step is taken\n", Why));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "Smhc: %a; reverting to the HOSC plan, the last rate that worked\n", Why));

  Status = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HOSC);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot revert to the HOSC plan: %r\n", Status));
    return Status;
  }

  SmhcSetClock (Private, Private->DataDivisor);

  //
  // The revert is measured, and with it the 12 MHz baseline: if the plan was
  // wrong, the driver's own report still says what the fallback actually runs
  // at.  The error is reported either way - the caller must not build a DDR
  // step on a plan that had to be abandoned.
  //
  SmhcReportThroughput (Private);

  return EFI_DEVICE_ERROR;
}

/**
  Read the probe's blocks (4 KiB from LBA 0) of the user area through the
  driver's own command path, into a caller-supplied buffer.

  Used by the sample-delay sweep: a long transfer is proof that the bus samples,
  and going through SmhcSendCommand means a failed attempt leaves the controller
  in exactly the state the existing data-path recovery expects to find it in.
  Taking a caller buffer rather than a local one is what lets the sweep compare
  what it read with what a known-good tap read earlier.

  @param[in,out] Private  Driver state; the card must be selected and ready.
  @param[out]    Buffer   Receives Length bytes.
  @param[in]     Length   Byte count, a whole number of 512-byte blocks from
                          LBA 0; A733_SMHC_SAMP_DELAY_PROBE_BYTES everywhere but
                          the write test's single-block shape.

  @retval EFI_SUCCESS  The blocks were read.
  @retval other        The command or its data failed.
**/
STATIC
EFI_STATUS
SmhcCardReadProbeBlocks (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    UINT8              *Buffer,
  IN     UINT32             Length
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;

  //
  // A multi-block read, not a single block.  A 512-byte transfer is forgiving
  // enough that almost every tap passes it - the sweep reported a window
  // covering 30 of 32 taps - while a transfer of several kilobytes is not, and
  // it is the long transfer the bus actually has to survive.  Probing with one
  // block therefore calibrated the tap for the wrong thing and left the real
  // data path failing at the end of the transfer.
  //
  //
  // Exactly the shape the real transfers have: arm the count with CMD23 and
  // read with CMD18, no auto CMD12.  Probing with the auto-stop path instead
  // measured that path's robustness, not the sampling point - it narrowed the
  // passing window to three taps on a bus that reads megabytes cleanly.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_SET_BLOCK_COUNT, Length / A733_SMHC_BLOCK_SIZE, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
  if (EFI_ERROR (SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US))) {
    return EFI_DEVICE_ERROR;
  }

  Private->CountArmed = TRUE;

  SmhcInitCommand (&Command, A733_SMHC_CMD_READ_MULTIPLE, 0, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1);
  // LBA 0, whose argument is 0 in both of the card's addressing modes.
  Command.Buffer    = Buffer;
  Command.Length    = Length;
  Command.BlockSize = A733_SMHC_BLOCK_SIZE;
  Command.Read      = TRUE;

  //
  // The attempt's own budget, not the data path's: a wrong tap is expected here,
  // and a tap that fails without latching an error bit must not cost a full data
  // timeout or the sweep would take minutes.
  //
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);

  Private->CountArmed = FALSE;

  return Status;
}

/**
  Count the bytes of a probe read that differ from a reference copy, and report
  where the first one is.

  @param[in]  Buffer      The bytes just read.
  @param[in]  Reference   The copy taken at a known-good sample point.
  @param[in]  Length      How many bytes to compare.
  @param[out] FirstOffset Receives the offset of the first difference.

  @return How many bytes differ.
**/
STATIC
UINT32
SmhcCardCompareProbe (
  IN  CONST UINT8  *Buffer,
  IN  CONST UINT8  *Reference,
  IN  UINT32       Length,
  OUT UINT32       *FirstOffset
  )
{
  UINT32  Index;
  UINT32  Differ;

  Differ       = 0;
  *FirstOffset = 0;

  for (Index = 0; Index < Length; Index++) {
    if (Buffer[Index] != Reference[Index]) {
      if (Differ == 0) {
        *FirstOffset = Index;
      }

      Differ++;
    }
  }

  return Differ;
}

/**
  A checksum of a probe read, so one log line identifies what it contains.

  @param[in] Buffer  The bytes, A733_SMHC_SAMP_DELAY_PROBE_BYTES of them.

  @return A 32-bit FNV-1a hash of them.
**/
STATIC
UINT32
SmhcCardProbeChecksum (
  IN CONST UINT8  *Buffer
  )
{
  UINT32  Index;
  UINT32  Hash;

  Hash = 2166136261U;
  for (Index = 0; Index < A733_SMHC_SAMP_DELAY_PROBE_BYTES; Index++) {
    Hash ^= Buffer[Index];
    Hash *= 16777619U;
  }

  return Hash;
}

//
// The DDR step's reference copy of the probe read, taken through the SDR tap
// before anything is switched.  Static rather than stack: it outlives the capture
// and the sweep frame is already the deepest one here.
//
STATIC UINT8  mSmhcDdrReference[A733_SMHC_SAMP_DELAY_PROBE_BYTES];
STATIC UINT8  mSmhcHs200Reference[A733_SMHC_SAMP_DELAY_PROBE_BYTES];
//
// The HS400 step's copy, taken on the SDR bus before the card is moved through
// DDR - it runs before the HS200 step, so the HS200 copy is not there yet.
//
STATIC UINT8  mSmhcHs400Reference[A733_SMHC_SAMP_DELAY_PROBE_BYTES];

/**
  Find the host's receive sample delay for the DDR bus by sweeping the tap line.

  DDR samples DATA on both edges of the card clock, so the hold window is half
  that of a single-data-rate read and the tap boot0 calibrated for its SDR52
  traffic (32 taps on this board) is no longer inside it - which is why the first
  DDR read fails and why the switch above cannot be finished by a fixed value.
  This is the procedure every delay-line mode needs, HS200 and HS400 included:
  walk the taps, read one block at each, and keep the centre of the longest run
  that reads, because that is the tap furthest from both edges of the window.

  The value is a property of the card *and* the board's routing, so it is found
  on each boot rather than remembered; boot0's own value is evidence for the
  range, not for the answer.  A tap that fails is recovered by the existing
  data-path recovery (smhcSendCommand re-initializes the controller and puts the
  DDR bit back, since the driver holds it), so a failure never ends the sweep and
  never costs more than one attempt.

  The pass criterion is a parameter, because a command's status says nothing
  about the bytes that came back.  A caller that hands over a reference copy of
  the probe read gets a tap that must also return that copy - the criterion a
  DDR bus needs, and the one that would have caught this driver's earlier
  mis-calibrations.  A caller that passes NULL keeps the status-only criterion
  the HS200 step was brought up on.

  The per-tap report is a parameter too, so that the steps that were calibrated
  with one summary line keep exactly that output.  When it is on, every tap prints
  one line - the tap, both delay registers as they read back, the status, the
  controller's raw status words and how the data compared - so one boot says
  whether an empty window is the register set's fault or the criterion's.  The
  command path's own per-failure logging is turned off for the duration
  (Private->Quiet): those lines explain what a tap line already summarises, and 32
  of them would bury the grid.

  @param[in,out] Private       Driver state; the card and the controller must both
                              be in 8-bit DDR already, with the DDR bit set for the
                              recovery to re-apply.
  @param[in]     Mode          The timing this sweep is being run for, for the lines
                              it prints: "DDR" or "HS200".
  @param[in]     Reference     A copy of the probe read taken at a known-good sample
                              point, or NULL to judge each tap by its status alone.
  @param[in]     ReportEachTap TRUE to print one line per tap.

  @retval TRUE   A window was found and its centre written to the controller.
  @retval FALSE  No tap read at all; the caller must revert the whole step.
**/
STATIC
BOOLEAN
SmhcCardSweepSampleDelay (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST CHAR8        *Mode,
  IN     CONST UINT8        *Reference,
  IN     BOOLEAN            ReportEachTap
  )
{
  BOOLEAN     Pass[A733_SMHC_SAMP_DELAY_TAPS / A733_SMHC_SAMP_DELAY_STEP];
  UINT8       Block[A733_SMHC_SAMP_DELAY_PROBE_BYTES];
  UINT32      Count;
  UINT32      Index;
  UINT32      Run;
  UINT32      RunFirst;
  UINT32      Best;
  UINT32      BestFirst;
  UINT32      BestLast;
  UINT32      Chosen;
  UINT32      Passes;
  UINT32      Tap;
  UINT32      SampDl;
  UINT32      DsDl;
  UINT32      Differ;
  UINT32      FirstOffset;
  UINT32      StatusPasses;
  UINT32      DataOnlyFailures;
  EFI_STATUS  Status;

  Count            = A733_SMHC_SAMP_DELAY_TAPS / A733_SMHC_SAMP_DELAY_STEP;
  StatusPasses     = 0;
  DataOnlyFailures = 0;

  Private->Quiet = TRUE;
  for (Index = 0; Index < Count; Index++) {
    Tap = Index * A733_SMHC_SAMP_DELAY_STEP;
    SmhcSetSampleDelay (Private->Base, Tap);
    SampDl = SmhcRead (Private->Base, A733_SMHC_SAMP_DL);
    DsDl   = SmhcRead (Private->Base, A733_SMHC_DS_DL);

    Status = SmhcCardReadProbeBlocks (Private, Block, A733_SMHC_SAMP_DELAY_PROBE_BYTES);

    Differ      = 0;
    FirstOffset = 0;
    if (!EFI_ERROR (Status) && (Reference != NULL)) {
      Differ = SmhcCardCompareProbe (Block, Reference, A733_SMHC_SAMP_DELAY_PROBE_BYTES, &FirstOffset);
    }

    if (!EFI_ERROR (Status)) {
      StatusPasses++;
      if ((Reference != NULL) && (Differ != 0)) {
        DataOnlyFailures++;
      }
    }

    if (ReportEachTap && PcdGetBool (PcdA733SmhcDumpEnable)) {
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "Smhc: %a tap %u: samp_dl %08x, ds_dl %08x, read %r, rint %08x idst %08x, data not compared\n",
          Mode,
          Tap,
          SampDl,
          DsDl,
          Status,
          Private->IntStatus,
          Private->IdstStatus
          ));
      } else if (Reference == NULL) {
        DEBUG ((
          DEBUG_ERROR,
          "Smhc: %a tap %u: samp_dl %08x, ds_dl %08x, read %r, rint %08x idst %08x, the status is the criterion\n",
          Mode,
          Tap,
          SampDl,
          DsDl,
          Status,
          Private->IntStatus,
          Private->IdstStatus
          ));
      } else if (Differ == 0) {
        DEBUG ((
          DEBUG_ERROR,
          "Smhc: %a tap %u: samp_dl %08x, ds_dl %08x, read %r, rint %08x idst %08x, all %u bytes match the reference\n",
          Mode,
          Tap,
          SampDl,
          DsDl,
          Status,
          Private->IntStatus,
          Private->IdstStatus,
          (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES
          ));
      } else {
        DEBUG ((
          DEBUG_ERROR,
          "Smhc: %a tap %u: samp_dl %08x, ds_dl %08x, read %r, rint %08x idst %08x, "
          "%u/%u bytes differ from the reference (first at %u) - status passed, data did not\n",
          Mode,
          Tap,
          SampDl,
          DsDl,
          Status,
          Private->IntStatus,
          Private->IdstStatus,
          Differ,
          (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
          FirstOffset
          ));
      }
    }

    Pass[Index] = !EFI_ERROR (Status) && ((Reference == NULL) || (Differ == 0));
  }

  Private->Quiet = FALSE;

  //
  // The longest run of consecutive passes, and its centre.  A window that runs
  // off either end of the sweep is still a window; the centre simply sits at the
  // edge of what was tried, which is the most useful answer that data allows.
  //
  Best      = 0;
  BestFirst = 0;
  BestLast  = 0;
  Run       = 0;
  RunFirst  = 0;
  Passes    = 0;

  for (Index = 0; Index < Count; Index++) {
    if (!Pass[Index]) {
      Run = 0;
      continue;
    }

    if (Run == 0) {
      RunFirst = Index;
    }

    Run++;
    Passes++;
    if (Run > Best) {
      Best      = Run;
      BestFirst = RunFirst;
      BestLast  = Index;
    }
  }

  if (Best == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a sample delay sweep: none of the %u taps tried (0..%u step %u) %a; "
      "the %a step cannot be trusted on this card\n",
      Mode,
      Count,
      A733_SMHC_SAMP_DELAY_TAPS - A733_SMHC_SAMP_DELAY_STEP,
      A733_SMHC_SAMP_DELAY_STEP,
      (Reference == NULL) ? "read a block back" : "read a block back with the reference's data",
      Mode
      ));
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a sample delay sweep: %u of %u taps reached DTC, %u of those returned data that differs from "
      "the reference, criterion %a\n",
      Mode,
      StatusPasses,
      Count,
      DataOnlyFailures,
      (Reference == NULL) ? "status only" : "status and data"
      ));

    return FALSE;
  }

  Chosen = ((BestFirst + BestLast) / 2U) * A733_SMHC_SAMP_DELAY_STEP;
  SmhcSetSampleDelay (Private->Base, Chosen);
  Private->SampleDelay = Chosen;

  DEBUG ((
    DEBUG_INFO,
    "Smhc: %a sample delay sweep: %u taps tried 0..%u step %u, %u passed the criterion (window %u..%u, %u attempts), "
    "%u reached DTC and %u of those returned data that differs from the reference, chosen %u, samp_dl %08x, ds_dl %08x, criterion %a\n",
    Mode,
    Count,
    A733_SMHC_SAMP_DELAY_TAPS - A733_SMHC_SAMP_DELAY_STEP,
    A733_SMHC_SAMP_DELAY_STEP,
    Passes,
    BestFirst * A733_SMHC_SAMP_DELAY_STEP,
    BestLast * A733_SMHC_SAMP_DELAY_STEP,
    Best,
    StatusPasses,
    DataOnlyFailures,
    Chosen,
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    (Reference == NULL) ? "status only" : "status and data"
    ));

  return TRUE;
}

/**
  The probe read into a buffer of its own: the status-only convenience the caller
  that does not compare data uses (the multi-block shape diagnostic below).

  @param[in,out] Private  Driver state; the card must be selected and ready.

  @retval EFI_SUCCESS  The blocks were read.
  @retval other        The command or its data failed.
**/
STATIC
EFI_STATUS
SmhcCardReadBlockZero (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  UINT8  Block[A733_SMHC_SAMP_DELAY_PROBE_BYTES];

  return SmhcCardReadProbeBlocks (Private, Block, A733_SMHC_SAMP_DELAY_PROBE_BYTES);
}

/**
  Diagnostic: read the same 4 KiB twice - once armed by CMD23, once ended by the
  controller's own auto CMD12 - and report which shape completes.

  This is the one controller-side difference in the driver's multi-block path.
  EmmcDxe arms CMD23 before every CMD18, so only the count-armed shape has ever
  run under EmmcDxe's traffic; the driver's own path can also use the
  controller's STOP_CMD_FLAG CMD12 (a caller that does not arm a count).  If the
  auto-CMD12 shape produces a DTC where the count-armed one does not, the card's
  own end-of-count is what the controller fails on; if neither does, the CMD23
  path is not the difference.

  Runs once per boot, on the controller under test only, after the at-speed bus
  has been proven by a read and before the throughput measurement - so the
  measurement immediately after it also says whether the probe left the bus in a
  state transfers work in.  A failed shape leaves the controller for
  SmhcSendCommand's own recovery (Private->DataFault); the CMD12 after each shape
  additionally releases a card a failed read may have left streaming, and the
  final count-armed read is the proof that the bus is usable again.

  Nothing here decides anything: the lines are evidence, and the caller carries
  on either way.

  @param[in,out] Private  Driver state; the card must be selected and ready.
**/
STATIC
VOID
SmhcDiagMultiBlockShapes (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         CountStatus;
  EFI_STATUS         AutoStatus;
  UINT8              Block[A733_SMHC_SAMP_DELAY_PROBE_BYTES];
  UINT32             Blocks;
  UINT32             Rint;
  UINT32             Idst;
  BOOLEAN            CountDtc;
  BOOLEAN            AutoDtc;

  Blocks = (UINT32)(sizeof (Block) / A733_SMHC_BLOCK_SIZE);

  SMHC_DIAG ((
    DEBUG_ERROR,
    "Smhc: %a multi-block shape probe: %u bytes from LBA 0, once armed by CMD23 and once ended by auto CMD12\n",
    Private->Name,
    (UINT32)sizeof (Block)
    ));

  //
  // Stop anything a previous transfer may have left streaming.  CMD12 on an idle
  // card answers R1 and changes nothing, and on a card still sending it is what
  // ends the transfer, so it is issued before, between and after the two shapes.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_STOP_TRANSMISSION, 0, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
  Command.Abort = TRUE;
  SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);

  //
  // Shape 1: what EmmcDxe sends - SET_BLOCK_COUNT, then READ_MULTIPLE with no
  // auto-stop, so the card ends the transfer itself and the controller only has
  // to count the bytes.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_SET_BLOCK_COUNT, Blocks, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
  CountStatus = SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);

  if (!EFI_ERROR (CountStatus)) {
    SmhcInitCommand (&Command, A733_SMHC_CMD_READ_MULTIPLE, 0, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1);
    Command.Buffer    = Block;
    Command.Length    = sizeof (Block);
    Command.BlockSize = A733_SMHC_BLOCK_SIZE;
    Command.Read      = TRUE;
    CountStatus       = SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);
  }

  Rint     = Private->IntStatus;
  Idst     = Private->IdstStatus;
  CountDtc = (Rint & A733_SMHC_RINT_DTC) != 0;

  SMHC_DIAG ((
    DEBUG_ERROR,
    "Smhc:   shape CMD23(%u)+CMD18, no auto-stop: %r, rint %08x (DTC %u), idst %08x\n",
    Blocks,
    CountStatus,
    Rint,
    CountDtc ? 1U : 0U,
    Idst
    ));

  SmhcInitCommand (&Command, A733_SMHC_CMD_STOP_TRANSMISSION, 0, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
  Command.Abort = TRUE;
  SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);

  //
  // Shape 2: no SET_BLOCK_COUNT; the controller sends CMD12 itself once the byte
  // count has moved (SMHC_CMD bit 12, STOP_CMD_FLAG).
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_READ_MULTIPLE, 0, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1);
  Command.Buffer    = Block;
  Command.Length    = sizeof (Block);
  Command.BlockSize = A733_SMHC_BLOCK_SIZE;
  Command.Read      = TRUE;
  Command.AutoStop  = TRUE;
  AutoStatus        = SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);

  Rint    = Private->IntStatus;
  Idst    = Private->IdstStatus;
  AutoDtc = (Rint & A733_SMHC_RINT_DTC) != 0;

  SMHC_DIAG ((
    DEBUG_ERROR,
    "Smhc:   shape CMD18 with auto CMD12 (STOP_CMD_FLAG), no CMD23: %r, rint %08x (DTC %u), idst %08x\n",
    AutoStatus,
    Rint,
    AutoDtc ? 1U : 0U,
    Idst
    ));

  SMHC_DIAG ((
    DEBUG_ERROR,
    "Smhc:   shape verdict: CMD23 %a, auto CMD12 %a\n",
    CountDtc ? "reached DTC" : "did not reach DTC",
    AutoDtc ? "reached DTC" : "did not reach DTC"
    ));

  //
  // Leave the card and the controller where the rest of the boot expects them:
  // end any streaming transfer, then prove the bus with the shape every real
  // transfer uses.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_STOP_TRANSMISSION, 0, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
  Command.Abort = TRUE;
  SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);

  AutoStatus = SmhcCardReadBlockZero (Private);
  SMHC_DIAG ((
    (EFI_ERROR (AutoStatus)) ? DEBUG_ERROR : DEBUG_INFO,
    "Smhc: %a multi-block shape probe done; the bus reads a 4 KiB CMD23+CMD18 transfer again: %r\n",
    Private->Name,
    AutoStatus
    ));
}

/**
  Verify the tap the sweep chose, with the sample FIFO back in circuit.

  The sweep runs with SMHC_SFC bit 0 set - the sample-FIFO bypass - which is how
  the vendor calibrates a delay line, while normal traffic runs with it clear.  So
  the tap the sweep found is only the traffic's tap if the same read works through
  the FIFO, and this read is what says so.  It is also the one measurement that
  separates the two ways a DDR step can fail: a tap that passed the sweep and
  fails here blames the sweep's criterion (or the sampler it used), while no tap
  passing at all blames the register set.

  @param[in,out] Private    Driver state; the chosen tap must be programmed.
  @param[in]     Reference  The probe's reference copy, or NULL.
  @param[in]     Mode       The timing this read is proving, for its lines: "DDR"
                           or "SDR+DDR-regs".

  @retval TRUE  The chosen tap read the reference's data through the FIFO.
**/
STATIC
BOOLEAN
SmhcCardVerifyChosenTap (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST UINT8        *Reference,
  IN     CONST CHAR8        *Mode
  )
{
  UINT8       Block[A733_SMHC_SAMP_DELAY_PROBE_BYTES];
  UINT32      Differ;
  UINT32      FirstOffset;
  UINT32      Attempts;
  UINT32      Done;
  UINT32      TicksPerMicro;
  UINT64      Frequency;
  UINT64      Start;
  EFI_STATUS  Status;

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);

  //
  // Timed as well as compared: 4 KiB takes about 40 us on a 50 MHz 8-bit DDR
  // bus and about 80 us on the 50 MHz SDR one, so the figure says whether the
  // double-data-rate transfer happened at all, which the bytes alone cannot.
  //
  //
  // The delay line is written with the card clock stopped and re-started around
  // the write in the vendor's own driver, and the sweep ends on a tap that is not
  // the chosen one - so the first read here can land while the delay register and
  // the clock it belongs to are still settling.  Re-programme both and retry: a
  // tap whose window the sweep proved reads on the second attempt, while a bus
  // that is simply wrong fails all three.
  //
  Attempts = 0;
  do {
    A733_SMHC_COMMAND  Stop;
    A733_SMHC_COMMAND  Status13;

    //
    // The sweep's last attempts are failures, and a card whose block was cut off
    // stays in the data state until something ends it - which is what made every
    // command after the sweep time out while the same tap had answered a moment
    // before.  End any transfer still in flight, then ask for the status: the
    // two answers say whether the card is there at all.
    //
    SmhcInitCommand (&Stop, A733_SMHC_CMD_STOP_TRANSMISSION, 0, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
    SmhcSendCommand (Private, &Stop, A733_SMHC_CMD_TIMEOUT_US);

    SmhcInitCommand (&Status13, A733_SMHC_CMD_SEND_STATUS, Private->Rca << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 at ds_dl tap %u, attempt %u: CMD12 sent, CMD13 %r\n",
      Private->StrobeDelay,
      Attempts + 1,
      SmhcSendCommand (Private, &Status13, A733_SMHC_CMD_TIMEOUT_US)
      ));

    SmhcSetStrobeDelay (Private->Base, Private->StrobeDelay);
    SmhcSetClock (Private, Private->ClockDivisor);

    Start  = GetPerformanceCounter ();
    Status = SmhcCardReadProbeBlocks (Private, Block, A733_SMHC_SAMP_DELAY_PROBE_BYTES);
    Done   = (UINT32)((GetPerformanceCounter () - Start) / TicksPerMicro);
  } while (EFI_ERROR (Status) && (++Attempts < A733_SMHC_HS400_VERIFY_ATTEMPTS));

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a tap %u with the sample FIFO back in circuit: read %r after %u us "
      "(rint %08x idst %08x); the sweep's tap does not work for traffic\n",
      Mode,
      Private->SampleDelay,
      Status,
      Done,
      Private->IntStatus,
      Private->IdstStatus
      ));
    return FALSE;
  }

  if (Reference == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a tap %u with the sample FIFO back in circuit: read %r after %u us, "
      "no reference to compare against\n",
      Mode,
      Private->SampleDelay,
      Status,
      Done
      ));
    return TRUE;
  }

  Differ = SmhcCardCompareProbe (Block, Reference, A733_SMHC_SAMP_DELAY_PROBE_BYTES, &FirstOffset);

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: %a tap %u with the sample FIFO back in circuit: read %r after %u us, %u/%u bytes differ from the "
    "reference (first at %u), checksum %08x (reference %08x)%a\n",
    Mode,
    Private->SampleDelay,
    Status,
    Done,
    Differ,
    (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
    FirstOffset,
    SmhcCardProbeChecksum (Block),
    SmhcCardProbeChecksum (Reference),
    (Differ == 0) ? "" : "; the sweep's criterion and the traffic's sampler disagree"
    ));

  return Differ == 0;
}

/**
  Control run: does the DDR host-side register set read an SDR bus?

  A DDR attempt can end with no tap passing for two reasons that look identical
  in the log: the host register set is wrong (so nothing on the bus reads at any
  tap), or the sample point is (so the bus reads, at taps the sweep cannot aim
  at).  This run holds the card still - SDR 8-bit at HS_TIMING 1, which is a bus
  this port reads correctly - and applies only the host's half of the DDR52
  timing: NTSR 1x, both DRV_DL output phases at 180 degrees and DS_DL = 0x80.
  The controller's DDR bit stays clear, because the card is not in DDR.

  If that reads (a window and matching data at some taps), the register set is
  sound and the DDR failure lives in the sample point or in the DDR data path.
  If it reads nowhere, the register set is what stops the bus, and the next thing
  to change is one of those three registers - not the sweep.

  It runs only after a failed DDR attempt has already been reverted and proved by
  a read, and it puts every register it touched back before returning: the SDR
  path resumes in exactly the state it was in, which the caller's read then
  re-checks.

  @param[in,out] Private       Driver state; the card must be in SDR already.
  @param[in]     SampDlBefore  SMHC_SAMP_DL before the DDR attempt.
  @param[in]     NtsrBefore    SMHC_NTSR before the DDR attempt.
  @param[in]     DrvDlBefore   SMHC_DRV_DL before the DDR attempt.
  @param[in]     DsDlBefore    SMHC_DS_DL before the DDR attempt.
  @param[in]     SfcBefore     SMHC_SFC before the DDR attempt.
  @param[in]     Reference     The probe's reference copy, or NULL.
**/
STATIC
VOID
SmhcCardDdrRegisterControl (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             SampDlBefore,
  IN     UINT32             NtsrBefore,
  IN     UINT32             DrvDlBefore,
  IN     UINT32             DsDlBefore,
  IN     UINT32             SfcBefore,
  IN     CONST UINT8        *Reference
  )
{
  BOOLEAN            Window;
  UINT32             SampleDelayBefore;
  A733_SMHC_EXT_CSD  Probe;
  EFI_STATUS         Status;

  SampleDelayBefore = Private->SampleDelay;

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: DDR register control: the card stays in SDR (HS_TIMING 1, BUS_WIDTH %u) and the host gets "
    "NTSR 1x, DRV_DL %08x and DS_DL %08x with the DDR bit clear; a window here means the register set is "
    "sound and the DDR failure is the sample point\n",
    A733_SMHC_EXT_CSD_WIDTH_8BIT,
    A733_SMHC_DRV_DL_DDR,
    A733_SMHC_DS_DL_DDR52
    ));

  //
  // HostDdrTiming makes the recovery between two taps re-apply this set rather
  // than the SDR one; without it the first failing tap would put NTSR and DRV_DL
  // back and every later tap would be measured in a different configuration.
  //
  Private->HostDdrTiming = TRUE;

  SmhcWrite (
    Private->Base,
    A733_SMHC_NTSR,
    SmhcRead (Private->Base, A733_SMHC_NTSR) & ~A733_SMHC_NTSR_2X_TIMING_MODE
    );
  SmhcWrite (Private->Base, A733_SMHC_DRV_DL, A733_SMHC_DRV_DL_DDR);
  SmhcWrite (Private->Base, A733_SMHC_DS_DL, A733_SMHC_DS_DL_DDR52);

  //
  // The same shape the DDR attempt used, so that the card's timing is the only
  // variable between the two: bypass the sample FIFO for the sweep, bring it back
  // for the read that judges the tap it chose.
  //
  SmhcSetSampleFifoBypass (Private->Base, TRUE);
  Window = SmhcCardSweepSampleDelay (Private, "SDR+DDR-regs", Reference, FALSE);
  SmhcSetSampleFifoBypass (Private->Base, FALSE);

  if (Window) {
    Window = SmhcCardVerifyChosenTap (Private, Reference, "SDR+DDR-regs");
  }

  //
  // Unconditional restore of everything this run and the sweep inside it wrote.
  // SAMP_DL is the tap the SDR path samples with, and the three above are the
  // values the DDR attempt found on entry.
  //
  Private->HostDdrTiming = FALSE;

  SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_NTSR, NtsrBefore);
  SmhcWrite (Private->Base, A733_SMHC_DRV_DL, DrvDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_DS_DL, DsDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_SFC, SfcBefore);

  Private->SampleDelay = SampleDelayBefore;

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: DDR register control: %a; SDR sampling state put back as ntsr %08x drv_dl %08x samp_dl %08x "
    "ds_dl %08x sfc %08x\n",
    Window ? "the SDR bus reads with the DDR host timing in place, so the register set is not the fault" :
    "the SDR bus does not read with the DDR host timing in place, so the register set is the fault",
    SmhcRead (Private->Base, A733_SMHC_NTSR),
    SmhcRead (Private->Base, A733_SMHC_DRV_DL),
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    SmhcRead (Private->Base, A733_SMHC_SFC)
    ));

  Status = SmhcCardReadExtCsd (Private, &Probe);
  DEBUG ((
    (EFI_ERROR (Status)) ? DEBUG_ERROR : DEBUG_INFO,
    "Smhc: DDR register control: the SDR bus answers a 512-byte EXT_CSD read afterwards: %r at tap %u\n",
    Status,
    Private->SampleDelay
    ));
}

/**
  Move the card and the controller to 8-bit DDR timing, or take both back to SDR.

  DDR is the point of the speed-up: at 8 bits and 52 MHz the bus moves a byte on
  each clock edge, so the same wire moves twice the data an SDR bus does, and
  neither side needs training to do it (that is HS200/HS400, which need 1.8 V
  and a tuning sequence this port does not have).

  The switches have to agree and they are made in the order the vendor's driver
  for this IP makes them, which is card first and host second:

  - EXT_CSD[185] = 2 (High Speed DDR timing) and then EXT_CSD[183] = 6 (8-bit
    DDR), the order Linux's mmc_select_hs_ddr uses.  Both are commands, and a
    command has no data phase, so the two travel on the command/response pair
    either side of the switch is happy with;
  - the controller's DDR mode bit and the host-side timing that goes with it
    (SMHC_CTRL bit 10, then NTSR 1x, both DRV_DL output phases, DS_DL and CSDC -
    see SmhcSetDdrMode).  Data on the bus is only ever moved after this;
  - the sample-delay sweep, because a DDR bus has half the hold window an SDR one
    has and no fixed tap is right for it; then the same probe read with the
    sample FIFO back in circuit, which is the configuration the traffic uses;
  - a read of EXT_CSD as the final verification, since that read is the first
    data command on the bus both sides have just changed.

  Any failure - a CMD6 that does not answer, a read that fails, a value that does
  not read back - takes both sides back to 8-bit SDR at HS_TIMING 1 (or to
  whatever width was kept) *and* the module clock back to the SDR plan, because
  the card must end in a state EmmcDxe's FAT reads work in and the driver must
  not leave a clock behind that only DDR was using.  The delay registers this
  step changes but does not sweep (DS_DL, SFC) are restored to the values they
  held on entry.

  The DDR phase runs on a plan of its own (A733_SMHC_PLAN_DDR), which is meant to
  be a module clock at four times the card rate with SMHC_CLKDIV's field at 1 -
  the vendor's own recipe for 8-bit DDR52.  That plan is entered only here, after
  the card has been read back in SDR, it is stated register by register in the log
  rather than assumed, and it is left only if the DDR reads below verify -
  otherwise the SDR plan is put back and re-verified.

  @param[in,out] Private  Driver state; Ddr and HsTiming are filled in.
  @param[in]     ExtCsd   The EXT_CSD read of the SDR stage.

  @retval EFI_SUCCESS      The card and the controller are in 8-bit DDR on the
                           DDR plan.
  @retval EFI_UNSUPPORTED  The card does not list DDR52 (a clean skip).
  @retval other            The switch failed and both sides were reverted.
**/
STATIC
EFI_STATUS
SmhcCardTryDdr (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     A733_SMHC_EXT_CSD  *ExtCsd
  )
{
  EFI_STATUS   Status;
  EFI_STATUS   PlanStatus;
  EFI_STATUS   RefStatus;
  UINT32       SdrPlan;
  UINT32       SampDlBefore;
  UINT32       DsDlBefore;
  UINT32       SfcBefore;
  UINT32       NtsrBefore;
  UINT32       DrvDlBefore;
  UINT32       DtoMs;
  UINT32       Khz;
  UINT32       ModuleKhz;
  CONST UINT8  *Reference;

  if ((ExtCsd->DeviceType & A733_SMHC_DEVICE_TYPE_DDR52) == 0) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: DDR step skipped: EXT_CSD[196] device type %02x has no bit 2 (DDR52 at 52 MHz)\n",
      ExtCsd->DeviceType
      ));

    return EFI_UNSUPPORTED;
  }

  SdrPlan = Private->ClockPlan;

  //
  // The reference the sweep compares against, taken here - on the 50 MHz SDR bus
  // that is the one configuration this port knows reads correctly - and taken
  // through the same command shape every probe uses.  A status-only criterion has
  // mis-calibrated this driver twice, so the DDR sweep gets data to answer to as
  // well.  If this read fails there is nothing to compare, and the sweep falls
  // back to the status-only criterion, which the log says out loud.
  //
  RefStatus = SmhcCardReadProbeBlocks (Private, mSmhcDdrReference, A733_SMHC_SAMP_DELAY_PROBE_BYTES);
  Reference = EFI_ERROR (RefStatus) ? NULL : mSmhcDdrReference;

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: DDR step: SDR reference read of %u bytes from LBA 0 at %u kHz: %r, checksum %08x; "
    "the sweep's criterion is %a\n",
    (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
    SmhcClockKhz (Private),
    RefStatus,
    (Reference == NULL) ? 0U : SmhcCardProbeChecksum (mSmhcDdrReference),
    (Reference == NULL) ? "status only (no reference)" : "status and data"
    ));

  DEBUG ((
    DEBUG_INFO,
    "Smhc: DDR step: EXT_CSD[196] %02x lists DDR52; moving the module clock to the DDR plan %a and checking it\n",
    ExtCsd->DeviceType,
    SmhcClockPlanName (Private->ClockProfile, A733_SMHC_PLAN_DDR)
    ));

  //
  // The module clock first: it is the controller's sampling rate that DDR
  // doubles, and the card is not touched until it is in place and has been
  // proven by a read.
  //
  Status = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_DDR);
  if (!EFI_ERROR (Status)) {
    SmhcSetClock (Private, Private->DataDivisor);
    Status = SmhcCardReadExtCsd (Private, ExtCsd);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the DDR plan did not read back (%r); the card is untouched and stays in SDR\n", Status));

    PlanStatus = SmhcSelectClockPlan (Private, SdrPlan);
    if (!EFI_ERROR (PlanStatus)) {
      SmhcSetClock (Private, Private->DataDivisor);
      if (EFI_ERROR (SmhcCardReadExtCsd (Private, ExtCsd))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the SDR plan did not read back after the DDR plan either\n"));
      }
    } else {
      DEBUG ((DEBUG_ERROR, "Smhc: cannot put the SDR plan back: %r\n", PlanStatus));
    }

    return EFI_DEVICE_ERROR;
  }

  //
  // What the plan actually landed as, register by register.  This is the one
  // item of the vendor's recipe that is about the clock rather than the delays -
  // 8-bit DDR52 wants a module clock at four times the card rate with
  // SMHC_CLKDIV's field at 1 - and it is stated rather than assumed, because a
  // ratio that is 2 or 8 is the difference between a bus the delay lines describe
  // and one they do not.
  //
  Khz       = SmhcClockKhz (Private);
  ModuleKhz = Private->ModuleClockHz / 1000U;

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: DDR step: the DDR plan reads back as module %u kHz, card %u kHz, SMHC_CLKDIV %08x (field %u), "
    "module/card ratio %u; 8-bit DDR52 wants 4 with field 1\n",
    ModuleKhz,
    Khz,
    SmhcRead (Private->Base, A733_SMHC_CLKDIV),
    SmhcRead (Private->Base, A733_SMHC_CLKDIV) & A733_SMHC_CLKDIV_CCLK_DIV_MASK,
    (Khz == 0) ? 0U : (ModuleKhz / Khz)
    ));

  if ((Khz == 0) ||
      ((ModuleKhz / Khz) != 4U) ||
      ((SmhcRead (Private->Base, A733_SMHC_CLKDIV) & A733_SMHC_CLKDIV_CCLK_DIV_MASK) != 1U))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: DDR step: the DDR plan is NOT the vendor's 4x/divider-1 recipe for 8-bit DDR52; "
      "the delay lines below are described for that ratio, so an empty window here is the clock's\n"
      ));
  }

  //
  // Everything this step changes and does not sweep, as it stands before the
  // switch: the sample delay is boot0's calibration for this card and board and
  // has to come back on any failure; DS_DL and SFC are values only this step ever
  // touches; NTSR and DRV_DL are here as the evidence the log then shows changed.
  //
  SampDlBefore = SmhcRead (Private->Base, A733_SMHC_SAMP_DL);
  DsDlBefore   = SmhcRead (Private->Base, A733_SMHC_DS_DL);
  SfcBefore    = SmhcRead (Private->Base, A733_SMHC_SFC);
  NtsrBefore   = SmhcRead (Private->Base, A733_SMHC_NTSR);
  DrvDlBefore  = SmhcRead (Private->Base, A733_SMHC_DRV_DL);

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: DDR step: entering with ntsr %08x drv_dl %08x samp_dl %08x ds_dl %08x sfc %08x csdc %08x\n",
    NtsrBefore,
    DrvDlBefore,
    SampDlBefore,
    DsDlBefore,
    SfcBefore,
    SmhcRead (Private->Base, A733_SMHC_CSDC)
    ));

  DEBUG ((
    DEBUG_INFO,
    "Smhc: DDR step: switching the card first - HS_TIMING to %u then bus width to %u - and the host's DDR bit after\n",
    A733_SMHC_HS_TIMING_DDR52,
    A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR
    ));

  Status = SmhcCardSetHsTiming (Private, A733_SMHC_HS_TIMING_DDR52, "ddr");
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the DDR HS_TIMING switch failed: %r\n", Status));
  } else {
    Status = SmhcCardSwitch (Private, A733_SMHC_EXT_CSD_BUS_WIDTH, A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the DDR bus-width switch failed: %r\n", Status));
    } else {
      //
      // The card is in DDR from the moment those two CMD6s are answered, and the
      // driver says so *before* the first DDR read: a failed DDR read sets the
      // data-fault flag, and the recovery that follows re-applies the driver's
      // idea of the bus - which has to be "DDR" or the first failing tap would
      // clear the controller's DDR bit and no later tap could ever pass.
      //
      Private->Ddr = TRUE;

      //
      // The host's half, now that the card's is in place: DDR bit plus NTSR 1x,
      // both output phases at 180 degrees, DS_DL and CSDC (SmhcSetDdrMode), then
      // the sample-FIFO bypass for the sweep.  HostDdrTiming is what the
      // recovery path re-applies after a failed tap, so it must be set before
      // the first attempt.
      //
      Private->HostDdrTiming = TRUE;
      SmhcSetDdrMode (Private, TRUE);

      SmhcSetSampleFifoBypass (Private->Base, TRUE);

      DEBUG ((
        DEBUG_ERROR,
        "Smhc: DDR step: host switched - ctrl %08x (ddr bit %u), ntsr %08x drv_dl %08x samp_dl %08x ds_dl %08x "
        "sfc %08x csdc %08x; sweeping with the sample FIFO bypassed\n",
        SmhcRead (Private->Base, A733_SMHC_CTRL),
        (UINT32)Private->DdrModeBit,
        SmhcRead (Private->Base, A733_SMHC_NTSR),
        SmhcRead (Private->Base, A733_SMHC_DRV_DL),
        SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
        SmhcRead (Private->Base, A733_SMHC_DS_DL),
        SmhcRead (Private->Base, A733_SMHC_SFC),
        SmhcRead (Private->Base, A733_SMHC_CSDC)
        ));

      if (!SmhcCardSweepSampleDelay (Private, "DDR", Reference, TRUE)) {
        Status = EFI_DEVICE_ERROR;
      }

      //
      // The bypass is for the calibration, not for the traffic: clear it before
      // any transfer whose result anyone is going to use, and say what it reads
      // back as.  The read below then judges the chosen tap through the same
      // sampler the FAT path will use.
      //
      SmhcSetSampleFifoBypass (Private->Base, FALSE);

      DEBUG ((
        DEBUG_ERROR,
        "Smhc: DDR step: sample FIFO bypass cleared (sfc %08x); samp_dl %08x, state tap %u\n",
        SmhcRead (Private->Base, A733_SMHC_SFC),
        SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
        Private->SampleDelay
        ));

      if (!EFI_ERROR (Status) && !SmhcCardVerifyChosenTap (Private, Reference, "DDR")) {
        Status = EFI_DEVICE_ERROR;
      }

      if (!EFI_ERROR (Status)) {
        Status = SmhcCardReadExtCsd (Private, ExtCsd);
        if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "Smhc: the read over the DDR bus failed with the chosen sample delay: %r\n", Status));
        } else {
          SmhcProbeHsTimingRead (Private, "after the DDR switch (at the chosen tap)", A733_SMHC_HS_TIMING_DDR52, ExtCsd);
        }
      }
    }
  }

  if (!EFI_ERROR (Status) &&
      ((ExtCsd->HsTiming != A733_SMHC_HS_TIMING_DDR52) ||
       (ExtCsd->BusWidth != A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR)))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: the card reads back HS_TIMING %u and BUS_WIDTH %u after the DDR switch, not %u and %u\n",
      ExtCsd->HsTiming,
      ExtCsd->BusWidth,
      A733_SMHC_HS_TIMING_DDR52,
      A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR
      ));
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    Private->Ddr      = TRUE;
    Private->HsTiming = ExtCsd->HsTiming;

    //
    // The manual halves the data timeout in an 8-bit DDR bus, and the field is
    // already at its maximum (see A733_SMHC_DDR_DTO_HALVES), so what is left to
    // do is state the bound it now means: the field counts card clocks, two of
    // which pass per clock in DDR, so the wall-clock bound is the field over
    // twice the card clock - 168 ms at 50 MHz.  The card's own access time is a
    // millisecond or two, so this is a margin and not a constraint.
    //
    Khz   = SmhcClockKhz (Private);
    DtoMs = (Khz == 0) ? 0 : (UINT32)((UINT64)A733_SMHC_TMOUT_DTO_MAX / (A733_SMHC_DDR_DTO_HALVES * (UINT64)Khz));

    DEBUG ((
      DEBUG_INFO,
      "Smhc: DDR timing: EXT_CSD[185] %u and EXT_CSD[183] %u read back, controller DDR mode bit %u, "
      "data timeout %u ms at %u kHz\n",
      ExtCsd->HsTiming,
      ExtCsd->BusWidth,
      Private->DdrModeBit,
      DtoMs,
      Khz
      ));

    return EFI_SUCCESS;
  }

  //
  // Revert: the SDR sampling state first, because every read from here on - the
  // CMD6 writes and the verifying read below - is a single-data-rate one and must
  // sample the way the SDR path above did.  That is the tapped delay, plus the two
  // registers this step changes and does not sweep: DS_DL and the sample-FIFO
  // bypass.  Then the card (its two CMD6 writes are commands, and putting it back
  // in SDR timing is what makes the verifying read a read of an SDR card), then
  // the controller's DDR bit and the rest of the host timing, then the module
  // clock back to the plan this step started from, then a read to prove they
  // agree again.
  //
  SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_DS_DL, DsDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_SFC, SfcBefore);

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: reverting to %u-bit SDR, HS_TIMING %u, sample delay %u taps (0x%08x as boot0 left it), "
    "ds_dl 0x%08x and sfc 0x%08x put back, and to the %a plan\n",
    Private->BusWidth,
    Private->HighSpeed ? 1U : 0U,
    SampDlBefore & A733_SMHC_SAMP_DL_SW_MASK,
    SampDlBefore,
    DsDlBefore,
    SfcBefore,
    SmhcClockPlanName (Private->ClockProfile, SdrPlan)
    ));

  SmhcCardSetHsTiming (Private, Private->HighSpeed ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U, "revert-ddr");
  SmhcCardSwitch (Private, A733_SMHC_EXT_CSD_BUS_WIDTH, Private->BusWidth);

  //
  // The host side back to the single-data-rate set, and the flag with it: from
  // here on a recovery must re-apply the SDR timing, not the DDR one.
  //
  Private->HostDdrTiming = FALSE;
  SmhcSetDdrMode (Private, FALSE);

  Private->Ddr = FALSE;

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: revert: ntsr %08x drv_dl %08x samp_dl %08x ds_dl %08x sfc %08x csdc %08x ctrl %08x (ddr bit %u)\n",
    SmhcRead (Private->Base, A733_SMHC_NTSR),
    SmhcRead (Private->Base, A733_SMHC_DRV_DL),
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    SmhcRead (Private->Base, A733_SMHC_SFC),
    SmhcRead (Private->Base, A733_SMHC_CSDC),
    SmhcRead (Private->Base, A733_SMHC_CTRL),
    (UINT32)Private->DdrModeBit
    ));

  //
  // The card is back in SDR timing, so the plan under it can be put back too,
  // and the read below is a read of the whole path that will now carry the FAT
  // traffic.
  //
  PlanStatus = SmhcSelectClockPlan (Private, SdrPlan);
  if (EFI_ERROR (PlanStatus)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot put the %a plan back: %r\n", SmhcClockPlanName (Private->ClockProfile, SdrPlan), PlanStatus));
  } else {
    SmhcSetClock (Private, Private->DataDivisor);
  }

  Status = SmhcCardReadExtCsd (Private, ExtCsd);
  if (EFI_ERROR (Status)) {
    //
    // The card does not answer on the SDR bus the revert put it back on.  That
    // leaves the driver holding a card clock and a card timing it cannot vouch
    // for, and the FAT path above needs both: the one plan whose rate is known is
    // asked for instead, and the card is only trusted once it has answered there.
    //
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: the card does not answer after the revert (%r); falling back to the HOSC plan\n",
      Status
      ));

    PlanStatus = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HOSC);
    if (EFI_ERROR (PlanStatus)) {
      DEBUG ((DEBUG_ERROR, "Smhc: cannot fall back to the HOSC plan: %r\n", PlanStatus));
    } else {
      SmhcSetClock (Private, Private->DataDivisor);
      Private->HighSpeed = FALSE;

      if (EFI_ERROR (SmhcCardSetHighSpeed (Private, ExtCsd, SmhcClockKhz (Private)))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer at the HOSC plan either\n"));
      }
    }
  } else {
    Private->HighSpeed = ExtCsd->HsTiming == A733_SMHC_HS_TIMING_HIGH_SPEED;
    Private->HsTiming  = ExtCsd->HsTiming;

    SmhcProbeHsTimingRead (Private, "after the DDR revert", Private->HighSpeed ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U, ExtCsd);

    DEBUG ((
      DEBUG_INFO,
      "Smhc: reverted: EXT_CSD[185] HS_TIMING %u, EXT_CSD[183] BUS_WIDTH %u read back at %u kHz (controller DDR mode bit %u)\n",
      ExtCsd->HsTiming,
      ExtCsd->BusWidth,
      SmhcClockKhz (Private),
      Private->DdrModeBit
      ));

    //
    // The step failed with the card and the host both back on the SDR bus, so
    // this is the one moment where the host's DDR register set can be tested on
    // a bus that is known to read - the control run the failure needs.  It puts
    // everything back before returning, and its own read proves that.
    //
    SmhcCardDdrRegisterControl (
      Private,
      SampDlBefore,
      NtsrBefore,
      DrvDlBefore,
      DsDlBefore,
      SfcBefore,
      Reference
      );
  }

  return EFI_DEVICE_ERROR;
}

/**
  Try the card's HS200 timing, once the DDR step has failed or been skipped.

  HS200 is the last step this port can take without a full tuning sequence: a
  200 MHz card clock, 8 bits, single edge - so the bus stays SDR and the width
  stays 2 - and, on this board, no voltage switch at all, because the eMMC's I/O
  supply is already 1.8 V (the PMIC reads back as CLDO5 = 1800 mV).  It is tried
  *empirically* rather than from the capability byte: this card's EXT_CSD[196]
  reads 0x57, whose HS200-at-1.8 V bit (bit 3) is *clear* while HS400-at-1.8 V
  (bit 6) is set, a combination no reading of the spec explains, so the read-back
  below - not that byte - is what decides.

  The order is the one Linux's mmc_select_hs200 uses, and the reason for it: the
  card is switched at the *current* SDR clock, because a 200 MHz clock on a card
  still in HS_TIMING 1 would be four times its 52 MHz limit; the read-back proves
  the card took the switch; only then is the module clock raised to the HS200
  plan.  HS200 needs a sample point exactly as DDR did, so the tap is swept at the
  final clock with the same sweep the DDR step uses, and a read at speed proves
  the whole path before this step reports anything.

  The driver reports success by its state, not by a line: the entry point's tail
  prints the one HS200 line once the throughput measurement has run, so that a
  boot's headline number and its state appear together.

  @param[in,out] Private  Driver state; the card must be in 8-bit SDR and must not
                         be in DDR.

  @retval EFI_SUCCESS  The card is in HS200 and the driver is on the HS200 plan.
  @retval other        Nothing was left changed: the card is back in SDR on the
                       plan this step started from, with its sample delay and its
                       timing mode restored, and a read has proved it.
**/
STATIC
EFI_STATUS
SmhcCardTryHs200 (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS         Status;
  EFI_STATUS         PlanStatus;
  A733_SMHC_EXT_CSD  ExtCsd;
  UINT32             SdrPlan;
  UINT32             NtsrBefore;
  UINT32             SampDlBefore;
  CONST CHAR8        *Reason;
  EFI_STATUS         Hs200RefStatus;
  UINT8              *Hs200Reference;
  A733_SMHC_EXT_CSD  Capability;

  //
  // Gate on what the card says it can do, the same way the DDR52 and HS400 steps
  // are gated: HS200 needs 1.8 V signalling here, so only EXT_CSD[196] bit 4
  // qualifies - a card that lists only the 1.2 V bit (bit 5) would be asked for a
  // mode this board cannot power, and the switch it would attempt is exactly the
  // one that looks like a refusal afterwards.
  //
  if (EFI_ERROR (SmhcCardReadExtCsd (Private, &Capability))) {
    return EFI_DEVICE_ERROR;
  }

  if ((Capability.DeviceType & A733_SMHC_DEVICE_TYPE_HS200_1V8) == 0) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS200 step skipped: EXT_CSD[196] device type %02x has no bit 4 (HS200 at 1.8 V)%a\n",
      Capability.DeviceType,
      ((Capability.DeviceType & A733_SMHC_DEVICE_TYPE_HS200_1V2) != 0)
        ? "; the card lists HS200 at 1.2 V only, which this board cannot supply"
        : ""
      ));

    return EFI_UNSUPPORTED;
  }

  SdrPlan         = Private->ClockPlan;
  Hs200RefStatus  = EFI_DEVICE_ERROR;
  Hs200Reference  = NULL;
  NtsrBefore   = SmhcRead (Private->Base, A733_SMHC_NTSR);
  SampDlBefore = SmhcRead (Private->Base, A733_SMHC_SAMP_DL);
  Reason       = NULL;

  //
  // Probes 1 and 3, before anything is switched: the clock the card is running on
  // now, and the 512-byte EXT_CSD, its byte 185 and its sector count, plus the
  // CSD, as the 50 MHz reference everything at 200 MHz is compared against.
  //
  SmhcProbeClock (Private, "before the HS200 switch");
  SmhcProbeCapture50 (Private);

  //
  // The data the HS200 sweep answers to, read here on the 50 MHz SDR bus that
  // reads correctly, through the same command shape as every probe.  Without it
  // the sweep accepts any tap that reaches data-transfer-complete, and at 200 MHz
  // that is every tap - a criterion that cannot tell a working bus from a silent
  // one, which is why the step's own read-back is the only thing that ever said
  // this card was still in High Speed timing.
  //
  Hs200RefStatus = SmhcCardReadProbeBlocks (Private, mSmhcHs200Reference, A733_SMHC_SAMP_DELAY_PROBE_BYTES);
  Hs200Reference = EFI_ERROR (Hs200RefStatus) ? NULL : mSmhcHs200Reference;

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: HS200: 50 MHz reference read of %u bytes from LBA 0: %r, checksum %08x; the sweep's criterion is %a\n",
    (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
    Hs200RefStatus,
    (Hs200Reference == NULL) ? 0U : SmhcCardProbeChecksum (mSmhcHs200Reference),
    (Hs200Reference == NULL) ? "status only (no reference)" : "status and data"
    ));

  //
  // The sample/output timing mode, which the manual (11.3.5.6) names as the
  // first thing to choose for a phase offset and whose own default is 1: set it
  // here so the step does not depend on the DDR step having run before it.  It is
  // put back on every failure path below.
  //
  SmhcWrite (
    Private->Base,
    A733_SMHC_NTSR,
    SmhcRead (Private->Base, A733_SMHC_NTSR) | A733_SMHC_NTSR_2X_TIMING_MODE
    );

  //
  // The switch, at the clock the card is already running at.
  //
  //
  // The DDR step ran before this one and a recovery re-asserts whatever this
  // driver believes the bus is.  Leaving Ddr set here means every failed read
  // during the sweep writes DDR timing back over the switch, and the read that
  // verifies it then reports 2 - which reads exactly like a card refusing
  // HS200.  Clear it: from here on the bus is HS200, single edge.
  //
  Private->Ddr = FALSE;

  Status = SmhcCardSetHsTiming (Private, A733_SMHC_HS_TIMING_HS200, "hs200");
  if (EFI_ERROR (Status)) {
    Reason = "the HS_TIMING = 3 switch did not answer";
  } else {
    //
    // Record the new timing before anything else touches the bus.  A failed
    // read during the sweep recovers the controller, and the recovery
    // re-asserts the timing this driver holds - so a stale 1 (or the DDR step's
    // 2) silently writes the switch back and every later read reports the old
    // mode, which looks exactly like a card that refused HS200.
    //
    Private->HsTiming  = A733_SMHC_HS_TIMING_HS200;
    Private->HighSpeed = TRUE;
  }
  //
  // No read-back here.  Writing HS_TIMING = 3 changes the card's interface the
  // moment the command completes, while the sample delay in the controller is
  // still the one calibrated for single-data-rate timing - which is exactly the
  // state in which a verifying read fails or returns garbage, and it was read as
  // "the card refused HS200" while the switch itself had been accepted.  The
  // sweep below is what finds a working sample point; the read-back that decides
  // whether the mode took belongs after it, not before.
  //

  //
  // Whether the switch held at the clock it was made at.  HS200 changes the
  // card's interface the moment CMD6 returns, and the read-back further down is
  // taken at 200 MHz - where a bad sample point and a card that refused the mode
  // look the same.  Finding a tap here, at the bring-up clock, and reading
  // EXT_CSD[185] with it separates the two: 3 means the card took the mode and
  // everything that goes wrong afterwards is the fast bus; 1 means it did not.
  //
  if ((Reason == NULL) && (Hs200Reference != NULL)) {
    A733_SMHC_EXT_CSD  AtSpeed;

    if (SmhcCardSweepSampleDelay (Private, "HS200 at the bring-up clock", Hs200Reference, FALSE)) {
      if (EFI_ERROR (SmhcCardReadExtCsd (Private, &AtSpeed))) {
        DEBUG ((DEBUG_ERROR, "Smhc: HS200 at the bring-up clock: the read-back failed\n"));
      } else {
        DEBUG ((
          DEBUG_ERROR,
          "Smhc: HS200 at the bring-up clock (%u kHz): EXT_CSD[185] %u, EXT_CSD[183] %u, device type %02x\n",
          SmhcClockKhz (Private),
          AtSpeed.HsTiming,
          AtSpeed.BusWidth,
          AtSpeed.DeviceType
          ));
      }
    } else {
      DEBUG ((DEBUG_ERROR, "Smhc: HS200 at the bring-up clock: no tap read a block back\n"));
    }
  }

  //
  // The clock, now that the card can take it.  There is deliberately no read
  // here: at 200 MHz the tap that is still in the register was calibrated for
  // 50 MHz, so a read would fail for exactly the reason the sweep below exists.
  // The plan switch has already proved the CCU and the dividers by reading the
  // register back, and the sweep is the first thing that touches the bus at the
  // new rate, so an unreachable clock shows up as an empty window and the revert
  // below, not as a failure blamed on the card.
  //
  if (Reason == NULL) {
    Status = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HS200);
    if (EFI_ERROR (Status)) {
      Reason = "the HS200 plan could not be set";
    } else {
      SmhcSetClock (Private, Private->DataDivisor);

      //
      // Probe 1, at the plan the card is supposed to be running 200 MHz on.
      //
      SmhcProbeClock (Private, "at the HS200 plan");

      DEBUG ((
        DEBUG_INFO,
        "Smhc: HS200: card clock at the HS200 plan is %u kHz (module %u kHz, divider %u), tap present is %u taps\n",
        SmhcClockKhz (Private),
        Private->ModuleClockHz / 1000U,
        Private->DataDivisor,
        SmhcRead (Private->Base, A733_SMHC_SAMP_DL) & A733_SMHC_SAMP_DL_SW_MASK
        ));
    }
  }

  //
  // The sample point, at the final clock: the same sweep, the same recovery, the
  // same choice of the centre of the widest window.
  //
  if (Reason == NULL) {
    if (!SmhcCardSweepSampleDelay (Private, "HS200", Hs200Reference, FALSE)) {
      Reason = "no sample-delay tap read a block back";
    } else {
      //
      // The two read shapes separated on the bus: the sweep above proves a
      // multi-block read (CMD23 + CMD18) returns the reference's bytes at this
      // clock, while the switch's read-back is a single block (CMD8).  If only
      // the single block goes wrong, the mode took and the verification path is
      // what is broken - two very different next steps.
      //
      {
        A733_SMHC_COMMAND  Single;
        UINT8              SingleBlock[A733_SMHC_BLOCK_SIZE];
        UINT32             FirstDiff;

        ZeroMem (SingleBlock, sizeof (SingleBlock));
        SmhcInitCommand (&Single, A733_SMHC_CMD_READ_SINGLE_BLOCK, 0, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1);
        Single.Buffer    = SingleBlock;
        Single.Length    = A733_SMHC_BLOCK_SIZE;
        Single.BlockSize = A733_SMHC_BLOCK_SIZE;
        Single.Read      = TRUE;

        Status = SmhcSendCommand (Private, &Single, A733_SMHC_CMD_TIMEOUT_US);
        if (EFI_ERROR (Status)) {
          DEBUG ((DEBUG_ERROR, "Smhc: HS200: a single-block read of LBA 0 at the chosen tap failed: %r\n", Status));
        } else if ((Hs200Reference != NULL) && (CompareMem (SingleBlock, Hs200Reference, A733_SMHC_BLOCK_SIZE) != 0)) {
          for (FirstDiff = 0; FirstDiff < A733_SMHC_BLOCK_SIZE; FirstDiff++) {
            if (SingleBlock[FirstDiff] != Hs200Reference[FirstDiff]) {
              break;
            }
          }

          DEBUG ((
            DEBUG_ERROR,
            "Smhc: HS200: the single-block read returned data that differs from the reference at byte %u (%02x vs %02x); the multi-block sweep read it correctly at the same tap\n",
            FirstDiff,
            SingleBlock[FirstDiff],
            Hs200Reference[FirstDiff]
            ));
        } else {
          DEBUG ((DEBUG_ERROR, "Smhc: HS200: the single-block read at the chosen tap matches the reference\n"));
        }
      }

      //
      // Probe 3, at the chosen tap: two EXT_CSD reads compared with each other
      // and with the 50 MHz reference, and a CSD read whose capacity is compared
      // with the reference's.
      //
      SmhcProbeExtCsdAtSpeed (Private);

      Status = SmhcCardReadExtCsd (Private, &ExtCsd);
      if (EFI_ERROR (Status)) {
        Reason = "the read at the chosen tap failed";
      } else {
        SmhcProbeHsTimingRead (Private, "after the HS200 switch (at the chosen tap)", A733_SMHC_HS_TIMING_HS200, &ExtCsd);

        if (ExtCsd.HsTiming != A733_SMHC_HS_TIMING_HS200) {
          //
          // The sweep's failed reads recover the controller, and a recovery
          // re-asserts the timing the driver has recorded - which was still the
          // DDR step's 2.  So the card can end the sweep in the timing the switch
          // it was just given did not name.  Assert HS200 again now that a working
          // sample point is in the register, which is also where the spec puts the
          // second half of this: tune, then confirm the timing.
          //
          Status = SmhcCardSetHsTiming (Private, A733_SMHC_HS_TIMING_HS200, "reassert");
          if (EFI_ERROR (Status)) {
            Reason = "the HS_TIMING = 3 switch did not answer after the sweep";
          } else if (EFI_ERROR (SmhcCardReadExtCsd (Private, &ExtCsd))) {
            Reason = "the read after re-asserting HS_TIMING 3 failed";
          } else {
            SmhcProbeHsTimingRead (Private, "after re-asserting", A733_SMHC_HS_TIMING_HS200, &ExtCsd);
          }
        }
      }
    }

    if ((Reason == NULL) &&
        ((ExtCsd.HsTiming != A733_SMHC_HS_TIMING_HS200) ||
         (ExtCsd.BusWidth != A733_SMHC_EXT_CSD_WIDTH_8BIT)))
    {
      DEBUG ((
        DEBUG_ERROR,
        "Smhc: after the HS200 sweep the card reads HS_TIMING %u and BUS_WIDTH %u (expected %u and %u), EXT_CSD rev %u, device type %02x\n",
        ExtCsd.HsTiming,
        ExtCsd.BusWidth,
        A733_SMHC_HS_TIMING_HS200,
        A733_SMHC_EXT_CSD_WIDTH_8BIT,
        ExtCsd.ExtCsdRev,
        ExtCsd.DeviceType
        ));
      Reason = "the card's timing changed at the chosen tap";
    }
  }

  if (Reason == NULL) {
    //
    // Success.  The driver stays on the HS200 plan from here: the tap, the
    // divider and the width in its state are the ones the FAT traffic will use.
    //
    Private->Hs200    = TRUE;
    Private->HsTiming = A733_SMHC_HS_TIMING_HS200;

    return EFI_SUCCESS;
  }

  //
  // Failure: nothing may be left that only HS200 liked.  The sample delay first,
  // because the reads that follow - the CMD6 writes and the proving read - are
  // single-data-rate ones and must sample the way the SDR path did; then the
  // timing mode; then the card, back to the timing it had; then the plan, back to
  // the one this step started from; then a read to prove all four agree.
  //
  SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_NTSR, NtsrBefore);

  SmhcCardSetHsTiming (Private, Private->HighSpeed ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U, "revert-hs200");
  SmhcCardSwitch (Private, A733_SMHC_EXT_CSD_BUS_WIDTH, Private->BusWidth);

  Private->SampleDelay = SampDlBefore & A733_SMHC_SAMP_DL_SW_MASK;

  PlanStatus = SmhcSelectClockPlan (Private, SdrPlan);
  if (EFI_ERROR (PlanStatus)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot put the %a plan back: %r\n", SmhcClockPlanName (Private->ClockProfile, SdrPlan), PlanStatus));
  } else {
    SmhcSetClock (Private, Private->DataDivisor);
  }

  Status = SmhcCardReadExtCsd (Private, &ExtCsd);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer after the HS200 revert (%r); falling back to the HOSC plan\n", Status));

    PlanStatus = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HOSC);
    if (EFI_ERROR (PlanStatus)) {
      DEBUG ((DEBUG_ERROR, "Smhc: cannot fall back to the HOSC plan: %r\n", PlanStatus));
    } else {
      SmhcSetClock (Private, Private->DataDivisor);
      Private->HighSpeed = FALSE;

      if (EFI_ERROR (SmhcCardSetHighSpeed (Private, &ExtCsd, SmhcClockKhz (Private)))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer at the HOSC plan either\n"));
      }
    }
  } else {
    //
    // Reading the timing back is not restoring it: the HS200 attempt left the
    // card in whatever timing the failed switch put it in, and the SDR path at
    // 50 MHz is only legal in High Speed timing.  Write it back rather than
    // record what is there, or the revert leaves the card at compatibility
    // timing on a 50 MHz bus - out of spec, and one more failure away from a
    // bus the FAT path cannot read.
    //
    if (ExtCsd.HsTiming != A733_SMHC_HS_TIMING_HIGH_SPEED) {
      if (EFI_ERROR (SmhcCardSetHighSpeed (Private, &ExtCsd, SmhcClockKhz (Private)))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the card would not take HS_TIMING 1 back after the HS200 attempt\n"));
      }

      if (EFI_ERROR (SmhcCardReadExtCsd (Private, &ExtCsd))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer after HS_TIMING 1 was restored\n"));
      } else {
        SmhcProbeHsTimingRead (Private, "after the HS200 revert", A733_SMHC_HS_TIMING_HIGH_SPEED, &ExtCsd);
      }
    } else {
      SmhcProbeHsTimingRead (Private, "after the HS200 revert", A733_SMHC_HS_TIMING_HIGH_SPEED, &ExtCsd);
    }

    Private->HighSpeed = ExtCsd.HsTiming == A733_SMHC_HS_TIMING_HIGH_SPEED;
    Private->HsTiming  = ExtCsd.HsTiming;
  }

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: HS200: not taken (%a); the card is back in SDR at %u kHz, HS_TIMING %u, BUS_WIDTH %u, tap %u\n",
    Reason,
    SmhcClockKhz (Private),
    ExtCsd.HsTiming,
    ExtCsd.BusWidth,
    Private->SampleDelay
    ));

  return EFI_DEVICE_ERROR;
}

/**
  Find the host's data-strobe delay for HS400 by sweeping SMHC_DS_DL.

  HS400 is the one mode whose data is sampled against the strobe the card sends
  back, so the tap that matters is DS_DL's and not SAMP_DL's - SAMP_DL is the line
  HS200 is calibrated on, and at HS400 it stays on the command line.  Everything
  else about the procedure is the one three other modes already use here: one
  probe read per tap, judged by its status *and* by the data against the reference
  copy taken on the SDR bus, then the centre of the longest run of passing taps.

  Two details differ, and both belong to the mode rather than to this code: the
  sample FIFO stays in circuit (HS400's data path runs through it, so there is
  nothing to bypass for the sweep), and the tap is written to SMHC_DS_DL instead of
  SMHC_SAMP_DL.

  A failed tap is recovered by the command path, and that recovery re-applies the
  HS400 host set through Private->HostHs400Timing.  Without it the first failure
  would leave EDSD, CSDC, SFC and DS_DL at their reset values and every later tap
  would be measured through a sampler the mode does not use - which reads in the
  log as a bus with no window at all.

  @param[in,out] Private    Driver state; the card must be in HS_TIMING 3 at bus
                            width 6 and the controller in HS400 mode.
  @param[in]     Mode       The timing this sweep is for, for its lines: "HS400".
  @param[in]     Reference  The probe's reference copy, or NULL to judge each tap
                            by its status alone.

  @retval TRUE   A window was found and its centre written to SMHC_DS_DL.
  @retval FALSE  No tap read at all; the caller must revert the whole step.
**/
STATIC
BOOLEAN
SmhcCardSweepStrobeDelay (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST CHAR8        *Mode,
  IN     CONST UINT8        *Reference
  )
{
  BOOLEAN     Pass[A733_SMHC_SAMP_DELAY_TAPS / A733_SMHC_SAMP_DELAY_STEP];
  UINT8       Block[A733_SMHC_SAMP_DELAY_PROBE_BYTES];
  UINT32      Count;
  UINT32      Index;
  UINT32      Run;
  UINT32      RunFirst;
  UINT32      Best;
  UINT32      BestFirst;
  UINT32      BestLast;
  UINT32      Chosen;
  UINT32      Passes;
  UINT32      Tap;
  UINT32      Differ;
  UINT32      FirstOffset;
  UINT32      StatusPasses;
  UINT32      DataOnlyFailures;
  UINT32      CommandCompletes;
  CONST CHAR8  *Verdict;
  EFI_STATUS  Status;

  Count            = A733_SMHC_SAMP_DELAY_TAPS / A733_SMHC_SAMP_DELAY_STEP;
  StatusPasses     = 0;
  DataOnlyFailures = 0;
  CommandCompletes = 0;

  Private->Quiet = TRUE;
  for (Index = 0; Index < Count; Index++) {
    Tap = Index * A733_SMHC_SAMP_DELAY_STEP;
    SmhcSetStrobeDelay (Private->Base, Tap);
    Private->StrobeDelay = Tap;

    Status = SmhcCardReadProbeBlocks (Private, Block, A733_SMHC_SAMP_DELAY_PROBE_BYTES);

    Differ      = 0;
    FirstOffset = 0;
    if (!EFI_ERROR (Status) && (Reference != NULL)) {
      Differ = SmhcCardCompareProbe (Block, Reference, A733_SMHC_SAMP_DELAY_PROBE_BYTES, &FirstOffset);
    }

    //
    // Counted separately from DTC: a tap whose command completes but whose data
    // does not says the command line is sampled correctly and the data path is
    // not, which is the opposite conclusion from a tap whose command never
    // completes.  Both look like "no window" in the summary alone.
    //
    if ((Private->IntStatus & A733_SMHC_RINT_CC) != 0) {
      CommandCompletes++;
    }

    if (!EFI_ERROR (Status)) {
      StatusPasses++;
      if ((Reference != NULL) && (Differ != 0)) {
        DataOnlyFailures++;
      }
    }

    if (EFI_ERROR (Status)) {
      Verdict = "the read failed, so its data was not compared";
    } else if ((Reference == NULL) || (Differ == 0)) {
      Verdict = "the read's data matches the reference";
    } else {
      Verdict = "the read passed and its data differs from the reference";
    }

    SMHC_DIAG ((
      DEBUG_ERROR,
      "Smhc: %a ds_dl tap %u: ds_dl %08x samp_dl %08x sfc %08x, read %r, rint %08x idst %08x, "
      "%u/%u bytes differ (first at %u), %a\n",
      Mode,
      Tap,
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_SFC),
      Status,
      Private->IntStatus,
      Private->IdstStatus,
      Differ,
      (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
      FirstOffset,
      Verdict
      ));

    Pass[Index] = !EFI_ERROR (Status) && ((Reference == NULL) || (Differ == 0));
  }

  Private->Quiet = FALSE;

  Best      = 0;
  BestFirst = 0;
  BestLast  = 0;
  Run       = 0;
  RunFirst  = 0;
  Passes    = 0;

  for (Index = 0; Index < Count; Index++) {
    if (!Pass[Index]) {
      Run = 0;
      continue;
    }

    if (Run == 0) {
      RunFirst = Index;
    }

    Run++;
    Passes++;
    if (Run > Best) {
      Best      = Run;
      BestFirst = RunFirst;
      BestLast  = Index;
    }
  }

  if (Best == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a ds_dl sweep: none of the %u taps tried (0..%u step %u) %a; the %a step cannot be trusted on this card\n",
      Mode,
      Count,
      A733_SMHC_SAMP_DELAY_TAPS - A733_SMHC_SAMP_DELAY_STEP,
      A733_SMHC_SAMP_DELAY_STEP,
      (Reference == NULL) ? "read a block back" : "read a block back with the reference's data",
      Mode
      ));
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a ds_dl sweep: %u of %u taps reached DTC, %u of those returned data that differs from the reference, "
      "%u of %u saw their commands complete (rint CC), criterion %a\n",
      Mode,
      StatusPasses,
      Count,
      DataOnlyFailures,
      CommandCompletes,
      Count,
      (Reference == NULL) ? "status only" : "status and data"
      ));

    return FALSE;
  }

  Chosen = ((BestFirst + BestLast) / 2U) * A733_SMHC_SAMP_DELAY_STEP;
  SmhcSetStrobeDelay (Private->Base, Chosen);
  Private->StrobeDelay = Chosen;

  DEBUG ((
    DEBUG_INFO,
    "Smhc: %a ds_dl sweep: %u taps tried 0..%u step %u, %u passed the criterion (window %u..%u, %u attempts), "
    "%u reached DTC and %u of those returned data that differs from the reference, chosen %u, ds_dl %08x, "
    "samp_dl %08x, sfc %08x, criterion %a\n",
    Mode,
    Count,
    A733_SMHC_SAMP_DELAY_TAPS - A733_SMHC_SAMP_DELAY_STEP,
    A733_SMHC_SAMP_DELAY_STEP,
    Passes,
    BestFirst * A733_SMHC_SAMP_DELAY_STEP,
    BestLast * A733_SMHC_SAMP_DELAY_STEP,
    Best,
    StatusPasses,
    DataOnlyFailures,
    Chosen,
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    SmhcRead (Private->Base, A733_SMHC_SFC),
    (Reference == NULL) ? "status only" : "status and data"
    ));

  return TRUE;
}

/**
  Verify the DS_DL tap the sweep chose, with a timed read of its own.

  The sweep's own read at the chosen tap already compared the data, and this read
  is the second, independent one the step's report stands on: it is taken after
  the sweep has finished and its state has been written back, so a tap that only
  worked because of what the sweep left in a register behind it is caught here.
  Timed as well, because 4 KiB takes about 20 us on a 200 MHz double-edge bus and
  about 80 us on the 50 MHz SDR one, and the figure says whether the rate arrived.

  @param[in,out] Private    Driver state; the chosen tap must be programmed.
  @param[in]     Reference  The probe's reference copy, or NULL.

  @retval TRUE  The read completed and, when there is a reference, matched it.
**/
STATIC
EFI_STATUS
SmhcCardSendStatusWord (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    UINT32             *Word
  );

STATIC
BOOLEAN
SmhcCardVerifyStrobeTap (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST UINT8        *Reference
  )
{
  UINT8       Block[A733_SMHC_SAMP_DELAY_PROBE_BYTES];
  UINT32      Attempts;
  UINT32      CardWord;
  UINT32      Differ;
  UINT32      FirstOffset;
  UINT32      Done;
  UINT32      TicksPerMicro;
  UINT64      Frequency;
  UINT64      Start;
  EFI_STATUS  Status;

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);

  //
  // The sweep's last attempts are failures, and a card whose block was cut off
  // sits in the data state until something ends it - which is why every command
  // after the sweep timed out at a tap the sweep itself had just read correctly.
  // End any transfer still in flight, ask the card for its status, re-programme
  // the delay line and its clock, and only then read; a tap the sweep proved
  // answers on the second attempt, a wrong bus fails all three.
  //
  Attempts = 0;
  do {
    A733_SMHC_COMMAND  Stop;

    SmhcInitCommand (&Stop, A733_SMHC_CMD_STOP_TRANSMISSION, 0, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
    SmhcSendCommand (Private, &Stop, A733_SMHC_CMD_TIMEOUT_US);
    SmhcCardSendStatusWord (Private, &CardWord);

    SmhcSetStrobeDelay (Private->Base, Private->StrobeDelay);
    SmhcSetClock (Private, Private->ClockDivisor);

    Start  = GetPerformanceCounter ();
    Status = SmhcCardReadProbeBlocks (Private, Block, A733_SMHC_SAMP_DELAY_PROBE_BYTES);
    Done   = (UINT32)((GetPerformanceCounter () - Start) / TicksPerMicro);
  } while (EFI_ERROR (Status) && (++Attempts < A733_SMHC_HS400_VERIFY_ATTEMPTS));

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 at ds_dl tap %u: the %u-byte read from LBA 0 failed %r after %u us "
      "(rint %08x idst %08x), ds_dl %08x samp_dl %08x sfc %08x\n",
      Private->StrobeDelay,
      (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
      Status,
      Done,
      Private->IntStatus,
      Private->IdstStatus,
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_SFC)
      ));
    return FALSE;
  }

  if (Reference == NULL) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 at ds_dl tap %u: the %u-byte read from LBA 0 completed in %u us (%u attempts), "
      "no reference to compare against\n",
      Private->StrobeDelay,
      (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
      Done
      ));
    return TRUE;
  }

  Differ = SmhcCardCompareProbe (Block, Reference, A733_SMHC_SAMP_DELAY_PROBE_BYTES, &FirstOffset);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: HS400 at ds_dl tap %u: the %u-byte read from LBA 0 completed in %u us, %u/%u bytes differ from the "
    "reference (first at %u), checksum %08x (reference %08x)%a\n",
    Private->StrobeDelay,
    (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
    Done,
    Differ,
    (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
    FirstOffset,
    SmhcCardProbeChecksum (Block),
    SmhcCardProbeChecksum (Reference),
    (Differ == 0) ? "" : "; the sweep's tap and this read disagree"
    ));

  return Differ == 0;
}

//
// The write-back test's read-back buffer, static for the same reason the
// reference copies are: the frame is already the deepest one here.
//
STATIC UINT8  mSmhcHs400WriteBack[A733_SMHC_SAMP_DELAY_PROBE_BYTES];

//
// The two SMHC_DRV_DL values the write sweep tries, with bit 16 - the command
// line's 180-degree output phase, which every mode in this driver has set - held.
// Bit 17 set is what the DDR52 register set leaves behind and HS400 inherits; bit
// 17 clear is the other row of the manual's table 11-9 for this mode.  A write
// drives DATA from the host and gets only the CRC-status token back, so this
// output phase is a setting no read-side sweep can calibrate.
//
STATIC CONST UINT32  mSmhcHs400WriteDrvDl[A733_SMHC_HS400_WRITE_DRV_DL_COUNT] = {
  A733_SMHC_DRV_DL_DDR,        // bit 17 set: DATA driven at 180 degrees
  A733_SMHC_DRV_DL_CMD_PH180   // bit 17 clear: DATA driven at 90 degrees
};

/**
  Ask the card for its status word (CMD13) at the driver's relative address.

  The word says where the card is: bits 12:9 are CURRENT_STATE, bit 8 is
  READY_FOR_DATA and bit 15 is the error bit.  Nothing here decides anything -
  the callers read the word and say what they make of it - and the command is a
  good "is the card still there" probe as well, because it carries no data.

  @param[in,out] Private    Driver state; the card must be addressed.
  @param[out]    Word       Receives CMD13's R1 status word, 0 when it did not answer.
  @param[in]     TimeoutUs  Budget for this one command.

  @retval EFI_SUCCESS  The card answered.
  @retval other        The command failed.
**/
STATIC
EFI_STATUS
SmhcCardSendStatusWordWithin (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    UINT32             *Word,
  IN     UINT64             TimeoutUs
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;

  SmhcInitCommand (&Command, A733_SMHC_CMD_SEND_STATUS, Private->Rca << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
  Status = SmhcSendCommand (Private, &Command, TimeoutUs);
  *Word  = Command.Response[0];

  return Status;
}

/**
  Ask the card for its status word (CMD13) at the driver's relative address.

  The word says where the card is: bits 12:9 are CURRENT_STATE, bit 8 is
  READY_FOR_DATA and bit 15 is the error bit.  Nothing here decides anything -
  the callers read the word and say what they make of it - and the command is a
  good "is the card still there" probe as well, because it carries no data.

  @param[in,out] Private  Driver state; the card must be addressed.
  @param[out]    Word     Receives CMD13's R1 status word, 0 when it did not answer.

  @retval EFI_SUCCESS  The card answered.
  @retval other        The command failed.
**/
STATIC
EFI_STATUS
SmhcCardSendStatusWord (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    UINT32             *Word
  )
{
  return SmhcCardSendStatusWordWithin (Private, Word, A733_SMHC_CMD_TIMEOUT_US);
}

/**
  Let a failed transfer's recovery run before the next write attempt.

  A failed data phase leaves the controller reset and Private->DataFault set, and
  the recovery runs at the head of the next command.  It re-applies the driver's
  idea of the bus, which includes SMHC_DRV_DL - a register the write sweep sets
  per attempt and the driver does not otherwise track - so a sweep that wrote its
  output phase and then sent a command would have it replaced halfway through the
  attempt.  This sends a data-less CMD13 while Quiet is set: it carries no data,
  so the recovery has nothing to re-frame, and the caller's DRV_DL write that
  follows it is the one the attempt runs under.

  @param[in,out] Private  Driver state.
**/
STATIC
VOID
SmhcCardSettleAfterFault (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  UINT32   Word;
  BOOLEAN  Quiet;

  if (!Private->DataFault) {
    return;
  }

  Quiet          = Private->Quiet;
  Private->Quiet = TRUE;
  SmhcCardSendStatusWord (Private, &Word);
  Private->Quiet = Quiet;
}

//
// The recovery's prove read, one block, static for the same reason the other
// probe buffers here are: the frame is already the deepest one in this file.
//
STATIC UINT8  mSmhcHs400RecoverProbe[A733_SMHC_HS400_RECOVER_PROBE_BYTES];

/**
  End a transfer the card is still in, and wait until it can take another.

  A write the card never finished leaves it in CURRENT_STATE 6 (receiving): the
  data phase was cut off without a CMD12, and until something ends it the card
  ignores every command the state does not allow - a CMD25 sent there is never
  answered and comes back as a response timeout in a few microseconds, which is
  indistinguishable from a wrong write setting unless the card is recovered first.
  CMD12 is the command that ends it, and its own status carries no information:
  on a card that had already stopped it answers R1 and changes nothing.

  READY_FOR_DATA alone is not the test: the card sets that bit while still in the
  receive state, so the state field and the ready bit are read together.  The poll
  is bounded, because a card that never reaches TRAN is a finding to report rather
  than something to wait for.

  @param[in,out] Private     Driver state.
  @param[out]    StopStatus  Receives CMD12's own result, for the log.
  @param[out]    FirstWord   Receives the first CMD13 word.
  @param[out]    LastWord    Receives the last CMD13 word.
  @param[out]    Polls       Receives how many CMD13 polls it took.

  @retval EFI_SUCCESS  The card is in TRAN with READY_FOR_DATA set.
  @retval other        It did not get there within the poll budget.
**/
STATIC
EFI_STATUS
SmhcCardStopAndWaitReady (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    EFI_STATUS         *StopStatus,
  OUT    UINT32             *FirstWord,
  OUT    UINT32             *LastWord,
  OUT    UINT32             *Polls
  )
{
  A733_SMHC_COMMAND  Stop;
  UINT32             Index;
  UINT32             Word;
  EFI_STATUS         Status;

  *FirstWord = 0;
  *LastWord  = 0;
  *Polls     = 0;

  SmhcInitCommand (&Stop, A733_SMHC_CMD_STOP_TRANSMISSION, 0, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
  Stop.Abort   = TRUE;
  *StopStatus = SmhcSendCommand (Private, &Stop, A733_SMHC_CMD_TIMEOUT_US);

  for (Index = 0; Index < A733_SMHC_HS400_RECOVER_POLLS; Index++) {
    Word   = 0;
    Status = SmhcCardSendStatusWord (Private, &Word);
    (*Polls)++;

    if (*Polls == 1) {
      *FirstWord = Word;
    }

    *LastWord = Word;

    if (!EFI_ERROR (Status) &&
        (((Word >> A733_SMHC_R1_STATE_SHIFT) & 0xFU) == A733_SMHC_R1_STATE_TRAN) &&
        ((Word & A733_SMHC_R1_READY_FOR_DATA) != 0))
    {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (A733_SMHC_HS400_RECOVER_POLL_US);
  }

  return EFI_TIMEOUT;
}

/**
  Prove that the bus carries data at the taps that are programmed now.

  One 512-byte read of LBA 0 through the same CMD23+CMD18 shape every transfer
  uses, timed.  The status is what decides - a read that returns is the proof the
  next write attempt needs - while the compare against the reference is reported
  beside it, because a read that returns the wrong bytes says something about the
  write that came before it, not about whether the bus is alive.

  @param[in,out] Private    Driver state.
  @param[in]     Reference  The step's reference copy of LBA 0, or NULL.
  @param[in]     Tag        What is being recovered, for the line.
  @param[in]     Stage      Which attempt this is, for the line.

  @retval TRUE  The read completed and the bus carried a block.
**/
STATIC
BOOLEAN
SmhcCardProveBusBlock (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST UINT8        *Reference,
  IN     CONST CHAR8        *Tag,
  IN     CONST CHAR8        *Stage
  )
{
  UINT32      Differ;
  UINT32      FirstOffset;
  UINT32      Done;
  UINT32      TicksPerMicro;
  UINT64      Frequency;
  UINT64      Start;
  EFI_STATUS  Status;

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);

  Start  = GetPerformanceCounter ();
  Status = SmhcCardReadProbeBlocks (Private, mSmhcHs400RecoverProbe, A733_SMHC_HS400_RECOVER_PROBE_BYTES);
  Done   = (UINT32)((GetPerformanceCounter () - Start) / TicksPerMicro);

  Differ      = 0;
  FirstOffset = 0;
  if (!EFI_ERROR (Status) && (Reference != NULL)) {
    Differ = SmhcCardCompareProbe (mSmhcHs400RecoverProbe, Reference, A733_SMHC_HS400_RECOVER_PROBE_BYTES, &FirstOffset);
  }

  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "Smhc: HS400 %a: prove read (%a) of %u bytes from LBA 0 at samp_dl %08x ds_dl %08x: %r in %u us, rint %08x idst %08x, "
    "%u/%u bytes differ from the reference (first at %u)\n",
    Tag,
    Stage,
    (UINT32)A733_SMHC_HS400_RECOVER_PROBE_BYTES,
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    Status,
    Done,
    Private->IntStatus,
    Private->IdstStatus,
    Differ,
    (UINT32)A733_SMHC_HS400_RECOVER_PROBE_BYTES,
    FirstOffset
    ));

  return !EFI_ERROR (Status);
}

/**
  Bring the card and the bus back after a failed write, and prove it.

  Every attempt of the write sweep used to run on whatever the previous failure
  left behind, which was a card stuck in the receive state: one real measurement
  and sixty-three lines that said nothing about their settings.  This is the
  sequence that makes the next line mean something:

  1. CMD12, whose status is not used, to end the transfer the card is still in;
  2. CMD13 polled until CURRENT_STATE is TRAN and READY_FOR_DATA is set, reporting
     the first word, the last word and how many reads it took;
  3. a 512-byte read of LBA 0 at the taps in the registers, which is the proof;
  4. if that read fails, the card's own timing (HS_TIMING 3 through the helper the
     step enters the mode with) and then the host's HS400 register set are written
     again - a card that dropped the switch or a controller the recovery reset -
     and the read is repeated;
  5. if it still fails, once more at the mode's entry command tap, because the tap
     the caller is testing may simply be one no read can pass, and that is a
     different statement from "the bus is dead".  The caller's tap goes back
     afterwards either way.

  Only (3), (4) or (5) returning a block lets a caller treat its next setting as
  measurable; FALSE means the card or the controller is gone and the sweep has
  nothing left to measure.

  @param[in,out] Private     Driver state; DS_DL and the caller's SAMP_DL are
                             left as they were.
  @param[in]     Reference   The step's reference copy of LBA 0, or NULL.
  @param[in]     Tag         What is being recovered, for the lines.
  @param[out]    StatusWord  Receives the last CMD13 word.
  @param[out]    LastRint    Receives the last RINTSTS value seen.

  @retval TRUE   The card answers and the bus carried a block; the next setting
                 may be tried.
  @retval FALSE  It could not be proved; no further setting can be measured.
**/
STATIC
BOOLEAN
SmhcCardRecoverWriteBus (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST UINT8        *Reference,
  IN     CONST CHAR8        *Tag,
  OUT    UINT32             *StatusWord,
  OUT    UINT32             *LastRint
  )
{
  UINT32      FirstWord;
  UINT32      LastWord;
  UINT32      Polls;
  UINT32      AttemptTap;
  EFI_STATUS  StopStatus;
  EFI_STATUS  ReadyStatus;
  EFI_STATUS  TimingStatus;
  BOOLEAN     Proven;

  AttemptTap = SmhcRead (Private->Base, A733_SMHC_SAMP_DL) & A733_SMHC_SAMP_DL_SW_MASK;

  ReadyStatus = SmhcCardStopAndWaitReady (Private, &StopStatus, &FirstWord, &LastWord, &Polls);
  *StatusWord = LastWord;
  *LastRint   = Private->IntStatus;

  DEBUG ((
    EFI_ERROR (ReadyStatus) ? DEBUG_ERROR : DEBUG_INFO,
    "Smhc: HS400 %a: recovery - CMD12 %r (its status is not used), then %u CMD13 reads: first word %08x (state %u, "
    "ready %u, error %u), last word %08x (state %u, ready %u, error %u)\n",
    Tag,
    StopStatus,
    Polls,
    FirstWord,
    (FirstWord >> A733_SMHC_R1_STATE_SHIFT) & 0xFU,
    (FirstWord & A733_SMHC_R1_READY_FOR_DATA) ? 1U : 0U,
    (FirstWord & A733_SMHC_R1_ERROR) ? 1U : 0U,
    LastWord,
    (LastWord >> A733_SMHC_R1_STATE_SHIFT) & 0xFU,
    (LastWord & A733_SMHC_R1_READY_FOR_DATA) ? 1U : 0U,
    (LastWord & A733_SMHC_R1_ERROR) ? 1U : 0U
    ));

  Proven = SmhcCardProveBusBlock (Private, Reference, Tag, "at the setting's own tap");

  if (!Proven) {
    //
    // The card's timing first, then the host's set: both are what a failed write
    // or the controller recovery behind it can leave in the wrong state, and a
    // read cannot tell which of them it is.  HS_TIMING 3 through the helper the
    // step uses keeps the card's side identical to the one the mode was entered
    // with; SmhcSetHs400Mode re-programs EDSD, CSDC, the sample FIFO and both
    // delay lines from the driver's state, which the caller's tap is part of.
    //
    TimingStatus = SmhcCardSetHsTiming (Private, A733_SMHC_HS_TIMING_HS400, "hs400-recover");
    SmhcSetHs400Mode (Private, TRUE);

    DEBUG ((
      EFI_ERROR (TimingStatus) ? DEBUG_ERROR : DEBUG_INFO,
      "Smhc: HS400 %a: the prove read failed, so the card's timing was asserted again (HS_TIMING %u: %r) and the "
      "host's HS400 set re-programmed - edsd %08x csdc %08x sfc %08x ds_dl %08x samp_dl %08x ntsr %08x drv_dl %08x "
      "ctrl %08x (ddr bit %u)\n",
      Tag,
      A733_SMHC_HS_TIMING_HS400,
      TimingStatus,
      SmhcRead (Private->Base, A733_SMHC_EDSD),
      SmhcRead (Private->Base, A733_SMHC_CSDC),
      SmhcRead (Private->Base, A733_SMHC_SFC),
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_NTSR),
      SmhcRead (Private->Base, A733_SMHC_DRV_DL),
      SmhcRead (Private->Base, A733_SMHC_CTRL),
      (UINT32)Private->DdrModeBit
      ));

    Proven = SmhcCardProveBusBlock (Private, Reference, Tag, "after HS_TIMING 3 and the host HS400 set were written again");
  }

  if (!Proven && (AttemptTap != A733_SMHC_HS400_CMD_TAPS)) {
    //
    // The tap being tested may be one no read can pass, which says nothing about
    // the card.  The recovery's own settle goes first: it is what runs the
    // controller reset the failed read left behind, and a command would otherwise
    // re-programme SAMP_DL from the driver's state over the tap set below.
    //
    SmhcCardSettleAfterFault (Private);
    SmhcSetSampleDelay (Private->Base, A733_SMHC_HS400_CMD_TAPS);

    Proven = SmhcCardProveBusBlock (Private, Reference, Tag, "at the mode's entry command tap, the one the step proved its read at");
  }

  //
  // The caller's tap goes back whatever the outcome was, in the register and in
  // the driver state together: Private->SampleDelay is what every later recovery
  // re-programs from, so the two must not disagree.
  //
  SmhcSetSampleDelay (Private->Base, AttemptTap);

  DEBUG ((
    Proven ? DEBUG_INFO : DEBUG_ERROR,
    "Smhc: HS400 %a: recovery %a - card %a, last CMD13 word %08x (state %u, ready %u), last rint %08x\n",
    Tag,
    Proven ? "reached" : "NOT reached",
    Proven ? "is in TRAN and the bus carried a 512-byte read of LBA 0" : "could not be brought back, so no setting after this one can be measured",
    LastWord,
    (LastWord >> A733_SMHC_R1_STATE_SHIFT) & 0xFU,
    (LastWord & A733_SMHC_R1_READY_FOR_DATA) ? 1U : 0U,
    *LastRint
    ));

  return Proven;
}

/**
  Write LBA 0's blocks back to it through the driver's own command path.

  Exactly the shape the pass-thru builds for EmmcDxe: CMD23 arms the block count,
  then CMD25 with the data moving host to card.  The caller passes the copy the
  probe read from these same blocks, so a block that programs is programmed with
  the bytes it already held, and a transfer that never reaches the card leaves the
  blocks untouched - which is what makes this safe to run before the mode is known
  to write at all.

  @param[in,out] Private  Driver state; the card must be selected and ready.
  @param[in]     Buffer   The bytes to write, read from LBA 0 in the first place.
  @param[in]     Length   Byte count, a whole number of 512-byte blocks.

  @retval EFI_SUCCESS  The command and its data phase completed.
  @retval other        The command or its data phase failed.
**/
STATIC
EFI_STATUS
SmhcCardWriteProbeBlocks (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST UINT8        *Buffer,
  IN     UINT32             Length
  )
{
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;

  SmhcInitCommand (&Command, A733_SMHC_CMD_SET_BLOCK_COUNT, Length / A733_SMHC_BLOCK_SIZE, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->CountArmed = TRUE;

  //
  // LBA 0, whose argument is 0 in both of the card's addressing modes.  The
  // attempt's own budget, like the read probe's: a setting that fails is expected
  // here, and one that fails without latching an error bit must not cost a full
  // data timeout or the sweep would take minutes.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_WRITE_MULTIPLE, 0, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1);
  Command.Buffer    = (VOID *)(UINTN)Buffer;
  Command.Length    = Length;
  Command.BlockSize = A733_SMHC_BLOCK_SIZE;
  Command.Read      = FALSE;

  Status = SmhcSendCommand (Private, &Command, A733_SMHC_SAMP_DELAY_ATTEMPT_US);

  Private->CountArmed = FALSE;

  return Status;
}

/**
  Wait for the card to finish programming the blocks a write just sent.

  A multi-block write's data phase ends when the last block's CRC-status token has
  been accepted, not when the card has programmed the blocks: it holds DATA0 low
  while it works, and a read sent into that window would be refused.  CMD13's
  READY_FOR_DATA is the answer, polled until a budget is spent.

  A CMD13 that fails while the card is programming is the card being busy, not a
  bus that has gone wrong: at HS400 it stops answering the status command exactly
  as it stops answering anything else, and it comes back on its own once the blocks
  are programmed.  So a failed poll does not end the wait - it is retried over the
  whole budget, and only a bus that has not answered by the end of the budget is
  reported as dead.  The spacing between attempts is a millisecond of wall time
  rather than a spin on the bus.

  @param[in,out] Private   Driver state.
  @param[out]    Word      Receives the last status word the card answered with.
  @param[out]    Polls     Receives how many status reads the card answered - the
                           figure the attempt line reports as "N status reads",
                           which is why a failed attempt is not counted.
  @param[out]    WaitEnd   Receives how the wait ended (A733_SMHC_WAIT_END_*).
  @param[out]    LastRint  Receives RINTSTS as the last failed status read left it.

  @retval EFI_SUCCESS       The card reports READY_FOR_DATA.
  @retval EFI_TIMEOUT       It answered, but never reported READY_FOR_DATA.
  @retval EFI_DEVICE_ERROR  It stopped answering within the budget.
**/
STATIC
EFI_STATUS
SmhcCardWaitWriteDone (
  IN OUT A733_SMHC_PRIVATE  *Private,
  OUT    UINT32             *Word,
  OUT    UINT32             *Polls,
  OUT    UINT32             *WaitEnd,
  OUT    UINT32             *LastRint
  )
{
  UINT32      Attempt;
  UINT32      Answered;
  UINT32      PollWord;
  UINT32      TicksPerMicro;
  UINT64      Deadline;
  UINT64      Frequency;
  UINT64      Start;
  EFI_STATUS  Status;

  *Word     = 0;
  *Polls    = 0;
  *LastRint = 0;
  *WaitEnd  = A733_SMHC_WAIT_END_BUDGET;

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);

  Start    = GetPerformanceCounter ();
  Deadline = (UINT64)TicksPerMicro * A733_SMHC_HS400_WRITE_READY_BUDGET_US;
  Answered = 0;
  Status   = EFI_DEVICE_ERROR;

  for (Attempt = 0; Attempt < A733_SMHC_HS400_WRITE_STATUS_POLLS; Attempt++) {
    PollWord = 0;
    Status   = SmhcCardSendStatusWordWithin (Private, &PollWord, A733_SMHC_HS400_WRITE_STATUS_CMD_US);

    if (EFI_ERROR (Status)) {
      //
      // Busy, not broken: remember what the failure looked like and keep asking
      // until the budget is gone.  A card that is programming sets nothing here
      // but its response timeout, which is the evidence the last line carries.
      //
      *LastRint = Private->IntStatus;
    } else {
      Answered++;
      *Word = PollWord;

      if ((PollWord & A733_SMHC_R1_READY_FOR_DATA) != 0) {
        *Polls   = Answered;
        *WaitEnd = A733_SMHC_WAIT_END_READY;
        return EFI_SUCCESS;
      }
    }

    if ((GetPerformanceCounter () - Start) >= Deadline) {
      break;
    }

    gBS->Stall (A733_SMHC_HS400_WRITE_STATUS_POLL_US);
  }

  *Polls = Answered;

  if (!EFI_ERROR (Status)) {
    *WaitEnd = A733_SMHC_WAIT_END_BUDGET;
    return EFI_TIMEOUT;
  }

  *WaitEnd = A733_SMHC_WAIT_END_DEAD_BUS;
  return EFI_DEVICE_ERROR;
}

/**
  The word the attempt line prints for how a readiness wait ended.

  @param[in] WaitEnd  One of A733_SMHC_WAIT_END_*.

  @return A short name for it.
**/
STATIC
CONST CHAR8 *
SmhcWaitEndName (
  IN UINT32  WaitEnd
  )
{
  switch (WaitEnd) {
    case A733_SMHC_WAIT_END_READY:
      return "ready";
    case A733_SMHC_WAIT_END_BUDGET:
      return "budget - answered, never ready";
    case A733_SMHC_WAIT_END_DEAD_BUS:
      return "dead bus - stopped answering";
    default:
      return "not run - the write failed first";
  }
}

/**
  Write LBA 0's own bytes back to LBA 0, then read them back and compare.

  This is the write-side counterpart of the DS_DL sweep's probe, and it is safe by
  construction rather than by care: the bytes it writes are the bytes the probe
  read from the same eight blocks at the start of the step, so a block that
  programs holds what it already held and a transfer that fails leaves the blocks
  it never reached untouched.  No other LBA is touched, and the backup GPT at the
  end of the device is nowhere near it.

  A failed write is followed by three things that no host status bit can supply:
  CMD13, which says where the card ended up and whether it reports an error; the
  recovery, which ends the transfer the card is stuck in and proves the bus with a
  read (SmhcCardRecoverWriteBus); and a read of LBA 0, which adds the one thing
  the status cannot - a write of identical bytes that never reached the card looks
  exactly like one that did, unless the blocks come back changed.  All are
  printed, because the next round has to know whether the card saw the data or
  never started, and because every later setting is only measurable if the bus was
  proved in between.

  A write that completes is followed by the readiness wait and then by the
  read-back, and only the read-back decides: the card holds DATA0 low while it
  programs, so both the status command and a read sent too early fail with a
  response timeout that says nothing about the setting.  The wait spends its whole
  budget before it gives up (SmhcCardWaitWriteDone), and the read is retried a few
  times a millisecond apart before the setting is called bad.

  @param[in,out] Private    Driver state; the card must be in HS400, selected and
                            ready, with DS_DL, SAMP_DL and DRV_DL already at the
                            values this attempt is to measure.
  @param[in]     Reference  What the probe read from LBA 0, at least Length
                            bytes of it.
  @param[in]     Length     Byte count to write and read back: a whole number of
                            512-byte blocks from LBA 0.  The step's criterion is
                            the 4 KiB probe; the single-block shape the boot's own
                            GPT repair uses is measured with the same code.
  @param[out]    WriteUs    Receives how long the CMD25 took.
  @param[out]    BusProven  Receives whether the bus may be written to again: TRUE
                            when the write succeeded or the recovery after it did,
                            FALSE when the card could not be brought back.
  @param[out]    LastWord   Receives the recovery's last CMD13 word, 0 when no
                            recovery ran.
  @param[out]    LastRint   Receives the recovery's last RINTSTS value.

  @retval TRUE   The write completed and its read-back matched what was written.
  @retval FALSE  It did not; the log carries the evidence.
**/
STATIC
BOOLEAN
SmhcCardWriteBackTest (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST UINT8        *Reference,
  IN     UINT32             Length,
  OUT    UINT32             *WriteUs,
  OUT    BOOLEAN            *BusProven,
  OUT    UINT32             *LastWord,
  OUT    UINT32             *LastRint
  )
{
  UINT32      Differ;
  UINT32      FirstOffset;
  UINT32      Polls;
  UINT32      ReadUs;
  UINT32      WaitEnd;
  UINT32      WaitRint;
  UINT32      Tries;
  UINT32      Word;
  UINT32      WriteRint;
  UINT32      WriteIdst;
  UINT32      DrvDl;
  UINT32      SampDl;
  UINT32      DsDl;
  UINT32      TicksPerMicro;
  UINT32      KibPerSec;
  UINT64      Frequency;
  UINT64      Start;
  EFI_STATUS  WriteStatus;
  EFI_STATUS  WaitStatus;
  EFI_STATUS  ReadStatus;
  EFI_STATUS  Status;
  BOOLEAN     Passed;

  //
  // The register values this attempt runs under, read before the first command:
  // a failed attempt has its registers replaced by the recovery, so reading them
  // afterwards would report the recovery's values and not the attempt's.
  //
  DrvDl  = SmhcRead (Private->Base, A733_SMHC_DRV_DL);
  SampDl = SmhcRead (Private->Base, A733_SMHC_SAMP_DL);
  DsDl   = SmhcRead (Private->Base, A733_SMHC_DS_DL);

  SmhcCardSettleAfterFault (Private);

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);

  Differ      = 0;
  FirstOffset = 0;
  Polls       = 0;
  ReadUs      = 0;
  WaitEnd     = A733_SMHC_WAIT_END_NOT_RUN;
  WaitRint    = 0;
  Tries       = 0;
  Word        = 0;
  //
  // Both are "not run" until they are: an attempt whose write failed never waits
  // for the card and never reads back, and the attempt line has to say that
  // rather than report a success that never happened.
  //
  WaitStatus = EFI_NOT_READY;
  ReadStatus = EFI_NOT_READY;
  *BusProven = TRUE;
  *LastWord  = 0;
  *LastRint  = 0;
  Passed     = FALSE;

  Start       = GetPerformanceCounter ();
  WriteStatus = SmhcCardWriteProbeBlocks (Private, Reference, Length);
  *WriteUs    = (UINT32)((GetPerformanceCounter () - Start) / TicksPerMicro);
  WriteRint   = Private->IntStatus;
  WriteIdst   = Private->IdstStatus;

  if (EFI_ERROR (WriteStatus)) {
    //
    // Where the card is, straight away: the one question the host cannot answer
    // from its side.  This also runs the controller recovery, so the CMD12 below
    // goes out on a controller the driver has configured again.
    //
    Status = SmhcCardSendStatusWord (Private, &Word);

    if (!Private->Quiet) {
      SMHC_DIAG ((
        DEBUG_ERROR,
        "Smhc: HS400 write test: CMD25 arg %08x, %u blocks of %u bytes at LBA 0 (no other block is written), "
        "trans_dir 1 (host to card), ctrl ddr_sel %u; the write failed after %u us, rint %08x idst %08x\n",
        0U,
        Length / A733_SMHC_BLOCK_SIZE,
        (UINT32)A733_SMHC_BLOCK_SIZE,
        (UINT32)Private->DdrModeBit,
        *WriteUs,
        WriteRint,
        WriteIdst
        ));

      SMHC_DIAG ((
        DEBUG_ERROR,
        "Smhc: HS400 write test: CMD13 straight after the failed write: %r, word %08x (state %u, ready %u, error %u); "
        "the card %a\n",
        Status,
        Word,
        (Word >> A733_SMHC_R1_STATE_SHIFT) & 0xFU,
        (Word & A733_SMHC_R1_READY_FOR_DATA) ? 1U : 0U,
        (Word & A733_SMHC_R1_ERROR) ? 1U : 0U,
        EFI_ERROR (Status) ? "did not answer the status read either" :
        "is still there and answered, so it survived the write it did not acknowledge"
        ));
    }

    //
    // End the transfer the card is still in and prove the bus, before anything
    // else is measured: a card left in the receive state ignores the next write,
    // and every line the sweep printed after the first failure used to be that
    // silence rather than a verdict on its setting.
    //
    *BusProven = SmhcCardRecoverWriteBus (Private, Reference, "write test", LastWord, LastRint);

    //
    // Reading the same blocks back is the only way to tell "never started" from
    // "wrote something else": the bytes written were the bytes already there, so
    // a match proves nothing, while a difference proves the data did reach the
    // card and arrived changed.  It is only worth doing on a bus the recovery
    // proved - a read the card is still not answering would say nothing.
    //
    if (*BusProven) {
      ReadStatus = SmhcCardReadProbeBlocks (Private, mSmhcHs400WriteBack, Length);
      if (!EFI_ERROR (ReadStatus)) {
        Differ = SmhcCardCompareProbe (mSmhcHs400WriteBack, Reference, Length, &FirstOffset);
      }
    }

    if (!Private->Quiet) {
      SMHC_DIAG ((
        DEBUG_ERROR,
        "Smhc: HS400 write test: LBA 0 read back after the recovery: %r, %u/%u bytes differ from the reference "
        "(first at %u) - %a\n",
        ReadStatus,
        Differ,
        Length,
        FirstOffset,
        !(*BusProven) ? "the bus was not proved, so the blocks were not read back" :
        (EFI_ERROR (ReadStatus) || (Differ == 0)) ?
        "no evidence that any block was programmed" :
        "the write did reach the card and the blocks came back changed"
        ));
    }
  } else {
    WaitStatus = SmhcCardWaitWriteDone (Private, &Word, &Polls, &WaitEnd, &WaitRint);

    //
    // The readiness poll is information, not the verdict.  A card that is
    // programming answers CMD13 with READY_FOR_DATA clear - and at HS400 it stops
    // answering at all for as long as it needs - which made a write that had
    // succeeded and been read back byte-for-byte look like a failure and sent the
    // sweep on to settings that were worse.  What decides is the read-back below:
    // it is the only evidence that says the bytes are in the card.
    //
    // So a wait that spent its budget is reported and then stepped over, in one
    // line of its own, because the two ways of spending it mean different things
    // and a later round has to be able to tell them apart.
    //
    if (EFI_ERROR (WaitStatus) && !Private->Quiet) {
      SMHC_DIAG ((
        DEBUG_ERROR,
        "Smhc: HS400 write test: drv_dl %08x samp_dl %08x - the card never reported READY_FOR_DATA within %u ms, "
        "wait ended %a; %u status reads answered, last word %08x (state %u, ready %u, error %u), rint of the last "
        "failed read %08x - reading back anyway, because the read-back is the verdict and this is not\n",
        DrvDl,
        SampDl,
        A733_SMHC_HS400_WRITE_READY_BUDGET_US / 1000U,
        SmhcWaitEndName (WaitEnd),
        Polls,
        Word,
        (Word >> A733_SMHC_R1_STATE_SHIFT) & 0xFU,
        (Word & A733_SMHC_R1_READY_FOR_DATA) ? 1U : 0U,
        (Word & A733_SMHC_R1_ERROR) ? 1U : 0U,
        WaitRint
        ));
    }

    //
    // A read 4 us after the write fails with a response timeout because the card
    // is still programming, not because the setting is wrong, so a failed
    // read-back is repeated - a millisecond apart, up to a fixed count - before it
    // is allowed to condemn the setting.  The time reported is that of the last
    // attempt, which is the one whose status and data the line describes.
    //
    for (Tries = 0; Tries < A733_SMHC_HS400_WRITE_READBACK_TRIES; Tries++) {
      if (Tries != 0) {
        //
        // A failed read leaves the controller to be reconfigured, and that
        // reconfiguration writes DRV_DL from the driver's own idea of the bus
        // rather than this attempt's - which is the sweep's whole dimension.  The
        // settle runs the recovery; the two writes after it put this attempt's
        // setting back before the bus is used again, exactly as the sweep does
        // before each attempt.
        //
        SmhcCardSettleAfterFault (Private);
        SmhcWrite (Private->Base, A733_SMHC_DRV_DL, DrvDl);
        SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDl);
        gBS->Stall (A733_SMHC_HS400_WRITE_STATUS_POLL_US);
      }

      Start      = GetPerformanceCounter ();
      ReadStatus = SmhcCardReadProbeBlocks (Private, mSmhcHs400WriteBack, Length);
      ReadUs     = (UINT32)((GetPerformanceCounter () - Start) / TicksPerMicro);

      if (!EFI_ERROR (ReadStatus)) {
        Differ = SmhcCardCompareProbe (mSmhcHs400WriteBack, Reference, Length, &FirstOffset);
        Tries++;
        break;
      }
    }
  }

  Passed = !EFI_ERROR (WriteStatus) && !EFI_ERROR (ReadStatus) && (Differ == 0);

  KibPerSec = (*WriteUs == 0) ? 0U
             : (UINT32)(((UINT64)Length / 1024U) * 1000000U / *WriteUs);

  //
  // One line per attempt, the shape the read-side sweeps use: the values tried,
  // what the command did, how the card's readiness wait ended and how the reads
  // went, and - the one thing this attempt cannot report about itself - whether
  // the bus was proved afterwards, i.e. whether the next setting will be measured
  // at all.  The wait's own `%a` is chosen so that the three endings - ready, the
  // budget spent on a card that answered, the budget spent on a card that stopped
  // answering - are told apart without reading further.
  //
  SMHC_DIAG ((
    Passed ? DEBUG_INFO : DEBUG_ERROR,
    "Smhc: HS400 write drv_dl %08x samp_dl %08x ds_dl %08x: %u bytes at LBA 0 written %r in %u us (%u KiB/s), card ready "
    "%r after %u status reads, wait %a (last word %08x, rint %08x), read-back %r after %u tries in %u us, %u/%u bytes "
    "differ (first at %u), rint %08x idst %08x, criterion %a; bus %a\n",
    DrvDl,
    SampDl,
    DsDl,
    Length,
    WriteStatus,
    *WriteUs,
    KibPerSec,
    WaitStatus,
    Polls,
    SmhcWaitEndName (WaitEnd),
    Word,
    WaitRint,
    ReadStatus,
    Tries,
    ReadUs,
    Differ,
    Length,
    FirstOffset,
    WriteRint,
    WriteIdst,
    Passed ? "met" : "not met",
    *BusProven ? "proved for the next setting" : "dead - nothing after this is measurable"
    ));

  return Passed;
}

/**
  Find the host settings HS400 needs to write, using the write-back test.

  A read is sampled by the host against the strobe the card returns, which is why
  the read side needs DS_DL.  A write has no such strobe in its path: the host
  drives DATA and the card answers with the CRC-status token alone, so what a write
  depends on is

  - the phase the host drives DATA with - SMHC_DRV_DL bit 17, whose bit 16 (the
    command line) is held at the 180 degrees every mode here uses.  Bit 17 set is
    what the DDR52 register set left behind and HS400 inherited; the manual's table
    11-9 gives this mode both values, so both are tried; and
  - the point the controller samples the token at, which at HS400 is SMHC_SAMP_DL -
    the command line's tap - tried over its whole range, 0..62 step 2, at each
    output phase.

  The first setting tried is the one the mode is already on, and it runs with the
  per-command decoding on: that is the boot's own first CMD25 in miniature, so its
  full register and descriptor dump is the evidence the next round is read
  against.  The single-block shape the boot's own GPT repair uses is measured
  straight after it, also with the decoding on, because a 4 KiB transfer and one
  block fail differently if the block count is what the mode is missing.  Every
  setting after that runs quiet, one line each, the way the read-side sweeps do.

  A setting only means something on a bus that can carry data, and a failed write
  leaves the card in the receive state, where every command but CMD12 is ignored.
  So every attempt that fails is followed by the recovery inside the write-back
  test (CMD12, CMD13 until TRAN, then a 512-byte read of LBA 0), and the sweep
  stops rather than print settings it could not measure: its last line then names
  how many of the planned settings were never reached, which is a different
  statement from "this setting is wrong".

  The entry setting is tried by the same criterion as the grid, so a boot that
  enters on a good tap keeps it without the grid running at all.  When the grid
  does run, the tap kept is the centre of the longest run of passes rather than
  the first pass: an edge tap passes on the day it is measured and fails when the
  card's timing or the board's temperature moves, which is exactly the flakiness a
  first-pass rule produced.  The whole grid is bounded by a wall-clock budget, so
  a mode in which nothing writes costs a few seconds of boot and not a minute; a
  sweep stopped by it says so instead of reading as a verdict on the mode.

  Two further dimensions exist and are swept only when the two real ones are empty:
  NTSR's 1x/2x mode bit as a single A/B before the sweep (the port inherits bit 31
  = 0 from the DDR step while the module clock is twice the card clock; reads are
  strobe-sampled and unaffected, so only the write is measured), and CSDC's
  CRC-status token over its whole field, which the vendor programs as 6 for HS400
  and never verified on this board.

  SAMP_DL is on the read path too at HS400, so a setting kept for the write is
  proved by a data-compared read before it is kept.  If nothing passes, DRV_DL,
  SAMP_DL, CSDC and NTSR are put back to the values the mode was entered with: the
  write side is not what decides whether the mode is taken (the reads and the
  card's read-back already did), so a failure here leaves the mode exactly as it
  was.

  @param[in,out] Private    Driver state; the card must be in HS400 and ready.
  @param[in]     Reference  The probe's 4 KiB copy of LBA 0.

  @retval TRUE   A setting wrote a block and read it back, and is now programmed.
  @retval FALSE  No setting did; the registers are back as they were.
**/
STATIC
BOOLEAN
SmhcCardSweepWriteTiming (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     CONST UINT8        *Reference
  )
{
  UINT32   DrvDlBefore;
  UINT32   SampDlBefore;
  UINT32   EntryTap;
  UINT32   SampleDelayBefore;
  UINT32   NtsrBefore;
  UINT32   AttemptTap;
  UINT32   Ntsr;
  UINT32   ChosenDrvDl;
  UINT32   ChosenTap;
  UINT32   ChosenCsdc;
  UINT32   ChosenWriteUs;
  UINT32   Phase;
  UINT32   Index;
  UINT32   Count;
  UINT32   Planned;
  UINT32   Tried;
  UINT32   Csdc;
  UINT32   Tap;
  UINT32   WriteUs;
  UINT32   ShapeUs;
  UINT32   AbUs;
  UINT32   LastWord;
  UINT32   LastRint;
  UINT32   KibPerSec;
  UINT32   Run;
  UINT32   RunFirst;
  UINT32   Best;
  UINT32   BestFirst;
  UINT32   BestLast;
  UINT32   Passes;
  UINT32   TicksPerMicro;
  UINT32   SweepUs;
  UINT64   Frequency;
  UINT64   SweepStart;
  BOOLEAN  EntryPassed;
  BOOLEAN  Quiet;
  BOOLEAN  BusProven;
  BOOLEAN  Stopped;
  BOOLEAN  CutShort;
  BOOLEAN  Passed;
  BOOLEAN  Found;

  Count             = A733_SMHC_SAMP_DELAY_TAPS / A733_SMHC_SAMP_DELAY_STEP;
  DrvDlBefore       = SmhcRead (Private->Base, A733_SMHC_DRV_DL);
  SampDlBefore      = SmhcRead (Private->Base, A733_SMHC_SAMP_DL);
  NtsrBefore        = SmhcRead (Private->Base, A733_SMHC_NTSR);
  EntryTap          = SampDlBefore & A733_SMHC_SAMP_DL_SW_MASK;
  SampleDelayBefore = Private->SampleDelay;
  ChosenDrvDl       = DrvDlBefore;
  ChosenTap         = EntryTap;
  ChosenCsdc        = SmhcRead (Private->Base, A733_SMHC_CSDC);
  ChosenWriteUs     = 0;
  Planned           = A733_SMHC_HS400_WRITE_DRV_DL_COUNT * Count;
  Tried             = 0;
  LastWord          = 0;
  LastRint          = 0;
  BusProven         = TRUE;
  Stopped           = FALSE;
  CutShort          = FALSE;
  EntryPassed       = FALSE;
  Found             = FALSE;

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (UINT32)(Frequency / 1000000U);
  SweepStart    = GetPerformanceCounter ();
  SweepUs       = 0;

  SMHC_DIAG ((
    DEBUG_INFO,
    "Smhc: HS400 write sweep: the current setting first, then drv_dl %08x and %08x with samp_dl 0..%u step %u at each, "
    "the centre of the widest run of passes kept rather than the first pass; the criterion is a 4 KiB write of LBA 0's own "
    "bytes back to LBA 0 with a matching read-back, every setting tried after a failed one waits for the card to answer "
    "from TRAN and for a 512-byte read of LBA 0 to come back first, and the whole sweep stops after %u ms\n",
    mSmhcHs400WriteDrvDl[0],
    mSmhcHs400WriteDrvDl[1],
    A733_SMHC_SAMP_DELAY_TAPS - A733_SMHC_SAMP_DELAY_STEP,
    A733_SMHC_SAMP_DELAY_STEP,
    A733_SMHC_HS400_WRITE_SWEEP_BUDGET_US / 1000U
    ));

  AttemptTap  = EntryTap;
  Found       = SmhcCardWriteBackTest (Private, Reference, A733_SMHC_SAMP_DELAY_PROBE_BYTES, &WriteUs, &BusProven, &LastWord, &LastRint);
  EntryPassed = Found;
  Tried++;

  if (Found) {
    ChosenDrvDl   = DrvDlBefore;
    ChosenTap     = AttemptTap;
    ChosenWriteUs = WriteUs;
  }

  if (!BusProven) {
    Stopped = TRUE;
  }

  //
  // The boot's own shape, once: one 512-byte block from LBA 0, which is what the
  // GPT repair's CMD25 moves.  It is reported and not used as the criterion - the
  // 4 KiB transfer is the stricter one - but the pair is what tells a mode that
  // cannot write at all from one that cannot write more than a block.
  //
  if (!Stopped) {
    SMHC_DIAG ((
      DEBUG_INFO,
      "Smhc: HS400 write sweep: the single-block shape the boot's own CMD25 uses, for comparison\n"
      ));

    SmhcCardWriteBackTest (Private, Reference, A733_SMHC_BLOCK_SIZE, &ShapeUs, &BusProven, &LastWord, &LastRint);

    if (!BusProven) {
      Stopped = TRUE;
    }
  }

  //
  // NTSR's mode bit, as one A/B before the full sweep.  It is not part of the two
  // dimensions the sweep exists for, and the port never programmed it for HS400:
  // it inherits bit 31 = 0 (1x) from the DDR step while the module clock is 400
  // MHz against a 200 MHz card clock.  2x is the other value the manual's mode
  // field allows, and a write is the only thing that can tell - a read is sampled
  // off the card's strobe in this mode and does not use it.
  //
  if (!Found && !Stopped) {
    SMHC_DIAG ((
      DEBUG_INFO,
      "Smhc: HS400 write sweep: NTSR's mode bit as a single A/B first - the port inherits bit 31 = 0 (1x) from the DDR "
      "step while the module clock is %u kHz against a %u kHz card clock, i.e. 2x the card clock; bit 31 = 1 is the "
      "manual's other value and reads are strobe-sampled here, so the write is what decides it\n",
      Private->ModuleClockHz / 1000U,
      SmhcClockKhz (Private)
      ));

    for (Index = 0; (Index < 2) && !Stopped; Index++) {
      //
      // The settle first: it runs the controller reset a failed read left behind,
      // and that reset re-programs NTSR from the DDR state.  The NTSR write has to
      // be the last thing before the attempt for the attempt to run with it.
      //
      SmhcCardSettleAfterFault (Private);
      Ntsr = SmhcRead (Private->Base, A733_SMHC_NTSR);
      if (Index == 0) {
        Ntsr &= ~A733_SMHC_NTSR_2X_TIMING_MODE;
      } else {
        Ntsr |= A733_SMHC_NTSR_2X_TIMING_MODE;
      }

      SmhcWrite (Private->Base, A733_SMHC_NTSR, Ntsr);

      Passed = SmhcCardWriteBackTest (
                 Private,
                 Reference,
                 A733_SMHC_SAMP_DELAY_PROBE_BYTES,
                 &AbUs,
                 &BusProven,
                 &LastWord,
                 &LastRint
                 );

      SMHC_DIAG ((
        Passed ? DEBUG_INFO : DEBUG_INFO,
        "Smhc: HS400 write sweep: NTSR bit 31 = %u (%a timing, ntsr %08x read back) with drv_dl %08x samp_dl %08x: the "
        "4 KiB write-back %a in %u us; bus %a\n",
        Index,
        (Index == 0) ? "1x" : "2x",
        SmhcRead (Private->Base, A733_SMHC_NTSR),
        SmhcRead (Private->Base, A733_SMHC_DRV_DL),
        SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
        Passed ? "passed" : "did not pass",
        AbUs,
        BusProven ? "proved for the next setting" : "dead - the sweep stops here"
        ));

      if (!BusProven) {
        Stopped = TRUE;
      }
    }

    SmhcWrite (Private->Base, A733_SMHC_NTSR, NtsrBefore);

    SMHC_DIAG ((
      DEBUG_INFO,
      "Smhc: HS400 write sweep: ntsr %08x put back after the A/B (bit 31 = %u), the 1x timing the DDR step left; the "
      "sweep continues on it\n",
      SmhcRead (Private->Base, A733_SMHC_NTSR),
      ((NtsrBefore & A733_SMHC_NTSR_2X_TIMING_MODE) != 0) ? 1U : 0U
      ));
  }

  //
  // The two dimensions that are actually calibrated, and the reason this phase
  // does not stop at its first pass: a tap at the edge of the window passes today
  // and fails the moment temperature or the card's own timing moves, so what is
  // kept is the centre of the longest run of passes - the tap furthest from both
  // edges - exactly as the read-side sweeps choose theirs.  The entry setting is
  // tried before any of this by the same criterion, so a boot that enters on a
  // good tap never reaches the grid at all and keeps what it entered with.
  //
  for (Phase = 0; (Phase < A733_SMHC_HS400_WRITE_DRV_DL_COUNT) && !Found && !Stopped && !CutShort; Phase++) {
    Quiet = Private->Quiet;

    Best      = 0;
    BestFirst = 0;
    BestLast  = 0;
    Run       = 0;
    RunFirst  = 0;
    Passes    = 0;

    for (Index = 0; (Index < Count) && !Stopped; Index++) {
      Tap = Index * A733_SMHC_SAMP_DELAY_STEP;

      //
      // How long the sweep has spent so far, checked before each setting rather
      // than after it: the budget exists to bound the boot, and a setting started
      // with it already gone is one whose verdict would arrive after it.
      //
      SweepUs = (UINT32)((GetPerformanceCounter () - SweepStart) / TicksPerMicro);
      if (SweepUs >= A733_SMHC_HS400_WRITE_SWEEP_BUDGET_US) {
        CutShort = TRUE;
        DEBUG ((
          DEBUG_INFO,
          "Smhc: HS400 write sweep: %u us spent and the sweep's %u ms budget is gone at drv_dl %08x tap %u, so the sweep "
          "stops here; %u of %u planned settings were measured, and the setting entered with is kept\n",
          SweepUs,
          A733_SMHC_HS400_WRITE_SWEEP_BUDGET_US / 1000U,
          mSmhcHs400WriteDrvDl[Phase],
          Tap,
          Tried,
          Planned
          ));
        break;
      }

      if ((Tap == EntryTap) && (mSmhcHs400WriteDrvDl[Phase] == DrvDlBefore)) {
        //
        // The setting the boot entered with, at the output phase it entered
        // with: the first attempt measured it by this same criterion, so its
        // verdict is this grid point's verdict and retrying it would only spend
        // the bus.  At the other output phase it is a different setting and is
        // measured like every other grid point.
        //
        Passed = EntryPassed;
      } else {
        Private->Quiet = TRUE;
        SmhcCardSettleAfterFault (Private);
        SmhcWrite (Private->Base, A733_SMHC_DRV_DL, mSmhcHs400WriteDrvDl[Phase]);
        SmhcSetSampleDelay (Private->Base, Tap);
        Private->SampleDelay = Tap;

        Passed         = SmhcCardWriteBackTest (
                           Private,
                           Reference,
                           A733_SMHC_SAMP_DELAY_PROBE_BYTES,
                           &WriteUs,
                           &BusProven,
                           &LastWord,
                           &LastRint
                           );
        Private->Quiet = Quiet;
        Tried++;
      }

      if (Passed) {
        Passes++;
        if (Run == 0) {
          RunFirst = Index;
        }

        Run++;
        if (Run > Best) {
          Best      = Run;
          BestFirst = RunFirst;
          BestLast  = Index;
        }
      } else {
        Run = 0;
      }

      if (!BusProven) {
        Stopped = TRUE;
      }
    }

    if ((Best == 0) || Stopped || CutShort) {
      continue;
    }

    //
    // The centre of the widest run, then the same test once more at it: the grid
    // measured those taps a moment ago, and this is what leaves the controller on
    // the chosen setting with a verdict that belongs to that setting and not to
    // its neighbour.
    //
    ChosenDrvDl = mSmhcHs400WriteDrvDl[Phase];
    ChosenTap   = ((BestFirst + BestLast) / 2U) * A733_SMHC_SAMP_DELAY_STEP;

    SmhcCardSettleAfterFault (Private);
    SmhcWrite (Private->Base, A733_SMHC_DRV_DL, ChosenDrvDl);
    SmhcSetSampleDelay (Private->Base, ChosenTap);
    Private->SampleDelay = ChosenTap;

    Passed = SmhcCardWriteBackTest (
               Private,
               Reference,
               A733_SMHC_SAMP_DELAY_PROBE_BYTES,
               &ChosenWriteUs,
               &BusProven,
               &LastWord,
               &LastRint
               );
    Tried++;

    DEBUG ((
      Passed ? DEBUG_INFO : DEBUG_INFO,
      "Smhc: HS400 write sweep: drv_dl %08x passed at %u of %u taps (window %u..%u, %u attempts), so the centre %u was kept "
      "and tried again: %a in %u us\n",
      ChosenDrvDl,
      Passes,
      Count,
      BestFirst * A733_SMHC_SAMP_DELAY_STEP,
      BestLast * A733_SMHC_SAMP_DELAY_STEP,
      Best,
      ChosenTap,
      Passed ? "it passed" : "it did not pass, so this output phase is not kept",
      ChosenWriteUs
      ));

    if (Passed) {
      Found = TRUE;
    } else if (!BusProven) {
      Stopped = TRUE;
    }
  }

  //
  // The third dimension, only if the two that are actually being calibrated wrote
  // nothing at all: CSDC's low field is the CRC-status token the controller
  // expects after a write.  The vendor writes 6 for HS400 (and this port enters
  // with 6), so values the card accepts that the vendor does not use would say the
  // token is not what this board needs.  Measured at the mode's own DRV_DL and
  // SAMP_DL, one dimension at a time, so a hit here cannot be a side effect of a
  // swept tap.
  //
  if (!Found && !Stopped && !CutShort) {
    SmhcCardSettleAfterFault (Private);
    SmhcWrite (Private->Base, A733_SMHC_DRV_DL, DrvDlBefore);
    SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDlBefore);
    Private->SampleDelay = SampleDelayBefore;

    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 write sweep: neither output phase at any samp_dl tap writes, so csdc - the CRC-status token value the "
      "controller detects after a write, 6 for HS400 and 3 for every other mode, and 6 unverified on this board - is swept "
      "over its whole field 0..%u at drv_dl %08x samp_dl %08x\n",
      A733_SMHC_HS400_CSDC_COUNT - 1,
      DrvDlBefore,
      SampDlBefore
      ));

    for (Csdc = 0; (Csdc < A733_SMHC_HS400_CSDC_COUNT) && !Found && !Stopped; Csdc++) {
      SweepUs = (UINT32)((GetPerformanceCounter () - SweepStart) / TicksPerMicro);
      if (SweepUs >= A733_SMHC_HS400_WRITE_SWEEP_BUDGET_US) {
        CutShort = TRUE;
        DEBUG ((
          DEBUG_INFO,
          "Smhc: HS400 write sweep: %u us spent and the sweep's %u ms budget is gone at csdc %u, so the sweep stops here; "
          "%u of %u planned settings were measured, and the setting entered with is kept\n",
          SweepUs,
          A733_SMHC_HS400_WRITE_SWEEP_BUDGET_US / 1000U,
          Csdc,
          Tried,
          Planned
          ));
        break;
      }

      SmhcCardSettleAfterFault (Private);
      SmhcWrite (Private->Base, A733_SMHC_CSDC, Csdc);

      Passed = SmhcCardWriteBackTest (
                 Private,
                 Reference,
                 A733_SMHC_SAMP_DELAY_PROBE_BYTES,
                 &WriteUs,
                 &BusProven,
                 &LastWord,
                 &LastRint
                 );

      SMHC_DIAG ((
        Passed ? DEBUG_INFO : DEBUG_INFO,
        "Smhc: HS400 write sweep: csdc %u (csdc %08x read back) at drv_dl %08x samp_dl %08x: the 4 KiB write-back %a in "
        "%u us; bus %a\n",
        Csdc,
        SmhcRead (Private->Base, A733_SMHC_CSDC),
        SmhcRead (Private->Base, A733_SMHC_DRV_DL),
        SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
        Passed ? "passed" : "did not pass",
        WriteUs,
        BusProven ? "proved for the next setting" : "dead - the sweep stops here"
        ));

      if (Passed) {
        Found         = TRUE;
        ChosenCsdc    = Csdc;
        ChosenDrvDl   = DrvDlBefore;
        ChosenTap     = EntryTap;
        ChosenWriteUs = WriteUs;
      }

      if (!BusProven) {
        Stopped = TRUE;
      }
    }

    if (!Found) {
      SmhcWrite (Private->Base, A733_SMHC_CSDC, A733_SMHC_CSDC_HS400);
    }
  }

  if (Found) {
    //
    // SAMP_DL is the read path's sampler for the command line at HS400, so the
    // setting the write kept has to read as well as write before it stays.
    //
    Found = SmhcCardVerifyStrobeTap (Private, Reference);
    if (!Found) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: HS400 write sweep: drv_dl %08x samp_dl %08x csdc %u writes but does not read the same data back\n",
        ChosenDrvDl,
        A733_SMHC_SAMP_DL_SW_EN | ChosenTap,
        ChosenCsdc
        ));
    }
  }

  if (!Found) {
    SmhcCardSettleAfterFault (Private);
    SmhcWrite (Private->Base, A733_SMHC_DRV_DL, DrvDlBefore);
    SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDlBefore);
    SmhcWrite (Private->Base, A733_SMHC_CSDC, A733_SMHC_CSDC_HS400);
    Private->SampleDelay = SampleDelayBefore;

    if (Stopped) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: HS400 write sweep: the sweep stops because the card could not be brought back (last CMD13 word %08x: state %u, "
        "ready %u; last rint %08x); %u of %u planned drv_dl x samp_dl settings had been measured by then, so the rest - and any "
        "csdc value or mode-bit case not reached - were never measured and this log says nothing about them\n",
        LastWord,
        (LastWord >> A733_SMHC_R1_STATE_SHIFT) & 0xFU,
        (LastWord & A733_SMHC_R1_READY_FOR_DATA) ? 1U : 0U,
        LastRint,
        Tried,
        Planned
        ));
    }

    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 write sweep: %a (%u of %u planned settings measured, each on a bus a 512-byte read had just proved); "
      "drv_dl %08x and samp_dl %08x put back, so the mode is left exactly as it was - driven at drv_dl %08x, sampled at "
      "ds_dl %08x and samp_dl %08x, csdc %08x, ntsr %08x, ctrl %08x\n",
      Stopped ? "no setting both wrote a block and read it back among those measured, and the sweep was cut short - not a "
                "verdict on the settings it never reached"
              : CutShort ? "no setting both wrote a block and read it back among those measured, and the sweep stopped at its "
                           "time budget - not a verdict on the settings it never reached"
              : "no setting both wrote a block and read it back - every planned setting was measured with the bus proved "
                "first, so this is a verdict on the mode, not on the measurement",
      Tried,
      Planned,
      DrvDlBefore,
      SampDlBefore,
      SmhcRead (Private->Base, A733_SMHC_DRV_DL),
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      SmhcRead (Private->Base, A733_SMHC_CSDC),
      SmhcRead (Private->Base, A733_SMHC_NTSR),
      SmhcRead (Private->Base, A733_SMHC_CTRL)
      ));

    return FALSE;
  }

  KibPerSec = (ChosenWriteUs == 0) ? 0U
             : (UINT32)(((UINT64)A733_SMHC_SAMP_DELAY_PROBE_BYTES / 1024U) * 1000000U / ChosenWriteUs);
  SweepUs   = (UINT32)((GetPerformanceCounter () - SweepStart) / TicksPerMicro);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: HS400 write sweep: %u settings tried in %u us%a, kept drv_dl %08x samp_dl %08x csdc %u - 4 KiB written and read "
    "back in %u us (%u KiB/s); ds_dl %08x (tap %u), ntsr %08x, ctrl %08x\n",
    Tried,
    SweepUs,
    CutShort ? " before the time budget stopped it" : "",
    ChosenDrvDl,
    A733_SMHC_SAMP_DL_SW_EN | ChosenTap,
    ChosenCsdc,
    ChosenWriteUs,
    KibPerSec,
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    Private->StrobeDelay,
    SmhcRead (Private->Base, A733_SMHC_NTSR),
    SmhcRead (Private->Base, A733_SMHC_CTRL)
    ));

  //
  // A csdc other than the mode's own value is a finding about this board, and one
  // no later recovery keeps: SmhcSetHs400Mode writes 6 every time it is called.
  // The search that follows therefore still runs on 6 unless this value is what
  // the mode is changed to, which is the next round's decision, not this step's.
  //
  if (ChosenCsdc != A733_SMHC_CSDC_HS400) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 write sweep: csdc %u wrote where the mode's own 6 does not - the CRC-status token is the finding here, "
      "and SmhcSetHs400Mode still programmes 6, so this setting lasts only until the next recovery\n",
      ChosenCsdc
      ));
  }

  return TRUE;
}

/**
  Report the one line an HS400 boot leaves behind, with the rate it measured.

  The shape is the HS200 line's, because the two are read against each other: the
  state the card ended in, the tap behind it, and the rate that state produced.
  The rate quoted is the marginal one - the bytes the 256 KiB read adds over the
  32 KiB one, which is 224 KiB, divided by the time it adds - because that is the
  only figure that describes the bus rather than the fixed cost of a transfer;
  the large read's absolute figure stands in when the two times cannot be told
  apart, and the line says which was used.

  @param[in] Private  Driver state; Hs400 must be set.
  @param[in] Rate     The measurement the step just took.
**/
STATIC
VOID
SmhcCardReportHs400Rate (
  IN A733_SMHC_PRIVATE  *Private,
  IN A733_SMHC_RATE     *Rate
  )
{
  UINT32  KibPerSec;
  UINT32  CeilingKib;
  UINT32  Percent;
  UINT32  Khz;

  Khz = SmhcClockKhz (Private);

  //
  // An 8-bit bus at 200 MHz single edge carries Khz * 1000 * 8 / 8 bytes a second;
  // HS400 drives and samples on both edges, so its ceiling is twice that, in KiB/s
  // Khz * 1000 * 2 / 1024.
  //
  CeilingKib = (UINT32)(((UINT64)Khz * 2000U) / 1024U);

  if (Rate->MarginalValid) {
    KibPerSec = Rate->MarginalKibPerSec;
  } else {
    KibPerSec = Rate->BigKibPerSec;
  }

  if (KibPerSec == 0) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400: %u kHz, HS_TIMING %u, BUS_WIDTH %u, ds_dl tap %u (0x%08x), samp_dl %08x, throughput not measured %a\n",
      Khz,
      Private->HsTiming,
      A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR,
      Private->StrobeDelay,
      SmhcRead (Private->Base, A733_SMHC_DS_DL),
      SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
      Rate->MarginalValid ? "marginal" : "absolute"
      ));

    return;
  }

  Percent = (CeilingKib == 0) ? 0 : (UINT32)(((UINT64)KibPerSec * 100U) / CeilingKib);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: HS400: %u kHz, HS_TIMING %u, BUS_WIDTH %u, ds_dl tap %u (0x%08x), samp_dl %08x, throughput %u KiB/s %a "
    "(%u %% of the %u KiB/s an 8-bit double-edge bus at that clock carries%a)\n",
    Khz,
    Private->HsTiming,
    A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR,
    Private->StrobeDelay,
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    KibPerSec,
    Rate->MarginalValid ? "marginal" : "absolute",
    Percent,
    CeilingKib,
    (Percent >= A733_SMHC_HS400_HEALTHY_PERCENT) ? "" : ", below the expected rate"
    ));
}

/**
  Try the card's HS400 timing, before HS200.

  HS400 is the fastest mode this card lists: EXT_CSD[196] reads 0x57, whose bit 6
  is HS400 at 1.8 V, and the board's eMMC I/O supply is already 1.8 V, so nothing
  has to be switched for it.  It is 8 bits with a clock on both edges at 200 MHz,
  i.e. twice what HS200 moves over the same wires, and - unlike HS200 - its data
  is sampled against the strobe the card returns rather than against the host's
  own clock, which is why it needs a tap of its own on SMHC_DS_DL.

  The mode is entered the way the spec and the vendor's driver enter it, and each
  step is a card-side or host-side switch that has to be in place before the next:

  1. the card goes through 8-bit DDR first, HS_TIMING 1 and then BUS_WIDTH 6, at
     the clock it is already running at (50 MHz, the SDR plan's data divider);
  2. HS_TIMING 3 with BUS_WIDTH still 6, which is HS400 on the card's side;
  3. the host's half: the DDR framing the DDR step owns (NTSR 1x, both DRV_DL
     output phases, CTRL's DDR bit), then HS400's own fields - SMHC_EDSD bit 31,
     SMHC_CSDC's CRC-status token 6 and SMHC_SFC bit 0 clear, i.e. the sample FIFO
     *in* circuit, the opposite of the DDR and HS200 sweeps;
  4. the module clock to the plan HS200 uses, because 200 MHz on the card is the
     same 800 MHz module at divider 1 - no new rate is introduced;
  5. the DS_DL sweep, at that clock, one read per tap, compared against a copy of
     LBA 0 read on the SDR bus before any of this;
  6. the read-backs: a second data-compared read at the chosen tap, then the
     card's EXT_CSD, whose bytes 185 and 183 must read 3 and 6;
  7. the write side, which no read calibrates: LBA 0's own bytes written back to
     LBA 0 and read again, and if that fails, SMHC_DRV_DL's data output phase and
     SMHC_SAMP_DL swept with that test as the criterion (SmhcCardSweepWriteTiming).

  A failure in 7 does not undo the mode: the writes it reports on are evidence for
  the next round, and the registers go back to the values 6 left them at.

  Any failure takes everything back to the state the HS200 step's failure path
  leaves - 8-bit SDR, HS_TIMING 1, the SDR plan, the sample delay, DS_DL and SFC
  the SDR path had, EDSD bit 31 clear and CSDC back to 3 - and proves it with a
  read.  The revert itself is logged, register by register.

  The driver reports success by its state and by one rate line: Private->Hs400 is
  what the entry point's tail and the ladder below it read.

  @param[in,out] Private  Driver state; the card must be in 8-bit SDR and must not
                         be in DDR.
  @param[in,out] ExtCsd   The EXT_CSD read of the SDR stage; refreshed by the
                         step's own read-backs.

  @retval EFI_SUCCESS      The card is in HS400 and the driver is on the HS400 plan.
  @retval EFI_UNSUPPORTED  The card does not list HS400 (a clean skip).
  @retval other            The switch failed and both sides were reverted.
**/
STATIC
EFI_STATUS
SmhcCardTryHs400 (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN OUT A733_SMHC_EXT_CSD  *ExtCsd
  )
{
  EFI_STATUS         Status;
  EFI_STATUS         PlanStatus;
  EFI_STATUS         RefStatus;
  A733_SMHC_EXT_CSD  AtSpeed;
  A733_SMHC_RATE     Rate;
  UINT32             SdrPlan;
  UINT32             EdsdBefore;
  UINT32             CsdcBefore;
  UINT32             SfcBefore;
  UINT32             DsDlBefore;
  UINT32             SampDlBefore;
  UINT32             NtsrBefore;
  UINT32             DrvDlBefore;
  UINT8              HsTimingBefore;
  BOOLEAN            HighSpeedBefore;
  CONST UINT8        *Reference;
  CONST CHAR8        *Reason;

  if ((ExtCsd->DeviceType & A733_SMHC_DEVICE_TYPE_HS400) == 0) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 step skipped: EXT_CSD[196] device type %02x has no bit 6 (HS400 at 1.8 V)\n",
      ExtCsd->DeviceType
      ));

    return EFI_UNSUPPORTED;
  }

  //
  // HS400 is an 8-bit mode and its entry goes through the 8-bit DDR width, so a
  // bus that had to be narrowed above cannot take it: the width is the card's and
  // the wiring's, and a 4- or 1-bit bus at 200 MHz is neither what this step is for
  // nor what its revert would leave the FAT path on.
  //
  if (Private->BusWidth != A733_SMHC_BUS_8BIT) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: HS400 step skipped: the bus is %u-bit and HS400 is 8-bit only\n",
      Private->BusWidth
      ));

    return EFI_UNSUPPORTED;
  }

  SdrPlan          = Private->ClockPlan;
  HsTimingBefore   = Private->HsTiming;
  HighSpeedBefore  = Private->HighSpeed;
  Reason           = NULL;

  //
  // Everything this step changes and does not sweep, as it stands before the
  // first switch.  EDSD and CSDC are HS400-only; DS_DL and SFC are the SDR path's
  // and come back on any failure; the rest is evidence the log then shows changed.
  //
  EdsdBefore   = SmhcRead (Private->Base, A733_SMHC_EDSD);
  CsdcBefore   = SmhcRead (Private->Base, A733_SMHC_CSDC);
  SfcBefore    = SmhcRead (Private->Base, A733_SMHC_SFC);
  DsDlBefore   = SmhcRead (Private->Base, A733_SMHC_DS_DL);
  SampDlBefore = SmhcRead (Private->Base, A733_SMHC_SAMP_DL);
  NtsrBefore   = SmhcRead (Private->Base, A733_SMHC_NTSR);
  DrvDlBefore  = SmhcRead (Private->Base, A733_SMHC_DRV_DL);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: HS400 step: EXT_CSD[196] %02x lists HS400 (bit 6); entering with EXT_CSD[185] %u EXT_CSD[183] %u at "
    "%u kHz - edsd %08x csdc %08x sfc %08x ds_dl %08x samp_dl %08x ntsr %08x drv_dl %08x ctrl %08x\n",
    ExtCsd->DeviceType,
    ExtCsd->HsTiming,
    ExtCsd->BusWidth,
    SmhcClockKhz (Private),
    EdsdBefore,
    CsdcBefore,
    SfcBefore,
    DsDlBefore,
    SampDlBefore,
    NtsrBefore,
    DrvDlBefore,
    SmhcRead (Private->Base, A733_SMHC_CTRL)
    ));

  //
  // The data every read at 200 MHz is judged against, taken on the 50 MHz SDR bus
  // that reads correctly, through the same command shape every probe uses.
  //
  RefStatus = SmhcCardReadProbeBlocks (Private, mSmhcHs400Reference, A733_SMHC_SAMP_DELAY_PROBE_BYTES);
  Reference = EFI_ERROR (RefStatus) ? NULL : mSmhcHs400Reference;

  DEBUG ((
    DEBUG_INFO,
    "Smhc: HS400 step: SDR reference read of %u bytes from LBA 0 at %u kHz: %r, checksum %08x; "
    "the ds_dl sweep's criterion is %a\n",
    (UINT32)A733_SMHC_SAMP_DELAY_PROBE_BYTES,
    SmhcClockKhz (Private),
    RefStatus,
    (Reference == NULL) ? 0U : SmhcCardProbeChecksum (mSmhcHs400Reference),
    (Reference == NULL) ? "status only (no reference)" : "status and data"
    ));

  //
  // The card's three switches, in the order the mode is defined in: 8-bit DDR
  // first (HS_TIMING 1, then BUS_WIDTH 6), because HS400 is entered from the DDR
  // bus and not from SDR, and only then HS_TIMING 3 with the width untouched.
  //
  DEBUG ((
    DEBUG_INFO,
    "Smhc: HS400 step: switching the card - HS_TIMING %u, then BUS_WIDTH %u, then HS_TIMING %u - at %u kHz\n",
    A733_SMHC_HS_TIMING_HIGH_SPEED,
    A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR,
    A733_SMHC_HS_TIMING_HS400,
    SmhcClockKhz (Private)
    ));

  Status = SmhcCardSetHsTiming (Private, A733_SMHC_HS_TIMING_HIGH_SPEED, "hs400-ddr");
  if (EFI_ERROR (Status)) {
    Reason = "the HS_TIMING 1 switch (the DDR half of the HS400 entry) did not answer";
  } else {
    Status = SmhcCardSwitch (Private, A733_SMHC_EXT_CSD_BUS_WIDTH, A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR);
    if (EFI_ERROR (Status)) {
      Reason = "the 8-bit DDR bus-width switch did not answer";
    } else {
      Status = SmhcCardSetHsTiming (Private, A733_SMHC_HS_TIMING_HS400, "hs400");
      if (EFI_ERROR (Status)) {
        Reason = "the HS_TIMING 3 switch did not answer";
      } else {
        //
        // The card is in HS400 from the moment those commands answer, so the
        // driver records it before the first read: a failed read sets the
        // data-fault flag and the recovery that follows re-applies the driver's
        // idea of the bus, which has to be HS400 for any later tap to pass.
        //
        Private->HsTiming  = A733_SMHC_HS_TIMING_HS400;
        Private->HighSpeed = TRUE;
        Private->Ddr       = TRUE;

        //
        // The host's half, in two parts: the DDR framing the DDR step already
        // owns (NTSR 1x, both DRV_DL output phases, CTRL's DDR bit, and CSDC and
        // DS_DL with it), then the three fields HS400 replaces - EDSD bit 31,
        // CSDC 6, and the sample FIFO back in circuit - plus DS_DL's first tap
        // and the command line's.
        //
        Private->HostDdrTiming = TRUE;
        SmhcSetDdrMode (Private, TRUE);

        //
        // The command line's tap, which SmhcSetHs400Mode programs and a recovery
        // re-applies, is driver state: the vendor's initial value here, and the
        // write sweep's choice later.
        //
        Private->SampleDelay     = A733_SMHC_HS400_CMD_TAPS;
        Private->StrobeDelay     = A733_SMHC_HS400_DS_TAPS;
        Private->HostHs400Timing = TRUE;
        SmhcSetHs400Mode (Private, TRUE);

        DEBUG ((
          DEBUG_INFO,
          "Smhc: HS400 step: host switched - edsd %08x (md en %u) csdc %08x sfc %08x (bypass %u) ds_dl %08x "
          "samp_dl %08x ntsr %08x drv_dl %08x ctrl %08x (ddr bit %u); the sample FIFO stays in circuit, so there is "
          "no sweep bypass to clear this time\n",
          SmhcRead (Private->Base, A733_SMHC_EDSD),
          (UINT32)((SmhcRead (Private->Base, A733_SMHC_EDSD) & A733_SMHC_EDSD_HS400_MD_EN) != 0),
          SmhcRead (Private->Base, A733_SMHC_CSDC),
          SmhcRead (Private->Base, A733_SMHC_SFC),
          (UINT32)((SmhcRead (Private->Base, A733_SMHC_SFC) & A733_SMHC_SFC_BYPASS) != 0),
          SmhcRead (Private->Base, A733_SMHC_DS_DL),
          SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
          SmhcRead (Private->Base, A733_SMHC_NTSR),
          SmhcRead (Private->Base, A733_SMHC_DRV_DL),
          SmhcRead (Private->Base, A733_SMHC_CTRL),
          (UINT32)Private->DdrModeBit
          ));
      }
    }
  }

  //
  // The clock, now that the card can take it.  Deliberately no read here: at
  // 200 MHz the DS_DL tap in the register is the vendor's initial value and the
  // command line's tap was calibrated for 50 MHz, so a read would fail for the
  // reason the sweep exists; the plan switch has already proved the CCU by reading
  // its register back.
  //
  if (Reason == NULL) {
    PlanStatus = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HS400);
    if (EFI_ERROR (PlanStatus)) {
      Reason = "the HS400 plan could not be set";
    } else {
      SmhcSetClock (Private, Private->DataDivisor);

      //
      // Let the new clock and the HS400 timing settle before the FIRST traffic
      // on them.  The probe below is that traffic, and without this wait its
      // read comes back with a data CRC error and a data start error on a bus
      // that is not marginal - it is the mode switch that has not landed yet,
      // and the recovery that follows the failed probe leaves the state the DS
      // sweep then measures.  A traced boot never showed it because the trace
      // lines between the switch and the probe supplied the delay by accident;
      // with the trace masked every tap read fails and the mode arrives only
      // after enough failed attempts have piled up.
      //
      MicroSecondDelay (100000);

      SmhcProbeClock (Private, "at the HS400 plan");

      DEBUG ((
        DEBUG_INFO,
        "Smhc: HS400: card clock at the %a plan is %u kHz (module %u kHz, divider %u) - the same 200 MHz path the "
        "HS200 step uses; ds_dl %08x samp_dl %08x\n",
        SmhcClockPlanName (Private->ClockProfile, A733_SMHC_PLAN_HS400),
        SmhcClockKhz (Private),
        Private->ModuleClockHz / 1000U,
        Private->DataDivisor,
        SmhcRead (Private->Base, A733_SMHC_DS_DL),
        SmhcRead (Private->Base, A733_SMHC_SAMP_DL)
        ));
    }
  }

  //
  // The strobe tap, at the final clock.
  //
  if (Reason == NULL) {
    if (!SmhcCardSweepStrobeDelay (Private, "HS400", Reference)) {
      Reason = "no DS_DL tap read a block back";
    }
  }

  //
  // The read-backs: the data at the chosen tap, then the card's own statement of
  // its timing, which is the pair that separates a bus that works from a card that
  // never took the mode.
  //
  if (Reason == NULL) {
    if (!SmhcCardVerifyStrobeTap (Private, Reference)) {
      Reason = "the data-compared read at the chosen DS_DL tap did not match the reference";
    } else {
      //
      // One retry: the read-back is a single-block command where everything the
      // sweep proved is multi-block, so a single framing hiccup on it would throw
      // away a mode whose data path is good.  A card that refused the switch reads
      // back the same on both attempts, which is the answer that decides.
      //
      Status = SmhcCardReadExtCsd (Private, &AtSpeed);
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_INFO,
          "Smhc: HS400 step: the EXT_CSD read at ds_dl tap %u failed %r; retrying it once at the same tap\n",
          Private->StrobeDelay,
          Status
          ));

        Status = SmhcCardReadExtCsd (Private, &AtSpeed);
      }

      if (EFI_ERROR (Status)) {
        Reason = "the EXT_CSD read at the chosen DS_DL tap failed";
      } else {
        SmhcProbeHsTimingRead (Private, "after the HS400 switch (at the chosen ds_dl tap)", A733_SMHC_HS_TIMING_HS400, &AtSpeed);

        DEBUG ((
          DEBUG_INFO,
          "Smhc: HS400 step: at ds_dl tap %u the card reads EXT_CSD[185] %u and EXT_CSD[183] %u (expected %u and %u), "
          "EXT_CSD[196] %02x; edsd %08x csdc %08x sfc %08x ds_dl %08x samp_dl %08x ntsr %08x ctrl %08x\n",
          Private->StrobeDelay,
          AtSpeed.HsTiming,
          AtSpeed.BusWidth,
          A733_SMHC_HS_TIMING_HS400,
          A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR,
          AtSpeed.DeviceType,
          SmhcRead (Private->Base, A733_SMHC_EDSD),
          SmhcRead (Private->Base, A733_SMHC_CSDC),
          SmhcRead (Private->Base, A733_SMHC_SFC),
          SmhcRead (Private->Base, A733_SMHC_DS_DL),
          SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
          SmhcRead (Private->Base, A733_SMHC_NTSR),
          SmhcRead (Private->Base, A733_SMHC_CTRL)
          ));

        if ((AtSpeed.HsTiming != A733_SMHC_HS_TIMING_HS400) ||
            (AtSpeed.BusWidth != A733_SMHC_EXT_CSD_WIDTH_8BIT_DDR))
        {
          Reason = "the card's timing changed at the chosen DS_DL tap";
        }
      }
    }
  }

  if (Reason == NULL) {
    //
    // The write side, which no read calibrates: LBA 0's own bytes are written
    // back to LBA 0 and read again, and if the mode as entered cannot write, the
    // two host settings a write depends on - the DATA output phase and the
    // command line's tap - are swept with that test as the criterion.
    //
    // Nothing here decides whether the mode is taken: the reads and the card's
    // own read-back above already did, and a write failure leaves the mode and
    // every register as it was.  What the lines are for is the next round: which
    // setting writes, or that none does.
    //
    if (Reference == NULL) {
      DEBUG ((DEBUG_ERROR, "Smhc: HS400 write test: skipped, the SDR reference read it writes back did not happen\n"));
    } else {
      SmhcCardSweepWriteTiming (Private, Reference);
    }

    //
    // The sweep's last act is a 4 KiB write-back, and a card programming it can
    // stop answering commands for seconds - at HS400 the sweep's own readiness
    // poll budgets 200 ms and steps over a timeout by design.  Returning with
    // the card still busy hands the block layer a first read that times out for
    // a reason that has nothing to do with the bus or the taps: the partition
    // scan is that read, and with the boot's tracing masked it arrives before
    // the card is done.  Poll CMD13 until the card answers from TRAN with
    // READY_FOR_DATA set; a card that is already idle costs one command, and a
    // card that never becomes ready is logged rather than waited for forever.
    //
    {
      UINT32      Word;
      UINT32      Polls;
      EFI_STATUS  ReadyStatus;

      Word = 0;
      for (Polls = 0; Polls < A733_SMHC_CARD_READY_POLLS; Polls++) {
        ReadyStatus = SmhcCardSendStatusWord (Private, &Word);
        if (!EFI_ERROR (ReadyStatus) &&
            (((Word >> A733_SMHC_R1_STATE_SHIFT) & 0xFU) == A733_SMHC_R1_STATE_TRAN) &&
            ((Word & A733_SMHC_R1_READY_FOR_DATA) != 0))
        {
          break;
        }

        MicroSecondDelay (A733_SMHC_CARD_READY_POLL_US);
      }

      if (Polls != 0) {
        DEBUG ((
          EFI_ERROR (ReadyStatus) ? DEBUG_INFO : DEBUG_INFO,
          "Smhc: HS400 step: the card needed %u readiness poll(s) after the write sweep (%u ms); last word %08x (state %u, ready %u)\n",
          Polls,
          Polls,
          Word,
          (Word >> A733_SMHC_R1_STATE_SHIFT) & 0xFU,
          (UINT32)((Word & A733_SMHC_R1_READY_FOR_DATA) != 0)
          ));
      }

      if (Polls == A733_SMHC_CARD_READY_POLLS) {
        DEBUG ((
          DEBUG_INFO,
          "Smhc: HS400 step: the card still did not report READY_FOR_DATA after %u ms; the first block read may time out\n",
          (UINT32)(A733_SMHC_CARD_READY_POLLS * A733_SMHC_CARD_READY_POLL_US / 1000U)
          ));
      }
    }

    //
    // Success.  The driver stays on the HS400 plan from here: the strobe tap, the
    // divider and the width in its state are the ones the FAT traffic will use,
    // and the measurement below is what the mode is worth on this board.
    //
    Private->Hs400    = TRUE;
    Private->Hs200    = FALSE;
    Private->Ddr      = TRUE;
    Private->HsTiming = A733_SMHC_HS_TIMING_HS400;

    *ExtCsd = AtSpeed;

    if (EFI_ERROR (SmhcMeasureRate (Private, &Rate))) {
      DEBUG ((DEBUG_ERROR, "Smhc: HS400: the throughput measurement failed; the mode is kept on its read-back and tap\n"));
    } else {
      SmhcCardReportHs400Rate (Private, &Rate);
    }

    return EFI_SUCCESS;
  }

  //
  // Failure.  The revert has to leave what the HS200 step's failure path leaves,
  // and the order is the one the command line dictates: its tap back first, the
  // card clock down second (both while the card is still in HS400), because the
  // card has to be told to leave HS400 over a command line sampled the way the
  // 50 MHz clock will sample it - a CMD6 sent at 200 MHz with the SDR tap in the
  // register is what fails here if the order is the other way round.  Only then is
  // the host's HS400 set taken down and every delay register put back, and a read
  // proves the whole path afterwards.
  //
  DEBUG ((
    DEBUG_ERROR,
    "Smhc: HS400: not taken (%a); reverting - edsd %08x csdc %08x sfc %08x ds_dl %08x samp_dl %08x ntsr %08x "
    "drv_dl %08x ctrl %08x (ddr bit %u) on entry, to %u-bit SDR at HS_TIMING %u on the %a plan\n",
    Reason,
    EdsdBefore,
    CsdcBefore,
    SfcBefore,
    DsDlBefore,
    SampDlBefore,
    NtsrBefore,
    DrvDlBefore,
    SmhcRead (Private->Base, A733_SMHC_CTRL),
    (UINT32)Private->DdrModeBit,
    Private->BusWidth,
    HighSpeedBefore ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U,
    SmhcClockPlanName (Private->ClockProfile, SdrPlan)
    ));

  SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDlBefore);

  PlanStatus = SmhcSelectClockPlan (Private, SdrPlan);
  if (EFI_ERROR (PlanStatus)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot put the %a plan back: %r\n", SmhcClockPlanName (Private->ClockProfile, SdrPlan), PlanStatus));
  } else {
    SmhcSetClock (Private, Private->DataDivisor);
  }

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: HS400 revert: samp_dl %08x put back and the card clock brought to %u kHz on the %a plan; "
    "the card is switched back to HS_TIMING %u and BUS_WIDTH %u next\n",
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    SmhcClockKhz (Private),
    SmhcClockPlanName (Private->ClockProfile, Private->ClockPlan),
    HighSpeedBefore ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U,
    Private->BusWidth
    ));

  Status = SmhcCardSetHsTiming (Private, HighSpeedBefore ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U, "revert-hs400");
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the card did not take HS_TIMING %u back: %r\n", HighSpeedBefore ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U, Status));
  }

  Status = SmhcCardSwitch (Private, A733_SMHC_EXT_CSD_BUS_WIDTH, Private->BusWidth);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the card did not take BUS_WIDTH %u back: %r\n", Private->BusWidth, Status));
  }

  //
  // The host's side: the HS400 fields and the DDR framing out, then every delay
  // register to the value it held on entry.  SmhcSetDdrMode(FALSE) writes CSDC and
  // DRV_DL and NTSR itself, which is why the explicit writes follow it rather than
  // precede it.
  //
  Private->HostHs400Timing = FALSE;
  Private->HostDdrTiming   = FALSE;
  Private->Ddr             = FALSE;
  Private->Hs400           = FALSE;
  Private->HsTiming        = HsTimingBefore;
  Private->HighSpeed       = HighSpeedBefore;

  SmhcSetHs400Mode (Private, FALSE);
  SmhcSetDdrMode (Private, FALSE);

  SmhcWrite (Private->Base, A733_SMHC_EDSD, EdsdBefore);
  SmhcWrite (Private->Base, A733_SMHC_CSDC, CsdcBefore);
  SmhcWrite (Private->Base, A733_SMHC_NTSR, NtsrBefore);
  SmhcWrite (Private->Base, A733_SMHC_DRV_DL, DrvDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_SAMP_DL, SampDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_DS_DL, DsDlBefore);
  SmhcWrite (Private->Base, A733_SMHC_SFC, SfcBefore);

  Private->SampleDelay = SampDlBefore & A733_SMHC_SAMP_DL_SW_MASK;
  Private->StrobeDelay = DsDlBefore & A733_SMHC_DS_DL_SW_MASK;

  //
  // The card's timing is back to what it was but the read that proves it is the
  // one that matters: EXT_CSD is a data command on the SDR bus the FAT path will
  // use.  A card that does not answer there ends on the baseline plan instead,
  // which is the one rate in the driver that is known exactly.
  //
  Status = SmhcCardReadExtCsd (Private, &AtSpeed);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer after the HS400 revert (%r); falling back to the HOSC plan\n", Status));

    PlanStatus = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HOSC);
    if (EFI_ERROR (PlanStatus)) {
      DEBUG ((DEBUG_ERROR, "Smhc: cannot fall back to the HOSC plan: %r\n", PlanStatus));
    } else {
      SmhcSetClock (Private, Private->DataDivisor);
      Private->HighSpeed = FALSE;

      if (EFI_ERROR (SmhcCardSetHighSpeed (Private, &AtSpeed, SmhcClockKhz (Private)))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer at the HOSC plan either\n"));
      }
    }
  } else {
    SmhcProbeHsTimingRead (Private, "after the HS400 revert", HighSpeedBefore ? A733_SMHC_HS_TIMING_HIGH_SPEED : 0U, &AtSpeed);

    if ((AtSpeed.HsTiming != A733_SMHC_HS_TIMING_HIGH_SPEED) && HighSpeedBefore) {
      //
      // Reading the timing back is not restoring it: a 50 MHz bus is only legal in
      // High Speed timing, so the card gets the switch again rather than the driver
      // recording what the failed attempt left.
      //
      if (EFI_ERROR (SmhcCardSetHighSpeed (Private, &AtSpeed, SmhcClockKhz (Private)))) {
        DEBUG ((DEBUG_ERROR, "Smhc: the card would not take HS_TIMING 1 back after the HS400 attempt\n"));
      }

      Status = SmhcCardReadExtCsd (Private, &AtSpeed);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer after HS_TIMING 1 was restored\n"));
      } else {
        SmhcProbeHsTimingRead (Private, "after the HS400 revert and HS_TIMING 1", A733_SMHC_HS_TIMING_HIGH_SPEED, &AtSpeed);
      }
    }

    if (!EFI_ERROR (Status)) {
      Private->HighSpeed = AtSpeed.HsTiming == A733_SMHC_HS_TIMING_HIGH_SPEED;
      Private->HsTiming  = AtSpeed.HsTiming;
      *ExtCsd            = AtSpeed;
    }
  }

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: HS400 revert done: EXT_CSD[185] %u EXT_CSD[183] %u read back at %u kHz, %u-bit, plan %a; "
    "edsd %08x (md en %u) csdc %08x sfc %08x ds_dl %08x samp_dl %08x ntsr %08x drv_dl %08x ctrl %08x (ddr bit %u)\n",
    AtSpeed.HsTiming,
    AtSpeed.BusWidth,
    SmhcClockKhz (Private),
    Private->BusWidth,
    SmhcClockPlanName (Private->ClockProfile, Private->ClockPlan),
    SmhcRead (Private->Base, A733_SMHC_EDSD),
    (UINT32)((SmhcRead (Private->Base, A733_SMHC_EDSD) & A733_SMHC_EDSD_HS400_MD_EN) != 0),
    SmhcRead (Private->Base, A733_SMHC_CSDC),
    SmhcRead (Private->Base, A733_SMHC_SFC),
    SmhcRead (Private->Base, A733_SMHC_DS_DL),
    SmhcRead (Private->Base, A733_SMHC_SAMP_DL),
    SmhcRead (Private->Base, A733_SMHC_NTSR),
    SmhcRead (Private->Base, A733_SMHC_DRV_DL),
    SmhcRead (Private->Base, A733_SMHC_CTRL),
    (UINT32)Private->DdrModeBit
    ));

  return EFI_DEVICE_ERROR;
}

/**
  Widen the bus and speed it up as far as the card and this controller allow.

  Runs at the identification clock and leaves the card at the data clock.  The
  first thing it does is raise the card clock to the plan's *bring-up* divider,
  and that is the order the board settled: the EXT_CSD read is a data command, a
  plan whose bring-up divider keeps the module clock at a small multiple of the
  card clock is the only configuration a data transfer is known to work in on
  this hardware (see A733SmhcDxe.h), and the identification divider is far too
  large a ratio for one.  The bring-up clock is also legal while the card is
  still in compatibility timing (whose limit is 26 MHz), which is what makes the
  CMD6 switches below spec-correct at that clock; only then is the clock raised
  to the at-speed divider, which the card's High Speed timing allows.

  The steps after that are measured, and each one is logged with the number it
  produced:

  - a read of EXT_CSD at the at-speed clock, which is what confirms the timing
    still holds at speed (a card left in compatibility timing cannot answer a
    data command at 50 MHz, and a bus that is wrong answers with a CRC error);
  - the SDR throughput measurement, which is also what the DDR step is gated on;
  - the DDR step, when the card lists DDR52 and the SDR bus measured the rate its
    plan claims.

  @param[in,out] Private  Driver state; BusWidth, HighSpeed and Ddr are filled
                         in, and the card clock ends at the plan's data divider.

  @retval EFI_SUCCESS       The fastest workable configuration is in use.
  @retval EFI_DEVICE_ERROR  The bus read back nothing at any width.
**/
STATIC
EFI_STATUS
SmhcCardWidenBus (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS         Status;
  A733_SMHC_EXT_CSD  ExtCsd;
  A733_SMHC_RATE     Rate;
  UINT32             Sectors;
  UINT32             Khz;
  BOOLEAN            Proven;
  BOOLEAN            OnChosenPlan;

  OnChosenPlan = TRUE;

  //
  // The first data transfer of the boot is the read below, so the card clock
  // goes to the bring-up divider *before* it: the identification divider is only
  // for the data-less sequence that just finished.
  //
  SmhcSetClock (Private, Private->BringUpDivisor);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: identified and selected; card clock raised to the bring-up divider %u (%u kHz) for the EXT_CSD read and the card's own configuration\n",
    Private->BringUpDivisor,
    SmhcClockKhz (Private)
    ));

  Status = SmhcCardReadExtCsd (Private, &ExtCsd);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: reading EXT_CSD failed: %r\n", Status));
    return Status;
  }

  Sectors = (UINT32)ExtCsd.SecCount[0] |
            ((UINT32)ExtCsd.SecCount[1] << 8) |
            ((UINT32)ExtCsd.SecCount[2] << 16) |
            ((UINT32)ExtCsd.SecCount[3] << 24);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: eMMC RCA %u, EXT_CSD rev %u, width support %02x, device type %02x, %u sectors\n",
    Private->Rca,
    ExtCsd.ExtCsdRev,
    ExtCsd.BusWidth,
    ExtCsd.DeviceType,
    Sectors
    ));

  //
  // Widen the bus, widest first, and keep the widest width that survives a
  // read.  The read is the test because a bus that is too wide shows up as a
  // data CRC error or a start-bit error - the card driving lines the host is
  // not sampling right - rather than as anything the driver could ask the card
  // about.
  //
  // The driver does not gate this on EXT_CSD[183] (printed above): that byte is
  // the card's bus-width mode, and which of its bits mean "supports 4/8 bits" is
  // not something this port could confirm against a second source, so nothing
  // here depends on interpreting it.  Widening blindly and keeping what reads
  // back needs no interpretation at all, and is what the in-tree SDHCI driver
  // and Linux's mmc core do.
  //
  Status = SmhcCardTryBusWidth (Private, A733_SMHC_BUS_8BIT, &ExtCsd);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the 8-bit bus failed to read back (%r), trying 4-bit\n", Status));

    Status = SmhcCardTryBusWidth (Private, A733_SMHC_BUS_4BIT, &ExtCsd);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the 4-bit bus failed to read back (%r), falling back to 1-bit\n", Status));

      Status = SmhcCardTryBusWidth (Private, A733_SMHC_BUS_1BIT, &ExtCsd);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Smhc: the 1-bit bus did not read EXT_CSD either: %r\n", Status));
        return EFI_DEVICE_ERROR;
      }
    }
  }

  //
  // High Speed (SDR at up to 52 MHz, no tuning, no 1.8 V) when the device lists
  // it in EXT_CSD[196] bit 1 - the same bit the in-tree SDHCI driver reads for
  // SdMmcMmcHsSdr.  The switch is made here, at the bring-up clock, and not after
  // the clock goes to the at-speed divider: the card is still in compatibility
  // timing, whose limit is 26 MHz, and the plan's bring-up divider is chosen to
  // stay under it.  HS200 and HS400 are deliberately not attempted - both need
  // tuning and a 1.8 V signalling switch, and neither is worth a wrong answer
  // about a FAT partition.
  //
  Private->HsTiming = 0;
  if ((ExtCsd.DeviceType & A733_SMHC_DEVICE_TYPE_HIGH_SPEED) != 0) {
    Status = SmhcCardSetHighSpeed (Private, &ExtCsd, SmhcClockKhz (Private));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the card is not in High Speed timing: %r\n", Status));
    }
  } else {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: EXT_CSD[196] device type %02x does not list High Speed (bit 1); the card stays in compatibility timing, "
      "which limits the bus to 26 MHz\n",
      ExtCsd.DeviceType
      ));
  }

  //
  // The device is addressed, selected and (when it can be) in High Speed timing,
  // so the card clock can go up to the plan's at-speed divider.  Nothing about
  // the card is taken on trust here: the read below is a data command at the new
  // clock, and a card left in compatibility timing - or a bus the clock is too
  // fast for - cannot answer it.
  //
  if (Private->DataDivisor != Private->BringUpDivisor) {
    SmhcSetClock (Private, Private->DataDivisor);
    Khz = SmhcClockKhz (Private);

    DEBUG ((
      DEBUG_INFO,
      "Smhc: on %a timing; card clock raised to the at-speed divider %u (%u kHz)\n",
      Private->HighSpeed ? "high-speed" : "compatibility",
      Private->DataDivisor,
      Khz
      ));
  } else {
    //
    // The plan's two dividers are the same, so the clock the EXT_CSD read above
    // ran on *is* the at-speed clock and no change is delivered: this is the
    // baseline plan, whose sequence is then exactly the one the data path was
    // verified at.
    //
    Khz = SmhcClockKhz (Private);
  }

  //
  // A controller whose timing registers were never calibrated cannot read a
  // block at this clock with the tap it starts with - the block errors at its
  // end bit while every command still works.  SMHC3 is exactly that case: boot0
  // only ever used sdc2, so nothing ever tuned SMHC3's sample delay.  Sweep it
  // here, before the read that is supposed to prove the bus, the same way the
  // DDR and HS200 steps do.
  //
  if (!SmhcCardSweepSampleDelay (Private, "SDR", NULL, FALSE)) {
    DEBUG ((DEBUG_ERROR, "Smhc: no sample-delay tap read a block back at %u kHz\n", Khz));
  }

  Status = SmhcCardReadExtCsd (Private, &ExtCsd);
  if (EFI_ERROR (Status)) {
    //
    // The clock the plan asked for is not one this card can work at.  The plan
    // the port was verified at is the fallback - the same 12 MHz the data path
    // ran at before any of this - and the card is only trusted after it has
    // answered a data command there, because the point of falling back is to end
    // on a bus that works rather than on one that might.
    //
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: the bus did not read back at %u kHz (%r); falling back to the HOSC plan\n",
      Khz,
      Status
      ));

    Status = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HOSC);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: cannot fall back to the HOSC plan: %r\n", Status));
      return Status;
    }

    SmhcSetClock (Private, Private->DataDivisor);
    Private->HighSpeed = FALSE;
    OnChosenPlan       = FALSE;

    Status = SmhcCardSetHighSpeed (Private, &ExtCsd, SmhcClockKhz (Private));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the card does not answer at the HOSC plan either: %r\n", Status));
      return Status;
    }

    Khz = SmhcClockKhz (Private);
  }

  DEBUG ((
    DEBUG_INFO,
    "Smhc: SDR stage: bus %u-bit, EXT_CSD[185] HS_TIMING %u, EXT_CSD[183] BUS_WIDTH %u read back at %u kHz\n",
    Private->BusWidth,
    ExtCsd.HsTiming,
    ExtCsd.BusWidth,
    Khz
    ));

  //
  // The multi-block shape probe.  It runs here, after the read above has proven
  // the at-speed bus and before the measurement, so the measurement that follows
  // also says whether the probe left a bus transfers work in.  It is diagnostic
  // only and decides nothing.
  //
  SmhcDiagMultiBlockShapes (Private);

  //
  // The SDR bus as it stands, measured at two sizes: the absolute rates are the
  // record, and the rate between the two sizes is what the gate judges, because
  // a single transfer's figure is dominated by the fixed cost it carries (the
  // per-command debug output alone is about 40 ms of console time).  With the
  // diagnostic dump on, the 12 MHz plan is measured as well, so that one boot
  // reports both the plan's rate and the baseline it is read against; the plan is
  // put back and verified with a read before anything is built on top of it.
  //
  // A measurement that cannot be taken is not a reason to lose the card: the
  // driver keeps the bus it has, says so, and skips the step that would have
  // needed the number.
  //
  Status = SmhcMeasureRate (Private, &Rate);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the SDR rate measurement failed (%r); no DDR step\n", Status));
    return EFI_SUCCESS;
  }

  if (PcdGetBool (PcdA733SmhcDumpEnable) && (Private->ClockPlan != A733_SMHC_PLAN_HOSC)) {
    if (EFI_ERROR (SmhcCardMeasureBaseline (Private))) {
      DEBUG ((DEBUG_ERROR, "Smhc: the SDR baseline measurement did not restore the plan; no DDR step\n"));
      Rate.MarginalValid = FALSE;   // the bus is not on the plan the driver chose any more
    }
  }

  Status = SmhcCardCheckSdrRate (Private, &Rate, &Proven);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "Smhc: no DDR step after the SDR rate check\n"));
    return EFI_SUCCESS;
  }

  if (!Proven) {
    DEBUG ((DEBUG_INFO, "Smhc: no DDR step: the SDR bus's marginal rate is not proven to be the plan's\n"));
    return EFI_SUCCESS;
  }

  //
  // HS400 first: it is the fastest mode the card lists (EXT_CSD[196] bit 6), and
  // on this board it needs no voltage switch - the eMMC's I/O supply is already
  // 1.8 V.  It runs exactly where the DDR step was eligible - the fast plan, with
  // its rate proven - and not otherwise: a bus the gate could not vouch for, or one
  // the driver had to rescue onto the baseline plan, is not one to push to 200 MHz.
  // Its own revert puts the card back in 8-bit SDR at the SDR plan, which is the
  // state the HS200 step below wants to start from, so the ladder reads
  // SDR -> HS400 -> HS200 -> DDR52 and each rung is left only if it failed or is
  // not offered.
  //
  if (Proven && (Private->ClockPlan == A733_SMHC_PLAN_DDR_FROM)) {
    Status = A733_SMHC_HS400_ENABLED ? SmhcCardTryHs400 (Private, &ExtCsd) : EFI_UNSUPPORTED;
    if (Status == EFI_UNSUPPORTED) {
      DEBUG ((DEBUG_INFO, "Smhc: no HS400 step: the card does not offer it\n"));
    } else if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the HS400 step failed and was reverted: %r\n", Status));
    }
  }

  //
  // HS200 next: single edge at 200 MHz is worth twice the DDR52 bus, and it is
  // the rung below HS400 on the same 200 MHz clock path.
  //
  if (Proven && (Private->ClockPlan == A733_SMHC_PLAN_DDR_FROM) && !Private->Hs400) {
    Status = SmhcCardTryHs200 (Private);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the HS200 step failed and was reverted: %r\n", Status));
    }
  }

  if (!OnChosenPlan || (Private->ClockPlan != A733_SMHC_PLAN_DDR_FROM)) {
    //
    // DDR turns a 50 MHz SDR bus into the 100 MB/s one the DDR52 mode is for; on
    // the baseline plan it would be a 24 MB/s bus, and on a plan the driver had
    // to fall back to it would be a step on top of a bus that is already not the
    // one the driver chose.  Neither is what this step is for, so it is skipped
    // and said.
    //
    DEBUG ((
      DEBUG_INFO,
      "Smhc: no DDR step: the bus is on the %a plan and the DDR step starts from the %a plan\n",
      SmhcClockPlanName (Private->ClockProfile, Private->ClockPlan),
      SmhcClockPlanName (Private->ClockProfile, A733_SMHC_PLAN_DDR_FROM)
      ));
  } else {
    //
    // PROBE: the DDR attempt leaves the card in HS_TIMING 2 while the host is
    // back on SDR, and every read after that times out - which makes the HS200
    // verification read stale buffer contents and look like a refusal.  Skip
    // DDR for this run so HS200 starts from a clean single-data-rate card.
    //
    Status = (!Private->Hs200 && !Private->Hs400 && A733_SMHC_DDR_ENABLED) ? SmhcCardTryDdr (Private, &ExtCsd) : EFI_UNSUPPORTED;
    if (Status == EFI_UNSUPPORTED) {
      //
      // Not a failure: the card does not offer DDR52, and the bus stays SDR.
      //
      DEBUG ((DEBUG_INFO, "Smhc: no DDR step: the card does not offer DDR52\n"));
    } else if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: the DDR step failed and was reverted: %r\n", Status));
    }
  }

  return EFI_SUCCESS;
}

/**
  Identify the card and take the bus as far as it will go.

  One wall-clock budget covers the whole phase that waits for the device to
  finish its power-up - the eMMC spec's own one second - and the round loop in
  SmhcCardIdentify spends it, re-initializing the controller between rounds so
  that a round which failed mid-command is not carried into the next one.  A
  failure ends in a dump of the controller and of what the diagnostic commands
  can still see, so the next run has something to read; a device that never
  answered CMD1 at all is reported as absent instead.

  @param[in,out] Private  Driver state; Rca, BusWidth, HighSpeed and
                          SectorAddressing are filled in.

  @retval EFI_SUCCESS       The card is selected and ready for data.
  @retval EFI_NO_MEDIA      The device did not answer as an eMMC device.
  @retval EFI_DEVICE_ERROR  A CMD6 or a verifying read failed.
  @retval EFI_TIMEOUT       A command did not complete.
**/
/**
  Identify the card and take the bus as far as it will go.
**/
EFI_STATUS
SmhcCardInit (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_DEADLINE  Budget;
  EFI_STATUS          Status;
  A733_SMHC_COMMAND   Command;

  Private->BusWidth        = A733_SMHC_BUS_1BIT;
  Private->HighSpeed       = FALSE;
  Private->HsTiming        = 0;
  Private->Ddr             = FALSE;
  Private->DdrModeBit      = FALSE;
  Private->Hs400           = FALSE;
  Private->HostHs400Timing = FALSE;

  SmhcArmDeadline (&Budget, A733_SMHC_IDENT_BUDGET_US);

  //
  // boot0 has just read its payload off this card, and an eMMC left in the boot
  // or transfer state can ignore everything until it is told to go back to
  // pre-idle: CMD0 with 0xFFFFFFFA is that request, where CMD0 with 0 is only
  // defined for a card that is already in a normal state.
  //
  SmhcInitCommand (&Command, A733_SMHC_CMD_GO_IDLE_STATE, 0xFFFFFFFAU, SdMmcCommandTypeBc, SdMmcResponseTypeR1);
  Command.SendInitSeq = TRUE;
  SmhcSendCommand (Private, &Command, A733_SMHC_IDENT_CMD_TIMEOUT_US);
  MicroSecondDelay (A733_SMHC_CMD0_SETTLE_US);

  Status = SmhcPrepareController (Private, Private->IdentDivisor);
  if (!EFI_ERROR (Status)) {
    //
    // The controller is now in the state its first command will be sent from,
    // so this is the point at which "is the interrupt enabled inside the
    // controller, and masked in where it should be" can be answered, and the
    // point at which the GIC's view of the same interrupt is worth printing
    // again (the first readback was before any of this was configured).
    //
    SmhcReportInterruptState (Private, "before the first identification command");

    Status = SmhcCardIdentify (Private, &Budget);
  }

  //
  // Identification runs at fOD, which the identification divider computes from
  // the module clock the plan selected - so a plan whose parent is not the rate
  // the CCU reports, or a divider that did not take, lands here as a card that
  // will not identify rather than as a mistimed bus.  The plan the port was
  // verified at (HOSC, 24 MHz) is then tried once: the card coming up slowly is
  // worth much more than a fast plan that does not come up at all, and the log
  // says which plan the run ended on.
  //
  if (EFI_ERROR (Status) && (Private->ClockPlan != A733_SMHC_PLAN_HOSC)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: identification failed on the %a plan (%r); retrying it on the HOSC plan\n",
      SmhcClockPlanName (Private->ClockProfile, Private->ClockPlan),
      Status
      ));

    SmhcArmDeadline (&Budget, A733_SMHC_IDENT_BUDGET_US);

    if (!EFI_ERROR (SmhcSelectClockPlan (Private, A733_SMHC_PLAN_HOSC))) {
      Status = SmhcPrepareController (Private, Private->IdentDivisor);
      if (!EFI_ERROR (Status)) {
        Status = SmhcCardIdentify (Private, &Budget);
      }
    }
  }

  if (EFI_ERROR (Status)) {
    if (Status == EFI_NO_MEDIA) {
      DEBUG ((DEBUG_INFO, "Smhc: no answer to CMD1 in %u us\n", A733_SMHC_IDENT_BUDGET_US));
    } else {
      DEBUG ((DEBUG_ERROR, "Smhc: identification failed: %r\n", Status));
      SmhcDumpState (Private);
    }

    return Status;
  }

  //
  // Everything from here on - the bus width, the timing and the clock itself -
  // is SmhcCardWidenBus's to arrange and to measure, and it makes the timing
  // switch before it raises the clock for that reason.
  //
  Status = SmhcCardWidenBus (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: widening the bus failed: %r\n", Status));
    SmhcDumpState (Private);
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "Smhc: card ready: %u-bit, %a timing, %u kHz card clock, %a bus\n",
    Private->BusWidth,
    Private->Hs400 ? "HS400" : (Private->Hs200 ? "HS200" : (Private->Ddr ? "DDR" : (Private->HighSpeed ? "high-speed" : "compatibility"))),
    SmhcClockKhz (Private),
    (Private->Ddr || Private->Hs400) ? "DDR" : "SDR"
    ));

  return EFI_SUCCESS;
}

/**
  Put the card back where a fresh identification can find it.

  Nothing the driver holds on the card's behalf survives a reset: the bus width
  and the timing are card state, and the card keeps its address only until it is
  reset.  So a device reset is the identification sequence from the top, which
  starts with CMD0 and the controller preparation, and which is also what makes
  ResetDevice cheap to get right.

  @param[in,out] Private  Driver state.

  @retval EFI_SUCCESS  The card is identified and selected again.
  @retval other        The card did not come back.
**/
EFI_STATUS
SmhcCardReset (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  Private->BusWidth   = A733_SMHC_BUS_1BIT;
  Private->HighSpeed  = FALSE;
  Private->CountArmed = FALSE;

  return SmhcCardInit (Private);
}