/** @file
  EFI_SD_MMC_PASS_THRU_PROTOCOL over the Allwinner A733's SMHC controllers.

  This driver owns the board's eMMC: it brings each controller the board declares
  up (A733SmhcCard.c through A733SmhcHci.c) and then hands the five pass-thru
  functions to MdeModulePkg, one handle per controller.  EmmcDxe, PartitionDxe
  and FatPkg do everything above that, which is what puts fs0: in front of the
  Shell.

  What this file adds on top of the command engine:

  - The board bring-up: CCU gate/reset/clock for the controller's module and its
    own pad group, programmed to the state a hardware read-back of boot0
    confirmed rather than inherited from it.

  - The pass-thru contract itself, including the two conventions EmmcDxe relies
    on: a packet's status block is RESP0..3 as the controller lays them out
    (word 0 is response bits 31:0), and an async packet's event is signalled
    only when the packet really completed.

  - The one place where a request does not fit a single command: a transfer
    larger than A733_SMHC_MAX_TRANSFER_BYTES is split, and each part is bounded
    with its own CMD23.

  Which controllers exist, and everything about each one that is a property of
  the board rather than of the silicon, comes from A733SmhcBoardControllers (see
  Library/A733SmhcBoardLib.h).  This file has no controller it knows by name: it
  walks that table, brings each entry up into its own A733_SMHC_PRIVATE, and
  publishes a handle per entry that survives.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Protocol/DevicePath.h>
#include <Protocol/SdMmcPassThru.h>

#include <Library/A733CcuLib.h>
#include <Library/A733GpioLib.h>
#include <Library/A733SmhcBoardLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <A733Ccu.h>
#include <A733Smhc.h>

#include "A733SmhcDxe.h"

// Drive strength the board's own bring-up left on every eMMC pin.
#define A733_SMHC2_DRIVE_STRENGTH  1U

#define A733_SMHC_PRIVATE_FROM_PASS_THRU(This) \
  BASE_CR (This, A733_SMHC_PRIVATE, PassThru)

/**
  The messaging device path subtype a controller's card is reported as.

  This is the only signal MdeModulePkg gets about what kind of card a pass-thru
  handle carries: EmmcDxe claims a handle whose node is MSG_EMMC_DP and refuses
  an SD node, SdDxe the other way round.  It therefore decides who binds, which is
  why one handle carries one card type.

  @param[in] CardType  The controller's card type, from its board table entry.

  @return MSG_EMMC_DP or MSG_SD_DP.
**/
STATIC
UINT8
SmhcCardDpSubtype (
  IN A733_SMHC_CARD_TYPE  CardType
  )
{
  return (CardType == A733SmhcCardSd) ? MSG_SD_DP : MSG_EMMC_DP;
}

/**
  The name of a card type, for the log.

  @param[in] CardType  The controller's card type.

  @return "eMMC" or "SD".
**/
STATIC
CONST CHAR8 *
SmhcCardTypeName (
  IN A733_SMHC_CARD_TYPE  CardType
  )
{
  return (CardType == A733SmhcCardSd) ? "SD" : "eMMC";
}

/**
  Give the controller its clock gate and its pins.

  Manual's programming sequence (11.3.6.1) opens with the clock: the BGR gate
  and reset, then the module's parent and divider; then the GPIO.  The pins are
  programmed here rather than left as boot0 arranged them, so the driver does
  not depend on a previous stage having run, and so the state it needs (pull
  ups on a bus with no card driving it) is the state it gets.

  The module, the pad function and the pad group all come from the controller's
  board table entry: two SMHC controllers can reach the same card through the
  same twelve port C pins (ALT3 selects SDC2, ALT4 selects SDC3), so a bring-up
  for one controller must not leave the pads muxed to the other.

  The module's parent and dividers are not set here but by SmhcSelectClockPlan,
  which the entry point calls next: which parent the module runs on is the
  driver's clock plan, and every card rate follows from it (see A733SmhcDxe.h).

  @param[in] Controller  The controller to bring up, from the board's table.

  @retval EFI_SUCCESS  The clock gate is open and the pins are muxed to this
                       controller.
  @retval other        The CCU is not modelled for that module, or a pin could
                       not be configured.
**/
STATIC
EFI_STATUS
SmhcBoardBringUp (
  IN CONST A733_SMHC_CONTROLLER  *Controller
  )
{
  EFI_STATUS  Status;
  UINT32      Index;

  Status = A733CcuEnableModule (Controller->CcuModule);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot enable %a in the CCU: %r\n", Controller->Name, Status));
    return Status;
  }

  for (Index = 0; Index < Controller->PadCount; Index++) {
    Status = A733GpioSetFunction (Controller->Pads[Index].Pin, Controller->PadFunction);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: pin %a cannot be muxed to %a: %r\n", Controller->Pads[Index].Signal, Controller->Name, Status));
      return Status;
    }

    Status = A733GpioSetPull (Controller->Pads[Index].Pin, Controller->Pads[Index].Pull);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: pin %a has no usable pull: %r\n", Controller->Pads[Index].Signal, Status));
      return Status;
    }

    Status = A733GpioSetDrive (Controller->Pads[Index].Pin, A733_SMHC2_DRIVE_STRENGTH);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Smhc: pin %a drive strength: %r\n", Controller->Pads[Index].Signal, Status));
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  One clock plan: an entry of a profile's table (see A733_SMHC_CLOCK_PROFILE).

  A plan is what the *module* clock is, not what the card sees: the controller's
  own SMHC_CLKDIV divides it down to the card, and this part's field divides by
  2 x (n + 1) - settled by measurement (see SmhcClockKhz) - so a plan states the
  module clock and the divider the phase wants.  BringUpDivisor covers the phase
  that carries data while the card is still in compatibility timing (the EXT_CSD
  read and the CMD6 switches, limit 26 MHz); DataDivisor covers the at-speed phase
  after the card's timing has been set.  The nominal rate is what the plan's own
  arithmetic gives; the library's rate is preferred when it has one, and it is the
  register read back that says whether the CCU took what was written.

  A profile's table is indexed by A733_SMHC_PLAN_*, so every profile has to fill
  every index: the two tables below differ in the parent they name and in nothing
  structural.
**/
typedef struct {
  CONST CHAR8   *Name;
  A733_CCU_SRC  Source;
  UINT32        ModuleM;
  UINT32        ModuleN;
  UINT32        BringUpDivisor;
  UINT32        DataDivisor;
  UINT32        NominalHz;
} A733_SMHC_CLOCK_PLAN;

/**
  The MMC profile: the parents SMHC2/SMHC3 can reach.

  - HOSC / 1: the 24 MHz oscillator the port has always clocked this controller
    from, with both dividers at 1, i.e. a 12 MHz card clock.  This is the plan the
    data path was verified at and the baseline every other measurement is read
    against.
  - PERI0_800M / 4 / 1: a 200 MHz module clock with the at-speed divider at 1, i.e.
    50 MHz on the card, and the bring-up divider at 3 (25 MHz, inside compatibility
    timing's 26 MHz).  This is the plan the SDR path and the DDR phase run on; the
    DDR row is a separate index because the two are separate decisions, not because
    the numbers differ.
  - PERI0_800M / 1 / 1: an 800 MHz module clock with the at-speed divider at 1, i.e.
    200 MHz on the card - HS200's own limit.  Only the HS200 step enters this plan,
    and only for as long as HS200 is in use; its bring-up divider is 3 (unused, and
    named only so no reader has to work out that nothing runs one there).
  - PERI0_800M / 2 / 1: a 400 MHz module clock with the at-speed divider at 0 -
    HS400, whose card clock is the module clock halved (the manual clears
    CCLK_DIV when HS400_MD_EN is set).

  These numbers are the ones the eMMC was measured at and are not to be adjusted.
**/
STATIC CONST A733_SMHC_CLOCK_PLAN  mA733SmhcMmcClockPlans[A733_SMHC_PLAN_MAX] = {
  { "HOSC 24 MHz /1",            A733_CCU_SRC_HOSC,       1U, 1U, 1U, 1U, A733_SMHC_PLAN_HOSC_HZ        },
  { "PERI0_800M /4 /1 (200MHz)", A733_CCU_SRC_PERI0_800M, 4U, 1U, 3U, 1U, A733_SMHC_PLAN_PERI0_HZ       },
  { "PERI0_800M /4 /1 (200MHz)", A733_CCU_SRC_PERI0_800M, 4U, 1U, 3U, 1U, A733_SMHC_PLAN_PERI0_DDR_HZ   },
  { "PERI0_800M /1 /1 (800MHz)", A733_CCU_SRC_PERI0_800M, 1U, 1U, 3U, 1U, A733_SMHC_PLAN_PERI0_HS200_HZ },
  { "PERI0_800M /2 /1 (400MHz)", A733_CCU_SRC_PERI0_800M, 2U, 1U, 3U, 0U, A733_SMHC_PLAN_PERI0_HS200_HZ },
};

/**
  The SD profile: the parents SMHC0/SMHC1 can reach.

  Those two modules have no PERI0_800M in their CCU class table (A733CcuLib's
  mSmhc01NandSources: HOSC, PERI0_400M, PERI0_300M, PERI1_400M, PERI1_300M), so a
  plan naming it comes back EFI_UNSUPPORTED from A733CcuSetModuleClockDivider.  The
  fastest parent they do have is PERI0_400M, and the module clock is then set by
  the driver's own divider:

  - HOSC / 1 / 1: the same 24 MHz oscillator as the MMC table's, i.e. a 24 MHz
    module clock, 12 MHz on the card at the data divider.
  - PERI0_400M / 2 / 1: 400 MHz / 2 = a 200 MHz module clock.  This part's model
    is card = module / (2 * (n + 1)) (see A733SmhcDxe.h and SmhcClockKhz), so with
    the field at 3 the bring-up clock is 200 / (2 * 4) = 25 MHz - just inside
    compatibility timing's 26 MHz, and the same number the eMMC's bring-up runs at
    from a different parent - and with the field at 1 the at-speed clock is
    200 / (2 * 2) = 50 MHz.  The SD backend uses these two rows and no other.
  - PERI0_400M / 1 / 1: an undivided 400 MHz module clock, i.e. the fastest this
    parent gives.  These two rows are the shape of the MMC table's HS200/HS400
    rows rather than rates an SD card ever asks for: an SD controller never runs
    the eMMC ladder, and they exist so both tables are indexed by the same
    A733_SMHC_PLAN_* values.
**/
STATIC CONST A733_SMHC_CLOCK_PLAN  mA733SmhcSdClockPlans[A733_SMHC_PLAN_MAX] = {
  { "HOSC 24 MHz /1",            A733_CCU_SRC_HOSC,       1U, 1U, 1U, 1U, A733_SMHC_PLAN_HOSC_HZ          },
  { "PERI0_400M /2 /1 (200MHz)", A733_CCU_SRC_PERI0_400M, 2U, 1U, 3U, 1U, A733_SMHC_PLAN_PERI0_SD_HZ     },
  { "PERI0_400M /2 /1 (200MHz)", A733_CCU_SRC_PERI0_400M, 2U, 1U, 3U, 1U, A733_SMHC_PLAN_PERI0_SD_HZ     },
  { "PERI0_400M /1 /1 (400MHz)", A733_CCU_SRC_PERI0_400M, 1U, 1U, 3U, 1U, A733_SMHC_PLAN_PERI0_SD_FAST_HZ },
  { "PERI0_400M /1 /1 (400MHz)", A733_CCU_SRC_PERI0_400M, 1U, 1U, 3U, 0U, A733_SMHC_PLAN_PERI0_SD_FAST_HZ },
};

/**
  The plan table a controller's plans come from.

  @param[in] Profile  The controller's clock profile, from its board entry.

  @return The profile's table; the MMC one for any value that is not the SD
          profile, since a controller that does not say otherwise is an eMMC.
**/
STATIC CONST A733_SMHC_CLOCK_PLAN *
SmhcClockPlanTable (
  IN A733_SMHC_CLOCK_PROFILE  Profile
  )
{
  return (Profile == A733SmhcClockProfileSd) ? mA733SmhcSdClockPlans : mA733SmhcMmcClockPlans;
}

/**
  Name a clock plan, for the log.

  @param[in] Profile  The controller's clock profile.
  @param[in] Plan     A733_SMHC_PLAN_*.

  @return The plan's name, or "-" when Plan is out of range.
**/
CONST CHAR8 *
SmhcClockPlanName (
  IN A733_SMHC_CLOCK_PROFILE  Profile,
  IN UINT32                   Plan
  )
{
  if (Plan >= A733_SMHC_PLAN_MAX) {
    return "-";
  }

  return SmhcClockPlanTable (Profile)[Plan].Name;
}

/**
  Apply a clock plan to the CCU and record what the hardware and the library
  actually say about it.

  Steps, in the order the answer matters:

  1. Write the parent and both dividers (A733CcuSetModuleClockDivider), then read
     the register back.  The read-back is not a formality: the module's FACTOR_N
     field is the one the port has never exercised - A733CcuSetModuleClock used
     to leave it alone and this driver used to clear it by hand - so a write that
     does not take has to be caught here rather than absorbed into a rate nobody
     checked.  If FACTOR_N reads back unprogrammed while FACTOR_M took, the same
     divisor is reached as M = M * N, N = 1 (the product is what divides the
     parent) and the plan is applied that way, with the log saying so.
  2. Take the module clock from A733CcuGetModuleClockHz, which decodes the
     register just written.  The library reports 0 for a parent whose rate it
     cannot verify; the plan's nominal rate stands in then, and the log says so.
  3. Compute the identification divider so the card clock lands at or below
     A733_SMHC_IDENT_KHZ, and take the bring-up and at-speed dividers from the
     plan.  The identification one is the only large divider in the driver, and
     the only phase that uses it is the data-less identification sequence: a
     data transfer on it is the configuration that mis-frames on this hardware
     (see A733SmhcDxe.h).

  @param[in,out] Private  Driver state; the clock fields are filled in.
  @param[in]     Plan     A733_SMHC_PLAN_HOSC or A733_SMHC_PLAN_PERI0.

  @retval EFI_SUCCESS            The plan is applied and the dividers computed.
  @retval EFI_INVALID_PARAMETER  Plan is out of range.
  @retval other                  The CCU refused the plan, or the fields did not
                                 take; the driver state is unchanged.
**/
EFI_STATUS
SmhcSelectClockPlan (
  IN OUT A733_SMHC_PRIVATE  *Private,
  IN     UINT32             Plan
  )
{
  EFI_STATUS                  Status;
  UINTN                       Base;
  UINT32                      Value;
  UINT32                      Source;
  UINT32                      FactorM;
  UINT32                      FactorN;
  UINT32                      AppliedM;
  UINT32                      AppliedN;
  UINT32                      IdentDivisor;
  UINT32                      Divisor;
  UINT64                      ModuleHz;
  BOOLEAN                     NominalUsed;
  CONST A733_SMHC_CLOCK_PLAN  *Table;

  if (Plan >= A733_SMHC_PLAN_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The plan is read from the table belonging to this controller's profile: the
  // two profiles differ in the parent their fast plans name, and a plan taken from
  // the other table would name a parent this module cannot reach.
  //
  Table = SmhcClockPlanTable (Private->ClockProfile);

  Base = (UINTN)PcdGet64 (PcdA733CcuBase);

  AppliedM = Table[Plan].ModuleM;
  AppliedN = Table[Plan].ModuleN;

  Status = A733CcuSetModuleClockDivider (
             Private->CcuModule,
             Table[Plan].Source,
             AppliedM,
             AppliedN
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the CCU cannot select %a: %r\n", Table[Plan].Name, Status));
    return Status;
  }

  Value   = MmioRead32 (Base + Private->CcuClkReg);
  FactorM = Value & A733_CCU_CLK_FACTOR_M_MASK;
  FactorN = (Value & A733_CCU_CLK_FACTOR_N_MASK) >> A733_CCU_CLK_FACTOR_N_SHIFT;

  if ((AppliedN > A733_CCU_CLK_FACTOR_MIN) && (FactorN != (AppliedN - 1))) {
    //
    // FACTOR_N did not take.  The parent is divided by M * N, so if the M field
    // did take, the plan's nominal rate is still reachable with N = 1 and
    // M = M * N - which is how the plan is applied from here, so the rest of the
    // driver runs at the rate it was designed around and the measurement still
    // happens.
    //
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: CCU %a CLK %08x: FACTOR_N reads back %u for the %u that was written%a\n",
      Private->Name,
      Value,
      FactorN,
      AppliedN - 1,
      (FactorM == (AppliedM - 1)) ? "; retrying as M = M * N, N = 1" : ", and FACTOR_M did not take either"
      ));

    if (FactorM != (AppliedM - 1)) {
      DEBUG ((DEBUG_ERROR, "Smhc: neither divider field took; the module clock is not the plan's\n"));
      return EFI_DEVICE_ERROR;
    }

    AppliedM = AppliedM * AppliedN;
    AppliedN = A733_CCU_CLK_FACTOR_MIN;
    if (AppliedM > A733_CCU_CLK_FACTOR_MAX) {
      DEBUG ((DEBUG_ERROR, "Smhc: %u is past the FACTOR_M field's range\n", AppliedM));
      return EFI_DEVICE_ERROR;
    }

    Status = A733CcuSetModuleClockDivider (Private->CcuModule, Table[Plan].Source, AppliedM, AppliedN);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Value   = MmioRead32 (Base + Private->CcuClkReg);
    FactorM = Value & A733_CCU_CLK_FACTOR_M_MASK;
    FactorN = (Value & A733_CCU_CLK_FACTOR_N_MASK) >> A733_CCU_CLK_FACTOR_N_SHIFT;
    if (FactorM != (AppliedM - 1)) {
      DEBUG ((DEBUG_ERROR, "Smhc: CCU %a CLK %08x does not hold FACTOR_M %u either\n", Private->Name, Value, AppliedM - 1));
      return EFI_DEVICE_ERROR;
    }
  }

  Source      = (Value >> A733_CCU_CLK_SRC_SHIFT) & ((1U << A733_CCU_CLK_SRC_WIDTH) - 1U);
  ModuleHz    = A733CcuGetModuleClockHz (Private->CcuModule);
  NominalUsed = FALSE;
  if (ModuleHz == 0) {
    ModuleHz    = Table[Plan].NominalHz;
    NominalUsed = TRUE;
  }

  //
  // The identification divider is derived, never written as a literal: the plan
  // is free to change the module clock, and fOD is a property of the card, not
  // of the plan.  Round the quotient up, so the card clock lands at or below
  // 400 kHz, and 0 stays the full module clock.
  //
  Divisor = (UINT32)((ModuleHz + (A733_SMHC_IDENT_KHZ * 1000U) - 1U) / (A733_SMHC_IDENT_KHZ * 1000U));
  if (Divisor <= 1) {
    Divisor = 1;
  }

  IdentDivisor = Divisor - 1;
  if (IdentDivisor > A733_SMHC_CLKDIV_CCLK_DIV_MASK) {
    //
    // The field cannot reach fOD from this plan's module clock, so a plan like
    // the HS200 one - 800 MHz - can never run identification.  That is only a
    // fault on the plan the driver starts on: the high-rate plans are entered
    // after the card is already selected, and their identification divider is
    // computed for completeness and logged rather than used.  (A device reset
    // re-runs identification on whatever plan is current, which is why the
    // driver's only reset path is the pass-thru one EmmcDxe never calls.)
    //
    DEBUG ((
      DEBUG_INFO,
      "Smhc: a %u kHz module clock cannot be divided under fOD (%u kHz) by the controller's %u-bit field; "
      "this plan cannot identify, and %a\n",
      (UINT32)(ModuleHz / 1000),
      A733_SMHC_IDENT_KHZ,
      (UINT32)(A733_SMHC_CLKDIV_CCLK_DIV_MASK + 1),
      (Plan == A733_SMHC_PLAN_DEFAULT) ? "it is the plan the driver starts on" :
      "it is only entered after identification"
      ));
    IdentDivisor = A733_SMHC_CLKDIV_CCLK_DIV_MASK;
  }

  Private->ClockPlan      = Plan;
  Private->ModuleClockHz  = (UINT32)ModuleHz;
  Private->IdentDivisor   = IdentDivisor;
  Private->BringUpDivisor = Table[Plan].BringUpDivisor;
  Private->DataDivisor    = Table[Plan].DataDivisor;

  //
  // The register as it reads back, and the rate the library decodes from it: the
  // two numbers a measurement is checked against.
  //
  DEBUG ((
    DEBUG_INFO,
    "Smhc: CCU %a CLK %08x (gate %u, source %u, FACTOR_M %u, FACTOR_N %u), library reports %u kHz%a\n",
    Private->Name,
    Value,
    (Value & A733_CCU_CLK_GATING) != 0,
    Source,
    FactorM,
    FactorN,
    (UINT32)(ModuleHz / 1000),
    NominalUsed ? ", which is the plan's nominal rate (the library has no verified rate for that parent)" : ""
    ));

  //
  // Every rate is stated with the field's two readings, because which one the
  // hardware implements is exactly what the throughput measurement settles: the
  // driver's arithmetic (card = module / (field + 1)) is the one this port has
  // always used, the other (card = module / 2 / (field + 1), i.e. the vendor's
  // module clock being twice the card clock for SDR) is what the manual's own
  // "divided by 2*n" text and the vendor's driver for this controller agree on.
  //
  DEBUG ((
    DEBUG_INFO,
    "Smhc: clock plan %a: %u kHz module -> identification %u kHz / div %u (other reading %u kHz), "
    "bring-up %u kHz / div %u, at speed %u kHz / div %u (other reading %u kHz)\n",
    Table[Plan].Name,
    (UINT32)(ModuleHz / 1000),
    (UINT32)(ModuleHz / (IdentDivisor + 1U) / 1000U),
    IdentDivisor,
    (UINT32)(ModuleHz / (2U * (IdentDivisor + 1U)) / 1000U),
    (UINT32)(ModuleHz / (Private->BringUpDivisor + 1U) / 1000U),
    Private->BringUpDivisor,
    (UINT32)(ModuleHz / (Private->DataDivisor + 1U) / 1000U),
    Private->DataDivisor,
    (UINT32)(ModuleHz / (2U * (Private->DataDivisor + 1U)) / 1000U)
    ));

  return EFI_SUCCESS;
}

/**
  Build the controller's own device path node, and the end node after it.

  EmmcDxe appends the card's node to the controller's path, so the controller
  handle needs a well-formed path of its own before EmmcDxe can bind.  The node
  is a vendor node carrying the platform's own GUID and the name of the
  controller from its board table entry (Private->Name), so the device the Shell
  and the boot manager list is named after the controller a reader would look
  for.

  @param[in] Name  The controller's name, its table entry's Name.

  @return The device path, or NULL when the pool allocation failed.
**/
STATIC
EFI_DEVICE_PATH_PROTOCOL *
SmhcBuildControllerDevicePath (
  IN CONST CHAR8  *Name
  )
{
  UINTN                     NodeSize;
  UINTN                     PathSize;
  UINTN                     NameSize;
  UINT8                     *Path;
  VENDOR_DEVICE_PATH        *Vendor;
  EFI_DEVICE_PATH_PROTOCOL  *End;

  // The controller's name follows the GUID without its terminator;
  // VENDOR_DEVICE_PATH in this edk2 revision carries no data member, only the
  // header and the GUID.
  NameSize = AsciiStrLen (Name);
  NodeSize = sizeof (VENDOR_DEVICE_PATH) + NameSize;
  PathSize = NodeSize + sizeof (EFI_DEVICE_PATH_PROTOCOL);

  Path = AllocateZeroPool (PathSize);
  if (Path == NULL) {
    return NULL;
  }

  Vendor                  = (VENDOR_DEVICE_PATH *)Path;
  Vendor->Header.Type     = HARDWARE_DEVICE_PATH;
  Vendor->Header.SubType  = HW_VENDOR_DP;
  SetDevicePathNodeLength (&Vendor->Header, NodeSize);
  CopyGuid (&Vendor->Guid, &gA733TokenSpaceGuid);
  CopyMem ((UINT8 *)Vendor + sizeof (VENDOR_DEVICE_PATH), Name, NameSize);

  End                    = (EFI_DEVICE_PATH_PROTOCOL *)(Path + NodeSize);
  End->Type              = END_DEVICE_PATH_TYPE;
  End->SubType           = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength (End, sizeof (EFI_DEVICE_PATH_PROTOCOL));

  return (EFI_DEVICE_PATH_PROTOCOL *)Path;
}

/**
  Send a command, splitting a transfer too large for one command.

  The controller's SMHC_BYTCNT is 32 bits, so a UINT32 length always fits; the
  cap this driver applies is its own (see A733_SMHC_MAX_TRANSFER_BYTES).  Each
  part is preceded by its own CMD23, so the card stops at the part's block count
  instead of running past the host's byte counter - the caller's CMD23 covers
  the whole request and would let the card keep sending.

  The card is in sector addressing (the driver selects it in CMD1 and checks the
  CSD), so a data command's argument is a sector number and each part advances
  it by that part's block count.

  @param[in]     Private    Driver state.
  @param[in,out] Command    Command to send, with its whole transfer.
  @param[in]     TimeoutUs  Budget for one command.

  @retval EFI_SUCCESS  Every part was sent.
  @retval other        The first part that failed.
**/
STATIC
EFI_STATUS
SmhcExecuteCommand (
  IN     A733_SMHC_PRIVATE  *Private,
  IN OUT A733_SMHC_COMMAND  *Command,
  IN     UINT64             TimeoutUs
  )
{
  EFI_STATUS         Status;
  A733_SMHC_COMMAND  Count;
  UINT8              *Buffer;
  UINT32             Remaining;
  UINT32             PartBytes;
  UINT32             PartBlocks;
  UINT32             Argument;
  BOOLEAN            MultiBlock;

  MultiBlock = (Command->Index == A733_SMHC_CMD_READ_MULTIPLE) ||
               (Command->Index == A733_SMHC_CMD_WRITE_MULTIPLE);

  if ((Command->Length <= A733_SMHC_MAX_TRANSFER_BYTES) || !MultiBlock) {
    return SmhcSendCommand (Private, Command, TimeoutUs);
  }

  Buffer    = (UINT8 *)Command->Buffer;
  Remaining = Command->Length;
  Argument  = Command->Argument;

  while (Remaining != 0) {
    PartBytes = (Remaining > A733_SMHC_MAX_TRANSFER_BYTES) ? A733_SMHC_MAX_TRANSFER_BYTES : Remaining;
    PartBytes = (PartBytes / A733_SMHC_BLOCK_SIZE) * A733_SMHC_BLOCK_SIZE;
    PartBlocks = PartBytes / A733_SMHC_BLOCK_SIZE;

    //
    // SET_BLOCK_COUNT for this part.  The argument is a plain block count: the
    // caller's own CMD23 also carried no reliable-write bit, so overwriting the
    // count is what the card expects.
    //
    ZeroMem (&Count, sizeof (Count));
    Count.Index        = A733_SMHC_CMD_SET_BLOCK_COUNT;
    Count.Argument     = PartBlocks;
    Count.CommandType  = SdMmcCommandTypeAc;
    Count.ResponseType = SdMmcResponseTypeR1;

    Status = SmhcSendCommand (Private, &Count, TimeoutUs);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Command->Buffer     = Buffer;
    Command->Length     = PartBytes;
    Command->Argument   = Argument;
    Command->BlockSize  = A733_SMHC_BLOCK_SIZE;
    // Bounded by the CMD23 above, so the controller must not send its own
    // CMD12 on top of it.
    Command->AutoStop   = FALSE;

    Status = SmhcSendCommand (Private, Command, TimeoutUs);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Buffer    += PartBytes;
    Remaining -= PartBytes;
    if (Private->SectorAddressing) {
      Argument += PartBlocks;
    } else {
      Argument += PartBytes;
    }
  }

  return EFI_SUCCESS;
}

/**
  Read a number of blocks from LBA 0 and time it.

  The read goes through the driver's own command path - CMD23 to bound it, then
  CMD18, the two commands EmmcDxe sends for a disk read - into a page-aligned
  buffer below 4 GB, so the descriptors describe it directly and the number that
  comes out is the bus's rate rather than the rate of a transfer the driver had
  to bounce through its DMA pool.  (A page allocation is DWORD aligned, which the
  descriptor's word-address field needs, and cache-line aligned, which a read's
  invalidate needs.)  LBA 0 is the one block whose argument is 0 in both of the
  card's addressing modes, so the measurement does not depend on how the CSD
  describes the device.

  The timer is the platform's free-running counter, read through TimerLib - the
  same clock SmhcArmDeadline and every timeout in the driver are built on, so the
  elapsed figure here and the budgets there are the same measurement.

  @param[in]  Private     Driver state; the card must be selected and ready.
  @param[in]  Blocks      512-byte blocks to read.
  @param[out] ElapsedUs   Receives the elapsed microseconds.
  @param[out] KibPerSec   Receives the measured rate in KiB/s.

  @retval EFI_SUCCESS           The read completed; both outputs are set.
  @retval EFI_INVALID_PARAMETER Blocks is 0.
  @retval EFI_UNSUPPORTED       The read is larger than one command can move.
  @retval EFI_OUT_OF_RESOURCES  No buffer below 4 GB was available.
  @retval other                 The read failed; the outputs are 0.
**/
EFI_STATUS
SmhcMeasureThroughput (
  IN  A733_SMHC_PRIVATE  *Private,
  IN  UINT32             Blocks,
  OUT UINT32             *ElapsedUs,
  OUT UINT32             *KibPerSec
  )
{
  EFI_STATUS            Status;
  A733_SMHC_COMMAND     Count;
  A733_SMHC_COMMAND     Read;
  EFI_PHYSICAL_ADDRESS  Address;
  UINTN                 Pages;
  UINTN                 Bytes;
  UINT64                Frequency;
  UINT64                TicksPerMicro;
  UINT64                Start;
  UINT64                Ticks;

  *ElapsedUs = 0;
  *KibPerSec = 0;

  if (Blocks == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Bytes = Blocks * A733_SMHC_BLOCK_SIZE;
  if (Bytes > A733_SMHC_MAX_TRANSFER_BYTES) {
    return EFI_UNSUPPORTED;
  }

  Address = A733_SMHC_DMA_POOL_LIMIT - Bytes;
  Pages   = EFI_SIZE_TO_PAGES (Bytes);
  Status  = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, Pages, &Address);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: no %u-byte buffer below 4 GB for the throughput measurement: %r\n", (UINT32)Bytes, Status));
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // The ceiling is the pool limit less the whole transfer, not the pool limit
  // itself: the descriptors' address field has to hold the buffer's *last*
  // byte, and a buffer sitting in the last 256 KiB below 4 GB would be moved
  // through the driver's bounce buffer instead - which is a correct transfer
  // but not the one this measurement is trying to time.
  //

  //
  // CMD23 first, exactly as EmmcDxe arms a multi-block read: the card then ends
  // the transfer itself at Blocks, so the controller must not send a CMD12 of
  // its own on top of it.
  //
  ZeroMem (&Count, sizeof (Count));
  Count.Index        = A733_SMHC_CMD_SET_BLOCK_COUNT;
  Count.Argument     = Blocks;
  Count.CommandType  = SdMmcCommandTypeAc;
  Count.ResponseType = SdMmcResponseTypeR1;

  Status = SmhcSendCommand (Private, &Count, A733_SMHC_DATA_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the throughput measurement's CMD23 failed: %r\n", Status));
    gBS->FreePages (Address, Pages);
    return Status;
  }

  Private->CountArmed = TRUE;

  ZeroMem (&Read, sizeof (Read));
  Read.Index         = A733_SMHC_CMD_READ_MULTIPLE;
  Read.Argument      = 0;   // LBA 0
  Read.CommandType   = SdMmcCommandTypeAdtc;
  Read.ResponseType  = SdMmcResponseTypeR1;
  Read.Buffer        = (VOID *)(UINTN)Address;
  Read.Length        = Bytes;
  Read.BlockSize     = A733_SMHC_BLOCK_SIZE;
  Read.Read          = TRUE;
  Read.AutoStop      = FALSE;   // bounded by the CMD23 above

  Start  = GetPerformanceCounter ();
  Status = SmhcSendCommand (Private, &Read, A733_SMHC_DATA_TIMEOUT_US);
  Ticks  = GetPerformanceCounter () - Start;

  Frequency     = GetPerformanceCounterProperties (NULL, NULL);
  TicksPerMicro = (Frequency == 0) ? 1 : (Frequency / 1000000U);

  *ElapsedUs = (UINT32)(Ticks / TicksPerMicro);
  if (!EFI_ERROR (Status) && (*ElapsedUs != 0)) {
    *KibPerSec = (UINT32)DivU64x32 (MultU64x32 (Bytes / 1024U, 1000000U), *ElapsedUs);
  }

  //
  // The card keeps no block count of its own once the transfer has ended, but
  // the driver's record of whether one is armed is what the next multi-block
  // command reads (see SmhcPassThru), so it is cleared here rather than left
  // claiming a count the card is not holding.
  //
  Private->CountArmed = FALSE;

  gBS->FreePages (Address, Pages);

  return Status;
}

/**
  Log one throughput measurement, in the form every step is reported in.

    Smhc: throughput 256 KiB in 21000 us = 12190 KiB/s at 12000 kHz 8-bit SDR

  The rate the line states is the card clock the driver believes it is running -
  SMHC_CLKDIV read back against the module clock the plan selected - and the mode
  is what the card is in: SDR, or DDR once the DDR step has taken and been read
  back.  Splitting the measurement from the line lets a caller act on the number
  (the DDR step is gated on it) while still reporting it.

  @param[in] Private      Driver state; the card must be selected and ready.
  @param[in] Kib          The size the line states, in KiB.
  @param[in] ElapsedUs    Elapsed microseconds of the measurement.
  @param[in] KibPerSec    The measured rate.
**/
VOID
SmhcLogThroughput (
  IN A733_SMHC_PRIVATE  *Private,
  IN UINT32             Kib,
  IN UINT32             ElapsedUs,
  IN UINT32             KibPerSec
  )
{
  UINT32  Bits;

  Bits = (Private->BusWidth == A733_SMHC_BUS_8BIT) ? 8U :
         ((Private->BusWidth == A733_SMHC_BUS_4BIT) ? 4U : 1U);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: throughput %u KiB in %u us = %u KiB/s at %u kHz %u-bit %a\n",
    Kib,
    ElapsedUs,
    KibPerSec,
    SmhcClockKhz (Private),
    Bits,
    Private->Ddr ? "DDR" : "SDR"
    ));
}

/**
  Measure the bus at both sizes, log the three rates, and return them.

  The two absolute lines are the record; the marginal one is what the DDR gate
  judges, because only the difference between two sizes describes the bus rather
  than the bus plus the fixed cost a single transfer carries.  On this platform
  that fixed cost is large: the driver's own per-command debug output is about
  40 ms of 115200-baud console time, eight times a 256 KiB transfer at 50 MHz, so
  a single transfer's absolute rate says almost nothing about the bus.

  @param[in]  Private  Driver state; the card must be selected and ready.
  @param[out] Rate     Receives both measurements and the marginal rate.

  @retval EFI_SUCCESS  Both reads completed and Rate is filled in.
  @retval other        The first read that failed; Rate's times are then 0.
**/
EFI_STATUS
SmhcMeasureRate (
  IN  A733_SMHC_PRIVATE  *Private,
  OUT A733_SMHC_RATE     *Rate
  )
{
  EFI_STATUS  Status;
  UINT32      SmallKiB;
  UINT32      BigKiB;

  ZeroMem (Rate, sizeof (*Rate));

  SmallKiB = A733_SMHC_THROUGHPUT_SMALL_BLOCKS / 2U;
  BigKiB   = A733_SMHC_THROUGHPUT_BLOCKS / 2U;

  Status = SmhcMeasureThroughput (Private, A733_SMHC_THROUGHPUT_SMALL_BLOCKS, &Rate->SmallUs, &Rate->SmallKibPerSec);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the throughput measurement's %u KiB read failed: %r\n", SmallKiB, Status));
    return Status;
  }

  Rate->SmallKib = SmallKiB;
  SmhcLogThroughput (Private, SmallKiB, Rate->SmallUs, Rate->SmallKibPerSec);

  Status = SmhcMeasureThroughput (Private, A733_SMHC_THROUGHPUT_BLOCKS, &Rate->BigUs, &Rate->BigKibPerSec);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the throughput measurement's %u KiB read failed: %r\n", BigKiB, Status));
    return Status;
  }

  Rate->BigKib = BigKiB;
  SmhcLogThroughput (Private, BigKiB, Rate->BigUs, Rate->BigKibPerSec);

  //
  // The marginal rate: the bytes the larger read adds over the smaller, over the
  // time it adds.  Everything a transfer pays once - the command handshake and,
  // above all, the per-command debug output - cancels in the subtraction, so what
  // is left is the bus.  The two times have to differ enough for the subtraction
  // to mean something, which is why the sizes are a factor of eight apart.
  //
  if ((Rate->BigUs > Rate->SmallUs) && (Rate->BigKib > Rate->SmallKib)) {
    Rate->MarginalKibPerSec = (UINT32)DivU64x32 (
                                        MultU64x32 (Rate->BigKib - Rate->SmallKib, 1000000U),
                                        Rate->BigUs - Rate->SmallUs
                                        );
    Rate->MarginalValid = TRUE;

    DEBUG ((
      DEBUG_INFO,
      "Smhc: throughput %u KiB marginal in %u us = %u KiB/s at %u kHz %u-bit %a\n",
      Rate->BigKib - Rate->SmallKib,
      Rate->BigUs - Rate->SmallUs,
      Rate->MarginalKibPerSec,
      SmhcClockKhz (Private),
      (Private->BusWidth == A733_SMHC_BUS_8BIT) ? 8U : ((Private->BusWidth == A733_SMHC_BUS_4BIT) ? 4U : 1U),
      Private->Ddr ? "DDR" : "SDR"
      ));
  } else {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: the %u KiB and %u KiB reads took %u us and %u us, which is not enough of a difference to give a marginal rate\n",
      SmallKiB,
      BigKiB,
      Rate->SmallUs,
      Rate->BigUs
      ));
  }

  //
  // The phase traces of the two commands this pair was made of, printed here so
  // the console time the printing costs is not inside either measured window.
  //
  SmhcPhaseReport ();

  return EFI_SUCCESS;
}

/**
  Measure the bus at both sizes and log the three lines.

  The convenience wrapper around SmhcMeasureRate for the callers that only want
  the report.

  @param[in] Private  Driver state; the card must be selected and ready.

  @retval EFI_SUCCESS  Both reads completed and the lines were logged.
  @retval other        A read failed; the failure was logged instead.
**/
EFI_STATUS
SmhcReportThroughput (
  IN A733_SMHC_PRIVATE  *Private
  )
{
  A733_SMHC_RATE  Rate;

  return SmhcMeasureRate (Private, &Rate);
}

/**
  Report the one line an HS200 boot leaves behind, with the rate it measured.

  It is printed here rather than in the HS200 step because the rate only exists
  after the measurement, and it is at DEBUG_ERROR so that a boot's headline
  number cannot be filtered away.  The rate it quotes is the marginal one when
  the two sizes could be told apart - that is the bus - and the large read's
  absolute figure when they could not; the line says which.

  @param[in] Private  Driver state; Hs200 must be set.
  @param[in] Rate     The measurement the entry point just took.
**/
STATIC
VOID
SmhcReportHs200 (
  IN A733_SMHC_PRIVATE  *Private,
  IN A733_SMHC_RATE     *Rate
  )
{
  UINT32  KibPerSec;
  UINT32  CeilingKib;
  UINT32  Percent;
  UINT32  Khz;

  if (!Private->Hs200) {
    return;
  }

  Khz        = SmhcClockKhz (Private);
  CeilingKib = (UINT32)(((UINT64)Khz * 1000U * 8U) / (8U * 1024U));

  if (Rate->MarginalValid) {
    KibPerSec = Rate->MarginalKibPerSec;
  } else {
    KibPerSec = Rate->BigKibPerSec;
  }

  if (KibPerSec == 0) {
    //
    // The measurement itself failed (it says so on its own line): the card's
    // state is still worth reporting, which is why this line is not conditional
    // on the measurement.
    //
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: HS200: %u kHz, HS_TIMING %u, BUS_WIDTH %u, tap %u, throughput not measured %a\n",
      Khz,
      Private->HsTiming,
      A733_SMHC_EXT_CSD_WIDTH_8BIT,
      Private->SampleDelay,
      Rate->MarginalValid ? "marginal" : "absolute"
      ));

    return;
  }

  Percent = (CeilingKib == 0) ? 0 : (UINT32)(((UINT64)KibPerSec * 100U) / CeilingKib);

  DEBUG ((
    DEBUG_ERROR,
    "Smhc: HS200: %u kHz, HS_TIMING %u, BUS_WIDTH %u, tap %u, throughput %u KiB/s %a (%u %% of the %u KiB/s an 8-bit bus at that clock carries%a)\n",
    Khz,
    Private->HsTiming,
    A733_SMHC_EXT_CSD_WIDTH_8BIT,
    Private->SampleDelay,
    KibPerSec,
    Rate->MarginalValid ? "marginal" : "absolute",
    Percent,
    CeilingKib,
    (Percent >= A733_SMHC_HS200_HEALTHY_PERCENT) ? "" : ", below the expected rate"
    ));
}

/**
  Hand a 128-bit response to the block drivers above in the framing they read.

  An SDHCI host controller stores an R2 response with its last byte dropped and
  with the eight-bit frame prefix in the top byte: word N holds response bits
  [32N+31:32N+8], and the byte shifted out of the word above lands in the top
  byte of the word below.  MdeModulePkg's SdDxe and EmmcDxe depend on that
  framing: SdGetCsd and SdGetCid copy the four words one byte up into SD_CSD or
  SD_CID, which undoes exactly that shift.

  This controller leaves the response as four plain words instead - RESP0 holds
  bits 31:0 and RESP3 bits 127:96 - so the words are re-framed here, at the
  pass-thru boundary only.  The driver's own card code reads Command.Response
  as the plain words and is unaffected.

  Without this the CSD's structure field lands one byte off: an SDHC card is
  then read as a version 1 card and its capacity comes out as a few hundred
  sectors, which puts every partition beyond the end of the medium.

  @param[out] Block     Status block the caller receives.
  @param[in]  Response  The four response words as the controller returned them.
**/
STATIC
VOID
SmhcPassThruLongResponse (
  OUT EFI_SD_MMC_STATUS_BLOCK  *Block,
  IN  CONST UINT32             *Response
  )
{
  Block->Resp0 = (Response[0] >> 8) | (Response[1] << 24);
  Block->Resp1 = (Response[1] >> 8) | (Response[2] << 24);
  Block->Resp2 = (Response[2] >> 8) | (Response[3] << 24);

  //
  // The prefix byte is the start bit, the transmission bit and the six
  // reserved bits ahead of the 128-bit response, not part of the response
  // itself, and this controller does not capture it.  Supplying it keeps the
  // framing identical to the SDHCI one.  No consumer in MdeModulePkg reads
  // it: SdDxe and EmmcDxe copy only the low 15 bytes of the status block into
  // SD_CSD or SD_CID, which drops this byte.
  //
  Block->Resp3 = (Response[3] >> 8) | (0x3FU << 24);
}

/**
  Send SD/MMC command to the card in the given slot.

  The packet is completed before this function returns: the controller's
  completion is delivered by its interrupt into the command engine, which waits
  for it (bounded by a deadline that only ever reports a timeout), so there is
  nothing left to wait for later.  When the caller passed an event it is
  signalled on the way out and the function reports EFI_SUCCESS, which is the
  async half of the contract - the outcome of the packet itself is in
  TransactionStatus, and EmmcDxe's asynchronous path frees the request from its
  event callback, so a signalled event and a failed return status would be two
  answers to the same question.

  @param[in]     This    The protocol instance.
  @param[in]     Slot    Slot number; only slot 0 exists.
  @param[in,out] Packet  The command, its data and its result.
  @param[in]     Event   Signalled when the packet has completed, or NULL for
                         blocking I/O.

  @retval EFI_SUCCESS           The command was sent; see TransactionStatus.
  @retval EFI_INVALID_PARAMETER Slot, packet or buffer is not usable.
  @retval EFI_NO_MEDIA          No card in the slot.
  @retval EFI_BAD_BUFFER_SIZE   A single-block command's transfer is too large.
  @retval EFI_DEVICE_ERROR      The card or the bus failed the command.
  @retval EFI_TIMEOUT           The card did not answer in time.
**/
STATIC
EFI_STATUS
EFIAPI
SmhcPassThru (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL        *This,
  IN     UINT8                                Slot,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet,
  IN     EFI_EVENT                            Event    OPTIONAL
  )
{
  A733_SMHC_PRIVATE  *Private;
  A733_SMHC_COMMAND  Command;
  EFI_STATUS         Status;
  UINT32             Length;
  VOID               *Buffer;
  BOOLEAN            Read;
  UINT64             Timeout;

  if ((This == NULL) || (Packet == NULL) ||
      (Packet->SdMmcCmdBlk == NULL) || (Packet->SdMmcStatusBlk == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Slot != A733_SMHC_SLOT) {
    return EFI_INVALID_PARAMETER;
  }

  Private = A733_SMHC_PRIVATE_FROM_PASS_THRU (This);

  if ((Packet->InTransferLength != 0) && (Packet->OutTransferLength != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Read = Packet->InTransferLength != 0;
  if (Read) {
    Length = Packet->InTransferLength;
    Buffer = Packet->InDataBuffer;
  } else {
    Length = Packet->OutTransferLength;
    Buffer = Packet->OutDataBuffer;
  }

  if ((Length != 0) && (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // An eMMC device moves whole 512-byte blocks and the IDMAC's descriptors are
  // built from DWORD-aligned buffers; a length that is neither is a caller this
  // driver cannot serve rather than a bus problem.  (A buffer the descriptors
  // cannot describe is handled deeper down, by bouncing it through the DMA
  // pool, so nothing here depends on the caller's alignment.)
  //
  if ((Length % 4 != 0) || (Length % A733_SMHC_BLOCK_SIZE != 0)) {
    DEBUG ((DEBUG_ERROR, "Smhc: CMD%u asks for %u bytes, not a whole number of blocks\n", Packet->SdMmcCmdBlk->CommandIndex, Length));
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Command, sizeof (Command));
  Command.Index        = (UINT8)Packet->SdMmcCmdBlk->CommandIndex;
  Command.Argument     = Packet->SdMmcCmdBlk->CommandArgument;
  Command.CommandType  = Packet->SdMmcCmdBlk->CommandType;
  Command.ResponseType = Packet->SdMmcCmdBlk->ResponseType;
  Command.Buffer       = Buffer;
  Command.Length       = Length;
  Command.Read         = Read;

  if (Length != 0) {
    if ((Command.Index == A733_SMHC_CMD_READ_MULTIPLE) || (Command.Index == A733_SMHC_CMD_WRITE_MULTIPLE)) {
      //
      // A multi-block transfer counts bytes but is built from 512-byte blocks.
      //
      Command.BlockSize = A733_SMHC_BLOCK_SIZE;
    } else {
      //
      // A single-block data command (CMD8, CMD17, CMD24) moves its whole
      // length as one block, and SMHC_BLKSIZ is 16 bits wide.
      //
      if (Length > A733_SMHC_BLKSIZ_MASK) {
        return EFI_BAD_BUFFER_SIZE;
      }

      Command.BlockSize = Length;
    }
  }

  //
  // A caller's CMD12 stops a transfer in progress; the controller has a bit for
  // exactly that, and CMD12 is the only command that uses it.
  //
  Command.Abort = (Command.Index == A733_SMHC_CMD_STOP_TRANSMISSION);

  //
  // A multi-block transfer has to end in one of two ways, and the driver has to
  // pick the right one: CMD23 arms the card with a block count, and the card
  // then ends the transfer itself; without a count the card streams until
  // CMD12, which the controller has to send (STOP_CMD_FLAG).  EmmcDxe arms
  // CMD23 before every CMD18/CMD25, which is also why the stock SDHCI driver
  // enables Auto CMD12 for SD cards only.
  //
  if (((Command.Index == A733_SMHC_CMD_READ_MULTIPLE) || (Command.Index == A733_SMHC_CMD_WRITE_MULTIPLE)) &&
      !Private->CountArmed)
  {
    Command.AutoStop = TRUE;
  }

  //
  // The caller's timeout is a transfer budget, so a command gets a floor of its
  // own: a card that is thinking must not look like a card that is broken.
  //
  Timeout = Packet->Timeout;
  if (Timeout < A733_SMHC_CMD_TIMEOUT_US) {
    Timeout = A733_SMHC_CMD_TIMEOUT_US;
  }

  //
  // A card slot's bus is this driver's to arrange, because the block driver
  // above it does not: SdDxe sends no CMD55, ACMD6 or CMD6, and the address
  // ACMD6 needs is one SdDxe assigns with its own CMD3.  So the address is taken
  // from the CMD7 that selects the card, and the switch is made once, in front
  // of the first data command after it.  (An eMMC needs none of this: CMD1,
  // EXT_CSD and the ladder in A733SmhcCard.c set its bus during identification.)
  //
  if (Private->CardType == A733SmhcCardSd) {
    if ((Command.Index == A733_SMHC_CMD_SELECT_CARD) && (Command.Argument != 0)) {
      Private->Rca = (Command.Argument >> 16) & 0xFFFFU;
    }

    if ((Length != 0) &&
        ((Command.Index == A733_SMHC_CMD_READ_SINGLE_BLOCK) ||
         (Command.Index == A733_SMHC_CMD_READ_MULTIPLE) ||
         (Command.Index == A733_SMHC_CMD_WRITE_SINGLE_BLOCK) ||
         (Command.Index == A733_SMHC_CMD_WRITE_MULTIPLE)))
    {
      SmhcSdPrepareDataCommand (Private);
    }
  }

  //
  // CMD7 with RCA 0 deselects a card.  Before any addressed CMD7 there is
  // nothing selected, so the command has nothing to do and this card answers
  // it with silence; SdDxe opens every discovery with exactly that command.
  // Skipping it keeps the controller out of a one-second response timeout and
  // leaves the stand-by state clean for the CMD3/CMD9/CMD10 sequence that
  // follows.
  //
  if ((Private->CardType == A733SmhcCardSd) &&
      (Command.Index == A733_SMHC_CMD_SELECT_CARD) &&
      (Command.Argument == 0) && (Private->Rca == 0))
  {
    ZeroMem (Packet->SdMmcStatusBlk, sizeof (*Packet->SdMmcStatusBlk));
    Packet->TransactionStatus = EFI_SUCCESS;

    if (Event != NULL) {
      gBS->SignalEvent (Event);
    }

    return EFI_SUCCESS;
  }

  //
  // A data command that timed out is sent again.  This controller reports a
  // card still busy with the previous transfer as a response timeout, and the
  // retry is idempotent: a read fills the same buffer, a write sends the same
  // block with the same bytes.  The vendor driver retries on the same signal
  // (its RESP_TIMEOUT handling is built around exactly this behaviour).  A
  // failure that is not a timeout is left alone - the card answered something
  // wrong, and sending it again would only repeat that.
  //
  {
    UINTN  Attempt;

    for (Attempt = 0; ; Attempt++) {
      Status = SmhcExecuteCommand (Private, &Command, Timeout);
      if (!EFI_ERROR (Status) || (Status != EFI_TIMEOUT) ||
          (Length == 0) || (Attempt >= 2))
      {
        break;
      }

      //
      // Give the card time to release the line before it is addressed again.
      //
      MicroSecondDelay (2000);
    }
  }

  //
  // Remember whether the next command has a block count behind it.  Only CMD23
  // arms one, and any other command between them clears it.
  //
  Private->CountArmed = (Command.Index == A733_SMHC_CMD_SET_BLOCK_COUNT);

  if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR2) {
    //
    // CMD10 reads the CID again, addressed by RCA; CMD2 already read the same
    // register during bring-up.  This card ignores the addressed form now and
    // then, and failing the whole discovery over a value the driver already
    // has would cost the boot, so the copy CMD2 returned is used instead.
    //
    if ((Private->CardType == A733SmhcCardSd) &&
        (Command.Index == A733_SMHC_CMD_SEND_CID) &&
        EFI_ERROR (Status) && Private->SdCidValid)
    {
      DEBUG ((
        DEBUG_WARN,
        "Smhc: %a: CMD10 failed (%r); using the CID read by CMD2\n",
        Private->Name,
        Status
        ));
      SmhcPassThruLongResponse (Packet->SdMmcStatusBlk, Private->SdCid);
      Status = EFI_SUCCESS;
    } else {
      SmhcPassThruLongResponse (Packet->SdMmcStatusBlk, Command.Response);
    }
  } else {
    CopyMem (Packet->SdMmcStatusBlk, Command.Response, sizeof (Command.Response));
  }

  //
  // A failed command on the card slot is worth one line even in a quiet build:
  // the block driver above gives up on the whole card when an identification
  // command fails, and its own diagnostics do not say which command it was.
  //
  if (EFI_ERROR (Status) && (Private->CardType == A733SmhcCardSd) &&
      !((Command.Index == A733_SMHC_CMD_SELECT_CARD) && (Command.Argument == 0)))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Smhc: %a: CMD%u arg %08x failed: %r\n",
      Private->Name,
      Command.Index,
      Command.Argument,
      Status
      ));
  }

  Packet->TransactionStatus = Status;

  if (Event != NULL) {
    gBS->SignalEvent (Event);
    return EFI_SUCCESS;
  }

  return Status;
}

/**
  Used to retrieve next slot numbers supported by the controller.

  @param[in,out] This  The protocol instance.
  @param[in,out] Slot  On 0xFF in, the first slot; afterwards the next one.

  @retval EFI_SUCCESS            Slot 0 was returned.
  @retval EFI_NOT_FOUND          Slot 0 has already been returned.
  @retval EFI_INVALID_PARAMETER  Slot is neither 0xFF nor the returned slot.
**/
STATIC
EFI_STATUS
EFIAPI
SmhcGetNextSlot (
  IN OUT EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN OUT UINT8                          *Slot
  )
{
  if ((This == NULL) || (Slot == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (*Slot == 0xFF) {
    *Slot = A733_SMHC_SLOT;
    return EFI_SUCCESS;
  }

  if (*Slot == A733_SMHC_SLOT) {
    return EFI_NOT_FOUND;
  }

  return EFI_INVALID_PARAMETER;
}

/**
  Allocate a device path node for the card in a slot.

  The node's subtype is the card type the controller's board entry names:
  MSG_EMMC_DP for an eMMC, MSG_SD_DP for an SD card.  That subtype is what
  EmmcDxe and SdDxe recognise a slot by - each refuses a node of the other type
  and stops enumerating the handle there - so it is the driver's whole answer to
  "who owns this controller", and it comes from the table rather than from the
  build.

  @param[in]  This        The protocol instance.
  @param[in]  Slot        Slot number.
  @param[out] DevicePath  Receives the node, which the caller frees.

  @retval EFI_SUCCESS           The node was built.
  @retval EFI_INVALID_PARAMETER The slot does not exist, or DevicePath is NULL.
  @retval EFI_OUT_OF_RESOURCES  No room for the node.
**/
STATIC
EFI_STATUS
EFIAPI
SmhcBuildDevicePath (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN  UINT8                          Slot,
  OUT EFI_DEVICE_PATH_PROTOCOL       **DevicePath
  )
{
  A733_SMHC_PRIVATE  *Private;
  EMMC_DEVICE_PATH   *Node;

  if ((This == NULL) || (DevicePath == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Slot != A733_SMHC_SLOT) {
    return EFI_INVALID_PARAMETER;
  }

  Private = A733_SMHC_PRIVATE_FROM_PASS_THRU (This);

  //
  // SD_DEVICE_PATH and EMMC_DEVICE_PATH have the same shape - a header and a slot
  // number - which is why one of them can carry either subtype.
  //
  Node = AllocateZeroPool (sizeof (EMMC_DEVICE_PATH));
  if (Node == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Node->Header.Type    = MESSAGING_DEVICE_PATH;
  Node->Header.SubType = SmhcCardDpSubtype (Private->CardType);
  SetDevicePathNodeLength (&Node->Header, sizeof (EMMC_DEVICE_PATH));
  Node->SlotNumber = Slot;

  *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Node;

  return EFI_SUCCESS;
}

/**
  Retrieve the slot a device path node names.

  The node has to be this controller's card type: a node of the other subtype
  belongs to the other kind of card driver and is not this handle's.

  @param[in]  This        The protocol instance.
  @param[in]  DevicePath  A card node.
  @param[out] Slot        Receives the slot number.

  @retval EFI_SUCCESS           Slot was returned.
  @retval EFI_INVALID_PARAMETER This or DevicePath is NULL.
  @retval EFI_UNSUPPORTED       The node is not this driver's node type.
**/
STATIC
EFI_STATUS
EFIAPI
SmhcGetSlotNumber (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN  EFI_DEVICE_PATH_PROTOCOL       *DevicePath,
  OUT UINT8                          *Slot
  )
{
  A733_SMHC_PRIVATE  *Private;

  if ((This == NULL) || (DevicePath == NULL) || (Slot == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = A733_SMHC_PRIVATE_FROM_PASS_THRU (This);

  if ((DevicePath->Type != MESSAGING_DEVICE_PATH) ||
      (DevicePath->SubType != SmhcCardDpSubtype (Private->CardType)) ||
      (DevicePathNodeLength (DevicePath) != sizeof (EMMC_DEVICE_PATH)))
  {
    return EFI_UNSUPPORTED;
  }

  if (((EMMC_DEVICE_PATH *)DevicePath)->SlotNumber != A733_SMHC_SLOT) {
    return EFI_NOT_FOUND;
  }

  *Slot = A733_SMHC_SLOT;
  return EFI_SUCCESS;
}

/**
  Reset the card in a slot.

  The block layer's own Reset reaches this, so it has to leave the card in the
  state the caller carries on from: an eMMC through its identification and EXT_CSD
  ladder, an SD card through the slot's identification followed by the address,
  selection and data bus the block driver expects.  A slot path that assumed
  EXT_CSD would be resetting a card that has no such register.

  @param[in] This  The protocol instance.
  @param[in] Slot  Slot number.

  @retval EFI_SUCCESS           The card is identified and selected again.
  @retval EFI_INVALID_PARAMETER The slot does not exist.
  @retval EFI_NO_MEDIA          No card in the slot.
  @retval other                 The card did not come back.
**/
STATIC
EFI_STATUS
EFIAPI
SmhcResetDevice (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN UINT8                          Slot
  )
{
  A733_SMHC_PRIVATE  *Private;
  EFI_STATUS         Status;

  if ((This == NULL) || (Slot != A733_SMHC_SLOT)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = A733_SMHC_PRIVATE_FROM_PASS_THRU (This);

  if (!SmhcCardPresent (Private->Base)) {
    return EFI_NO_MEDIA;
  }

  //
  // A slot is reset through its own path: the eMMC's re-identification walks
  // CMD1 and the EXT_CSD ladder, and an SD card has neither.
  //
  if (Private->CardType == A733SmhcCardSd) {
    Status = SmhcCardResetSd (Private);
  } else {
    Status = SmhcCardReset (Private);
  }

  DEBUG ((DEBUG_INFO, "Smhc: device reset: %r\n", Status));

  return Status;
}

/**
  Bring one controller and the card on it up, then publish its pass-thru handle.

  The order matters and is the driver's real dependency: the completion event
  exists before the interrupt handler is registered because the handler signals
  it, the handler exists before the first command because from then on the
  interrupt is what reports that a command finished, and the DMA pool exists
  before identification because the EXT_CSD read at the end of it is a data
  command like any other.  Everything this allocated is released again on the
  paths where the driver does not publish a protocol, and the interrupt handler
  and the DMA pool are released separately so that a failure in either is not
  left half-done.

  A card that is not there, or that does not identify, is not a failure: no
  protocol is installed for this controller, one DEBUG line says so, and the
  caller carries on with the next entry.  There is nothing for this image to do
  about a controller with no card on it, and hanging or failing the dispatch
  would take the console down with it.

  @param[in] Controller  The controller to bring up, from the board's table.

  @retval EFI_SUCCESS  Either the protocol is installed, or the controller came
                       up and simply has no usable card this boot.
  @retval other        The controller could not be brought up; nothing was
                       published for it, and it released what it had taken.
**/
STATIC
EFI_STATUS
SmhcStartController (
  IN CONST A733_SMHC_CONTROLLER  *Controller
  )
{
  EFI_STATUS                Status;
  A733_SMHC_PRIVATE         *Private;
  EFI_HANDLE                Handle;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  A733_SMHC_RATE            Rate;

  Private = AllocateZeroPool (sizeof (*Private));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Everything this instance needs to know about its controller comes from the
  // board's table entry: no part of the driver is specialised for one of them.
  // The interrupt and the CCU module in particular are per-controller facts, and
  // reading another controller's would be a measurement of hardware the card is
  // not on.
  //
  Private->Name                     = Controller->Name;
  Private->Base                     = Controller->Base;
  Private->Spi                      = Controller->Spi;
  Private->CcuModule                = Controller->CcuModule;
  Private->CcuClkReg                = Controller->CcuClkReg;
  Private->PadFunction              = Controller->PadFunction;
  Private->MaxBusWidth              = Controller->MaxBusWidth;
  Private->Removable                = Controller->Removable;
  Private->CardType                 = Controller->CardType;
  Private->ClockProfile             = Controller->ClockProfile;
  Private->BusWidth                 = A733_SMHC_BUS_1BIT;
  Private->PassThru.IoAlign         = 0;
  Private->PassThru.PassThru        = SmhcPassThru;
  Private->PassThru.GetNextSlot     = SmhcGetNextSlot;
  Private->PassThru.BuildDevicePath = SmhcBuildDevicePath;
  Private->PassThru.GetSlotNumber   = SmhcGetSlotNumber;
  Private->PassThru.ResetDevice     = SmhcResetDevice;

  DEBUG ((
    DEBUG_INFO,
    "Smhc: starting controller %a at 0x%08x (SPI %u, card type %a, %u-bit, %a)\n",
    Private->Name,
    (UINT32)Private->Base,
    Private->Spi,
    SmhcCardTypeName (Private->CardType),
    Private->MaxBusWidth,
    Private->Removable ? "removable" : "fixed"
    ));

  Status = SmhcBoardBringUp (Controller);
  if (EFI_ERROR (Status)) {
    FreePool (Private);
    return Status;
  }

  //
  // The clock plan is chosen here and applied once: it sets the module clock the
  // whole run uses and computes both controller dividers from it, and every card
  // rate the driver states afterwards comes out of the state this fills in (see
  // A733_SMHC_PLAN_DEFAULT).  It reports the CCU register as it reads back and
  // the rate the library decodes from it, which is what a throughput measurement
  // is checked against.
  //
  Status = SmhcSelectClockPlan (Private, A733_SMHC_PLAN_DEFAULT);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot set the %a clock plan: %r\n", Private->Name, Status));
    FreePool (Private);
    return Status;
  }

  //
  // Deliver the plan's identification clock to the controller before the first
  // command.  SMHC_CLKDIV holds what the previous stage left in it - boot0 never
  // writes the register and its reset value is 0, i.e. the whole module clock -
  // so without this the 74-clock initialization sequence CMD0 sends would run at
  // the module clock, in the one phase the eMMC spec requires to run at fOD.
  // Bounding the card's clock here also means a module clock whose rate the CCU
  // cannot report never reaches the card at its full rate, not even for the
  // length of one command.
  //
  SmhcSetClock (Private, Private->IdentDivisor);

  DEBUG ((
    DEBUG_INFO,
    "Smhc: %a at 0x%08x (SPI %u, CCU module %u, CLK_REG 0x%03x, pad function %u)\n",
    Private->Name,
    (UINT32)Private->Base,
    Private->Spi,
    Private->CcuModule,
    Private->CcuClkReg,
    Private->PadFunction
    ));

  //
  // Both SMHC clock registers, next to each other: this driver programs only the
  // one belonging to the controller it drives, and on SMHC3 that write does not
  // read back while the card still measures a ~50 MHz bus clock.  Printing the
  // pair says whether the rate came from this register at all or is left over in
  // the neighbour's.
  //
  DEBUG ((
    DEBUG_INFO,
    "Smhc: CCU SMHC2_CLK %08x SMHC2_BGR %08x | SMHC3_CLK %08x SMHC3_BGR %08x\n",
    MmioRead32 ((UINTN)PcdGet64 (PcdA733CcuBase) + A733_CCU_SMHC2_CLK),
    MmioRead32 ((UINTN)PcdGet64 (PcdA733CcuBase) + A733_CCU_SMHC2_BGR),
    MmioRead32 ((UINTN)PcdGet64 (PcdA733CcuBase) + A733_CCU_SMHC3_CLK),
    MmioRead32 ((UINTN)PcdGet64 (PcdA733CcuBase) + A733_CCU_SMHC3_BGR)
    ));

  //
  // The controller's version decides whether a descriptor address is written as
  // a word address (see A733_SMHC_DMA_ADDRESS_SHIFT), and this driver always
  // writes it as one.  It is printed once per controller, first, because it is
  // the one fact about that convention a run without a working data path can
  // still report: a controller whose version is below the rule would need byte
  // addresses, and a large transfer could fail on the shift alone.  The NDMA
  // mode register is printed with it because it is one of the registers this
  // driver never writes, so a value that differs between controllers is a
  // controller-side difference rather than something this driver set.
  //
  DEBUG ((
    DEBUG_INFO,
    "Smhc: %a controller version %06x, ndma_ctl %08x; descriptor addresses are written as word addresses, which this family requires at version >= %05x: %a\n",
    Private->Name,
    SmhcRead (Private->Base, A733_SMHC_VERSION) & A733_SMHC_VERSION_MASK,
    SmhcRead (Private->Base, A733_SMHC_NDMA_MODE_CTL),
    A733_SMHC_VERSION_WORD_ADDRESS,
    ((SmhcRead (Private->Base, A733_SMHC_VERSION) & A733_SMHC_VERSION_MASK) >= A733_SMHC_VERSION_WORD_ADDRESS)
      ? "this controller is at or above it, so the shift matches its convention"
      : "this controller is BELOW it, so the word-address shift may be wrong here"
    ));

  if (!SmhcCardWaitPresent (Private->Base, A733_SMHC_CD_SETTLE_US)) {
    //
    // DATA[3] read low for the whole settling budget, so there is certainly
    // nothing in the slot.  (The other way round is not as sharp: every data
    // line is pulled up, so an empty slot can still read high, and
    // identification is what settles that case.)
    //
    DEBUG ((DEBUG_INFO, "Smhc: no card on %a (STATUS.DATA[3] is low), pass-thru not installed\n", Private->Name));
    FreePool (Private);
    return EFI_SUCCESS;
  }

  //
  // A plain event, with no notify function: the interrupt handler signals it and
  // the command engine consumes the signal with CheckEvent, which is what makes
  // the handler's work - recording the status - visible without the handler
  // having to run any part of the driver itself.
  //
  Status = gBS->CreateEvent (0, TPL_CALLBACK, NULL, NULL, &Private->CompletionEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot create the completion event: %r\n", Status));
    FreePool (Private);
    return Status;
  }

  Status = SmhcInstallInterrupt (Private);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (Private->CompletionEvent);
    FreePool (Private);
    return Status;
  }

  Status = SmhcDmaAllocatePool (Private);
  if (EFI_ERROR (Status)) {
    SmhcUninstallInterrupt (Private);
    gBS->CloseEvent (Private->CompletionEvent);
    FreePool (Private);
    return Status;
  }

  //
  // SmhcCardInit prepares the controller itself, and every round of the
  // identification loop re-prepares it, so a retry after a failure starts from a
  // clean controller without a second copy of that preparation here.
  //
  // Which card path runs is the board's answer, not a build switch: an eMMC entry
  // goes through the eMMC identification, and a slot entry goes through the SD
  // path, which identifies the card and leaves it in ident for SdDxe - the eMMC
  // path would send CMD1 and CMD2 to a card that does not answer them, and the
  // slot's ladder would then be a walk through EXT_CSD an SD card does not have.
  //
  if (Private->CardType == A733SmhcCardSd) {
    Status = SmhcCardInitSd (Private);
  } else {
    Status = SmhcCardInit (Private);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "Smhc: no usable %a on %a (%r), pass-thru not installed\n",
      SmhcCardTypeName (Private->CardType),
      Private->Name,
      Status
      ));
    SmhcDmaFreePool (Private);
    SmhcUninstallInterrupt (Private);
    gBS->CloseEvent (Private->CompletionEvent);
    FreePool (Private);
    return EFI_SUCCESS;
  }

  DevicePath = SmhcBuildControllerDevicePath (Private->Name);
  if (DevicePath == NULL) {
    SmhcDmaFreePool (Private);
    SmhcUninstallInterrupt (Private);
    gBS->CloseEvent (Private->CompletionEvent);
    FreePool (Private);
    return EFI_OUT_OF_RESOURCES;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  DevicePath,
                  &gEfiSdMmcPassThruProtocolGuid,
                  &Private->PassThru,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Smhc: cannot install the pass-thru protocol: %r\n", Status));
    FreePool (DevicePath);
    SmhcDmaFreePool (Private);
    SmhcUninstallInterrupt (Private);
    gBS->CloseEvent (Private->CompletionEvent);
    FreePool (Private);
    return Status;
  }

  //
  // The measurement the whole speed-up is judged by, taken last and against the
  // clock plan as the card ends in it: reads through the driver's own command
  // path, with the module clock and the timing mode it ran at stated on the
  // same line.
  //
  // It runs unconditionally on the eMMC - and not at all on a slot: the card
  // there has no address yet (SdDxe assigns it), so a read here would be a read
  // from a card in ident, and the line it produces belongs to the first data
  // command the block driver makes instead.
  //
  // The per-command dump does not run: that dump is about 460 characters per
  // command, which at 115200 baud is 40 ms, and it sat inside the timed window
  // of every measurement taken until the phase trace showed it.  A number taken
  // with the dump on is a measurement of the UART.
  //
  if (Private->CardType != A733SmhcCardSd) {
    Status = SmhcMeasureRate (Private, &Rate);
    SmhcReportHs200 (Private, &Rate);
  }

  DEBUG ((
    DEBUG_INFO,
    "Smhc: EFI_SD_MMC_PASS_THRU_PROTOCOL installed on slot %u for %a\n",
    A733_SMHC_SLOT,
    (Private->CardType == A733SmhcCardSd) ? "SdDxe" : "EmmcDxe"
    ));

  return EFI_SUCCESS;
}

/**
  The user entry point: bring every controller the board declares up.

  The board supplies the table (A733SmhcBoardControllers) and this walks it, one
  pass-thru handle per entry.  One handle per controller is the unit
  MdeModulePkg needs: a handle carries one card type, because EmmcDxe and SdDxe
  each stop enumerating a handle's slots at the first slot that is not theirs.

  The entries are independent.  A controller that cannot be brought up is
  logged, has released everything it took, and is left out; the next entry is
  still tried.  The dispatch fails only when no controller came up at all, and
  then with that last controller's status - so a single-controller board behaves
  exactly as it did when its controller was a build-time constant, including the
  case where the card is absent (that is not an error: one line says so and no
  protocol is installed).

  @param[in] ImageHandle  The image's handle.
  @param[in] SystemTable  The system table.

  @retval EFI_SUCCESS  The driver did what it could with the hardware it found.
  @retval other        No controller could be brought up; the last failure.
**/
//
// ExitBootServices: leave the controllers quiet for the OS.
//
// The driver leaves interrupts masked-on and the command/DMA engine as the
// last transfer left it; the board's eMMC then reaches the kernel with a live
// IDMAC and the event enables the kernel's own init has not touched yet.  On
// this board the kernel's probe reports "smc 0 p2 err, DCE" and a CMD12
// timeout during its first card init, then recovers by disabling the command
// queue - the controller works, but it starts from a state nobody designed.
//
// Stop the interrupt and DMA enables and pulse the FIFO/DMA/controller
// resets, so the kernel initialises the block from its documented reset
// state.  The card is left alone: the OS resets and re-identifies it itself,
// and a card reset here would only change what it finds.
//
// Boot services are already gone when this runs: raw MMIO, bounded polls,
// no DEBUG.
//
STATIC
VOID
EFIAPI
A733SmhcExitBootServicesPark (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CONST A733_SMHC_CONTROLLER  *Controllers;
  UINT32                       Count;
  UINT32                       Index;
  UINTN                        Base;
  UINT32                       Value;
  UINTN                        Try;

  Controllers = A733SmhcBoardControllers (&Count);
  if (Controllers == NULL) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Base = Controllers[Index].Base;

    SmhcWrite (Base, A733_SMHC_INTMASK, 0);

    Value  = SmhcRead (Base, A733_SMHC_CTRL);
    Value &= ~(UINT32)(A733_SMHC_CTRL_DMA_ENB | A733_SMHC_CTRL_INT_ENB);
    SmhcWrite (Base, A733_SMHC_CTRL, Value);

    Value |= A733_SMHC_CTRL_FIFO_RST | A733_SMHC_CTRL_DMA_RST | A733_SMHC_CTRL_SOFT_RST;
    SmhcWrite (Base, A733_SMHC_CTRL, Value);

    for (Try = 0; Try < 1000; Try++) {
      if ((SmhcRead (Base, A733_SMHC_CTRL) &
           (A733_SMHC_CTRL_FIFO_RST | A733_SMHC_CTRL_DMA_RST | A733_SMHC_CTRL_SOFT_RST)) == 0)
      {
        break;
      }
      MicroSecondDelay (10);
    }

  }
}

EFI_STATUS
EFIAPI
A733SmhcDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST A733_SMHC_CONTROLLER  *Controllers;
  UINT32                      Count;
  UINT32                      Index;
  UINT32                      Started;
  EFI_STATUS                  Status;
  EFI_STATUS                  LastStatus;

  Controllers = A733SmhcBoardControllers (&Count);
  if ((Controllers == NULL) || (Count == 0)) {
    DEBUG ((DEBUG_ERROR, "Smhc: the board declares no SMHC controller\n"));
    return EFI_NOT_FOUND;
  }

  Started    = 0;
  LastStatus = EFI_SUCCESS;

  for (Index = 0; Index < Count; Index++) {
    Status = SmhcStartController (&Controllers[Index]);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "Smhc: controller %a left without a card: %r\n",
        Controllers[Index].Name,
        Status
        ));
      LastStatus = Status;
    } else {
      Started++;
    }
  }

  //
  // Park the controllers at ExitBootServices (see the callback).
  //
  {
    EFI_EVENT   ExitBsEvent;

    Status = gBS->CreateEvent (
                    EVT_SIGNAL_EXIT_BOOT_SERVICES,
                    TPL_CALLBACK,
                    A733SmhcExitBootServicesPark,
                    NULL,
                    &ExitBsEvent
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "Smhc: EBS park event: %r\n", Status));
    }
  }

  return (Started != 0) ? EFI_SUCCESS : LastStatus;
}
