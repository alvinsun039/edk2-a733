/** @file
  The SD side of the Allwinner A733 SMHC driver: a card *slot* rather than a
  soldered device.

  A733SmhcCard.c identifies an eMMC and nothing else.  Its sequence starts with
  CMD0/CMD1/CMD2 and then walks the eMMC's own ladder - CMD6 writes of
  EXT_CSD[183], the DDR52 step, HS200, HS400 - every step of which is a question an
  SD card either does not answer or answers differently: an SD card is identified
  through CMD8, CMD55/ACMD41 and CMD2, its bus width is set by ACMD6, and it has
  no EXT_CSD at all.  A slot therefore needs its own path, and the board's table
  says which controllers have one (A733_SMHC_CONTROLLER's CardType).

  This file is that path, and it has three parts:

  - Identification (SmhcCardInitSd), which stops at CMD2.  That stopping point is
    the whole contract with the block layer above.  MdeModulePkg's SdDxe takes a
    card from the *ident* state it is left in: its DiscoverUserArea sends
    CMD7(0) to force stand-by, assigns the relative address itself with CMD3 and
    reads CSD/CID back with CMD9/CMD10.  A host that had also sent CMD3 would be
    addressing the card by an address the block driver never learns, so CMD3 is
    deliberately not sent here.

  - The bus the block layer never asks for (SmhcSdPrepareDataCommand).  SdDxe
    sends no CMD55, ACMD6 or CMD6 at all, so the four data lines and the High
    Speed timing are this driver's to arrange - and ACMD6 is addressed to the
    RCA, which exists only after SdDxe's own CMD3.  The switch therefore runs
    lazily, in front of the first data command that follows an addressed CMD7,
    and only ever once per controller.

  - The device reset a slot needs (SmhcCardResetSd), which is the same
    identification followed by the address and the bus mode the block driver
    left the card in.

  Nothing here reads EXT_CSD: an SD card has none, and every step above is a
  command of the SD physical layer instead.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>

#include "A733SmhcDxe.h"

//
// The SD physical layer's commands, arguments and answer fields.
//
// Four of the indices this file uses collide with eMMC commands A733Smhc.h
// already names (8, 41, 55, 58), and the names there describe the eMMC protocol:
// CMD8 there is SEND_EXT_CSD and here SEND_IF_COND, and one name for both would
// be wrong for one of the two.  They are therefore named here, next to the only
// code that issues them.  CMD6 (SWITCH) keeps the shared name: it is the switch
// command in both protocols, with a protocol-specific argument.
//
#define A733_SMHC_SD_CMD_SEND_IF_COND    8U
#define A733_SMHC_SD_CMD_READ_OCR        58U
#define A733_SMHC_SD_CMD_SEND_OP_COND    41U
#define A733_SMHC_SD_CMD_APP_CMD         55U
#define A733_SMHC_SD_ACMD_SET_BUS_WIDTH  6U

//
// CMD8's argument: the voltage window this board supplies in bits 11:8 (pattern
// 1, i.e. 2.7-3.6 V) and the check pattern 0xAA in bits 7:0.  A card that
// accepts the voltage echoes both back in R7, which is what makes 0x1AA below
// the question and the answer at once.
//
#define A733_SMHC_SD_IF_COND_ARG   0x000001AAU
#define A733_SMHC_SD_IF_COND_ECHO  0x000001AAU
#define A733_SMHC_SD_IF_COND_MASK  0x00000FFFU

//
// ACMD41's argument: HCS (bit 30) asks a v2 card to report high capacity, and
// bits 23:0 are the OCR voltage window the card may accept - 3.2-3.4 V.  That is
// the same window CMD1 offers an eMMC (A733_SMHC_OCR_ARG), stated separately
// because here bit 30 is a question rather than a request.
//
#define A733_SMHC_SD_ACMD41_ARG  0x40FF8000U

// OCR bit 31: the card has finished its power-up cycle.  It reads 0 while the
// card is still busy and is the only bit ACMD41 is polled for.
#define A733_SMHC_SD_OCR_READY  BIT31

// OCR bit 30: CCS, the card is high capacity and addresses data in 512-byte
// blocks rather than by byte.
#define A733_SMHC_SD_OCR_CCS  BIT30

//
// The card's own bound on its power-up cycle is one second, and it is asked
// every 10 ms rather than continuously: a card that has not finished ignores
// ACMD41's busy bit and answers, so the spacing is what keeps the poll from
// being a busy loop on the command bus.
//
#define A733_SMHC_SD_POWER_UP_BUDGET_US  1000000U
#define A733_SMHC_SD_POWER_UP_STEP_US    10000U

// ACMD6 takes the width as a number: 0 is one data line, 2 is four.
#define A733_SMHC_SD_WIDTH_1BIT  0U
#define A733_SMHC_SD_WIDTH_4BIT  2U

//
// CMD6 (SWITCH_FUNC) in SD mode is a data command: it answers with 64 bytes of
// switch status, which is where the function a card actually selected is read
// from.  Its argument names one function per group, 0xF meaning "leave this
// group alone"; group 1 (bits 3:0) is the bus speed mode, 0 default speed and 1
// High Speed.  0x80FFFFF1 is therefore "set group 1 to High Speed, change
// nothing else" - mode 1 in bit 31, the SD spec's own example.
//
#define A733_SMHC_SD_SWITCH_SET_HS       0x80FFFFF1U
#define A733_SMHC_SD_SWITCH_STATUS_SIZE  64U

//
// Byte 16 of that status, low nibble: the function group 1 is set to.  It reads 1
// only when the card really took High Speed.  A card without the function
// returns its current one there and the command still succeeds, which is why
// this byte decides and not the command's status.
//
#define A733_SMHC_SD_SWITCH_GROUP1_BYTE  16U
#define A733_SMHC_SD_SWITCH_GROUP1_MASK  0x0FU
#define A733_SMHC_SD_SWITCH_GROUP1_HS    1U

/**
  Fill in a command that carries no data.

  The sibling of SmhcInitCommand in A733SmhcCard.c, for the same reason the
  command names above are restated here: the two card protocols build their
  commands in their own files.

  @param[out] Command       Command to clear and fill in.
  @param[in]  Index         Command index.
  @param[in]  Argument      Command argument.
  @param[in]  CommandType   EFI_SD_MMC_COMMAND_TYPE.
  @param[in]  ResponseType  EFI_SD_MMC_RESPONSE_TYPE.
**/
STATIC
VOID
SmhcSdInitCommand (
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
  Identify the card in a slot, up to the point the block driver takes over.

  The sequence is the SD physical layer's, in its own order, and each step is
  here for a reason the eMMC path cannot supply:

  1. CMD0 with argument 0 and the controller's initialization sequence: 74
     clocks that put the card in idle.  An SD card defines no other CMD0
     argument (the eMMC's 0xFFFFFFFA means nothing here).
  2. CMD8 (SEND_IF_COND) with the board's voltage window and a check pattern.
     A v2 card echoes both back; a v1 card does not answer at all, which is not a
     failure - the sequence continues without it and the card is simply v1, with
     neither high capacity nor High Speed.  An answer that does not echo is a
     card this board cannot run (a voltage mismatch), and no later command would
     work either.  Which one it was is stated where it is learned (a timeout) and
     on the one line this function ends on.
  3. CMD55 + ACMD41 until the card reports its power-up finished (OCR bit 31
     set), with HCS set so a v2 card may declare itself high capacity.  Both are
     re-sent every round: the app-command state does not survive a card that is
     still powering up.
  4. CMD58 for the OCR the card settled on: bit 30 says whether it addresses data
     in 512-byte blocks.
  5. CMD2 for the CID, which is as far as this driver goes - see the file header
     for why the card is left in ident.

  The clock is the caller's: the entry point delivers the plan's identification
  divider before any of this runs, and only the controller itself is prepared
  here.

  @param[in,out] Private  Driver state; Rca is cleared, SectorAddressing and the
                          bus-width/timing state are set to what identification
                          leaves, and the bus mode is left owed to
                          SmhcSdPrepareDataCommand.

  @retval EFI_SUCCESS   The card is identified and in the ident state.
  @retval EFI_NO_MEDIA  No card, no answer, or a card this board cannot supply
                        the voltage for - the normal empty-slot path.
  @retval other         The CCU refused the SD profile's plan, or the controller
                        did not come out of reset; neither is about the card.
**/
EFI_STATUS
SmhcCardInitSd (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS          Status;
  A733_SMHC_COMMAND   Command;
  A733_SMHC_DEADLINE  Budget;
  UINT32              Ocr;
  BOOLEAN             HighCapacity;
  CONST CHAR8         *Version;

  Private->BusWidth         = A733_SMHC_BUS_1BIT;
  Private->HighSpeed        = FALSE;
  Private->SdSpeedDone      = FALSE;
  Private->Rca              = 0;
  Private->SectorAddressing = FALSE;

  //
  // The entry point applies the plan and the interrupt is already installed, so
  // what is missing is the controller itself: boot0 configured SMHC0 to read
  // this card, and only a controller reset clears its FIFO, its IDMAC and a
  // command FSM that a previous stage left parked.  It also delivers the plan's
  // identification divider again, which is the clock the whole sequence below
  // runs at.
  //
  Status = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_DEFAULT);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a cannot be put on the SD clock plan: %r\n",
      Private->Name,
      Status
      ));
    return Status;
  }

  Status = SmhcPrepareController (Private, Private->IdentDivisor);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a did not come out of the controller reset: %r\n",
      Private->Name,
      Status
      ));
    return Status;
  }

  //
  // CMD0.  The controller generates the 74 clocks through SEND_INIT_SEQ, and the
  // settle after it is the card's own time to act on the reset before the next
  // command is aimed at it.
  //
  SmhcSdInitCommand (&Command, A733_SMHC_CMD_GO_IDLE_STATE, 0, SdMmcCommandTypeBc, SdMmcResponseTypeR1);
  Command.SendInitSeq = TRUE;
  Status              = SmhcSendCommand (Private, &Command, A733_SMHC_IDENT_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "Smhc: %a: CMD0 did not complete (%r)\n", Private->Name, Status));
    return EFI_NO_MEDIA;
  }

  MicroSecondDelay (A733_SMHC_CMD0_SETTLE_US);

  //
  // CMD8: the interface condition.  R7 carries the voltage window and the check
  // pattern back in its low 12 bits, so the answer to "is this card v2" and to
  // "does it accept 3.3 V" is the same word.
  //
  SmhcSdInitCommand (
    &Command,
    A733_SMHC_SD_CMD_SEND_IF_COND,
    A733_SMHC_SD_IF_COND_ARG,
    SdMmcCommandTypeBcr,
    SdMmcResponseTypeR7
    );
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_IDENT_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    Version = "v1";
    DEBUG ((
      DEBUG_INFO,
      "Smhc: %a: CMD8 did not answer (%r), so the card is SD v1: no high capacity and no High Speed\n",
      Private->Name,
      Status
      ));

    //
    // A response timeout can leave the controller's command FSM parked
    // mid-command, and a v1 card has to be able to continue past the CMD8 it
    // never answers.  The controller is therefore put back on a fresh reset -
    // which does not touch the card, its hardware reset line is not driven by
    // this - before ACMD41 is aimed at it.
    //
    Status = SmhcPrepareController (Private, Private->IdentDivisor);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: %a did not come back after CMD8: %r\n", Private->Name, Status));
      return EFI_NO_MEDIA;
    }
  } else if ((Command.Response[0] & A733_SMHC_SD_IF_COND_MASK) != A733_SMHC_SD_IF_COND_ECHO) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a: CMD8 answered %08x instead of echoing 1AA: the card does not accept this board's voltage\n",
      Private->Name,
      Command.Response[0]
      ));
    return EFI_NO_MEDIA;
  } else {
    Version = "v2";
  }

  //
  // ACMD41 until OCR bit 31 says the card's power-up cycle is over.  The
  // argument's HCS bit is a question to a v2 card and ignored by a v1 one; the
  // OCR that comes back is not acted on here beyond the busy bit, because CMD58
  // is what reports the card's final answer.
  //
  SmhcArmDeadline (&Budget, A733_SMHC_SD_POWER_UP_BUDGET_US);
  Ocr = 0;

  for ( ; ; ) {
    SmhcSdInitCommand (&Command, A733_SMHC_SD_CMD_APP_CMD, 0, SdMmcCommandTypeAc, SdMmcResponseTypeR1);
    Status = SmhcSendCommand (Private, &Command, A733_SMHC_IDENT_CMD_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: %a: CMD55 did not answer (%r), so there is no card in the slot\n",
        Private->Name,
        Status
        ));
      return EFI_NO_MEDIA;
    }

    SmhcSdInitCommand (
      &Command,
      A733_SMHC_SD_CMD_SEND_OP_COND,
      A733_SMHC_SD_ACMD41_ARG,
      SdMmcCommandTypeBcr,
      SdMmcResponseTypeR3
      );
    Status = SmhcSendCommand (Private, &Command, A733_SMHC_IDENT_CMD_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: %a: ACMD41 did not answer (%r), so there is no card in the slot\n",
        Private->Name,
        Status
        ));
      return EFI_NO_MEDIA;
    }

    Ocr = Command.Response[0];
    if ((Ocr & A733_SMHC_SD_OCR_READY) != 0) {
      break;
    }

    if (SmhcDeadlineExpired (&Budget)) {
      DEBUG ((
        DEBUG_INFO,
        "Smhc: %a: the card never finished its power-up cycle (OCR %08x after %u ms)\n",
        Private->Name,
        Ocr,
        A733_SMHC_SD_POWER_UP_BUDGET_US / 1000U
        ));
      return EFI_NO_MEDIA;
    }

    MicroSecondDelay (A733_SMHC_SD_POWER_UP_STEP_US);
  }

  //
  // CMD58 reads the OCR the card settled on.  Bit 30 (CCS) is this driver's own
  // copy of "the card addresses 512-byte blocks": the split path in SmhcPassThru
  // advances a data command's argument by it, and SdDxe reads the same answer
  // from the CSD instead, so the two never have to agree about it.
  //
  SmhcSdInitCommand (&Command, A733_SMHC_SD_CMD_READ_OCR, 0, SdMmcCommandTypeBcr, SdMmcResponseTypeR3);
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_IDENT_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    //
    // CMD58 is optional.  The ACMD41 response already carries the OCR,
    // CCS included, which is where Linux and U-Boot take it from - neither
    // issues CMD58 for an SD card.  Some cards stop answering commands for
    // a moment once their power-up completes, and this one does; keep the
    // ACMD41 answer instead of failing the slot.
    //
    DEBUG ((
      DEBUG_INFO,
      "Smhc: %a: CMD58 did not answer (%r); keeping the ACMD41 OCR %08x\n",
      Private->Name,
      Status,
      Ocr
      ));
  } else {
    Ocr = Command.Response[0];
  }

  HighCapacity              = (Ocr & A733_SMHC_SD_OCR_CCS) != 0;
  Private->SectorAddressing = HighCapacity;

  //
  // CMD2 asks for the CID and leaves the card in ident, which is where this
  // driver stops on purpose: SdDxe's CMD7(0)/CMD3/CMD9/CMD10/CMD7/CMD13 follows,
  // and it is SdDxe that assigns the address this driver then switches the bus
  // by (see SmhcSdPrepareDataCommand).
  //
  SmhcSdInitCommand (&Command, A733_SMHC_CMD_ALL_SEND_CID, 0, SdMmcCommandTypeBcr, SdMmcResponseTypeR2);
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_IDENT_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: %a: CMD2 did not answer (%r)\n", Private->Name, Status));
    return EFI_NO_MEDIA;
  }

  //
  // Keep the CID.  CMD10 reads the same register addressed by RCA, and the
  // cards that ignore that second read still answer CMD2, so this copy is what
  // the pass-thru falls back to when SdDxe's CMD10 times out (see SmhcPassThru).
  //
  CopyMem (Private->SdCid, Command.Response, sizeof (Private->SdCid));
  Private->SdCidValid = TRUE;

  DEBUG ((
    DEBUG_INFO,
    "Smhc: %a: SD %a card identified, OCR %08x, %a; leaving it in ident for the block driver\n",
    Private->Name,
    Version,
    Ocr,
    HighCapacity ? "high capacity, block addressing" : "standard capacity, byte addressing"
    ));

  return EFI_SUCCESS;
}

/**
  Switch the card and the controller to the bus the slot can carry.

  This is the half of an SD bring-up the block driver does not do.  SdDxe sends
  no CMD55, ACMD6 or CMD6, so unless the host arranges it the card runs on one
  data line in compatibility timing for the whole boot - and the switch cannot be
  made during identification, because ACMD6 is addressed to the card's RCA and
  the RCA is SdDxe's to assign.  It is therefore made here, in front of the first
  data command that follows an addressed CMD7, and the driver's latch
  (SdSpeedDone) makes it a once-per-controller event whatever the outcome.

  Steps, each of which can fail without failing the transfer that triggered it:

  1. The clock moves from the identification divider to the plan's bring-up
     divider.  Compatibility timing's limit is 26 MHz and the bring-up divider is
     the plan's rate inside it, so everything below runs there whatever the card
     supports.
  2. CMD55 + ACMD6 set the card's width, four data lines when the board wires
     them (MaxBusWidth), one otherwise.  The controller's CTYPE takes the same
     two values as ACMD6, so the card and the host are moved to the same width.
  3. CMD6 (SWITCH_FUNC) asks for High Speed, and the 64 bytes it answers with say
     whether the card took it.  A card that does not offer the function keeps its
     current one and still answers, so the status byte is what decides.
  4. Only once High Speed is real does the clock move to the plan's at-speed
     divider (50 MHz).  A card left in default speed stays on the bring-up
     divider, which is the clock its timing is defined for.

  @param[in,out] Private  Driver state; Rca must already hold the address the
                          block driver assigned, and BusWidth, HighSpeed and
                          SdSpeedDone are filled in.
**/
VOID
SmhcSdPrepareDataCommand (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS         Status;
  A733_SMHC_COMMAND  Command;
  UINT8              SwitchStatus[A733_SMHC_SD_SWITCH_STATUS_SIZE];
  CONST CHAR8        *Switch;
  BOOLEAN            FourBit;

  if ((Private->CardType != A733SmhcCardSd) || Private->SdSpeedDone) {
    return;
  }

  //
  // The address is SdDxe's, taken from the CMD7 that selected the card.  A data
  // command before that CMD7 has no address to switch, so the slot is left as
  // identification left it and the switch stays owed to the next data command.
  //
  if (Private->Rca == 0) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: %a: a data command arrived before any addressed CMD7; the card stays 1-bit at the identification clock\n",
      Private->Name
      ));
    return;
  }

  Private->SdSpeedDone = TRUE;

  SmhcSetClock (Private, Private->BringUpDivisor);

  FourBit = (Private->MaxBusWidth >= 4);

  SmhcSdInitCommand (
    &Command,
    A733_SMHC_SD_CMD_APP_CMD,
    Private->Rca << 16,
    SdMmcCommandTypeAc,
    SdMmcResponseTypeR1
    );
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  if (!EFI_ERROR (Status)) {
    SmhcSdInitCommand (
      &Command,
      A733_SMHC_SD_ACMD_SET_BUS_WIDTH,
      FourBit ? A733_SMHC_SD_WIDTH_4BIT : A733_SMHC_SD_WIDTH_1BIT,
      SdMmcCommandTypeAc,
      SdMmcResponseTypeR1
      );
    Status = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  }

  if (EFI_ERROR (Status)) {
    //
    // The card still answers on one line - it has not been told otherwise - so
    // this is a narrower bus rather than a broken one.  The failure can also
    // leave the controller's command FSM parked mid-command, and the caller's
    // transfer is what runs next, so the controller is put back on a fresh
    // reset at the same clock rather than carried into that transfer.
    //
    FourBit = FALSE;
    DEBUG ((
      DEBUG_INFO,
      "Smhc: %a: the width switch did not take (CMD55/ACMD6: %r); the card keeps one data line\n",
      Private->Name,
      Status
      ));

    if (EFI_ERROR (SmhcPrepareController (Private, Private->BringUpDivisor))) {
      DEBUG ((DEBUG_ERROR, "Smhc: %a did not come back after the width switch\n", Private->Name));
    }
  }

  SmhcSetBusWidth (Private->Base, FourBit ? A733_SMHC_BUS_4BIT : A733_SMHC_BUS_1BIT);
  Private->BusWidth = FourBit ? A733_SMHC_BUS_4BIT : A733_SMHC_BUS_1BIT;

  //
  // CMD6, and the 64-byte status it answers with.  The buffer is a local one on
  // purpose: the command engine bounces a read that does not fill whole cache
  // lines through its pool, so the descriptor never invalidates a neighbour's
  // line of the stack.
  //
  ZeroMem (SwitchStatus, sizeof (SwitchStatus));

  SmhcSdInitCommand (
    &Command,
    A733_SMHC_CMD_SWITCH,
    A733_SMHC_SD_SWITCH_SET_HS,
    SdMmcCommandTypeAdtc,
    SdMmcResponseTypeR1
    );
  Command.Buffer    = SwitchStatus;
  Command.Length    = sizeof (SwitchStatus);
  Command.BlockSize = sizeof (SwitchStatus);
  Command.Read      = TRUE;

  Status = SmhcSendCommand (Private, &Command, A733_SMHC_DATA_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    Switch = "CMD6 did not answer, so the card stays at default speed";

    //
    // A failed CMD6 can leave the controller's command FSM parked, and the
    // caller's transfer follows this function at once.  The driver's own
    // recovery flag is how that is said: the next command re-initializes the
    // controller and re-applies the width and the clock settled on here before
    // it is sent (see SmhcRecoverController), instead of being carried into a
    // controller a failed command left mid-FSM.
    //
    Private->DataFault = TRUE;
  } else if ((SwitchStatus[A733_SMHC_SD_SWITCH_GROUP1_BYTE] & A733_SMHC_SD_SWITCH_GROUP1_MASK) !=
             A733_SMHC_SD_SWITCH_GROUP1_HS)
  {
    Switch = "CMD6 answered but the card selected default speed";
  } else {
    Switch             = "CMD6 answered and selected High Speed, kept at the bring-up clock";
    Private->HighSpeed = TRUE;
    //
    // Do not move to DataDivisor (50 MHz) yet: on this board the 4-bit slot
    // times out intermittently at the at-speed divider (CMD18 HTO on the FAT
    // boot sector, retried by the retry logic).  The card keeps High Speed
    // timing; only the clock stays at the 25 MHz bring-up rate until the
    // sample delay for the slot is characterised.
    //
  }

  DEBUG ((
    Private->HighSpeed ? DEBUG_INFO : DEBUG_ERROR,
    "Smhc: %a: SD data bus is %u kHz, %u-bit, %a; the transfer goes ahead on it\n",
    Private->Name,
    SmhcClockKhz (Private),
    (Private->BusWidth == A733_SMHC_BUS_4BIT) ? 4U : 1U,
    Switch
    ));
}

/**
  Put the card in a slot back to the state the block driver left it in.

  A device reset takes everything the card holds on the driver's behalf with it:
  the address, the width and the timing.  The card is therefore identified from
  the top, given the same address the block driver addresses it by, selected, and
  put back on the bus SdDxe's next command expects - which is the state the
  caller of ResetDevice carries on from.  No EXT_CSD is involved: an SD card has
  none, and everything this needs is the identification sequence above plus CMD3
  and CMD7.

  @param[in,out] Private  Driver state; Rca, BusWidth and HighSpeed are rebuilt.

  @retval EFI_SUCCESS   The card is identified, addressed, selected and on its
                        data bus again.
  @retval EFI_NO_MEDIA  No card in the slot, or it did not identify.
  @retval other         A command of the recovery failed.
**/
EFI_STATUS
SmhcCardResetSd (
  IN OUT A733_SMHC_PRIVATE  *Private
  )
{
  EFI_STATUS         Status;
  A733_SMHC_COMMAND  Command;
  UINT32             Address;
  UINT32             Rca;

  //
  // The address SdDxe holds is the one from the last CMD7 it sent, so it is
  // asked for again: a card that takes the requested address leaves the block
  // driver's own copy of it valid across the reset.
  //
  Address = Private->Rca;

  Private->BusWidth    = A733_SMHC_BUS_1BIT;
  Private->HighSpeed   = FALSE;
  Private->SdSpeedDone = FALSE;
  Private->CountArmed  = FALSE;

  Status = SmhcCardInitSd (Private);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SmhcSdInitCommand (
    &Command,
    A733_SMHC_CMD_SET_RELATIVE_ADDR,
    Address << 16,
    SdMmcCommandTypeAc,
    SdMmcResponseTypeR6
    );
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a: CMD3 after the reset did not answer (%r); the card is left in ident\n",
      Private->Name,
      Status
      ));
    return Status;
  }

  Rca          = (Command.Response[0] >> 16) & 0xFFFFU;
  Private->Rca = Rca;

  if ((Address != 0) && (Rca != Address)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a: the card took RCA %u after the reset, not the %u the block driver addresses it by\n",
      Private->Name,
      Rca,
      Address
      ));
  }

  SmhcSdInitCommand (&Command, A733_SMHC_CMD_SELECT_CARD, Rca << 16, SdMmcCommandTypeAc, SdMmcResponseTypeR1b);
  Status = SmhcSendCommand (Private, &Command, A733_SMHC_CMD_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a: CMD7 after the reset did not answer (%r)\n",
      Private->Name,
      Status
      ));
    return Status;
  }

  //
  // The address is known and the card is selected, so the bus is applied now
  // instead of lazily: nothing is going to send another CMD7 before the next
  // data command.
  //
  SmhcSdPrepareDataCommand (Private);

  return EFI_SUCCESS;
}