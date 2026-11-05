/** @file
  SunxiRngDxe - minimal EFI_RNG_PROTOCOL for the Allwinner A733.

  The A733 (Cortex-A76/A55) has no FEAT_RNG (RNDR) and the vendor TF-A
  does not implement the SMCCC TRNG interface, so the standard
  SecurityPkg RngDxe finds zero usable algorithms and declines to
  install the protocol.  But DxeNetLib's [Depex] requires
  gEfiRngProtocolGuid to exist, which was silently blocking the entire
  network stack (DwEmacSnpDxe, SnpDxe, MnpDxe, Dhcp4Dxe, ... all stay
  "discovered but not loaded" forever).

  This driver installs the protocol backed by a non-cryptographic PRNG
  (xorshift64*) seeded from the ARM generic timer, and advertises the
  gEfiRngAlgorithmRaw algorithm so DxeNetLib's secure-algorithm list
  accepts it.  Sufficient for DHCP transaction IDs / TCP ISNs during
  boot; NOT for anything security-sensitive (replace with a real TRNG
  before enabling Secure Boot).

  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/Rng.h>
#include <Guid/Rng.h>

STATIC UINT64  mPrngState;

STATIC
UINT64
GetEntropySeed (
  VOID
  )
{
  // Mix the arch timer counter with a fixed salt.  The timer free-runs
  // from reset at a per-boot phase, so consecutive boots differ.
  return GetPerformanceCounter () ^ 0x9E3779B97F4A7C15ULL;
}

STATIC
UINT64
PrngNext (
  VOID
  )
{
  // xorshift64*.
  UINT64  X = mPrngState;

  X ^= X >> 12;
  X ^= X << 25;
  X ^= X >> 27;
  mPrngState = X;

  return X * 0x2545F4914F6CDD1DULL;
}

STATIC
EFI_STATUS
EFIAPI
SunxiRngGetInfo (
  IN  EFI_RNG_PROTOCOL  *This,
  IN  OUT UINTN         *RNGAlgorithmListSize,
  OUT EFI_RNG_ALGORITHM *RNGAlgorithmList
  )
{
  UINTN  Size;

  if ((This == NULL) || (RNGAlgorithmListSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Size = sizeof (EFI_RNG_ALGORITHM);
  if (*RNGAlgorithmListSize < Size) {
    *RNGAlgorithmListSize = Size;
    return EFI_BUFFER_TOO_SMALL;
  }

  if (RNGAlgorithmList == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *RNGAlgorithmListSize = Size;
  CopyMem (RNGAlgorithmList, &gEfiRngAlgorithmRaw, Size);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SunxiRngGetRng (
  IN  EFI_RNG_PROTOCOL  *This,
  IN  EFI_RNG_ALGORITHM *RNGAlgorithm OPTIONAL,
  IN  UINTN             RNGValueLength,
  OUT UINT8             *RNGValue
  )
{
  UINTN  Index;

  if ((This == NULL) || (RNGValueLength == 0) || (RNGValue == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (RNGAlgorithm != NULL &&
      !CompareGuid (RNGAlgorithm, &gEfiRngAlgorithmRaw))
  {
    return EFI_UNSUPPORTED;
  }

  for (Index = 0; Index < RNGValueLength; Index++) {
    RNGValue[Index] = (UINT8)(PrngNext () >> 56);
  }

  return EFI_SUCCESS;
}

STATIC EFI_RNG_PROTOCOL  mSunxiRng = {
  SunxiRngGetInfo,
  SunxiRngGetRng
};

EFI_STATUS
EFIAPI
SunxiRngDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;

  mPrngState = GetEntropySeed ();

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEfiRngProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mSunxiRng
                  );
  DEBUG ((DEBUG_INFO, "SunxiRng: EFI_RNG_PROTOCOL installed (Raw/PRNG) %r\n", Status));

  return Status;
}
