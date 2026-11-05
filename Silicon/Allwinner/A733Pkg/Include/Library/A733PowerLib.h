/** @file
  A733 PCK600 power-domain (PPU) library.

  PCK600 at 0x07060000 is the SoC-internal power-domain controller: each
  domain id owns a 0x1000 page holding a power switch command (+0x0) and a
  status register (+0x8).  Writing COMMAND_ON (0x8) powers the domain on;
  the status low nibble reads 0x8 once the rail is up.  The controller's
  own bus clock lives in the always-on R_CCU (CLK_R_PPU, 0x070101AC bit0)
  and is gated here before any domain is touched.

  Domain ids match bsp/include/dt-bindings/power/sun60iw2-power.h.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_POWER_LIB_H_
#define A733_POWER_LIB_H_

#include <Uefi/UefiBaseType.h>

typedef enum {
  A733_POWER_VI         = 0,   // camera / ISP
  A733_POWER_DE_SYS     = 1,   // display engine (de@0x5000000)
  A733_POWER_VE_DEC     = 2,   // video decoder
  A733_POWER_VE_ENC     = 3,   // video encoder
  A733_POWER_NPU        = 4,
  A733_POWER_GPU_TOP    = 5,
  A733_POWER_GPU_CORE   = 6,
  A733_POWER_PCIE       = 7,
  A733_POWER_USB2       = 8,
  A733_POWER_VO         = 9,   // video out LCD/DSI side
  A733_POWER_VO1        = 10   // hdmi0 + tcon3 + vo1
} A733_POWER_DOMAIN;

/**
  Power a domain on and wait until the rail is up.

  @param Domain  A733_POWER_DOMAIN id.

  @retval EFI_SUCCESS       Domain reports powered on.
  @retval EFI_TIMEOUT       The domain did not come up in time.
**/
EFI_STATUS
EFIAPI
A733PowerDomainOn (
  IN A733_POWER_DOMAIN  Domain
  );

/**
  Power a domain off.

  @param Domain  A733_POWER_DOMAIN id.
**/
VOID
EFIAPI
A733PowerDomainOff (
  IN A733_POWER_DOMAIN  Domain
  );

/**
  Query whether a domain is currently powered on.

  @param Domain  A733_POWER_DOMAIN id.

  @return  TRUE if the domain status reads powered-on.
**/
BOOLEAN
EFIAPI
A733PowerDomainIsOn (
  IN A733_POWER_DOMAIN  Domain
  );

#endif // A733_POWER_LIB_H_
