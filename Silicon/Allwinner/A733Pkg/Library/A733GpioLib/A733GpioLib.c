/** @file
  GPIO / pin-multiplexing library for the Allwinner A733 (sun60iw2p1).

  Register layouts and field meanings are in <A733Gpio.h>; this file adds the
  per-pin arithmetic and the register access.  Where the A733 manual's field
  encodings could not be reconciled against the hardware, the code refuses the
  request instead of presenting a guess as a value.

  The R_PIO (S_GPIO) domain is a second pin controller with a different layout;
  A733RpioSetOutput at the end of this file serves it, from the map in
  <A733Rpio.h>.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733GpioLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>

#include <A733Rpio.h>

/**
  Address a bank's register block.

  @param[in] Bank  Bank number, 0..A733_GPIO_BANK_MAX-1.

  @return The MMIO address of the bank's block.
**/
STATIC
UINTN
PioBankBase (
  IN UINT32  Bank
  )
{
  return (UINTN)PcdGet64 (PcdA733GpioBase)
         + A733_GPIO_PIO_OFFSET
         + (UINTN)Bank * A733_GPIO_BANK_STRIDE;
}

/**
  Address a register inside a pin's bank.

  @param[in] Pin     Pin number, A733_GPIO_PIN (Bank, Bit).
  @param[in] Offset  Register offset inside the bank block.

  @return The MMIO address of the register.
**/
STATIC
UINTN
PioPinReg (
  IN UINT32  Pin,
  IN UINT32  Offset
  )
{
  return PioBankBase (A733_GPIO_BANK_OF (Pin)) + Offset;
}

/**
  Address a global (non-bank) register in the PIO window.

  @param[in] Offset  Register offset from the PIO base.

  @return The MMIO address of the register.
**/
STATIC
UINTN
PioGlobalReg (
  IN UINT32  Offset
  )
{
  return (UINTN)PcdGet64 (PcdA733GpioBase) + Offset;
}

/**
  Name of a port function, for the dump.

  @param[in] Function  4-bit selector.

  @return The function's name, or "?" when Function is out of range.
**/
STATIC
CONST CHAR8 *
GpioFunctionName (
  IN UINT32  Function
  )
{
  STATIC CONST CHAR8  *mNames[] = {
    "in", "out", "alt2", "alt3", "alt4", "alt5", "alt6", "alt7",
    "alt8", "alt9", "alt10", "alt11", "alt12", "alt13", "alt14", "disable"
  };

  if (Function > A733_GPIO_FUNC_DISABLE) {
    return "?";
  }

  return mNames[Function];
}

/**
  Name of a pull setting, for the dump.

  @param[in] Pull  2-bit field.

  @return The setting's name, or "?" when Pull is out of range.
**/
STATIC
CONST CHAR8 *
GpioPullName (
  IN UINT32  Pull
  )
{
  STATIC CONST CHAR8  *mNames[] = { "disable", "up", "down", "reserved" };

  if (Pull > A733_GPIO_PULL_RESERVED) {
    return "?";
  }

  return mNames[Pull];
}

/**
  Set a pin's port function.

  @param[in] Pin       Pin number, A733_GPIO_PIN (Bank, Bit).
  @param[in] Function  Selector, 0..15 (A733_GPIO_FUNC).

  @retval EFI_SUCCESS            The selector was written.
  @retval EFI_INVALID_PARAMETER  Pin or Function is out of range.
**/
EFI_STATUS
EFIAPI
A733GpioSetFunction (
  IN UINT32  Pin,
  IN UINT32  Function
  )
{
  UINT32  Index;
  UINT32  Shift;
  UINT32  Value;

  if ((Pin >= A733_GPIO_PIN_MAX) || (Function > A733_GPIO_FUNC_DISABLE)) {
    return EFI_INVALID_PARAMETER;
  }

  Index = A733_GPIO_NUM_OF (Pin) >> 3;
  Shift = (A733_GPIO_NUM_OF (Pin) & 0x7U) * 4U;

  Value  = MmioRead32 (PioPinReg (Pin, A733_GPIO_CFG_OFFSET + Index * 4U));
  Value &= ~(A733_GPIO_FUNC_MASK << Shift);
  Value |= (Function & A733_GPIO_FUNC_MASK) << Shift;
  MmioWrite32 (PioPinReg (Pin, A733_GPIO_CFG_OFFSET + Index * 4U), Value);

  return EFI_SUCCESS;
}

/**
  Read a pin's port function.

  @param[in] Pin  Pin number, A733_GPIO_PIN (Bank, Bit).

  @return The pin's 4-bit selector (0..15), or A733_GPIO_FUNC_UNKNOWN when Pin
          is out of range.
**/
UINT32
EFIAPI
A733GpioGetFunction (
  IN UINT32  Pin
  )
{
  UINT32  Index;
  UINT32  Shift;

  if (Pin >= A733_GPIO_PIN_MAX) {
    return A733_GPIO_FUNC_UNKNOWN;
  }

  Index = A733_GPIO_NUM_OF (Pin) >> 3;
  Shift = (A733_GPIO_NUM_OF (Pin) & 0x7U) * 4U;

  return (MmioRead32 (PioPinReg (Pin, A733_GPIO_CFG_OFFSET + Index * 4U)) >> Shift)
         & A733_GPIO_FUNC_MASK;
}

/**
  Set a pin's pull.

  @param[in] Pin   Pin number, A733_GPIO_PIN (Bank, Bit).
  @param[in] Pull  A733_GPIO_PULL_DISABLE, _UP or _DOWN.

  @retval EFI_SUCCESS            The pull was written.
  @retval EFI_INVALID_PARAMETER  Pin is out of range, or Pull is the reserved
                                 encoding A733_GPIO_PULL_RESERVED.
**/
EFI_STATUS
EFIAPI
A733GpioSetPull (
  IN UINT32          Pin,
  IN A733_GPIO_PULL  Pull
  )
{
  UINT32  Index;
  UINT32  Shift;
  UINT32  Value;

  //
  // The reserved encoding would also land in the register, but naming a value
  // the manual leaves reserved as "a pull" would be a lie; refuse it.
  //
  if ((Pin >= A733_GPIO_PIN_MAX) || (Pull > A733_GPIO_PULL_DOWN)) {
    return EFI_INVALID_PARAMETER;
  }

  Index = A733_GPIO_NUM_OF (Pin) >> 4;
  Shift = (A733_GPIO_NUM_OF (Pin) & 0xFU) * 2U;

  Value  = MmioRead32 (PioPinReg (Pin, A733_GPIO_PULL_OFFSET + Index * 4U));
  Value &= ~(0x3U << Shift);
  Value |= ((UINT32)Pull & 0x3U) << Shift;
  MmioWrite32 (PioPinReg (Pin, A733_GPIO_PULL_OFFSET + Index * 4U), Value);

  return EFI_SUCCESS;
}

/**
  Set a pin's drive strength.

  @param[in] Pin    Pin number, A733_GPIO_PIN (Bank, Bit).
  @param[in] Drive  Raw drive code, 0..15.

  @retval EFI_SUCCESS            The code was written.
  @retval EFI_INVALID_PARAMETER  Pin or Drive is out of range.
**/
EFI_STATUS
EFIAPI
A733GpioSetDrive (
  IN UINT32  Pin,
  IN UINT32  Drive
  )
{
  UINT32  Index;
  UINT32  Shift;
  UINT32  Value;

  if ((Pin >= A733_GPIO_PIN_MAX) || (Drive > A733_GPIO_DRIVE_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  Index = A733_GPIO_NUM_OF (Pin) >> 3;
  Shift = (A733_GPIO_NUM_OF (Pin) & 0x7U) * 4U;

  Value  = MmioRead32 (PioPinReg (Pin, A733_GPIO_DRV_OFFSET + Index * 4U));
  Value &= ~(A733_GPIO_DRIVE_MASK << Shift);
  Value |= (Drive & A733_GPIO_DRIVE_MASK) << Shift;
  MmioWrite32 (PioPinReg (Pin, A733_GPIO_DRV_OFFSET + Index * 4U), Value);

  return EFI_SUCCESS;
}

/**
  Express a pin's direction through its port function.

  @param[in] Pin     Pin number, A733_GPIO_PIN (Bank, Bit).
  @param[in] Output  TRUE selects the GPIO output function, FALSE the input
                     function.

  @retval EFI_SUCCESS            The function selector was written.
  @retval EFI_INVALID_PARAMETER  Pin is out of range.
**/
EFI_STATUS
EFIAPI
A733GpioSetDirection (
  IN UINT32   Pin,
  IN BOOLEAN  Output
  )
{
  if (Pin >= A733_GPIO_PIN_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // No direction bit exists, so the direction is the port function: input is
  // selector 0, output is selector 1.  The output level itself is driven by
  // A733GpioWrite, which touches dat_set/dat_clr.
  //
  return A733GpioSetFunction (
           Pin,
           Output ? A733_GPIO_FUNC_OUTPUT : A733_GPIO_FUNC_INPUT
           );
}

/**
  Drive an output pin's level.

  @param[in] Pin    Pin number, A733_GPIO_PIN (Bank, Bit).
  @param[in] Value  TRUE drives the pin high, FALSE low.

  @retval EFI_SUCCESS            The level was driven.
  @retval EFI_INVALID_PARAMETER  Pin is out of range.
**/
EFI_STATUS
EFIAPI
A733GpioWrite (
  IN UINT32   Pin,
  IN BOOLEAN  Value
  )
{
  UINT32  Bit;

  if (Pin >= A733_GPIO_PIN_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  Bit = 1U << A733_GPIO_NUM_OF (Pin);

  //
  // dat_set / dat_clr are write-1-to-act, so only the one pin moves and every
  // other pin in the bank keeps its level.
  //
  MmioWrite32 (
    PioPinReg (Pin, Value ? A733_GPIO_DAT_SET_OFFSET : A733_GPIO_DAT_CLR_OFFSET),
    Bit
    );

  return EFI_SUCCESS;
}

/**
  Read a pin's level.

  @param[in] Pin  Pin number, A733_GPIO_PIN (Bank, Bit).

  @return TRUE when the pin reads high, FALSE when it reads low or Pin is out of
          range.
**/
BOOLEAN
EFIAPI
A733GpioRead (
  IN UINT32  Pin
  )
{
  if (Pin >= A733_GPIO_PIN_MAX) {
    return FALSE;
  }

  return (MmioRead32 (PioPinReg (Pin, A733_GPIO_DAT_OFFSET))
          & (1U << A733_GPIO_NUM_OF (Pin))) != 0;
}

/**
  Set a bank group's power voltage.

  @param[in] Bank          Bank whose group voltage to set.
  @param[in] VolMilliVolt  Requested voltage in mV.

  @retval EFI_UNSUPPORTED        Always; the voltage code table is not verified
                                 on this port.
  @retval EFI_INVALID_PARAMETER  Bank is out of range.
**/
EFI_STATUS
EFIAPI
A733GpioSetGroupVoltage (
  IN UINT32  Bank,
  IN UINT32  VolMilliVolt
  )
{
  if (Bank >= A733_GPIO_BANK_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Deliberately unused: no voltage code is written here.
  //
  (VOID)VolMilliVolt;

  //
  // Section 18.14.4.6 sets group voltage through GPIO_POW_VAL_SET_CTL and
  // reports it through GPIO_POW_VAL, but the manual's excerpt we hold does not
  // tie a millivolt value to a code, and no register on this board was read
  // back to confirm one.  Writing a guessed code could over-volt a group, so
  // the request is refused.  The read side (A733GpioDumpBanks) prints what the
  // hardware already reports.
  //
  DEBUG ((
    DEBUG_WARN,
    "A733Gpio: group voltage for bank %u is not implemented (code table unverified)\n",
    Bank
    ));

  return EFI_UNSUPPORTED;
}

/**
  Print every bank's configured pins through DebugLib.

  One line per bank names its GPIO_POW_MOD_SEL field and calls it adaptive or
  fixed.  Then one line per non-zero cfg, drv and pull word decodes that word's
  pins.
**/
VOID
EFIAPI
A733GpioDumpBanks (
  VOID
  )
{
  UINT32  Bank;
  UINT32  Word;
  UINT32  Field;
  UINT32  PowModSel;
  UINT32  Cfg[4];
  UINT32  Drv[4];
  UINT32  Pull[2];

  PowModSel = MmioRead32 (PioGlobalReg (A733_GPIO_POW_MOD_SEL));

  DEBUG ((
    DEBUG_INFO,
    "A733Gpio: PIO base 0x%08x, first bank at +0x%x, bank stride 0x%x\n",
    (UINT32)PcdGet64 (PcdA733GpioBase),
    A733_GPIO_PIO_OFFSET,
    A733_GPIO_BANK_STRIDE
    ));

  for (Bank = 0; Bank < A733_GPIO_BANK_MAX; Bank++) {
    for (Word = 0; Word < 4; Word++) {
      Cfg[Word] = MmioRead32 (PioBankBase (Bank) + A733_GPIO_CFG_OFFSET + Word * 4U);
      Drv[Word] = MmioRead32 (PioBankBase (Bank) + A733_GPIO_DRV_OFFSET + Word * 4U);
    }

    for (Word = 0; Word < 2; Word++) {
      Pull[Word] = MmioRead32 (PioBankBase (Bank) + A733_GPIO_PULL_OFFSET + Word * 4U);
    }

    //
    // Section 18.14.4.6: the group's two bits are 0b10 in adaptive (withstand
    // voltage) mode, any other value a fixed voltage.  The bits sit two per
    // bank, low bank first.
    //
    Field = (PowModSel >> (A733_GPIO_POW_MOD_SEL_WIDTH * Bank)) & 0x3U;
    DEBUG ((
      DEBUG_INFO,
      "A733Gpio: BANK %c POW_MOD_SEL=%u (%a)\n",
      'A' + Bank,
      Field,
      (Field == A733_GPIO_POW_MOD_SEL_ADAPTIVE) ? "adaptive" : "fixed"
      ));

    for (Word = 0; Word < 4; Word++) {
      if (Cfg[Word] == 0) {
        continue;
      }

      DEBUG ((
        DEBUG_INFO,
        "A733Gpio:   %c CFG%d[0x%02x]=%08x mux P%u..P%u: %a %a %a %a %a %a %a %a\n",
        'A' + Bank,
        Word,
        A733_GPIO_CFG_OFFSET + Word * 4U,
        Cfg[Word],
        Word * 8U,
        Word * 8U + 7U,
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 0, 3)),
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 4, 7)),
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 8, 11)),
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 12, 15)),
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 16, 19)),
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 20, 23)),
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 24, 27)),
        GpioFunctionName (BitFieldRead32 (Cfg[Word], 28, 31))
        ));
    }

    for (Word = 0; Word < 4; Word++) {
      if (Drv[Word] == 0) {
        continue;
      }

      DEBUG ((
        DEBUG_INFO,
        "A733Gpio:   %c DRV%d[0x%02x]=%08x drv P%u..P%u: %u %u %u %u %u %u %u %u\n",
        'A' + Bank,
        Word,
        A733_GPIO_DRV_OFFSET + Word * 4U,
        Drv[Word],
        Word * 8U,
        Word * 8U + 7U,
        BitFieldRead32 (Drv[Word], 0, 3),
        BitFieldRead32 (Drv[Word], 4, 7),
        BitFieldRead32 (Drv[Word], 8, 11),
        BitFieldRead32 (Drv[Word], 12, 15),
        BitFieldRead32 (Drv[Word], 16, 19),
        BitFieldRead32 (Drv[Word], 20, 23),
        BitFieldRead32 (Drv[Word], 24, 27),
        BitFieldRead32 (Drv[Word], 28, 31)
        ));
    }

    for (Word = 0; Word < 2; Word++) {
      if (Pull[Word] == 0) {
        continue;
      }

      DEBUG ((
        DEBUG_INFO,
        "A733Gpio:   %c PUL%d[0x%02x]=%08x pull P%u..P%u: "
        "%a %a %a %a %a %a %a %a %a %a %a %a %a %a %a %a\n",
        'A' + Bank,
        Word,
        A733_GPIO_PULL_OFFSET + Word * 4U,
        Pull[Word],
        Word * 16U,
        Word * 16U + 15U,
        GpioPullName (BitFieldRead32 (Pull[Word], 0, 1)),
        GpioPullName (BitFieldRead32 (Pull[Word], 2, 3)),
        GpioPullName (BitFieldRead32 (Pull[Word], 4, 5)),
        GpioPullName (BitFieldRead32 (Pull[Word], 6, 7)),
        GpioPullName (BitFieldRead32 (Pull[Word], 8, 9)),
        GpioPullName (BitFieldRead32 (Pull[Word], 10, 11)),
        GpioPullName (BitFieldRead32 (Pull[Word], 12, 13)),
        GpioPullName (BitFieldRead32 (Pull[Word], 14, 15)),
        GpioPullName (BitFieldRead32 (Pull[Word], 16, 17)),
        GpioPullName (BitFieldRead32 (Pull[Word], 18, 19)),
        GpioPullName (BitFieldRead32 (Pull[Word], 20, 21)),
        GpioPullName (BitFieldRead32 (Pull[Word], 22, 23)),
        GpioPullName (BitFieldRead32 (Pull[Word], 24, 25)),
        GpioPullName (BitFieldRead32 (Pull[Word], 26, 27)),
        GpioPullName (BitFieldRead32 (Pull[Word], 28, 29)),
        GpioPullName (BitFieldRead32 (Pull[Word], 30, 31))
        ));
    }
  }
}

/**
  Drive a pin in the R_PIO (S_GPIO) domain as a GPIO output at a given level.

  The R_PIO domain is a separate block with its own layout (see <A733Rpio.h>):
  one bank every 0x30 bytes, the level word at +0x10 inside the bank, and no
  dat_set/dat_clr aliases, so the level is a read-modify-write of that word.

  The level is written before the pad is switched to output: the switch is the
  moment the pad starts driving, and an enable that is active high must not be
  seen low for an instant on the way.  The pad's pull setting is left alone,
  which is what a caller driving a board's power-enable wants - the pull that
  holds the rail off before the firmware runs stays in place underneath.

  @param[in] Pin    Pin inside the R_PIO domain, A733_RPIO_PIN (Bank, Bit).
  @param[in] Level  TRUE drives the pin high, FALSE low.

  @retval EFI_SUCCESS            The level and the port function were written.
  @retval EFI_INVALID_PARAMETER  Pin is outside the domain.
**/
EFI_STATUS
EFIAPI
A733RpioSetOutput (
  IN UINT32   Pin,
  IN BOOLEAN  Level
  )
{
  UINTN   BankBase;
  UINTN   CfgReg;
  UINT32  Bit;
  UINT32  Value;
  UINT32  Shift;

  if (Pin >= A733_RPIO_PIN_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  BankBase = (UINTN)PcdGet64 (PcdA733RpioBase)
             + (UINTN)A733_RPIO_BANK_OF (Pin) * A733_RPIO_BANK_STRIDE;
  Bit   = A733_RPIO_NUM_OF (Pin);
  Shift = (Bit & 0x7U) * 4U;

  Value = MmioRead32 (BankBase + A733_RPIO_DAT_OFFSET);
  if (Level) {
    Value |= (UINT32)(1U << Bit);
  } else {
    Value &= ~(UINT32)(1U << Bit);
  }

  MmioWrite32 (BankBase + A733_RPIO_DAT_OFFSET, Value);

  CfgReg = BankBase + A733_RPIO_CFG_OFFSET + (UINTN)(Bit >> 3) * 4U;
  Value  = MmioRead32 (CfgReg);
  Value &= ~(A733_RPIO_FUNC_MASK << Shift);
  Value |= (A733_RPIO_FUNC_OUTPUT << Shift);
  MmioWrite32 (CfgReg, Value);

  DEBUG ((
    DEBUG_VERBOSE,
    "A733Gpio: R_PIO P%c%u = %u (cfg[0x%02x]=%08x)\n",
    'L' + A733_RPIO_BANK_OF (Pin),
    Bit,
    (UINT32)Level,
    (UINT32)(A733_RPIO_CFG_OFFSET + (Bit >> 3) * 4U),
    MmioRead32 (CfgReg)
    ));

  return EFI_SUCCESS;
}
