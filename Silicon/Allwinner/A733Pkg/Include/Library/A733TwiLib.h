/** @file
  Polled TWI (I2C) library for the Allwinner A733 (sun60iw2p1).

  Register layouts and field meanings are in <A733Twi.h>.  The library drives
  the two S_TWI instances this board uses, in master mode, polled, with no
  interrupts: the PMIC on S_TWI0 and the HYM8563 RTC on S_TWI1.  The platform
  needs the bus before any interrupt controller is up, and NACK as well as a
  dead bus come back as EFI_DEVICE_ERROR rather than a hang, because a device
  that does not answer must not block the boot path that already works.

  Each call names the controller it talks to, so the library holds no state
  and the two buses cannot leak into each other.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_TWI_LIB_H__
#define A733_TWI_LIB_H__

#include <Uefi.h>

/**
  Prepare S_TWI0 for polled master transfers at a requested bus rate.

  Muxes PL0/PL1 to S-TWI0 function 2 (R_PIO), soft-resets the controller,
  clears the slave-address registers, and programs CCR from the manual's
  Foscl = Fin / (2^CLK_N * (CLK_M + 1) * 10) formula.  The (CLK_N, CLK_M) pair
  chosen is the fastest whose resulting SCL does not exceed BusHz, so the bus
  is never driven above what the caller asked for; the input clock is
  PcdA733TwiClockHz (24 MHz for APBS1, the manual's own example rate).

  @param[in] BusHz  Requested SCL frequency in Hz.  100000 is the standard
                    rate; the calculated rate is returned in the log.

  @retval EFI_SUCCESS            The controller is ready.
  @retval EFI_INVALID_PARAMETER  BusHz is 0, or no divider reaches it.
**/
EFI_STATUS
EFIAPI
A733TwiInitS0 (
  IN UINT32  BusHz
  );

/**
  Prepare S_TWI1 the same way.

  S_TWI1 owns PL12/PL13 on R_PIO selector 3 - the vendor pinctrl table's
  "s_twi1".  Its clock gate and reset live in the always-on R_CCU, not in the
  CCU this library knows, so the caller opens them first.

  @param[in] BusHz  Requested SCL frequency in Hz.

  @retval EFI_SUCCESS            The controller is ready.
  @retval EFI_INVALID_PARAMETER  BusHz is 0, or no divider reaches it.
**/
EFI_STATUS
EFIAPI
A733TwiInitS1 (
  IN UINT32  BusHz
  );

/**
  Write Length bytes to a slave (START, address + W, bytes, STOP).

  @param[in] Base   MMIO base of the controller (PcdA733TwiBase for
                    S_TWI0, A733_TWI1_BASE for S_TWI1).
  @param[in] Slave  7-bit slave address.
  @param[in] Data   Bytes to transmit.
  @param[in] Length Number of bytes.

  @retval EFI_SUCCESS            Every byte was acknowledged.
  @retval EFI_INVALID_PARAMETER  Length is 0, or Slave exceeds 0x7F.
  @retval EFI_DEVICE_ERROR       The address or a data byte was not
                                 acknowledged, the bus errored, or a state
                                 timed out.
**/
EFI_STATUS
EFIAPI
A733TwiWrite (
  IN UINTN        Base,
  IN UINT8        Slave,
  IN CONST UINT8  *Data,
  IN UINTN        Length
  );

/**
  Read Length bytes from a slave (START, address + R, bytes, STOP).

  @param[in]  Base    MMIO base of the controller.
  @param[in]  Slave   7-bit slave address.
  @param[out] Data    Buffer for the bytes.
  @param[in]  Length  Number of bytes.

  @retval EFI_SUCCESS            Every byte was received.
  @retval EFI_INVALID_PARAMETER  Length is 0, or Slave exceeds 0x7F.
  @retval EFI_DEVICE_ERROR       The address was not acknowledged, the bus
                                 errored, or a state timed out.
**/
EFI_STATUS
EFIAPI
A733TwiRead (
  IN UINTN   Base,
  IN UINT8   Slave,
  OUT UINT8  *Data,
  IN UINTN   Length
  );

/**
  Read one register of a slave (START, address + W, register address,
  repeated START, address + R, one byte, STOP).

  This is the combined-transaction form a PMIC or RTC register read needs:
  the register address and the read are one transfer, with a repeated START
  and no STOP in between, so the slave cannot drop its register pointer.

  @param[in]  Base   MMIO base of the controller.
  @param[in]  Slave  7-bit slave address.
  @param[in]  Reg    Register address (one byte).
  @param[out] Value  The register's value.

  @retval EFI_SUCCESS            The register was read.
  @retval EFI_INVALID_PARAMETER  Slave exceeds 0x7F.
  @retval EFI_DEVICE_ERROR       The address was not acknowledged, the bus
                                 errored, or a state timed out.
**/
EFI_STATUS
EFIAPI
A733TwiReadReg (
  IN  UINTN  Base,
  IN  UINT8  Slave,
  IN  UINT8  Reg,
  OUT UINT8  *Value
  );

/**
  Write one register of a slave (START, address + W, register address, value,
  STOP).

  @param[in] Base   MMIO base of the controller.
  @param[in] Slave  7-bit slave address.
  @param[in] Reg    Register address (one byte).
  @param[in] Value  Value to write.

  @retval EFI_SUCCESS            The register was written.
  @retval EFI_INVALID_PARAMETER  Slave exceeds 0x7F.
  @retval EFI_DEVICE_ERROR       The address or a byte was not acknowledged,
                                 the bus errored, or a state timed out.
**/
EFI_STATUS
EFIAPI
A733TwiWriteReg (
  IN UINTN  Base,
  IN UINT8  Slave,
  IN UINT8  Reg,
  IN UINT8  Value
  );

#endif