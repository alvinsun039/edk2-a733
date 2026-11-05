/** @file
  Polled TWI (I2C) master for the Allwinner A733 (sun60iw2p1).

  Register layouts, field meanings and the status codes are in <A733Twi.h>;
  the R_PIO pin-mux constants are in <A733Rpio.h>.  The transfer sequence
  follows the manual's 18.11.6 programming guidelines, and the wait/step
  shapes match the vendor's own driver (mvtwsi.c), including the sun6i-and-newer
  rule that the control register's interrupt flag is cleared by writing a 1.

  The engine is polled, not interrupted: it is brought up in BDS, before the
  PMIC it serves is needed, and a PMIC that does not answer must return an
  error rather than block the boot.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733TwiLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>

#include <A733Rpio.h>
#include <A733Twi.h>

//
// A 7-bit slave address occupies bits [7:1] of the address byte, so the byte
// the controller transmits is (Slave << 1) | direction.
//
#define A733_TWI_SLAVE_MAX  0x7FU

/**
  Address a register inside the S_TWI0 window.

  @param[in] Offset  Register offset from the controller base.

  @return The MMIO address of the register.
**/
STATIC
UINTN
TwiReg (
  IN UINTN   Base,
  IN UINT32  Offset
  )
{
  return Base + Offset;
}

/**
  Set one R_PIO pin's port function.

  Manual: 18.14.5.2 puts the PL bank at +0x0000 of the S_GPIO block with the
  next bank 0x0030 later, and the port table gives PL0/PL1 function 2 as the
  S-TWI0 pairing.  The selector is a 4-bit field, eight pins per 32-bit word,
  exactly as on the main pin controller.

  @param[in] Pin       Pin inside the R_PIO domain, bank * 32 + bit.
  @param[in] Function  4-bit selector.
**/
STATIC
VOID
RpioSetFunction (
  IN UINT32  Pin,
  IN UINT32  Function
  )
{
  UINT32  Bank;
  UINT32  Bit;
  UINT32  Value;
  UINTN   Reg;

  Bank  = Pin / A733_RPIO_PINS_PER_BANK;
  Bit   = Pin % A733_RPIO_PINS_PER_BANK;
  Reg   = (UINTN)PcdGet64 (PcdA733RpioBase)
          + Bank * A733_RPIO_BANK_STRIDE
          + A733_RPIO_CFG_OFFSET
          + (Bit >> 3) * 4U;
  Value  = MmioRead32 (Reg);
  Value &= ~(A733_RPIO_FUNC_MASK << ((Bit & 0x7U) * 4U));
  Value |= (Function & A733_RPIO_FUNC_MASK) << ((Bit & 0x7U) * 4U);
  MmioWrite32 (Reg, Value);
}

/**
  Choose the (CLK_N, CLK_M) pair whose SCL is the fastest at or below BusHz.

  the A733 user manual, Foscl = Fin / (2^CLK_N * (CLK_M + 1) * 10), with CLK_N in
  [2:0] and CLK_M in [6:3].  The search is the vendor driver's: sweep every
  pair and keep the largest rate not above the request, so the bus is never
  driven faster than asked.  At Fin = 24 MHz and 100 kHz this lands on
  N = 1, M = 11 - the manual's own worked example.

  @param[in]  BusHz  Requested SCL frequency.
  @param[out] N      CLK_N value for the pair.
  @param[out] M      CLK_M value for the pair.

  @retval TRUE   A pair was found.
  @retval FALSE  No pair produces a rate at or below BusHz.
**/
STATIC
BOOLEAN
TwiComputeDivider (
  IN  UINT32  BusHz,
  OUT UINT32  *N,
  OUT UINT32  *M
  )
{
  UINT32  Fin;
  UINT32  BestHz;
  UINT32  Nn;
  UINT32  Mm;
  UINT32  Hz;

  Fin    = PcdGet32 (PcdA733TwiClockHz);
  BestHz = 0;
  *N     = 0;
  *M     = 0;

  if (Fin == 0) {
    return FALSE;
  }

  for (Nn = 0; Nn <= 7; Nn++) {
    for (Mm = 0; Mm <= 15; Mm++) {
      Hz = Fin / (10U * (Mm + 1U) * (1U << Nn));
      if ((Hz <= BusHz) && (Hz > BestHz)) {
        BestHz = Hz;
        *N     = Nn;
        *M     = Mm;
      }
    }
  }

  return (BestHz != 0);
}

/**
  Write the control register to advance the engine, then wait for a state.

  Every write sets A733_TWI_CNTR_CLEAR_IFLG: on this part the interrupt flag
  is write-1-to-clear, and the controller holds SCL low until it is cleared.
  The wait then watches that same flag, exactly as the vendor's driver does -
  the flag rises for every state the engine enters except idle, so its rise is
  what says a state can be read.

  @param[in]  Ack    TRUE asserts A_ACK for the next received byte.
  @param[in]  Start  TRUE sets M_STA (START or repeated START).
  @param[out] State  The status code the engine settled into.

  @retval EFI_SUCCESS    A state appeared within the timeout.
  @retval EFI_TIMEOUT    The interrupt flag never rose.
**/
STATIC
EFI_STATUS
TwiStep (
  IN UINTN  Base,
  IN  BOOLEAN  Ack,
  IN  BOOLEAN  Start,
  OUT UINT8    *State
  )
{
  UINT32  Cntr;
  UINT32  Elapsed;

  Cntr = A733_TWI_CNTR_BUS_EN | A733_TWI_CNTR_CLEAR_IFLG;
  if (Ack) {
    Cntr |= A733_TWI_CNTR_A_ACK;
  }

  if (Start) {
    Cntr |= A733_TWI_CNTR_M_STA;
  }

  MmioWrite32 (TwiReg (Base, A733_TWI_CNTR), Cntr);

  for (Elapsed = 0; Elapsed < A733_TWI_TIMEOUT_US; Elapsed++) {
    if ((MmioRead32 (TwiReg (Base, A733_TWI_CNTR)) & A733_TWI_CNTR_INT_FLAG) != 0) {
      *State = (UINT8)(MmioRead32 (TwiReg (Base, A733_TWI_STAT)) & A733_TWI_STAT_MASK);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

/**
  Release the bus with a STOP and wait for idle.

  Used both at the end of a good transfer and after a failure, so a NACKed
  transaction cannot leave SDA held low.  The result is not fatal: a failed
  STOP is reported and the caller's error stands.

  @param[in] Owner  Name of the caller, for the log.
**/
STATIC
VOID
TwiStop (
  IN UINTN  Base,
  IN CONST CHAR8  *Owner
  )
{
  UINT32  Elapsed;
  UINT8   State;

  MmioWrite32 (
    TwiReg (Base, A733_TWI_CNTR),
    A733_TWI_CNTR_BUS_EN | A733_TWI_CNTR_M_STP | A733_TWI_CNTR_CLEAR_IFLG
    );

  for (Elapsed = 0; Elapsed < A733_TWI_TIMEOUT_US; Elapsed++) {
    State = (UINT8)(MmioRead32 (TwiReg (Base, A733_TWI_STAT)) & A733_TWI_STAT_MASK);
    if (State == A733_TWI_STAT_IDLE) {
      return;
    }

    MicroSecondDelay (1);
  }

  DEBUG ((
    DEBUG_ERROR,
    "A733Twi: %a STOP did not reach idle (STAT %02x)\n",
    Owner,
    State
    ));
}

/**
  Send the address byte after a START.

  @param[in]  Slave    7-bit slave address.
  @param[in]  Read     TRUE selects the read direction bit.
  @param[out] State    The status code after the address byte.

  @retval EFI_SUCCESS  The address byte was acknowledged.
  @retval other        Timeout, NACK or an unexpected state.
**/
STATIC
EFI_STATUS
TwiSendAddress (
  IN UINTN  Base,
  IN  UINT8  Slave,
  IN  BOOLEAN  Read,
  OUT UINT8  *State
  )
{
  EFI_STATUS  Status;
  UINT8       Expected;

  MmioWrite32 (
    TwiReg (Base, A733_TWI_DATA),
    (UINT32)((Slave << 1) | (Read ? 1U : 0U))
    );

  Status = TwiStep (Base, TRUE, FALSE, State);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Expected = Read ? A733_TWI_STAT_ADDR_R_ACK : A733_TWI_STAT_ADDR_W_ACK;
  return (*State == Expected) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/**
  Send one data byte in a write transfer.

  @param[in]  Byte   The byte to transmit.
  @param[out] State  The status code after the byte.

  @retval EFI_SUCCESS  The byte was acknowledged.
  @retval other        Timeout, NACK or an unexpected state.
**/
STATIC
EFI_STATUS
TwiSendByte (
  IN UINTN  Base,
  IN  UINT8  Byte,
  OUT UINT8  *State
  )
{
  EFI_STATUS  Status;

  MmioWrite32 (TwiReg (Base, A733_TWI_DATA), Byte);
  Status = TwiStep (Base, TRUE, FALSE, State);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (*State == A733_TWI_STAT_DATA_W_ACK) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/**
  Receive one byte in a read transfer.

  the A733 user manual, every byte but the last is acknowledged; the last clears
  A_ACK so the slave sees a NACK and releases the bus.

  @param[in]  Ack    TRUE acknowledges this byte (it is not the last).
  @param[out] Byte   The received byte.
  @param[out] State  The status code after the byte.

  @retval EFI_SUCCESS  The byte was received with the expected ACK/NACK.
  @retval other        Timeout or an unexpected state.
**/
STATIC
EFI_STATUS
TwiReceiveByte (
  IN UINTN  Base,
  IN  BOOLEAN  Ack,
  OUT UINT8    *Byte,
  OUT UINT8    *State
  )
{
  EFI_STATUS  Status;
  UINT8       Expected;

  Status = TwiStep (Base, Ack, FALSE, State);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Expected = Ack ? A733_TWI_STAT_DATA_R_ACK : A733_TWI_STAT_DATA_R_NACK;
  if (*State != Expected) {
    return EFI_DEVICE_ERROR;
  }

  *Byte = (UINT8)(MmioRead32 (TwiReg (Base, A733_TWI_DATA)) & 0xFFU);
  return EFI_SUCCESS;
}

/**
  Assert a START (or repeated START) and confirm the engine reported it.

  @param[out] State  The status code after the START.

  @retval EFI_SUCCESS  A START or repeated START was reported.
  @retval other        Timeout or an unexpected state.
**/
STATIC
EFI_STATUS
TwiStart (
  IN UINTN  Base,
  OUT UINT8  *State
  )
{
  EFI_STATUS  Status;

  Status = TwiStep (Base, TRUE, TRUE, State);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((*State != A733_TWI_STAT_START) && (*State != A733_TWI_STAT_RESTART)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
TwiInitController (
  IN UINTN   Base,
  IN UINT32  BusHz
  );

/**
  Prepare S_TWI0 for polled master transfers at a requested bus rate.

  Muxes PL0/PL1 to the S-TWI0 function (R_PIO selector 2) and hands the
  controller to TwiInitController().

  @param[in] BusHz  Requested SCL frequency in Hz.

  @retval EFI_SUCCESS            The controller is ready.
  @retval EFI_INVALID_PARAMETER  BusHz is 0, or no divider reaches it.
**/
EFI_STATUS
EFIAPI
A733TwiInitS0 (
  IN UINT32  BusHz
  )
{
  RpioSetFunction (A733_RPIO_PL0, A733_RPIO_FUNC_STWI0);
  RpioSetFunction (A733_RPIO_PL1, A733_RPIO_FUNC_STWI0);

  return TwiInitController ((UINTN)PcdGet64 (PcdA733TwiBase), BusHz);
}

/**
  Prepare S_TWI1 the same way.

  S_TWI1 owns PL12/PL13 on R_PIO selector 3 - the vendor pinctrl table's
  "s_twi1" - and its clock gate and reset must already be open (they live
  in the always-on R_CCU, not in the CCU this library knows).

  @param[in] BusHz  Requested SCL frequency in Hz.

  @retval EFI_SUCCESS            The controller is ready.
  @retval EFI_INVALID_PARAMETER  BusHz is 0, or no divider reaches it.
**/
EFI_STATUS
EFIAPI
A733TwiInitS1 (
  IN UINT32  BusHz
  )
{
  RpioSetFunction (A733_RPIO_PIN (0, 12), A733_TWI_S1_FUNC);
  RpioSetFunction (A733_RPIO_PIN (0, 13), A733_TWI_S1_FUNC);

  return TwiInitController (A733_TWI1_BASE, BusHz);
}

/**
  Reset and configure one controller instance.

  @param[in] Base   MMIO base of the controller.
  @param[in] BusHz  Requested SCL frequency in Hz.

  @retval EFI_SUCCESS            The controller is ready.
  @retval EFI_INVALID_PARAMETER  BusHz is 0, or no divider reaches it.
**/
STATIC
EFI_STATUS
TwiInitController (
  IN UINTN  Base,
  IN UINT32  BusHz
  )
{
  UINT32  N;
  UINT32  M;
  UINT32  Elapsed;
  UINT32  RateHz;

  if (BusHz == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (!TwiComputeDivider (BusHz, &N, &M)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Twi: no divider reaches %u Hz from a %u Hz input\n",
      BusHz,
      PcdGet32 (PcdA733TwiClockHz)
      ));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Soft reset, then leave it to settle.  The reset clears CCR, so the bus
  // rate is programmed after it, never before.
  //
  // the A733 user manual, says SOFT_RST self-clears when the reset completes, but
  // the vendor's own driver treats this register as a no-op (it writes 0) and
  // resets the block through the CCU instead.  If the bit does not clear here
  // the bus is still worth trying, so this warns and carries on; the chip-ID
  // read is what actually decides whether the controller works.
  //
  MmioWrite32 (TwiReg (Base, A733_TWI_SRST), A733_TWI_SRST_SOFT_RST);
  for (Elapsed = 0; Elapsed < A733_TWI_TIMEOUT_US; Elapsed++) {
    if ((MmioRead32 (TwiReg (Base, A733_TWI_SRST)) & A733_TWI_SRST_SOFT_RST) == 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  if ((MmioRead32 (TwiReg (Base, A733_TWI_SRST)) & A733_TWI_SRST_SOFT_RST) != 0) {
    DEBUG ((
      DEBUG_WARN,
      "A733Twi: soft reset did not self-clear, continuing anyway\n"
      ));
  }

  MmioWrite32 (TwiReg (Base, A733_TWI_EFR), 0);

  //
  // The slave-address registers are only meaningful in slave mode; clearing
  // them keeps a stray address from being answered.
  //
  MmioWrite32 (TwiReg (Base, A733_TWI_ADDR), 0);
  MmioWrite32 (TwiReg (Base, A733_TWI_XADDR), 0);

  MmioWrite32 (TwiReg (Base, A733_TWI_CCR), (M << A733_TWI_CCR_CLK_M_SHIFT) | N);

  //
  // Open the bus and force it idle, in case the engine came up mid-transfer.
  //
  TwiStop (Base, "init");

  RateHz = PcdGet32 (PcdA733TwiClockHz) / (10U * (M + 1U) * (1U << N));
  DEBUG ((
    DEBUG_INFO,
    "A733Twi: bus at %u Hz (CLK_N %u, CLK_M %u, CCR %02x)\n",
    RateHz,
    N,
    M,
    (M << A733_TWI_CCR_CLK_M_SHIFT) | N
    ));

  return EFI_SUCCESS;
}


/**
  Write Length bytes to a slave (START, address + W, bytes, STOP).

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
  IN UINTN  Base,
  IN UINT8        Slave,
  IN CONST UINT8  *Data,
  IN UINTN        Length
  )
{  EFI_STATUS  Status;
  UINT8       State;
  UINTN       Index;

  if ((Length == 0) || (Slave > A733_TWI_SLAVE_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = TwiStart (Base, &State);
  if (!EFI_ERROR (Status)) {
    Status = TwiSendAddress (Base, Slave, FALSE, &State);
  }

  for (Index = 0; (Index < Length) && !EFI_ERROR (Status); Index++) {
    Status = TwiSendByte (Base, Data[Index], &State);
  }

  TwiStop (Base, "write");
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Twi: write to %02x failed at byte %u (STAT %02x, %r)\n",
      Slave,
      (UINT32)Index,
      State,
      Status
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read Length bytes from a slave (START, address + R, bytes, STOP).

  @param[in]  Slave   7-bit slave address.
  @param[out] Data    Buffer for the received bytes.
  @param[in]  Length  Number of bytes to read.

  @retval EFI_SUCCESS            Every byte was received.
  @retval EFI_INVALID_PARAMETER  Length is 0, or Slave exceeds 0x7F.
  @retval EFI_DEVICE_ERROR       The address was not acknowledged, the bus
                                 errored, or a state timed out.
**/
EFI_STATUS
EFIAPI
A733TwiRead (
  IN  UINTN  Base,
  IN  UINT8  Slave,
  OUT UINT8  *Data,
  IN  UINTN  Length
  )
{
  EFI_STATUS  Status;
  UINT8       State;
  UINTN       Index;

  if ((Length == 0) || (Slave > A733_TWI_SLAVE_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = TwiStart (Base, &State);
  if (!EFI_ERROR (Status)) {
    Status = TwiSendAddress (Base, Slave, TRUE, &State);
  }

  for (Index = 0; (Index < Length) && !EFI_ERROR (Status); Index++) {
    //
    // Every byte but the last is acknowledged.
    //
    Status = TwiReceiveByte (Base, (Index + 1) < Length, &Data[Index], &State);
  }

  TwiStop (Base, "read");
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Twi: read from %02x failed at byte %u (STAT %02x, %r)\n",
      Slave,
      (UINT32)Index,
      State,
      Status
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Read one register of a slave (START, address + W, register address,
  repeated START, address + R, one byte, STOP).

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
  IN UINTN  Base,
  IN  UINT8  Slave,
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{  EFI_STATUS  Status;
  UINT8       State;

  if (Slave > A733_TWI_SLAVE_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Register address phase, then a repeated START and the read.
  //
  Status = TwiStart (Base, &State);
  if (!EFI_ERROR (Status)) {
    Status = TwiSendAddress (Base, Slave, FALSE, &State);
  }

  if (!EFI_ERROR (Status)) {
    Status = TwiSendByte (Base, Reg, &State);
  }

  if (!EFI_ERROR (Status)) {
    Status = TwiStart (Base, &State);
  }

  if (!EFI_ERROR (Status)) {
    Status = TwiSendAddress (Base, Slave, TRUE, &State);
  }

  if (!EFI_ERROR (Status)) {
    Status = TwiReceiveByte (Base, FALSE, Value, &State);
  }

  TwiStop (Base, "readreg");
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Twi: read reg %02x of %02x failed (STAT %02x, %r)\n",
      Reg,
      Slave,
      State,
      Status
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Write one register of a slave (START, address + W, register address, value,
  STOP).

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
  )
{  EFI_STATUS  Status;
  UINT8       State;

  if (Slave > A733_TWI_SLAVE_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  Status = TwiStart (Base, &State);
  if (!EFI_ERROR (Status)) {
    Status = TwiSendAddress (Base, Slave, FALSE, &State);
  }

  if (!EFI_ERROR (Status)) {
    Status = TwiSendByte (Base, Reg, &State);
  }

  if (!EFI_ERROR (Status)) {
    Status = TwiSendByte (Base, Value, &State);
  }

  TwiStop (Base, "writereg");
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Twi: write reg %02x of %02x failed (STAT %02x, %r)\n",
      Reg,
      Slave,
      State,
      Status
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}
