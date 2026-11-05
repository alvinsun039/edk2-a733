/** @file
  The Radxa Cubie A7A's SMHC controller table.

  Two entries: the eMMC, on SMHC3, and the microSD slot, on SMHC0.

  The board's eMMC is on the port C pads, and those pads carry two controller
  functions - ALT3 selects SDC2 (SMHC2, 0x04022000), ALT4 selects SDC3 (SMHC3,
  0x04023000) - so which controller reaches the card is a pad-mux decision rather
  than a wiring one.  This board file takes SMHC3, the one the driver's HS400 path
  was measured on; SMHC2 is the same card through the other pad function and
  differs from this entry in Base, Spi, CcuModule, CcuClkReg and PadFunction, and
  in nothing else.

  Values that are not obvious from the names:

  - The interrupt: the manual's interrupt table numbers the SMHC controllers
    193..196, and the vendor device tree's GIC_SPI numbers are 32 lower (sdc0 is
    GIC_SPI 161), i.e. the same row read from the other end.  SMHC0 raises 193,
    SMHC2 195 and SMHC3 196.

  - The CCU: the module gate/reset register is 0x0D00 for SMHC0, 0x0D20 for
    SMHC2 and 0x0D30 for SMHC3, and the clock register named here is the one
    SmhcSelectClockPlan writes and reads back.

  - The clock profile: SMHC3 can take PERI0_800M, SMHC0 cannot - A733CcuLib's
    class table for SMHC0/SMHC1 (mSmhc01NandSources) stops at PERI0_400M - so the
    SD entry asks for the SD plan table and the eMMC entry for the MMC one.

  - The pads: the eMMC is on port C, function ALT4.  The data lines are not in
    order on the port - PC8 is D3 and PC10 is D0 - which is why each pin states
    the signal it carries.  The set is what a register dump of a booted board
    confirmed: these twelve pins pulled up at drive strength 1, except PC0 (DS)
    pulled down; PC12 is not part of the controller's pin group (the eighth data
    line, D7, is PC16).

  - The SD slot is on port F, PF0..PF5, six pins: D1, CMD, CLK, D0, D3, D2, all
    pulled up, four-bit bus.  As on port C the pins are not in signal order, so
    each one names what it carries rather than being read off its bit number.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733GpioLib.h>
#include <Library/A733SmhcBoardLib.h>

#include <A733Ccu.h>
#include <A733Smhc.h>

STATIC CONST A733_SMHC_PAD  mA733Smhc2Pins[] = {
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 0),  "DS",  A733_GPIO_PULL_DOWN },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 1),  "RST", A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 5),  "CLK", A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 6),  "CMD", A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 8),  "D3",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 9),  "D4",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 10), "D0",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 11), "D5",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 13), "D1",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 14), "D6",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 15), "D2",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_C, 16), "D7",  A733_GPIO_PULL_UP }
};

STATIC CONST A733_SMHC_PAD  mA733Smhc0Pins[] = {
  { A733_GPIO_PIN (A733_GPIO_BANK_F, 0), "D1",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_F, 1), "CMD", A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_F, 2), "CLK", A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_F, 3), "D0",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_F, 4), "D3",  A733_GPIO_PULL_UP },
  { A733_GPIO_PIN (A733_GPIO_BANK_F, 5), "D2",  A733_GPIO_PULL_UP }
};

STATIC CONST A733_SMHC_CONTROLLER  mA733SmhcControllers[] = {
  {
    "SMHC3",                                        // Name
    A733_SMHC3_BASE,                                // Base
    196,                                            // Spi
    A733_CCU_MODULE_SMHC3,                          // CcuModule
    A733_CCU_SMHC3_CLK,                             // CcuClkReg
    A733_GPIO_FUNC_ALT4,                            // PadFunction
    mA733Smhc2Pins,                                 // Pads
    (UINT32)ARRAY_SIZE (mA733Smhc2Pins),            // PadCount
    8,                                              // MaxBusWidth
    FALSE,                                          // Removable
    A733SmhcCardEmmc,                               // CardType
    A733SmhcClockProfileMmc                         // ClockProfile
  },
  {
    "SMHC0",                                        // Name
    0x04020000,                                     // Base
    193,                                            // Spi
    A733_CCU_MODULE_SMHC0,                          // CcuModule
    A733_CCU_SMHC0_CLK,                             // CcuClkReg
    A733_GPIO_FUNC_ALT2,                            // PadFunction
    mA733Smhc0Pins,                                 // Pads
    (UINT32)ARRAY_SIZE (mA733Smhc0Pins),            // PadCount
    4,                                              // MaxBusWidth
    TRUE,                                           // Removable
    A733SmhcCardSd,                                 // CardType
    A733SmhcClockProfileSd                          // ClockProfile
  }
};

/**
  The board's controllers, in the order the driver should bring them up.

  @param[out] Count  Receives the number of entries in the returned table.

  @return The table, which is static and must not be freed.
**/
CONST A733_SMHC_CONTROLLER *
A733SmhcBoardControllers (
  OUT UINT32  *Count
  )
{
  if (Count != NULL) {
    *Count = (UINT32)ARRAY_SIZE (mA733SmhcControllers);
  }

  return mA733SmhcControllers;
}