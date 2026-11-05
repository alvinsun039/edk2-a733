/** @file
  A733 PCK600 power-domain library implementation.

  Consolidated from A733HdmiDxe (PckPowerDomainOn) and SunxiUsbDxe
  (SunxiUsbPowerDomainOn), which each implemented the same sequence with
  their own macro set: open the R_CCU CLK_R_PPU gate, write COMMAND_ON
  (0x8) to the domain's power register, poll the status register until its
  low nibble reads 0x8.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/A733PowerLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define A733_PCK600_BASE       0x07060000UL
#define A733_R_CCU_BASE        0x07010000UL
#define R_CCU_R_PPU_GATE_REG   0x01AC
#define R_CCU_R_PPU_GATE_BIT   BIT0

#define PCK600_DOMAIN_STRIDE   0x1000
#define PCK600_PWR_OFFSET      0x0
#define PCK600_STATUS_OFFSET   0x8
#define PCK600_COMMAND_ON      0x8
#define PCK600_STATUS_ON       0x8
#define PCK600_STATUS_MASK     0xF
#define PCK600_POWER_TRIES     100
#define PCK600_POWER_DELAY_US  100

STATIC
UINTN
PowerDomainBase (
  IN A733_POWER_DOMAIN  Domain
  )
{
  return A733_PCK600_BASE + ((UINTN)Domain * PCK600_DOMAIN_STRIDE);
}

STATIC
VOID
EnsurePpuClocked (
  VOID
  )
{
  //
  // The PCK600 block hangs off the always-on R_CCU; its bus clock
  // (CLK_R_PPU) must be on before any PCK600 register answers.
  //
  MmioOr32 (A733_R_CCU_BASE + R_CCU_R_PPU_GATE_REG, R_CCU_R_PPU_GATE_BIT);
}

EFI_STATUS
EFIAPI
A733PowerDomainOn (
  IN A733_POWER_DOMAIN  Domain
  )
{
  UINTN   Base;
  UINTN   Tries;

  Base = PowerDomainBase (Domain);
  EnsurePpuClocked ();

  MmioWrite32 (Base + PCK600_PWR_OFFSET, PCK600_COMMAND_ON);

  for (Tries = 0; Tries < PCK600_POWER_TRIES; Tries++) {
    if ((MmioRead32 (Base + PCK600_STATUS_OFFSET) & PCK600_STATUS_MASK) ==
        PCK600_STATUS_ON) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (PCK600_POWER_DELAY_US);
  }

  DEBUG ((DEBUG_ERROR, "A733Power: domain %u did not power on (stat 0x%08x)\n",
          (UINT32)Domain, MmioRead32 (Base + PCK600_STATUS_OFFSET)));
  return EFI_TIMEOUT;
}

VOID
EFIAPI
A733PowerDomainOff (
  IN A733_POWER_DOMAIN  Domain
  )
{
  UINTN  Base;

  Base = PowerDomainBase (Domain);
  EnsurePpuClocked ();

  MmioWrite32 (Base + PCK600_PWR_OFFSET, 0);
}

BOOLEAN
EFIAPI
A733PowerDomainIsOn (
  IN A733_POWER_DOMAIN  Domain
  )
{
  UINTN  Base;

  Base = PowerDomainBase (Domain);
  EnsurePpuClocked ();

  return (MmioRead32 (Base + PCK600_STATUS_OFFSET) & PCK600_STATUS_MASK) ==
         PCK600_STATUS_ON;
}
