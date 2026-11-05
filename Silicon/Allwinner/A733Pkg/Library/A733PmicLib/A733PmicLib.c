/** @file
  AXP8191 PMIC support for the Allwinner A733 (sun60iw2p1).

  Register addresses and the voltage encoding are in <A733Pmic.h>; the TWI
  engine is <A733TwiLib.h>.  The rail table below is the vendor's
  pmu_axp8191_ctrl_tbl rows for CLDO1..CLDO5, transcribed one for one.  DCDC1
  is the one rail modelled outside that table, because it is what the board's
  USB2 hub is powered from and its control bit lives in a different register
  (DCDC_POWER_ON_OFF_CTL1) from the LDOs'.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/A733PmicLib.h>
#include <Library/A733TwiLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>

#include <A733Pmic.h>

//
// The chip-ID readback is informational and every consumer probes the PMIC,
// so it is printed on the first probe only.
//
STATIC BOOLEAN  mChipIdReported;

//
// The standard-mode I2C rate.  The PMIC is a slow device and there is nothing
// to gain from the fast-mode rate on a bring-up read.
//
#define A733_PMIC_BUS_HZ  100000U

typedef struct {
  UINT8    VolReg;
  UINT8    EnableBit;
} A733_PMIC_RAIL_DESC;

//
// Vendor pmu_axp8191_ctrl_tbl: CLDO1..CLDO5 are voltage registers 0x2F..0x33
// (mask 0x1F, 500 mV + 100 mV/step), enabled by bits 3..7 of
// LDO_POWER_ON_OFF_CTL2.
//
STATIC CONST A733_PMIC_RAIL_DESC  mRails[A733_PMIC_RAIL_MAX] = {
  { AXP8191_CLDO1OUT_VOL, 3 },
  { AXP8191_CLDO2OUT_VOL, 4 },
  { AXP8191_CLDO3OUT_VOL, 5 },
  { AXP8191_CLDO4OUT_VOL, 6 },
  { AXP8191_CLDO5OUT_VOL, 7 },
};

STATIC_ASSERT (
  ARRAY_SIZE (mRails) == A733_PMIC_RAIL_MAX,
  "rail table and A733_PMIC_RAIL out of step"
  );

/**
  Read a PMIC register, optionally tracing the value.

  @param[in]  Reg    Register address.
  @param[out] Value  The register's value.

  @retval EFI_SUCCESS       The register was read.
  @retval EFI_DEVICE_ERROR  The bus did not answer.
**/
STATIC
EFI_STATUS
PmicRead (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{
  EFI_STATUS  Status;

  Status = A733TwiReadReg ((UINTN)PcdGet64 (PcdA733TwiBase), AXP8191_SLAVE_ADDR, Reg, Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (PcdGetBool (PcdA733PmicVerbose)) {
    DEBUG ((
      DEBUG_INFO,
      "Pmic:   [%02x] read  %02x\n",
      Reg,
      *Value
      ));
  }

  return EFI_SUCCESS;
}

/**
  Write a PMIC register, optionally tracing the value.

  @param[in] Reg    Register address.
  @param[in] Value  Value to write.

  @retval EFI_SUCCESS       The register was written.
  @retval EFI_DEVICE_ERROR  The bus did not answer.
**/
STATIC
EFI_STATUS
PmicWrite (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  EFI_STATUS  Status;

  Status = A733TwiWriteReg ((UINTN)PcdGet64 (PcdA733TwiBase), AXP8191_SLAVE_ADDR, Reg, Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (PcdGetBool (PcdA733PmicVerbose)) {
    DEBUG ((
      DEBUG_INFO,
      "Pmic:   [%02x] write %02x\n",
      Reg,
      Value
      ));
  }

  return EFI_SUCCESS;
}

/**
  Bring S_TWI0 up at 100 kHz and probe the AXP8191.

  @retval EFI_SUCCESS            The chip answered and its ID matched.
  @retval EFI_DEVICE_ERROR       The chip did not answer.
  @retval EFI_INVALID_PARAMETER  The bus rate could not be programmed.
  @retval EFI_NOT_FOUND          The chip answered with an unexpected ID.
**/
EFI_STATUS
EFIAPI
A733PmicInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       ChipId;

  Status = A733TwiInitS0 (A733_PMIC_BUS_HZ);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Pmic: S_TWI0 did not come up, no PMIC access (%r)\n",
      Status
      ));
    return Status;
  }

  Status = A733TwiReadReg ((UINTN)PcdGet64 (PcdA733TwiBase), AXP8191_SLAVE_ADDR, AXP8191_CHIP_ID, &ChipId);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Pmic: no answer from address %02x on S_TWI0 (%r)\n",
      AXP8191_SLAVE_ADDR,
      Status
      ));
    return Status;
  }

  //
  // Name what the chip answered once: every consumer of this library probes
  // it, and three identical lines said nothing the first one did not.
  //
  if (!mChipIdReported) {
    mChipIdReported = TRUE;
    DEBUG ((
      DEBUG_INFO,
      "Pmic: chip-ID check on S_TWI0: read %02x, %a\n",
      ChipId,
      (ChipId == AXP8191_CHIP_ID_VALUE) ? "matches AXP8191 (03)" : "does not match AXP8191 (03)"
      ));
  }

  if (ChipId != AXP8191_CHIP_ID_VALUE) {
    return EFI_NOT_FOUND;
  }

  return EFI_SUCCESS;
}

/**
  Read the chip's ID register (0x0E).

  @param[out] ChipId  The register's value.

  @retval EFI_SUCCESS       The register was read.
  @retval EFI_DEVICE_ERROR  The bus did not answer.
**/
EFI_STATUS
EFIAPI
A733PmicGetChipId (
  OUT UINT8  *ChipId
  )
{
  if (ChipId == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return PmicRead (AXP8191_CHIP_ID, ChipId);
}

/**
  Read a rail's programmed voltage.

  @param[in]  Rail       Rail to read.
  @param[out] MilliVolt  The programmed voltage in mV.

  @retval EFI_SUCCESS            The voltage was read.
  @retval EFI_INVALID_PARAMETER  Rail is out of range.
  @retval EFI_DEVICE_ERROR       The bus did not answer.
**/
EFI_STATUS
EFIAPI
A733PmicGetRailMv (
  IN  A733_PMIC_RAIL  Rail,
  OUT UINT32          *MilliVolt
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  if ((Rail >= A733_PMIC_RAIL_MAX) || (MilliVolt == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = PmicRead (mRails[Rail].VolReg, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *MilliVolt = AXP8191_LDO_VOL_BASE
               + (UINT32)(Value & AXP8191_LDO_VOL_MASK) * AXP8191_LDO_VOL_STEP;

  return EFI_SUCCESS;
}

/**
  Report whether a rail's enable bit is set.

  @param[in] Rail  Rail to query.

  @return TRUE when the rail is enabled, FALSE when it is disabled or Rail is
          out of range (or the bus did not answer).
**/
BOOLEAN
EFIAPI
A733PmicRailEnabled (
  IN A733_PMIC_RAIL  Rail
  )
{
  UINT8  Ctl2;

  if (Rail >= A733_PMIC_RAIL_MAX) {
    return FALSE;
  }

  if (EFI_ERROR (PmicRead (AXP8191_LDO_POWER_ON_OFF_CTL2, &Ctl2))) {
    return FALSE;
  }

  return (Ctl2 & (1U << mRails[Rail].EnableBit)) != 0;
}

/**
  Read LDO_POWER_ON_OFF_CTL2 (0x21).

  @param[out] Value  The register's value.

  @retval EFI_SUCCESS       The register was read.
  @retval EFI_DEVICE_ERROR  The bus did not answer.
**/
EFI_STATUS
EFIAPI
A733PmicGetLdoCtl2 (
  OUT UINT8  *Value
  )
{
  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return PmicRead (AXP8191_LDO_POWER_ON_OFF_CTL2, Value);
}

/**
  Set a rail's voltage and ensure it is enabled.

  @param[in] Rail       Rail to set.
  @param[in] MilliVolt  Requested voltage in mV (500..3600, step 100).

  @retval EFI_SUCCESS            The voltage and enable bit were written.
  @retval EFI_INVALID_PARAMETER  Rail is out of range, or MilliVolt is not on
                                 the rail's grid.
  @retval EFI_DEVICE_ERROR       The bus did not answer.
**/
EFI_STATUS
EFIAPI
A733PmicSetRailMv (
  IN A733_PMIC_RAIL  Rail,
  IN UINT32          MilliVolt
  )
{
  EFI_STATUS  Status;
  UINT8       Value;
  UINT32      Code;

  if (Rail >= A733_PMIC_RAIL_MAX) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Refuse a voltage the rail cannot produce rather than rounding to one it
  // can: 500 mV + 100 mV * code, code 0..31.
  //
  if ((MilliVolt < AXP8191_LDO_VOL_MIN) || (MilliVolt > AXP8191_LDO_VOL_MAX) ||
      (((MilliVolt - AXP8191_LDO_VOL_BASE) % AXP8191_LDO_VOL_STEP) != 0))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Pmic: %u mV is off the CLDO grid (500..3600 mV, 100 mV steps)\n",
      MilliVolt
      ));
    return EFI_INVALID_PARAMETER;
  }

  Code = (MilliVolt - AXP8191_LDO_VOL_BASE) / AXP8191_LDO_VOL_STEP;

  //
  // Voltage first, then enable: the rail comes up already holding the value
  // it was asked for.
  //
  Status = PmicRead (mRails[Rail].VolReg, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PmicWrite (
             mRails[Rail].VolReg,
             (UINT8)((Value & ~AXP8191_LDO_VOL_MASK) | Code)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PmicRead (AXP8191_LDO_POWER_ON_OFF_CTL2, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return PmicWrite (
           AXP8191_LDO_POWER_ON_OFF_CTL2,
           (UINT8)(Value | (1U << mRails[Rail].EnableBit))
           );
}

/**
  Read DCDC1: its control word, its voltage register and what they mean.

  @param[out] State  The rail, as read.

  @retval EFI_SUCCESS            Both registers were read.
  @retval EFI_INVALID_PARAMETER  State is NULL.
  @retval EFI_DEVICE_ERROR       The bus did not answer.
**/
EFI_STATUS
EFIAPI
A733PmicGetDcdc1 (
  OUT A733_PMIC_DCDC1  *State
  )
{
  EFI_STATUS  Status;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = PmicRead (AXP8191_DCDC_POWER_ON_OFF_CTL1, &State->Ctl1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PmicRead (AXP8191_DC1OUT_VOL, &State->Vol);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  State->Enabled = (State->Ctl1 & (1U << AXP8191_DCDC1_ENABLE_BIT)) != 0;
  State->MilliVolt = AXP8191_DCDC_VOL_BASE
                     + (UINT32)(State->Vol & AXP8191_DCDC_VOL_MASK) * AXP8191_DCDC_VOL_STEP;

  return EFI_SUCCESS;
}

/**
  Set DCDC1's enable bit, then read the rail back.

  The write is a read-modify-write of the one word, so nothing else in
  DCDC_POWER_ON_OFF_CTL1 changes; a rail that already reads enabled is not
  written at all.

  @param[out] State  The rail, as read back.

  @retval EFI_SUCCESS            DCDC1 is enabled.
  @retval EFI_INVALID_PARAMETER  State is NULL.
  @retval EFI_DEVICE_ERROR       The bus did not answer.
**/
EFI_STATUS
EFIAPI
A733PmicEnableDcdc1 (
  OUT A733_PMIC_DCDC1  *State
  )
{
  EFI_STATUS  Status;
  UINT8       Ctl1;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = PmicRead (AXP8191_DCDC_POWER_ON_OFF_CTL1, &Ctl1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Ctl1 & (1U << AXP8191_DCDC1_ENABLE_BIT)) == 0) {
    Status = PmicWrite (
               AXP8191_DCDC_POWER_ON_OFF_CTL1,
               (UINT8)(Ctl1 | (1U << AXP8191_DCDC1_ENABLE_BIT))
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return A733PmicGetDcdc1 (State);
}

/**
  Read and, if needed, enable one of the ELDO rails (see the header).
**/
EFI_STATUS
EFIAPI
A733PmicEnableEldo (
  IN  BOOLEAN         Eldo2,
  IN  UINT32          MilliVolt,
  OUT A733_PMIC_LDO   *State
  )
{
  EFI_STATUS  Status;
  UINT8       LdoCtl;
  UINT8       Vol;
  UINT8       Bit;
  UINT8       Reg;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MilliVolt < AXP8191_LDO_VOL_MIN) || (MilliVolt > AXP8191_LDO_VOL_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  Bit = Eldo2 ? AXP8191_ELDO2_ENABLE_BIT : AXP8191_ELDO1_ENABLE_BIT;
  Reg = Eldo2 ? AXP8191_ELDO2OUT_VOL : AXP8191_ELDO1OUT_VOL;

  Status = PmicRead (AXP8191_LDO_POWER_ON_OFF_CTL3, &LdoCtl);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((LdoCtl & (1U << Bit)) == 0) {
    //
    // Off: program the voltage first and enable second, so the rail comes up at
    // its final level instead of at whatever the register happened to hold.
    //
    Vol = (UINT8)((MilliVolt - AXP8191_LDO_VOL_BASE) / AXP8191_LDO_VOL_STEP);
    Status = PmicWrite (Reg, (UINT8)((Vol & AXP8191_LDO_VOL_MASK) | 0x00));
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = PmicWrite (
               AXP8191_LDO_POWER_ON_OFF_CTL3,
               (UINT8)(LdoCtl | (1U << Bit))
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    MicroSecondDelay (2000);
  }

  //
  // Read back what the chip holds now, which is the only form of "it is on"
  // this function reports.
  //
  Status = PmicRead (AXP8191_LDO_POWER_ON_OFF_CTL3, &State->LdoCtl);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PmicRead (Reg, &State->Vol);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  State->Enabled   = (State->LdoCtl & (1U << Bit)) != 0;
  State->MilliVolt = AXP8191_LDO_VOL_BASE
                   + (((UINT32)State->Vol & AXP8191_LDO_VOL_MASK) * AXP8191_LDO_VOL_STEP);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
A733PmicReadReg (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{
  return A733TwiReadReg ((UINTN)PcdGet64 (PcdA733TwiBase), AXP8191_SLAVE_ADDR, Reg, Value);
}

EFI_STATUS
EFIAPI
A733PmicWriteReg (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  return A733TwiWriteReg ((UINTN)PcdGet64 (PcdA733TwiBase), AXP8191_SLAVE_ADDR, Reg, Value);
}
