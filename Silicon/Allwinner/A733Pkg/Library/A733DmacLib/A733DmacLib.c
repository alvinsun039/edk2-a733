/** @file
  The A733's system DMA controller (DMAC): registers, descriptor, channels.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Manual: chapter 13 "DMA".  The register map is 13.1.6/13.1.7, the descriptor
  is 13.1.4.2 with table 13-3, the programming order is 13.1.5.2, and the DRQ
  numbers are 13.1.4.1 table 13-2.  A733Dmac.h carries the offsets and the field
  encodings; this file is the state machine over them.

  What this library deliberately does not have: an interrupt path.  The chapter
  gives a channel four event bits and a shared line into the GIC (or the CPUS
  field), but a caller that polls gets the same information from the pend bits and
  the residual counter, and a polled completion cannot race a handler that owns
  two registers of the same block.  The one caller - the MUSB device driver's bulk
  OUT path - polls from its own interrupt and from its poll tick, which is why the
  API here is "start / is it done / how much is left" rather than a callback.
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>

#include <A733Ccu.h>
#include <A733Dmac.h>

#include <Library/A733CcuLib.h>

//
// The block this library drives.  Both controllers are identical register files
// at one address each (A733Dmac.h); DMAC0 is the CPUX-domain block the manual's
// DRQ table describes, and the init line prints DMAC1's security word beside it
// so a boot log says whether the other one is present too.
//
#define A733_DMAC_BASE  A733_DMAC0_BASE

//
// The descriptor a channel is programmed with.  One per channel, in .bss: the
// controller fetches it from DRAM by physical address, so it has to be somewhere
// that stays put, 64-byte aligned so each one owns whole cache lines and cleaning
// it cannot touch a neighbour's, and inside the low 4 GB.
//
STATIC A733_DMAC_DESC  mDescriptor[A733_DMAC_CHANNELS] __attribute__ ((aligned (64)));

//
// Channels this library has handed out.  The controller's own enable and status
// bits are the authority on whether a channel is in use; this only says which of
// them belong to a caller, so a channel somebody else programmed is never taken.
//
STATIC BOOLEAN  mHeld[A733_DMAC_CHANNELS];

STATIC BOOLEAN  mInitialised;

STATIC
UINTN
DmacReg (
  IN UINT32  Offset
  )
{
  return (UINTN)A733_DMAC_BASE + Offset;
}

/**
  Manual 13.1.5.1: the reset is de-asserted before the bus gate is opened, and
  the DMA0 clock is AHB_CLK with no parent or divider of its own (4.1.6.85), so
  there is nothing else to program.  The CCU rows are the same ones
  A733CcuEnableModule writes: DMA0_BGR_REG, gating bit 0 and reset bit 16.
**/
EFI_STATUS
EFIAPI
A733DmacInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Bgr;
  UINT32      Security;
  UINT32      OtherSecurity;

  if (mInitialised) {
    return EFI_SUCCESS;
  }

  Status = A733CcuEnableModule (A733_CCU_MODULE_DMA0);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Dmac: the CCU refuses the DMA0 gate (%r); the block is not initialised\n",
      Status
      ));
    return Status;
  }

  Bgr = MmioRead32 ((UINTN)PcdGet64 (PcdA733CcuBase) + A733_CCU_DMA0_BGR);

  //
  // Manual 13.1.7.2: the note under DMA_AUTO_GATE_REG asks software to set bit 2
  // when it initialises the controller - it disables auto gating of the MCLK
  // interface circuit, which is what the manual wants at init time.  The other
  // two bits stay as they are (auto gating enabled).
  //
  MmioOr32 (DmacReg (A733_DMAC_AUTO_GATE_REG), BIT2);

  Security      = MmioRead32 (DmacReg (A733_DMAC_SEC_REG));
  OtherSecurity = MmioRead32 ((UINTN)A733_DMAC1_BASE + A733_DMAC_SEC_REG);

  //
  // The two readings that say whether the block is there at all: SEC's reset
  // value is 0x0000_FFFF (every channel non-secure, 13.1.7.1) and an address
  // with no slave behind it reads as zeros.  The status word is printed for the
  // same reason - a channel somebody else left enabled would show up here.  On
  // this die the security word is what a first attempt at this library ran into:
  // SEC reads 0, so nothing can be armed until the secure world opens it, and
  // this line is the one that says so on any future boot (see A733Dmac.h).
  //
  DEBUG ((
    DEBUG_INFO,
    "Dmac: block 0x%08x, CCU DMA0_BGR reads 0x%08x (gate=%u reset=%u); "
    "SEC 0x%08x AUTO_GATE 0x%08x STA 0x%08x; the other block's SEC reads 0x%08x\n",
    (UINT32)A733_DMAC_BASE,
    Bgr,
    (UINT32)((Bgr & A733_CCU_BGR_SUB_CLK (A733_CCU_BGR_MAIN_SUB)) != 0),
    (UINT32)((Bgr & A733_CCU_BGR_SUB_RST (A733_CCU_BGR_MAIN_SUB)) != 0),
    Security,
    MmioRead32 (DmacReg (A733_DMAC_AUTO_GATE_REG)),
    MmioRead32 (DmacReg (A733_DMAC_STA_REG)),
    OtherSecurity
    ));

  mInitialised = TRUE;
  return EFI_SUCCESS;
}

BOOLEAN
EFIAPI
A733DmacIsInitialised (
  VOID
  )
{
  return mInitialised;
}

/**
  Manual 13.1.5.2 step 2: a channel is idle when it is not enabled.  The status
  bit is checked as well, because a channel that is finishing its queue can read
  as disabled for a moment while its last burst is still in the MBUS.
**/
EFI_STATUS
EFIAPI
A733DmacAcquireChannel (
  OUT UINT8  *Channel
  )
{
  UINT8   Index;
  UINT32  Busy;
  UINT32  Enable;

  if (Channel == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mInitialised) {
    return EFI_NOT_READY;
  }

  Busy = MmioRead32 (DmacReg (A733_DMAC_STA_REG));

  for (Index = 0; Index < A733_DMAC_CHANNELS; Index++) {
    if (mHeld[Index]) {
      continue;
    }

    Enable = MmioRead32 (DmacReg (A733_DMAC_EN (Index)));
    if (((Enable & A733_DMAC_EN_MASK) != 0) || ((Busy & (1U << Index)) != 0)) {
      continue;
    }

    mHeld[Index] = TRUE;
    *Channel     = Index;
    return EFI_SUCCESS;
  }

  return EFI_OUT_OF_RESOURCES;
}

VOID
EFIAPI
A733DmacReleaseChannel (
  IN UINT8  Channel
  )
{
  if (Channel >= A733_DMAC_CHANNELS) {
    return;
  }

  if (mHeld[Channel]) {
    A733DmacStop (Channel);
    mHeld[Channel] = FALSE;
  }
}

EFI_STATUS
EFIAPI
A733DmacStartTransfer (
  IN UINT8                     Channel,
  IN CONST A733_DMAC_TRANSFER  *Transfer
  )
{
  A733_DMAC_DESC  *Desc;
  UINT32          Config;

  if ((Channel >= A733_DMAC_CHANNELS) || (Transfer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mInitialised) {
    return EFI_NOT_READY;
  }

  //
  // The descriptor's address words are 32-bit and the bits above bit 31 live in
  // the Parameter word, which this library writes as zero (A733_DMAC_PARA_NONE).
  // A caller above 4 GB would have its transfer aimed at the wrong place, so it
  // is refused here rather than trusted.
  //
  if ((Transfer->Source > 0xFFFFFFFFULL) || (Transfer->Destination > 0xFFFFFFFFULL) ||
      ((UINTN)&mDescriptor[Channel] > 0xFFFFFFFFULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Manual 13.1.4.2: the byte counter is a package's data amount and tops out at
  // 2^25-1 bytes.  Zero would be a package that moves nothing, which is not a
  // transfer.
  //
  if ((Transfer->Length == 0) || (Transfer->Length > 0x01FFFFFFU)) {
    return EFI_INVALID_PARAMETER;
  }

  Config  = A733_DMAC_CFG_SRC_DRQ (Transfer->SourceDrq);
  Config |= A733_DMAC_CFG_SRC_BLOCK (Transfer->SourceBlock);
  Config |= A733_DMAC_CFG_SRC_WIDTH (Transfer->SourceWidth);
  Config |= A733_DMAC_CFG_DST_DRQ (Transfer->DestinationDrq);
  Config |= A733_DMAC_CFG_DST_BLOCK (Transfer->DestinationBlock);
  Config |= A733_DMAC_CFG_DST_WIDTH (Transfer->DestinationWidth);
  if (Transfer->SourceIoMode) {
    Config |= A733_DMAC_CFG_SRC_IO_MODE;
  }

  if (Transfer->DestinationIoMode) {
    Config |= A733_DMAC_CFG_DST_IO_MODE;
  }

  Desc              = &mDescriptor[Channel];
  Desc->Config      = Config;
  Desc->Source      = Transfer->Source;
  Desc->Destination = Transfer->Destination;
  Desc->Count       = Transfer->Length;
  Desc->Parameter   = A733_DMAC_PARA_NONE;
  Desc->Link        = A733_DMAC_LINK_END;

  //
  // The controller reads the descriptor out of DRAM, so what the CPU just wrote
  // has to be there before the channel is enabled - and the line must not be
  // left in the cache afterwards, or a later write-back would put an older
  // descriptor on top of this one.
  //
  WriteBackInvalidateDataCacheRange (Desc, sizeof (*Desc));

  //
  // 13.1.5.2 steps 4 and 5: the descriptor address first, the channel last.
  // Clearing the latched events before the start bit is what makes the next
  // reading of them this transfer's.
  //
  // The disable comes first because the manual's step 10 has the software
  // disable a channel when its queue ends - the controller does not clear EN
  // itself - so a second transfer on the same channel would otherwise be a "1"
  // written over a "1", i.e. no edge for the channel to start on.
  //
  MmioWrite32 (DmacReg (A733_DMAC_EN (Channel)), 0);
  MmioWrite32 (DmacReg (A733_DMAC_DESC_HIG (Channel)), 0);
  MmioWrite32 (DmacReg (A733_DMAC_DESC_ADDR (Channel)), (UINT32)(UINTN)Desc);
  MmioWrite32 (DmacReg (A733_DMAC_IRQ_PEND (Channel)), A733_DMAC_IRQ_ALL);
  MmioWrite32 (DmacReg (A733_DMAC_EN (Channel)), A733_DMAC_EN_ENABLE);

  return EFI_SUCCESS;
}

BOOLEAN
EFIAPI
A733DmacIdle (
  IN UINT8  Channel
  )
{
  if (Channel >= A733_DMAC_CHANNELS) {
    return TRUE;
  }

  return (MmioRead32 (DmacReg (A733_DMAC_STA_REG)) & (1U << Channel)) == 0;
}

BOOLEAN
EFIAPI
A733DmacCompleted (
  IN UINT8  Channel
  )
{
  UINT32  Pending;

  if (Channel >= A733_DMAC_CHANNELS) {
    return FALSE;
  }

  //
  // The latched package-end bit is the authoritative reading: the controller
  // sets it when the package's last byte has been written, and it is a status
  // bit - a channel's interrupt is left disabled here (DMA_IRQ_EN_REG is never
  // written), so nothing else reads it and no interrupt is raised.
  //
  Pending = MmioRead32 (DmacReg (A733_DMAC_IRQ_PEND (Channel)));
  if ((Pending & (A733_DMAC_IRQ_PKG_END | A733_DMAC_IRQ_QUEUE_END)) != 0) {
    return TRUE;
  }

  //
  // The second reading covers a pend bit that has not become visible yet: an
  // exhausted byte counter on a channel that has completed at least one package.
  // The package count is what keeps a freshly armed channel - whose counter has
  // not been loaded out of the descriptor yet and therefore reads zero - from
  // looking finished.
  //
  return (A733DmacResidual (Channel) == 0) &&
         ((MmioRead32 (DmacReg (A733_DMAC_PKG_NUM (Channel))) & 0xFFFFFFFFU) != 0);
}

UINT32
EFIAPI
A733DmacResidual (
  IN UINT8  Channel
  )
{
  if (Channel >= A733_DMAC_CHANNELS) {
    return 0;
  }

  return MmioRead32 (DmacReg (A733_DMAC_BCNT_LEFT (Channel))) & 0x01FFFFFFU;
}

VOID
EFIAPI
A733DmacStop (
  IN UINT8  Channel
  )
{
  if (Channel >= A733_DMAC_CHANNELS) {
    return;
  }

  //
  // Disable first: the channel must not fetch another descriptor while the
  // latched events are being cleared, and a channel left enabled would keep
  // writing into memory the caller may already have handed on.
  //
  MmioWrite32 (DmacReg (A733_DMAC_EN (Channel)), 0);
  MmioWrite32 (DmacReg (A733_DMAC_IRQ_PEND (Channel)), A733_DMAC_IRQ_ALL);
}

VOID
EFIAPI
A733DmacGetState (
  IN  UINT8            Channel,
  OUT A733_DMAC_STATE  *State
  )
{
  if (State == NULL) {
    return;
  }

  ZeroMem (State, sizeof (*State));

  State->Initialised = mInitialised;
  State->Security    = MmioRead32 (DmacReg (A733_DMAC_SEC_REG));
  State->AutoGate    = MmioRead32 (DmacReg (A733_DMAC_AUTO_GATE_REG));
  State->Busy        = MmioRead32 (DmacReg (A733_DMAC_STA_REG));

  if (Channel >= A733_DMAC_CHANNELS) {
    return;
  }

  State->Enable       = MmioRead32 (DmacReg (A733_DMAC_EN (Channel)));
  State->Pending      = MmioRead32 (DmacReg (A733_DMAC_IRQ_PEND (Channel)));
  State->Residual     = MmioRead32 (DmacReg (A733_DMAC_BCNT_LEFT (Channel))) & 0x01FFFFFFU;
  State->Source           = MmioRead32 (DmacReg (A733_DMAC_CUR_SRC (Channel)));
  State->Destination      = MmioRead32 (DmacReg (A733_DMAC_CUR_DEST (Channel)));
  State->PackageCount     = MmioRead32 (DmacReg (A733_DMAC_PKG_NUM (Channel)));
  State->DescriptorConfig = mDescriptor[Channel].Config;
}