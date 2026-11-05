/** @file
  The SMHC controllers a board has, as the data A733SmhcDxe is driven from.

  This is the boundary between the silicon driver and the board: the driver owns
  the controller's registers, the card protocols and the pass-thru protocol,
  and everything that is a property of *this* board - which controllers are
  wired, at which base, on which interrupt, out of which CCU module, through
  which pads, and what kind of card sits on each - comes from the table this
  header describes.  A different board supplies a different table and changes
  nothing in the driver.

  Two rules shape the table, and both come from MdeModulePkg:

  - One table entry is one controller, and the driver publishes one pass-thru
    handle per entry.  It has to be that way round because EmmcDxe and SdDxe
    each stop enumerating a handle's slots at the first slot that is not theirs
    (EmmcDxe.c: the SlotNumber check that returns EFI_UNSUPPORTED and ends the
    caller's loop; SdDxe.c the same).  A handle may therefore carry slots of one
    card type only, so a board whose eMMC and SD card are on different
    controllers publishes two handles, not one handle with two slots.

  - Card type is signalled only through the device path subtype BuildDevicePath
    returns: MSG_EMMC_DP (0x1D) for an eMMC, MSG_SD_DP (0x1A) for an SD card,
    both from MdePkg's Protocol/DevicePath.h.  That subtype is what makes
    EmmcDxe or SdDxe claim the handle, so CardType below is not a hint - it is
    the driver's answer to "who owns this controller".

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_SMHC_BOARD_H__
#define A733_SMHC_BOARD_H__

#include <Uefi.h>

#include <Library/A733GpioLib.h>

/**
  What kind of card a controller carries.  It selects the device path subtype
  the driver's BuildDevicePath reports, and nothing else: an eMMC is identified
  by A733SmhcCard.c whatever the controller, and a card type this driver has no
  identification for is simply not brought up.
**/
typedef enum {
  A733SmhcCardEmmc = 0,
  A733SmhcCardSd
} A733_SMHC_CARD_TYPE;

/**
  Which family of clock plans a controller can be run on.

  The CCU does not offer every module the same parents: SMHC2/SMHC3 can take
  PERI0_800M, while SMHC0/SMHC1 have no 800 MHz parent at all - their class table
  (A733CcuLib's mSmhc01NandSources) stops at PERI0_400M.  A plan naming a parent
  the module cannot reach is refused by the CCU, so the plan tables are per family
  and this field says which one a controller's plans are drawn from.

  It is a property of the controller, not of the card: a card type does not decide
  a parent, the module does.  A board that wired an eMMC to SMHC0 would still take
  the SD profile's parents, because SMHC0 has no others - which is why the profile
  is stated per controller here rather than derived from CardType.
**/
typedef enum {
  A733SmhcClockProfileMmc = 0,
  A733SmhcClockProfileSd
} A733_SMHC_CLOCK_PROFILE;

/**
  One pin of a controller's bus: the pin, the signal it carries, and the pull
  the bus wants on it.  The signal is for the log - a pin that cannot be muxed is
  named by what it does on the bus rather than by its port and bit.
**/
typedef struct {
  UINT32         Pin;       // A733_GPIO_PIN (Bank, Bit)
  CONST CHAR8    *Signal;   // The bus signal it carries: "CLK", "D0", ...
  A733_GPIO_PULL Pull;
} A733_SMHC_PAD;

/**
  One controller the board wires.

  Base, Spi, CcuModule, CcuClkReg and PadFunction name the same controller from
  five angles - the register window, the GIC's interrupt, the CCU's gate and
  clock, and the pad mux - and they are per-controller facts rather than
  constants of the driver: on this SoC two SMHC controllers can reach the same
  card through the same pads (port C carries SDC2 on ALT3 and SDC3 on ALT4), so
  nothing may be assumed from the base address.

  MaxBusWidth is the widest bus the board's wiring supports, which is not the
  same question as the widest the controller has.  Removable says whether the
  card may leave the slot; a soldered eMMC does not.

  ClockProfile selects which of the driver's clock plan tables this controller's
  plans are read from (see A733_SMHC_CLOCK_PROFILE).
**/
typedef struct {
  CONST CHAR8              *Name;          // "SMHC3", "SMHC0"
  UINTN                    Base;           // 0x04023000
  UINT32                   Spi;            // 196
  UINT32                   CcuModule;      // A733_CCU_MODULE_SMHC3
  UINT32                   CcuClkReg;      // A733_CCU_SMHC3_CLK
  UINT32                   PadFunction;    // A733_GPIO_FUNC_ALT4
  CONST A733_SMHC_PAD      *Pads;
  UINT32                   PadCount;
  UINT32                   MaxBusWidth;    // 8
  BOOLEAN                  Removable;      // FALSE
  A733_SMHC_CARD_TYPE      CardType;       // A733SmhcCardEmmc
  A733_SMHC_CLOCK_PROFILE  ClockProfile;   // A733SmhcClockProfileMmc
} A733_SMHC_CONTROLLER;

/**
  The board's controllers, in the order the driver should bring them up.

  @param[out] Count  Receives the number of entries in the returned table.

  @return The table, which is static and must not be freed.  It holds at least
          one entry for any board that has a controller at all.
**/
CONST A733_SMHC_CONTROLLER *
A733SmhcBoardControllers (
  OUT UINT32  *Count
  );

#endif