/** @file
  GPIO / pin controller register map for the Allwinner A733 (sun60iw2p1).

  The block uses the vendor's "AW_GPIO_V3" layout.  Every offset below was
  checked against the A733 user manual's GPIO chapter (chapter 18, section
  18.14: the register list in 18.14.5.1 and the register descriptions in
  18.14.6) and against the vendor register header's V3 layout and pinctrl
  driver; the three agree.

  The manual's memory map gives GPIO the 0x02000000..0x02001FFF (8 KB) window.
  Inside it, the global block sits first and the per-bank blocks follow, one
  0x80-byte block per bank, starting at +0x80 with the first bank:

    bank n block base = A733_GPIO_PIO_OFFSET + n * A733_GPIO_BANK_STRIDE

  Bank A (n = 0) therefore starts at +0x80 and bank B (n = 1) at +0x100, which
  is exactly where the manual puts PB_CFG0, so the two agree.  (The vendor
  header calls +0x80 the "offset for virtual PA port": the SoC's first real
  bank lands one block in.)

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_GPIO_H__
#define A733_GPIO_H__

#include <Uefi.h>

//
// Manual: memory map, GPIO window (0x02000000..0x02001FFF).  Exposed for the
// platform's MMU map so a mistyped size cannot silently leave a bank unmapped.
//
#define A733_GPIO_WINDOW_SIZE  0x2000U

//
// Manual: GPIO chapter, register list.  The global block is followed by one
// 0x80-byte block per bank; the first bank begins one block in.
//
#define A733_GPIO_PIO_OFFSET   0x80U
#define A733_GPIO_BANK_STRIDE  0x80U

//
// Manual: GPIO chapter, per-bank registers.  Offsets are relative to a bank's
// own block.  cfg/drv are 8 pins per word (4 bits each), pull is 16 pins per
// word (2 bits each); the index and shift arithmetic for a pin is in
// A733GpioLib.c.  dat_set/dat_clr are write-1-to-act and are how an output is
// driven (there is no separate direction bit on this SoC).
//
#define A733_GPIO_CFG_OFFSET      0x00U
#define A733_GPIO_DAT_OFFSET      0x10U
#define A733_GPIO_DAT_SET_OFFSET  0x14U
#define A733_GPIO_DAT_CLR_OFFSET  0x18U
#define A733_GPIO_DRV_OFFSET      0x20U
#define A733_GPIO_PULL_OFFSET     0x30U
//
// The interrupt registers and the per-bank secure register are inside the same
// block but outside this library's scope; the offsets are listed so the block
// can be sanity-checked, not because anything here writes them.
//
#define A733_GPIO_INT_CFG_OFFSET  0x40U
#define A733_GPIO_INT_CTL_OFFSET  0x50U
#define A733_GPIO_INT_STA_OFFSET  0x54U
#define A733_GPIO_INT_DEB_OFFSET  0x58U
#define A733_GPIO_SEC_OFFSET      0x70U

//
// Manual: GPIO chapter, global registers (relative to the PIO base).
//
// 0x000 holds a per-bank base-address config word; the manual's GPIO register
// list starts at 0x040 instead of spelling it out, and the vendor struct names
// it there, so it is kept as a documented offset but is not used.
//
#define A733_GPIO_BASE_ADDR_CFG_OFFSET  0x000U
//
// Manual: GPIO chapter, 0x0040 GPIO_POW_MOD_SEL (GPIO Group Withstand Voltage
// Mode Select Register).  Two bits per bank; section 18.14.4.6 calls 0b10
// "adaptive" (the mode the boot0 log reports as "board gpio withstand voltage
// auto mode") and every other value a fixed setting.
//
#define A733_GPIO_POW_MOD_SEL            0x040U
#define A733_GPIO_POW_MOD_SEL_WIDTH      2U
#define A733_GPIO_POW_MOD_SEL_ADAPTIVE   0x2U
//
// Manual: GPIO chapter, 0x0048 GPIO_POW_VAL (Group Power Value Register) and
// 0x0070 GPIO_POW_VAL_SET_CTL (Group Power Voltage Select Control Register).
// The value each code selects is not verified on this port; A733GpioSetGroupVoltage
// refuses to write them for that reason.
//
#define A733_GPIO_POW_VAL                0x048U
#define A733_GPIO_POW_VAL_SET_CTL        0x070U
// The vendor struct puts a version word one word after POW_VAL_SET_CTL; the
// manual's GPIO register list does not document it, so nothing reads it.
#define A733_GPIO_VERSION_ID             0x078U

// Pins per bank; a pin is a bank number and a bit slot within it.
#define A733_GPIO_PINS_PER_BANK  32U

//
// Banks A..K = 0..10, eleven contiguous banks, all inside the PIO window.  The
// vendor pinctrl description is (first_bank = A, num_banks = 11), so the letter
// index is the bank number; the manual's port table starts at PB and prints PB
// at +0x100 = the bank B block, which is how the two were tied together.  The
// R_PIO domain (PL/PM at 0x07025000) is a separate block and is not modelled.
//
typedef enum {
  A733_GPIO_BANK_A = 0,
  A733_GPIO_BANK_B,
  A733_GPIO_BANK_C,
  A733_GPIO_BANK_D,
  A733_GPIO_BANK_E,
  A733_GPIO_BANK_F,
  A733_GPIO_BANK_G,
  A733_GPIO_BANK_H,
  A733_GPIO_BANK_I,
  A733_GPIO_BANK_J,
  A733_GPIO_BANK_K,
  A733_GPIO_BANK_MAX
} A733_GPIO_BANK;

//
// A pin's bank and bit slot.  The vendor arithmetic is bank = Pin >> 5,
// num = Pin & 0x1f, i.e. bank * 32 + bit.
//
#define A733_GPIO_PIN(Bank, Pin)  ((UINT32)(Bank) * A733_GPIO_PINS_PER_BANK + (UINT32)(Pin))
#define A733_GPIO_BANK_OF(Pin)    ((UINT32)(Pin) / A733_GPIO_PINS_PER_BANK)
#define A733_GPIO_NUM_OF(Pin)     ((UINT32)(Pin) % A733_GPIO_PINS_PER_BANK)
#define A733_GPIO_PIN_MAX         (A733_GPIO_BANK_MAX * A733_GPIO_PINS_PER_BANK)

//
// Manual: GPIO chapter, port multiplex functions.  The 4-bit selector holds the
// port function: 0 is input, 1 is output, 2..14 are the alternate functions
// (which one a pin has is the pin's own mux table, not a property of this
// library), and 15 disables the I/O.
//
typedef enum {
  A733_GPIO_FUNC_INPUT = 0,
  A733_GPIO_FUNC_OUTPUT,
  A733_GPIO_FUNC_ALT2,
  A733_GPIO_FUNC_ALT3,
  A733_GPIO_FUNC_ALT4,
  A733_GPIO_FUNC_ALT5,
  A733_GPIO_FUNC_ALT6,
  A733_GPIO_FUNC_ALT7,
  A733_GPIO_FUNC_ALT8,
  A733_GPIO_FUNC_ALT9,
  A733_GPIO_FUNC_ALT10,
  A733_GPIO_FUNC_ALT11,
  A733_GPIO_FUNC_ALT12,
  A733_GPIO_FUNC_ALT13,
  A733_GPIO_FUNC_ALT14,
  A733_GPIO_FUNC_DISABLE
} A733_GPIO_FUNC;

#define A733_GPIO_FUNC_MASK  0xFU
// Returned by A733GpioGetFunction for a pin outside A733_GPIO_PIN_MAX, so a
// real selector (0..15) is never confused with "no such pin".
#define A733_GPIO_FUNC_UNKNOWN  0xFFFFFFFFU

//
// Manual: GPIO chapter, PUL registers.  2 bits per pin.
//
typedef enum {
  A733_GPIO_PULL_DISABLE = 0,
  A733_GPIO_PULL_UP      = 1,
  A733_GPIO_PULL_DOWN    = 2,
  A733_GPIO_PULL_RESERVED
} A733_GPIO_PULL;

//
// Drive strength is a raw 4-bit code.  The manual's table maps each code to an
// output impedance (r0/N); the port does not reproduce that table, so the
// library takes and returns the code itself.
//
#define A733_GPIO_DRIVE_MASK  0xFU
#define A733_GPIO_DRIVE_MAX   0xFU

#endif