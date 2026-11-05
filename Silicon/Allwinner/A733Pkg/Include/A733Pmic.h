/** @file
  AXP8191 PMIC register map, as used by the Allwinner A733 (sun60iw2p1) on
  the Radxa Cubie A7A.

  Register addresses are transcribed from the vendor's own PMIC driver
  (u-boot/drivers/sunxi_power/pmu_axp8191.c and its pmu_axp8191.h), which is
  also where the CLDO voltage encoding comes from.  The rail table there gives
  CLDO1..CLDO5 as register 0x2F..0x33, mask 0x1F, minimum 500 mV, step 100 mV,
  with the enable bit in LDO_POWER_ON_OFF_CTL2 (0x21) at bit 3..7.  The board's
  device tree puts the chip at 7-bit I2C address 0x36 on S_TWI0
  (x-powers,axp8191, reg = <0x36>), and the vendor probe accepts the part when
  CHIP_ID (0x0E) reads 0x03.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_PMIC_H__
#define A733_PMIC_H__

#include <Uefi.h>

//
// The chip's 7-bit I2C address on S_TWI0 (device tree: reg = <0x36>).
//
#define AXP8191_SLAVE_ADDR  0x36U

//
// Register addresses (vendor pmu_axp8191.h).
//
#define AXP8191_CHIP_ID                 0x0EU
#define AXP8191_CHIP_VER                0x0FU

#define AXP8191_DCDC_POWER_ON_OFF_CTL1  0x10U
#define AXP8191_DC1OUT_VOL              0x12U
#define AXP8191_LDO_POWER_ON_OFF_CTL2   0x21U
//
// LDO_POWER_ON_OFF_CTL3 (0x22): one enable bit per ELDO/DLDO.  ELDO1 is bit 6
// and ELDO2 bit 7 (vendor pmu_axp8191.c, the AXP8191_POWER_ELDO1/ELDO2 rows).
// These two rails are the DWC3's USB2 PHY: the schematic's VCC33-USB-2 is
// ELDO2 (3.3 V, page 4) and VCC33-18-USB-2 is ELDO1 (1.8 V, page 4), and the
// board's device tree gives neither rail regulator-boot-on - they are off
// until something enables them, which is why the U2P0 receiver was dead while
// its port state machine still reported RxDetect.
//
#define AXP8191_LDO_POWER_ON_OFF_CTL3   0x22U
#define AXP8191_ELDO1_ENABLE_BIT        6U
#define AXP8191_ELDO2_ENABLE_BIT        7U

#define AXP8191_ELDO1OUT_VOL  0x3AU
#define AXP8191_ELDO2OUT_VOL  0x3BU

#define AXP8191_CLDO1OUT_VOL  0x2FU
#define AXP8191_CLDO2OUT_VOL  0x30U
#define AXP8191_CLDO3OUT_VOL  0x31U
#define AXP8191_CLDO4OUT_VOL  0x32U
#define AXP8191_CLDO5OUT_VOL  0x33U

//
// CHIP_ID reads this on the AXP8191 (vendor probe: "AXP8191 Version-A-ID").
//
#define AXP8191_CHIP_ID_VALUE  0x03U

//
// The CLDO (and ALDO/BLDO/DLDO/ELDO) voltage registers share one encoding:
// bits [4:0] hold the code, and the rail is 500 mV + 100 mV * code.
//
#define AXP8191_LDO_VOL_MASK   0x1FU
#define AXP8191_LDO_VOL_BASE   500U
#define AXP8191_LDO_VOL_STEP   100U
#define AXP8191_LDO_VOL_MIN    500U
#define AXP8191_LDO_VOL_MAX    3600U

//
// DCDC1, the 3.3 V rail the board's USB2 hub is powered from: schematic page
// 14, U6's VD33 (pin 20) is net VCC_3V3_USB20HUB behind FB3, and that net is
// DCDC1 through R58.  (The hub's VDD5 is the 5 V input, fed through L7, which
// is not fitted - the board runs it from 3.3 V alone.)
// The enable bit is bit 0 of DCDC_POWER_ON_OFF_CTL1 (0x10) and the voltage
// register is DC1OUT_VOL (0x12), mask 0x1F, 1000 mV + 100 mV * code: the
// vendor's pmu_axp8191_ctrl_tbl row "dcdc1" (u-boot
// drivers/sunxi_power/pmu/pmu_axp8191.c), whose control bit is the one the
// driver reads back with pmic_bus_read()/pmic_bus_write().
//
#define AXP8191_DCDC1_ENABLE_BIT  0
#define AXP8191_DCDC_VOL_MASK     0x1FU
#define AXP8191_DCDC_VOL_BASE     1000U
#define AXP8191_DCDC_VOL_STEP     100U

#endif