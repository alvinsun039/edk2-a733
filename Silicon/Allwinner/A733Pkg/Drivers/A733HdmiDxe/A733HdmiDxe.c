/** @file
  A733HdmiDxe - Allwinner A733 HDMI output as EFI_GRAPHICS_OUTPUT_PROTOCOL.

  Brings the DE3.0 -> TCON3 -> DWC-HDMI pipeline up from cold for the
  active A733_HDMI_MODE (default VIC 16 = 1080p60; no EDID, the same
  fallback the vendor stack uses when a monitor presents no EDID),
  allocates the scanout buffer, and publishes a single-mode GOP.

  The GOP protocol shell (QueryMode/SetMode/Blt) mirrors the Orange Pi
  4 Pro SimpleFB driver; the difference is the backend: this driver owns
  the whole display pipeline instead of repointing a U-Boot-initialised
  scanout.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "A733HdmiDxe.h"
#include <A733SetupFormSetGuid.h>
#include <Guid/A733EdidInfo.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/VariableWrite.h>
#include <Guid/EventGroup.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/PerformanceLib.h>
#include <Library/UefiLib.h>

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mMode;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGop;
STATIC BOOLEAN                               mBltProbed = FALSE;
STATIC EFI_EVENT                             mHdmiExitBsEvent;
STATIC EFI_EVENT                             mVariableWriteEvent;

//
// Hot-plug state.  mPipelineUp gates the poll until the first bring-up has
// run; mFbAddr is the scanout buffer every pipeline apply programs; and
// mAppliedMode is what the DE/TCON/DWC stages were last driven with - the
// poll only re-applies when a (re)connected sink's best mode differs from
// it.  Static storage: InstallConfigurationTable only keeps the pointer,
// so the EDID block below cannot live on a stack frame.
//
STATIC EFI_PHYSICAL_ADDRESS        mFbAddr;
STATIC CONST A733_HDMI_MODE       *mAppliedMode;
STATIC BOOLEAN                     mPipelineUp;
STATIC BOOLEAN                     mHpdConnected;
STATIC EFI_EVENT                   mHpdPollEvent;
STATIC A733_EDID_INFO              mEdidInfo;

//
// Device path for the GOP handle. A vendor node with our own GUID keeps
// this handle distinct from the SimpleFB GOP the OPi uses.
//
#define SUNXI_HDMI_GOP_DP_GUID \
  { 0x9B3A1C2D, 0x4E5F, 0x6A7B, { 0x8C, 0x9D, 0x0E, 0x1F, 0x2A, 0x3B, 0x4C, 0x5D } }

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SUNXI_HDMI_GOP_DEVICE_PATH;
#pragma pack()

STATIC SUNXI_HDMI_GOP_DEVICE_PATH  mGopDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    SUNXI_HDMI_GOP_DP_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC
EFI_STATUS
EFIAPI
GopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
  )
{
  if ((ModeNumber != 0) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *Info = AllocateCopyPool (sizeof (mModeInfo), &mModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  *SizeOfInfo = sizeof (mModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                         ModeNumber
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL      *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer  OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta         OPTIONAL
  )
{
  UINT32                          *Fb;
  UINT32                           PixelsPerLine;
  UINT32                           FbW;
  UINT32                           FbH;
  UINTN                            Row;
  UINT32                          *DstLine;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *SrcLine;
  UINTN                            BltDelta;
  UINTN                            FbStrideBytes;

  Fb            = (UINT32 *)(UINTN)mMode.FrameBufferBase;
  PixelsPerLine = mModeInfo.PixelsPerScanLine;
  FbStrideBytes = (UINTN)PixelsPerLine * sizeof (UINT32);
  FbW           = mModeInfo.HorizontalResolution;
  FbH           = mModeInfo.VerticalResolution;
  BltDelta      = (Delta == 0) ? Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL) : Delta;

  //
  // One-shot Blt probe: if the text console drives our GOP we see
  // VideoFill (clear) and BufferToVideo (glyph) calls here.
  //
  if (!mBltProbed) {
    mBltProbed = TRUE;
    DEBUG ((DEBUG_INFO, "A733Hdmi: GOP Blt op=%u %ux%u+%u+%u (console active)\n",
            (UINT32)BltOperation, (UINT32)Width, (UINT32)Height,
            (UINT32)DestinationX, (UINT32)DestinationY));
  }

  if ((DestinationX + Width  > FbW) || (DestinationY + Height > FbH)) {
    if (BltOperation != EfiBltVideoToBltBuffer) {
      return EFI_INVALID_PARAMETER;
    }
  }

  switch (BltOperation) {
    case EfiBltVideoFill:
      if (BltBuffer == NULL) {
        return EFI_INVALID_PARAMETER;
      }
      {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Px = *BltBuffer;
        // BGRA8888 packing matches EFI_GRAPHICS_OUTPUT_BLT_PIXEL bytewise.
        UINT32  Word = 0xFF000000u
                       | ((UINT32)Px.Red   << 16)
                       | ((UINT32)Px.Green <<  8)
                       | ((UINT32)Px.Blue  <<  0);
        for (Row = 0; Row < Height; Row++) {
          UINTN  Col;
          DstLine = Fb + (DestinationY + Row) * PixelsPerLine + DestinationX;
          for (Col = 0; Col < Width; Col++) {
            DstLine[Col] = Word;
          }
        }
      }
      break;

    case EfiBltBufferToVideo:
      if (BltBuffer == NULL) {
        return EFI_INVALID_PARAMETER;
      }
      for (Row = 0; Row < Height; Row++) {
        DstLine = Fb + (DestinationY + Row) * PixelsPerLine + DestinationX;
        SrcLine = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)
                    ((UINT8 *)BltBuffer + (SourceY + Row) * BltDelta
                     + SourceX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        // BltPixel layout (B,G,R,Reserved) matches BGRA8888 word.
        CopyMem (DstLine, SrcLine, Width * sizeof (UINT32));
      }
      break;

    case EfiBltVideoToBltBuffer:
      if (BltBuffer == NULL) {
        return EFI_INVALID_PARAMETER;
      }
      for (Row = 0; Row < Height; Row++) {
        UINT32                         *Src = Fb + (SourceY + Row) * PixelsPerLine + SourceX;
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Dst = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)
                    ((UINT8 *)BltBuffer + (DestinationY + Row) * BltDelta
                     + DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
        CopyMem (Dst, Src, Width * sizeof (UINT32));
      }
      break;

    case EfiBltVideoToVideo:
      if (DestinationY <= SourceY) {
        for (Row = 0; Row < Height; Row++) {
          UINT32  *Src = Fb + (SourceY      + Row) * PixelsPerLine + SourceX;
          UINT32  *Dst = Fb + (DestinationY + Row) * PixelsPerLine + DestinationX;
          CopyMem (Dst, Src, Width * sizeof (UINT32));
        }
      } else {
        for (Row = Height; Row > 0; Row--) {
          UINT32  *Src = Fb + (SourceY      + Row - 1) * PixelsPerLine + SourceX;
          UINT32  *Dst = Fb + (DestinationY + Row - 1) * PixelsPerLine + DestinationX;
          CopyMem (Dst, Src, Width * sizeof (UINT32));
        }
      }
      break;

    default:
      return EFI_INVALID_PARAMETER;
  }

  //
  // DE3.0 fetches scanout through the system interconnect, not the CPU
  // cache: dirty lines must be written back or the pixels never arrive.
  //
  if (BltOperation != EfiBltVideoToBltBuffer) {
    WriteBackInvalidateDataCacheRange (
      (VOID *)((UINTN)Fb + DestinationY * FbStrideBytes),
      Height * FbStrideBytes
      );
  }

  return EFI_SUCCESS;
}

/**
  Allocate the scanout buffer below 4 GB.

  The DE's 32-bit TOP_LADDR0 holds the base; values above 4 GB would
  need the TOP_HADDR0 extension, so pin the allocation to the low
  window. EfiReservedMemoryType keeps the buffer out of the allocator
  (the OS gets it reserved, as the SimpleFB port does on the OPi).
**/
STATIC
EFI_STATUS
AllocateFrameBuffer (
  OUT EFI_PHYSICAL_ADDRESS  *FrameBufferBase
  )
{
  const A733_HDMI_MODE  *Mode;
  EFI_STATUS             Status;
  EFI_PHYSICAL_ADDRESS   FbAddr;
  UINTN                  FbPages;
  UINTN                  FbBytes;

  Mode    = A733HdmiActiveMode ();
  FbBytes = (UINTN)Mode->HActive * Mode->VActive * 4;
  FbPages = EFI_SIZE_TO_PAGES (FbBytes);
  //
  // Pin the scanout buffer low in DRAM (just above the firmware load
  // region).  Allwinner DE DMA on this SoC is most reliable against the
  // bottom of DRAM (0x40000000+); allocations higher up - even below
  // 2 GB - have shown the layer fetch returning nothing while fcolor
  // (no DMA) still scans.  DRAM base is 0x40000000; EDK2 + the FV live
  // in the first ~64 MB, so 0x48000000 leaves comfortable headroom.
  //
  FbAddr  = 0x48000000ULL - 1;             // AllocateMaxAddress, low DRAM

  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiReservedMemoryType,
                  FbPages,
                  &FbAddr
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: framebuffer alloc failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "A733Hdmi: framebuffer @ %016lx (%u pages)\n",
          FbAddr, (UINT32)FbPages));

  //
  // Start the buffer opaque black so the DE scanout shows a clean
  // background until the GOP/console draws over it.
  //
  SetMem32 ((VOID *)(UINTN)FbAddr, FbBytes, 0xFF000000);
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)FbAddr, FbBytes);

  *FrameBufferBase = FbAddr;
  return EFI_SUCCESS;
}

/**
  Driver entry point: allocate the buffer, bring the pipeline up, and
  publish the GOP.
**/
//
// ExitBootServices: stop the disp0 mixer scan (see A733DeQuiesce).
// Boot services are already gone: raw MMIO only, no allocation.
//
STATIC
VOID
EFIAPI
A733HdmiExitBootServicesQuiesce (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  A733DeQuiesce ();

  //
  // Mask every DWC-HDMI interrupt (global mute) as well: the driver
  // unmasked them during bring-up and a straggler -- the CEC line was
  // observed live -- fires into the OS before its driver has clocked
  // the block, faulting in interrupt context (dw_read Oops ~5 s into
  // the Linux boot).
  //
  MmioWrite8 ((UINTN)(HDMI_BASE + IH_MUTE_REG), 0x03);
}

/**
  Probe the sink EDID and publish the result as a configuration table.

  The DDC I2CM only answers once the HDMI core is clocked, so callers run
  this after A733HdmiApplyPipeline (bring-up, or a hot-plug reconnect).
  ProbeEdid caches its result; the hot-plug path clears that cache first
  so a different sink is actually re-read.
**/
STATIC
VOID
A733HdmiPublishEdidInfo (
  VOID
  )
{
  EFI_STATUS  EdidStatus;

  EdidStatus = A733HdmiProbeEdid ();
  if (EFI_ERROR (EdidStatus)) {
    return;
  }

  mEdidInfo.Valid              = mEdidNativeValid;
  mEdidInfo.NativeHActive      = mEdidNativeMode.HActive;
  mEdidInfo.NativeVActive      = mEdidNativeMode.VActive;
  mEdidInfo.NativePixelClockHz = mEdidNativeMode.PixelClockHz;

  if (EFI_ERROR (A733HdmiGetSupportedPresets (&mEdidInfo.SupportedMask))) {
    return;
  }

  //
  // Also carry every EDID detailed timing (incl. modes with no preset
  // match, e.g. 1920x1080@30), so a Setup page could offer them.
  //
  mEdidInfo.DtdCount = 0;
  A733HdmiGetEdidModes (
    A733_EDID_MAX_DTD,
    mEdidInfo.Dtd,
    &mEdidInfo.DtdCount
    );

  if (!EFI_ERROR (gBS->InstallConfigurationTable (
                        &gA733EdidInfoGuid,
                        &mEdidInfo
                        ))) {
    DEBUG ((DEBUG_INFO,
            "A733Hdmi: EDID info published (mask 0x%08x, %u DTD)\n",
            mEdidInfo.SupportedMask,
            (UINT32)mEdidInfo.DtdCount));
  }
}

/**
  Run the display pipeline for A733HdmiActiveMode()'s current answer.

  One stage per module, each printing its own register tear-down so a
  hang is attributable from the UART log.  RCQ commit goes LAST: on v352
  the shadow register blocks are only DMA'd into the live registers at a
  frame boundary, so the engine needs the DE + TCON + HDMI pipeline
  scanning before 0x8110 reports RCQ_FINISH (U-Boot triggers RCQ in
  atomic_flush() after crtc/tcon/hdmi enable for the same reason).  Every
  stage re-reads A733HdmiActiveMode(), which is what makes a plain re-run
  of this function re-target the whole chain.
**/
STATIC
VOID
A733HdmiApplyPipeline (
  VOID
  )
{
  CONST A733_HDMI_MODE  *Mode;
  EFI_STATUS             Status;

  Mode = A733HdmiActiveMode ();
  DEBUG ((DEBUG_INFO, "A733Hdmi: applying pipeline at %ux%u @ %u Hz\n",
          Mode->HActive, Mode->VActive, Mode->PixelClockHz));

  Status = A733DisplayClockInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: clock init failed: %r\n", Status));
    return;
  }

  A733DeInit (mFbAddr);
  A733TconInit ();
  A733HdmiHwInit ();
  A733DeRcqCommit ();

  DEBUG ((DEBUG_INFO, "A733Hdmi: fb[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
          ((UINT32 *)(UINTN)mFbAddr)[0], ((UINT32 *)(UINTN)mFbAddr)[1],
          ((UINT32 *)(UINTN)mFbAddr)[2], ((UINT32 *)(UINTN)mFbAddr)[3],
          ((UINT32 *)(UINTN)mFbAddr)[4], ((UINT32 *)(UINTN)mFbAddr)[5],
          ((UINT32 *)(UINTN)mFbAddr)[6], ((UINT32 *)(UINTN)mFbAddr)[7]));
}

/**
  Hot-plug poll: re-read the sink when HPD changes, re-target the mode.

  Runs once a second at TPL_CALLBACK (DxeCore stops the timer at
  ExitBootServices, and the EBS quiesce above stops the scanout anyway).
  A rising edge forces an EDID re-read - the DDC master is still clocked,
  the pipeline is up - and re-applies the pipeline when the new sink's
  best mode differs from what is on the wire.  The GOP's mode information
  is updated before the stages run, so anything asking after the poll
  sees the new geometry.

  The console keeps its own cached copy of the mode until it is rebound,
  so a mid-session switch to a *smaller* mode can clip text until the
  next full redraw; switching between same-size timings is invisible.
  On a mode change the monitor itself re-syncs (brief blank).

  @param[in] Event    The polling event (unused).
  @param[in] Context  Not used.
**/
STATIC
VOID
EFIAPI
A733HdmiHotPlugPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CONST A733_HDMI_MODE  *New;
  EFI_TPL                OldTpl;
  BOOLEAN                Hpd;

  if (!mPipelineUp) {
    return;
  }

  Hpd = (MmioRead8 ((UINTN)(HDMI_BASE + PHY_STAT0_REG)) & HDMI_PHY_HPD) != 0;
  if (Hpd == mHpdConnected) {
    return;
  }

  mHpdConnected = Hpd;
  DEBUG ((DEBUG_INFO, "A733Hdmi: hot-plug %a\n",
          Hpd ? "connected" : "disconnected"));
  if (!Hpd) {
    return;
  }

  //
  // Rising edge.  Console Blts run at TPL_CALLBACK: hold TPL_NOTIFY so
  // no Blt lands inside the pipeline re-apply.
  //
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  mEdidNativeValid = FALSE;
  A733HdmiPublishEdidInfo ();

  New = A733HdmiActiveMode ();
  if ((New->HActive != mAppliedMode->HActive) ||
      (New->VActive != mAppliedMode->VActive) ||
      (New->PixelClockHz != mAppliedMode->PixelClockHz))
  {
    if ((New->PhyPllN == 0) || (New->PhyPllP0 == 0)) {
      DEBUG ((DEBUG_INFO,
              "A733Hdmi: hot-plug mode %ux%u@%u Hz not drivable, keep current\n",
              New->HActive, New->VActive, New->PixelClockHz));
    } else {
      mModeInfo.HorizontalResolution = New->HActive;
      mModeInfo.VerticalResolution   = New->VActive;
      mModeInfo.PixelsPerScanLine    = New->HActive;
      mMode.FrameBufferSize          = (UINTN)New->HActive * New->VActive * 4;

      A733HdmiApplyPipeline ();
      mAppliedMode = New;
    }
  }

  gBS->RestoreTPL (OldTpl);
}

/**
  Whole-pipeline HDMI bring-up.  Runs once the variable store is up (see
  the entry point) so the DisplayMode variable can veto the output.

  @retval EFI_SUCCESS  GOP installed.
**/
STATIC
EFI_STATUS
A733HdmiBringUp (
  VOID
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS FbAddr;

  // Whole-pipeline bring-up time lands in the Shell `dp` report
  // (audit 2026-09-09, part 3).  This driver dispatches FIRST via
  // APRIORI, so "HdmiInit" is also a proxy for display-up latency.
  PERF_START (NULL, "HdmiInit", "A733Hdmi", 0);
  EFI_HANDLE           Handle;
  CONST A733_HDMI_MODE *Mode;
  CONST A733_HDMI_MODE *EdidMode;

  Mode = A733HdmiActiveMode ();
  DEBUG ((DEBUG_INFO, "A733Hdmi: entry, VIC %u (%ux%u)\n",
          Mode->Vic, Mode->HActive, Mode->VActive));

  //
  // Only modes with a measured top-PHY PLL anchor are drivable.  The
  // FC/PHY stages still carry their 1080p register images, so starting
  // the pipeline for an unanchored mode would leave DE/TCON at the new
  // geometry while the DWC core stayed at 1080p.
  //
  if ((Mode->PhyPllN == 0) || (Mode->PhyPllP0 == 0)) {
    DEBUG ((DEBUG_INFO,
            "A733Hdmi: VIC %u has no PHY anchor, not drivable yet\n",
            Mode->Vic));
    return EFI_UNSUPPORTED;
  }

  Status = AllocateFrameBuffer (&FbAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mFbAddr = FbAddr;

  A733HdmiApplyPipeline ();

  //
  // The HDMI core is up - probe the sink EDID now (the DDC I2CM needs
  // the core clock), then re-target: NATIVE resolves through what the
  // probe found, and the pipeline cannot have been driven from the EDID
  // before this point, because the DDC master needs that clock.
  //
  A733HdmiPublishEdidInfo ();

  //
  // Second pass: every pipeline stage re-reads A733HdmiActiveMode(), so
  // re-running the stages re-targets the whole chain if the sink's best
  // mode differs from what the pipeline came up with, and the GOP below
  // installs with the final mode.
  //
  EdidMode = A733HdmiActiveMode ();
  if ((EdidMode->HActive != Mode->HActive) ||
      (EdidMode->VActive != Mode->VActive) ||
      (EdidMode->PixelClockHz != Mode->PixelClockHz))
  {
    if ((EdidMode->PhyPllN == 0) || (EdidMode->PhyPllP0 == 0)) {
      DEBUG ((DEBUG_INFO,
              "A733Hdmi: EDID mode %ux%u@%u Hz not drivable, keep VIC %u\n",
              EdidMode->HActive, EdidMode->VActive,
              EdidMode->PixelClockHz, Mode->Vic));
    } else {
      Mode = EdidMode;
      A733HdmiApplyPipeline ();
    }
  }

  //
  // ~25-line CCU/TCON/DWC/PHY register dump with verdicts: bring-up
  // forensics, and the only machine-readable health check for the pipeline
  // on a board with no capture card.  It prints at DEBUG_INFO, which the
  // steady-state PcdDebugPrintErrorLevel (ERROR|WARN) masks; a verbose
  // build shows it.
  //
  A733HdmiDiag ();

  //
  // Single-mode GOP: geometry from the active video mode.
  //
  ZeroMem (&mModeInfo, sizeof (mModeInfo));
  mModeInfo.Version              = 0;
  mModeInfo.HorizontalResolution = Mode->HActive;
  mModeInfo.VerticalResolution   = Mode->VActive;
  mModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  mModeInfo.PixelsPerScanLine    = Mode->HActive;
  mModeInfo.PixelInformation.RedMask       = 0x00FF0000;
  mModeInfo.PixelInformation.GreenMask     = 0x0000FF00;
  mModeInfo.PixelInformation.BlueMask      = 0x000000FF;
  mModeInfo.PixelInformation.ReservedMask  = 0xFF000000;

  mMode.MaxMode         = 1;
  mMode.Mode            = 0;
  mMode.Info            = &mModeInfo;
  mMode.SizeOfInfo      = sizeof (mModeInfo);
  mMode.FrameBufferBase = FbAddr;
  mMode.FrameBufferSize = (UINTN)Mode->HActive * Mode->VActive * 4;

  mGop.QueryMode = GopQueryMode;
  mGop.SetMode   = GopSetMode;
  mGop.Blt       = GopBlt;
  mGop.Mode      = &mMode;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mGop,
                  &gEfiDevicePathProtocolGuid,
                  &mGopDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: GOP install failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO,
          "A733Hdmi: GOP installed, %ux%u BGRA8888 @%016lx\n",
          Mode->HActive, Mode->VActive, FbAddr));

  //
  // Park the DE scanout at ExitBootServices (see A733DeQuiesce): the
  // OS IOMMU driver rebuilds the de0 page tables and a live scanout DMA
  // would fault-storm CPU0 ~25 s into the boot.  The panel goes dark
  // here; the OS display driver repaints it from scratch.  Registering
  // is best-effort - failure only means the OS must quiesce the DE
  // itself (the nodisp DTB path).
  //
  Status = gBS->CreateEvent (
                  EVT_SIGNAL_EXIT_BOOT_SERVICES,
                  TPL_CALLBACK,
                  A733HdmiExitBootServicesQuiesce,
                  NULL,
                  &mHdmiExitBsEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: EBS quiesce event failed: %r\n", Status));
  }

  //
  // Hot-plug: poll HPD once a second from now on.  A sink that connects
  // (or reconnects with different timings) gets its EDID re-read and the
  // pipeline re-applied; a sink with the same best mode costs one MMIO
  // byte read per tick.  DxeCore stops the timer at ExitBootServices, so
  // nothing polls once the OS owns the hardware.
  //
  mAppliedMode  = Mode;
  mHpdConnected = (MmioRead8 ((UINTN)(HDMI_BASE + PHY_STAT0_REG)) & HDMI_PHY_HPD) != 0;
  mPipelineUp   = TRUE;
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL | EVT_TIMER,
                  TPL_CALLBACK,
                  A733HdmiHotPlugPoll,
                  NULL,
                  &mHpdPollEvent
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (
                    mHpdPollEvent,
                    TimerPeriodic,
                    EFI_TIMER_PERIOD_SECONDS (1)
                    );
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: hot-plug poll unavailable: %r\n", Status));
  } else {
    DEBUG ((DEBUG_INFO, "A733Hdmi: hot-plug poll on (HPD %a)\n",
            mHpdConnected ? "high" : "low"));
  }

  PERF_END (NULL, "HdmiInit", "A733Hdmi", 0);

  return EFI_SUCCESS;
}

/**
  VariableWrite notification: the variable store is usable, so the
  Display Mode page selection (PcdDisplayModePreset / PcdDisplayModeCustom
  DynamicHII PCDs) can be read by A733HdmiActiveMode when the pipeline
  comes up.  The event is closed after the first notification: the mode
  choice is a boot-time one (RESET_REQUIRED in the Setup UI).
**/
STATIC
VOID
EFIAPI
NotifyVariableWrite (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  gBS->CloseEvent (Event);

  Status = A733HdmiBringUp ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733Hdmi: bring-up failed: %r\n", Status));
  }
}

/**
  Entry point: wait for the variable write protocol, then bring the
  display pipeline up (unless DisplayMode vetoes it).

  The APRIORI-first dispatch is preserved: the entry itself only wires
  the notification (cheap), and the actual bring-up happens in the
  VariableWrite callback - still long before BDS builds the console.

  @param  ImageHandle  The image handle of the driver.
  @param  SystemTable  The system table.

  @retval EFI_SUCCESS  The entry point is executed successfully.
**/
EFI_STATUS
EFIAPI
A733HdmiDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *Registration;
  VOID        *VariableWriteProtocol;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  NotifyVariableWrite,
                  NULL,
                  &mVariableWriteEvent
                  );
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gEfiVariableWriteArchProtocolGuid,
                  mVariableWriteEvent,
                  &Registration
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // RegisterProtocolNotify only fires when the protocol is installed
  // AFTER this registration; check for an already-up store and fire the
  // callback ourselves (standard EDK2 pattern, mirrors A733SetupDxe).
  //
  Status = gBS->LocateProtocol (
                  &gEfiVariableWriteArchProtocolGuid,
                  NULL,
                  &VariableWriteProtocol
                  );
  if (!EFI_ERROR (Status)) {
    gBS->SignalEvent (mVariableWriteEvent);
  }

  return EFI_SUCCESS;
}