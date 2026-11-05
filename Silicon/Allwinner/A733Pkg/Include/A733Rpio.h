/** @file
  R_PIO (S_GPIO) pin-controller map for the Allwinner A733 (sun60iw2p1).

  The R_PIO domain is a separate block from the main pin controller modelled
  by A733Gpio.h.  Its layout is also different: there is no leading global
  block, the first bank (PL) starts at +0x0000, and the per-bank registers are
  packed rather than 0x80 apart.

  Bank offset arithmetic, and where it comes from: the manual's memory map
  gives S_GPIO 0x07025000---0x07026FFF (8K), and section 18.14.5.2 "S_GPIO
  Register List" pins the block base at 0x0702_5000 and lists, in order,
  PL_CFG0 at 0x0000, PL_CFG1 at 0x0004, PL_DAT at 0x0010, PL_DRV0/1 at
  0x0014/0x0018, PL_PUL0 at 0x0024, then PM_CFG0 at 0x0030.  So the first
  bank begins at +0x0000 and the stride to the next bank is 0x0030 - the
  stride is the distance from PL_CFG0 to PM_CFG0, read straight off that list,
  not copied from the main PIO's 0x80.

  The port table (section 18.14, "Port L Multiplex Function") gives PL0
  function 2 = S-TWI0-SCK and PL1 function 2 = S-TWI0-SDA.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_RPIO_H__
#define A733_RPIO_H__

#include <Uefi.h>

//
// Manual: memory map, CPUS subsystem (0x07025000---0x07026FFF, 8K).  Exposed
// for the platform's MMU map so a mistyped size cannot leave the block
// unmapped.
//
#define A733_RPIO_BASE        0x07025000UL
#define A733_RPIO_WINDOW_SIZE  0x2000U

//
// the A733 user manual, bank offsets inside the S_GPIO block, and the packed
// per-bank layout.  Only cfg is used here.
//
#define A733_RPIO_PL_BANK_OFFSET  0x0000U
#define A733_RPIO_PM_BANK_OFFSET  0x0030U
#define A733_RPIO_BANK_STRIDE     0x0030U
#define A733_RPIO_CFG_OFFSET      0x0000U
//
// PL_DAT at +0x0010 (PM_DAT at +0x0040, i.e. the same offset one bank in).
// Unlike the main pin controller, the S_GPIO block has no dat_set/dat_clr
// aliases: the manual's register list goes PL_CFG0/1, PL_DAT, PL_DRV0/1,
// PL_PUL0, so a level is changed by read-modify-writing this word.
//
#define A733_RPIO_DAT_OFFSET      0x0010U

//
// Pins per bank, and a pin's bank and bit slot (same arithmetic as the main
// PIO: bank * 32 + bit).  Bank 0 is PL, bank 1 is PM.
//
#define A733_RPIO_PINS_PER_BANK  32U
#define A733_RPIO_PIN(Bank, Pin)  ((UINT32)(Bank) * A733_RPIO_PINS_PER_BANK + (UINT32)(Pin))
#define A733_RPIO_BANK_OF(Pin)    ((UINT32)(Pin) / A733_RPIO_PINS_PER_BANK)
#define A733_RPIO_NUM_OF(Pin)     ((UINT32)(Pin) % A733_RPIO_PINS_PER_BANK)
#define A733_RPIO_PIN_MAX         (2U * A733_RPIO_PINS_PER_BANK)

//
// Manual: port table, Port L multiplex functions.  Function 2 is the S-TWI0
// pairing on PL0/PL1.  The 4-bit selector encoding matches the main PIO:
// 0 input, 1 output, 2..14 alternate functions, 15 disable.
//
#define A733_RPIO_PL0           A733_RPIO_PIN (0, 0)
#define A733_RPIO_PL1           A733_RPIO_PIN (0, 1)
#define A733_RPIO_PL2           A733_RPIO_PIN (0, 2)
#define A733_RPIO_PM5           A733_RPIO_PIN (1, 5)
#define A733_RPIO_FUNC_STWI0    2U
#define A733_RPIO_FUNC_OUTPUT   1U
#define A733_RPIO_FUNC_MASK     0xFU

#endif
