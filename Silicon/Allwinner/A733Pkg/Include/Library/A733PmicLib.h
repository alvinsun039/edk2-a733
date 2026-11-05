/** @file
  AXP8191 PMIC library for the Allwinner A733 (sun60iw2p1).

  Register addresses are in <A733Pmic.h>.  This library binds the AXP8191 on
  S_TWI0 and reports/sets its CLDO rails; it is what answers whether the
  board's eMMC I/O supply (CLDO5, "vcc-pc") is already at 1.8 V, by reading
  the chip rather than trusting the device tree.  DCDC1 - the 3.3 V rail the
  board's USB2 hub is powered from - is modelled beside them, because its
  enable bit is in its own register and the hub is invisible on any host
  controller while that rail is down.

  A PMIC that does not answer is not fatal: every entry point returns an error
  and the platform continues.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_PMIC_LIB_H__
#define A733_PMIC_LIB_H__

#include <Uefi.h>

/**
  The PMIC rails this library models.

  CLDO1..CLDO5 are contiguous registers (0x2F..0x33) with the same encoding
  and enabling, so all five are modelled; the board's eMMC I/O supply is
  CLDO5.
**/
typedef enum {
  A733_PMIC_CLDO1 = 0,
  A733_PMIC_CLDO2,
  A733_PMIC_CLDO3,
  A733_PMIC_CLDO4,
  A733_PMIC_CLDO5,
  A733_PMIC_RAIL_MAX
} A733_PMIC_RAIL;

/**
  Bring S_TWI0 up at 100 kHz and probe the AXP8191.

  Muxes the bus pins, initialises the TWI controller, reads CHIP_ID (0x0E)
  and logs the value and whether it matched the expected 0x03.  A failure here
  leaves the platform free to continue; nothing is left half-configured that a
  later transfer could trip over.

  @retval EFI_SUCCESS            The chip answered and its ID matched.
  @retval EFI_DEVICE_ERROR       The chip did not answer on S_TWI0.
  @retval EFI_INVALID_PARAMETER  The bus rate could not be programmed.
  @retval EFI_NOT_FOUND          The chip answered with an unexpected ID.
**/
EFI_STATUS
EFIAPI
A733PmicInit (
  VOID
  );

/**
  Read the chip's ID register (0x0E).

  @param[out] ChipId  The register's value.

  @retval EFI_SUCCESS       The register was read.
  @retval EFI_DEVICE_ERROR  The bus did not answer.

  @note The bus must already be up (A733PmicInit).
**/
EFI_STATUS
EFIAPI
A733PmicGetChipId (
  OUT UINT8  *ChipId
  );

/**
  Read a rail's programmed voltage.

  @param[in]  Rail       Rail to read.
  @param[out] MilliVolt  The programmed voltage in mV.

  @retval EFI_SUCCESS            The voltage was read.
  @retval EFI_INVALID_PARAMETER  Rail is out of range.
  @retval EFI_DEVICE_ERROR       The bus did not answer.

  @note The bus must already be up (A733PmicInit).
**/
EFI_STATUS
EFIAPI
A733PmicGetRailMv (
  IN  A733_PMIC_RAIL  Rail,
  OUT UINT32          *MilliVolt
  );

/**
  Report whether a rail's enable bit is set.

  @param[in] Rail  Rail to query.

  @return TRUE when the rail is enabled, FALSE when it is disabled or Rail is
          out of range.
**/
BOOLEAN
EFIAPI
A733PmicRailEnabled (
  IN A733_PMIC_RAIL  Rail
  );

/**
  Read LDO_POWER_ON_OFF_CTL2 (0x21), the register whose bits 3..7 enable
  CLDO1..CLDO5.

  Exposed so the platform's one-line PMIC report can print the register
  verbatim next to the rail it controls.

  @param[out] Value  The register's value.

  @retval EFI_SUCCESS       The register was read.
  @retval EFI_DEVICE_ERROR  The bus did not answer.

  @note The bus must already be up (A733PmicInit).
**/
EFI_STATUS
EFIAPI
A733PmicGetLdoCtl2 (
  OUT UINT8  *Value
  );

/**
  Set a rail's voltage and ensure it is enabled.

  The voltage register is written first, then the rail's enable bit in
  LDO_POWER_ON_OFF_CTL2.  Only voltages on the rail's 500 mV + 100 mV grid are
  accepted; anything else is refused rather than rounded, so a caller cannot
  ask for a value the hardware cannot produce.

  @param[in] Rail       Rail to set.
  @param[in] MilliVolt  Requested voltage in mV (500..3600, step 100).

  @retval EFI_SUCCESS            The voltage and enable bit were written.
  @retval EFI_INVALID_PARAMETER  Rail is out of range, or MilliVolt is not on
                                 the rail's grid.
  @retval EFI_DEVICE_ERROR       The bus did not answer.

  @note The bus must already be up (A733PmicInit).
**/
EFI_STATUS
EFIAPI
A733PmicSetRailMv (
  IN A733_PMIC_RAIL  Rail,
  IN UINT32          MilliVolt
  );

/**
  DCDC1 as the chip holds it: its control word, its voltage register, and the
  two facts a caller acts on.
**/
typedef struct {
  /// DCDC_POWER_ON_OFF_CTL1 (0x10), verbatim.
  UINT8      Ctl1;
  /// DC1OUT_VOL (0x12), verbatim.
  UINT8      Vol;
  /// CTL1's enable bit (bit 0) as read.
  BOOLEAN    Enabled;
  /// DC1OUT_VOL decoded: 1000 mV + 100 mV * code.
  UINT32     MilliVolt;
} A733_PMIC_DCDC1;

/**
  Read DCDC1, the 3.3 V rail the board's USB2 hub sits on.

  DCDC1 is not one of the LDOs: it is enabled by bit 0 of
  DCDC_POWER_ON_OFF_CTL1 (0x10) and its voltage lives in DC1OUT_VOL (0x12).
  Both are read here, so a caller can print what the chip holds rather than
  what a previous stage is believed to have programmed.

  @param[out] State  The rail, as read.

  @retval EFI_SUCCESS            The rail was read.
  @retval EFI_INVALID_PARAMETER  State is NULL.
  @retval EFI_DEVICE_ERROR       The bus did not answer.

  @note The bus must already be up (A733PmicInit).
**/
EFI_STATUS
EFIAPI
A733PmicGetDcdc1 (
  OUT A733_PMIC_DCDC1  *State
  );

/**
  Set DCDC1's enable bit, then read the rail back.

  A rail that is already enabled is left alone - no write is issued - so this
  is safe to call from every bring-up that needs the rail.  The returned state
  is a read of the chip after the write, which is the only form of "it is on"
  a log should carry: a chip that ignored the bit reads exactly like one that
  took it until the register is read again.

  @param[out] State  The rail, as read back.

  @retval EFI_SUCCESS            DCDC1 is enabled (by this call or already).
  @retval EFI_INVALID_PARAMETER  State is NULL.
  @retval EFI_DEVICE_ERROR       The bus did not answer.

  @note The bus must already be up (A733PmicInit).
**/
EFI_STATUS
EFIAPI
A733PmicEnableDcdc1 (
  OUT A733_PMIC_DCDC1  *State
  );

//
// One AXP8191 LDO: the enable lives in LDO_POWER_ON_OFF_CTL3 and the voltage
// code in its own register, both under the shared 500 mV + 100 mV * code
// encoding.
//
typedef struct {
  /// LDO_POWER_ON_OFF_CTL3 (0x22) as read.
  UINT8    LdoCtl;
  /// The rail's voltage register, verbatim.
  UINT8    Vol;
  /// The rail's enable bit in LdoCtl, as read.
  BOOLEAN  Enabled;
  /// Vol decoded: 500 mV + 100 mV * code.
  UINT32   MilliVolt;
} A733_PMIC_LDO;

/**
  Read and, if needed, enable one of the ELDO rails.

  ELDO1 (1.8 V) and ELDO2 (3.3 V) are the DWC3's USB2 PHY rails on this board
  (schematic page 4: VCC33-18-USB-2 and VCC33-USB-2).  Neither is
  regulator-boot-on in the device tree, so both are off unless something turns
  them on - and a PHY without its analog rails keeps its port state machine
  running while seeing nothing on the wire, which is exactly the symptom this
  exists to remove.

  A rail that is already enabled keeps its voltage: no write is issued, so a
  rail another stage programmed is not disturbed.  The returned state is a read
  of the chip after anything this call did.

  @param[in]  Eldo2   TRUE for ELDO2 (3.3 V), FALSE for ELDO1 (1.8 V).
  @param[in]  MilliVolt  Voltage to program when the rail has to be enabled.
  @param[out] State   The rail, as read back.

  @retval EFI_SUCCESS            The rail is enabled (by this call or already).
  @retval EFI_INVALID_PARAMETER  State is NULL, or MilliVolt is out of range.
  @retval EFI_DEVICE_ERROR       The bus did not answer.

  @note The bus must already be up (A733PmicInit).
**/
EFI_STATUS
EFIAPI
A733PmicEnableEldo (
  IN  BOOLEAN         Eldo2,
  IN  UINT32          MilliVolt,
  OUT A733_PMIC_LDO   *State
  );

/**
  Read one AXP8191 register over S_TWI0.

  The rail helpers above cover the rails this tree needs; a caller that
  has to touch a control bit they do not model (the GMAC's SWOUT1 enable
  is one) uses this instead of talking to the bus itself.

  @param[in]  Reg    Register address.
  @param[out] Value  Its value.

  @retval EFI_SUCCESS       The register was read.
  @retval EFI_DEVICE_ERROR  The PMIC did not answer.

**/
EFI_STATUS
EFIAPI
A733PmicReadReg (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  );

/**
  Write one AXP8191 register over S_TWI0.

  @param[in] Reg    Register address.
  @param[in] Value  Value to write.

  @retval EFI_SUCCESS       The register was written.
  @retval EFI_DEVICE_ERROR  The PMIC did not answer.

**/
EFI_STATUS
EFIAPI
A733PmicWriteReg (
  IN UINT8  Reg,
  IN UINT8  Value
  );

#endif
