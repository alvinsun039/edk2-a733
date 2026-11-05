/** @file
  GPIO / pin-multiplexing library for the Allwinner A733 (sun60iw2p1).

  Two blocks are served here: the main pin controller (banks A..K, A733_GPIO_*
  entry points) and the R_PIO / S_GPIO domain (banks L and M, A733_RPIO_*
  entry points, map in <A733Rpio.h>).  They have different layouts and separate
  bases, so they are kept apart rather than merged behind one pin number.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_GPIO_LIB_H__
#define A733_GPIO_LIB_H__

#include <Uefi.h>

#include <A733Gpio.h>

/**
  Set a pin's port function.

  Writes the pin's 4-bit selector in its bank's CFG register, changing nothing
  else in that word.  Use A733_GPIO_FUNC_OUTPUT to make a pin an output,
  A733_GPIO_FUNC_INPUT for an input, an A733_GPIO_FUNC_ALTn for a peripheral,
  or A733_GPIO_FUNC_DISABLE to release it.

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
  );

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
  );

/**
  Set a pin's pull.

  Writes the pin's 2-bit field in its bank's PUL register, changing nothing
  else in that word.

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
  );

/**
  Set a pin's drive strength.

  Writes the pin's 4-bit field in its bank's DRV register, changing nothing else
  in that word.  Drive is the raw code, 0..15.

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
  );

/**
  Express a pin's direction through its port function.

  This SoC has no separate direction bit: a pin is an input when its port
  function is 0 and an output when it is 1, and the level of an output is driven
  later with dat_set/dat_clr (A733GpioWrite).  A caller that wants to drive a
  pin must leave it on the GPIO output function; on an alternate function the
  pin follows its peripheral and any A733GpioWrite is ignored by the pad.

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
  );

/**
  Drive an output pin's level.

  Writes the pin's bit to the bank's dat_set (high) or dat_clr (low) register,
  so other pins in the bank keep their level.  The result is visible only when
  the pin's port function is the GPIO output function.

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
  );

/**
  Read a pin's level.

  @param[in] Pin  Pin number, A733_GPIO_PIN (Bank, Bit).

  @return TRUE when the pin reads high, FALSE when it reads low or Pin is out of
          range (a UINT32-sized status is not available here; the dump and the
          setters are where a caller sees an out-of-range pin reported).
**/
BOOLEAN
EFIAPI
A733GpioRead (
  IN UINT32  Pin
  );

/**
  Set a bank group's power voltage.

  Not implemented: the A733 sets a bank's group voltage through the global
  GPIO_POW_VAL / GPIO_POW_VAL_SET_CTL registers, and this port holds neither a
  measured encoding for the voltage codes nor a board that could confirm one.
  The library therefore refuses the request rather than writing a guessed code.

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
  );

/**
  Print every bank's configured pins through DebugLib.

  One line per bank names its GPIO_POW_MOD_SEL field and calls it adaptive or
  fixed (section 18.14.4.6: 0b10 is adaptive).  Then one line per non-zero cfg,
  drv and pull word decodes that word's pins; a bank with no configured pin
  contributes only its power line.
**/
VOID
EFIAPI
A733GpioDumpBanks (
  VOID
  );

/**
  Drive a pin in the R_PIO (S_GPIO) domain as a GPIO output at a given level.

  The R_PIO domain is a separate block at its own base with a packed layout of
  its own (see <A733Rpio.h>), so it has entry points of its own rather than
  sharing the main controller's: a pin number here is A733_RPIO_PIN, not
  A733_GPIO_PIN, and the two must not be mixed.

  The level is written before the pad is switched to output, so a rail that is
  enabled by an active-high pin never sees a low glitch on the way up.  The
  pad's pull is left as it was.

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
  );

#endif