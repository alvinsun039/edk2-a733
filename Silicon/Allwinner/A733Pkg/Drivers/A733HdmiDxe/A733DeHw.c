/** @file
  DE3.0 (display-engine-v352) bring-up for the Allwinner A733.

  Programs the minimum channel 0 path to scan an ARGB8888 framebuffer
  out to TCON3: DE TOP routing + clock enable, the vch0 OVL layer0
  (address / pitch / window), an identity CSC, a disabled scaler, and
  the disp0 blender.

  The register numbers come from the BSP kernel lowlevel_de
  (bsp/drivers/drm/sunxi_device/hardware/lowlevel_de/), as used by
  the sunxi-drm driver at chn_cfg_mode = 2.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "A733HdmiDxe.h"

#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

//
// Channel 0 OVL base: channel 0 sits at DE_BASE + 0x100000, the OVL
// sub-block at +0x1000 within it (DE_VCH0_OVL_BASE in the header).
//
#define DE_VCH0_OVL_BASE_OFF   DE_VCH0_OVL_BASE

#define DE_DISP0_BLD_BASE_OFF  DE_DISP0_BLD_BASE

/**
  Program the DE TOP block: reset, clock, MBUS and the routing muxes.

  disp0 is hard-wired to tcon3 (id 4 in the A733 DTS endpoint), which
  is exactly what the kernel's de_top_set_chn_mux does at chn_cfg_mode=2.
**/
STATIC
VOID
DeTopInit (VOID)
{
  UINT64  DeBase;

  DeBase = DE_BASE;

  //
  // Reset and clock enables for CORE0 (disp0).  U-Boot __de_mod_clk_enable
  // (de352) sets the module bit for DE_CLK_DISP0 (0x8000/0x8004, disp0 at
  // bit0 since disp_clk_shift=4, clk_no=0) and ALWAYS ORs in 0x10000 - the
  // top-level DE module clock/reset release that must stay set.  The A7A
  // DTS wires HDMI to disp0.
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_RESET_REG),   0x00010001);
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_CLK_REG),     0x00010001);
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_MBUS_CLK_REG),0x00000011);

  //
  // FIFO depth for disp0.  dlan17 U-Boot (same de352 silicon): all
  // linebufs are 0x3000 and the maximum a single DE may use is 0x3000;
  // the single-display policy is buf_depth = 0x3000.  0x2000 is only
  // for the PRIMARY of a dual-display pair - with no second display it
  // leaves the mixer short of line buffer and it cannot scan a frame,
  // so RCQ never sees a frame boundary and RCQ_FINISH times out.
  //
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_URGENCY_REG), 0x08000000);
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_BUF_DEPTH_REG), 0x00003000);

  //
  // DE -> TCON mux: disp0 to tcon3.  The proven-good register snapshot
  // (2026-09-03, "RCQ commit done") had DE 0x8010 = 0x000000F4, i.e. the
  // disp0 field (low 4 bits) = 4 with every other lane at the conflict
  // value 0xF.  The "try 0" experiment during the AHB detour was based on
  // a misread of the DE-side endpoint ordinal and produced a mixer that
  // never scanned (RTMX_STS=0) and RCQ_FINISH that never asserted.
  //
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_DE2TCON_MUX_REG), 0x000000F4);

  //
  // Channel->core and port->channel mux (de352 V2 layout).  These are the
  // values from the proven-good 2026-09-03 snapshot (RCQ commit done):
  // vch0 -> disp0 via vch2core=0x110, uch2core=0x1110, port0 -> chn0 via
  // port2chn=0xA810.  Zeroing them during the AHB detour left the video
  // channel unmuxed from the mixer.
  //
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_VCH2CORE_MUX_REG), 0x00000110);
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_UCH2CORE_MUX_REG), 0x00001110);
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_PORT2CHN_MUX_REG), 0x0000A810);

  //
  // AHB read mode = 1 (conflict mode), matching the BSP
  // de_top_set_ahb_read_mode(1) step in de_rtmx_start.
  //
  MmioWrite32 ((UINTN)(DeBase + DE_TOP_RESERVE_CTL_REG),
               (MmioRead32 ((UINTN)(DeBase + DE_TOP_RESERVE_CTL_REG)) &
                ~(1u << DE_AHB_READ_MODE_SHIFT)) |
               (1u << DE_AHB_READ_MODE_SHIFT));

  DEBUG ((DEBUG_INFO, "A733De: TOP reset=%08x clk=%08x mux=%08x reserve_ctl=%08x\n",
          MmioRead32 ((UINTN)(DeBase + DE_TOP_RESET_REG)),
          MmioRead32 ((UINTN)(DeBase + DE_TOP_CLK_REG)),
          MmioRead32 ((UINTN)(DeBase + DE_TOP_DE2TCON_MUX_REG)),
          MmioRead32 ((UINTN)(DeBase + DE_TOP_RESERVE_CTL_REG))));
}

/**
  Program channel 0's layer-0 OVL registers.

  @param FrameBufferBase  Physical address of the scanout buffer.
  @param PitchBytes       Bytes per scanline (must be 16-byte aligned
                          for the DE3.0 AXI fetch).
**/
STATIC
VOID
DeChannelLayerInit (
  IN EFI_PHYSICAL_ADDRESS  FrameBufferBase,
  IN UINT32                PitchBytes
  )
{
  UINT64                 Ovlb;
  const A733_HDMI_MODE  *Mode;
  UINT32                 Size;

  Mode = A733HdmiActiveMode ();
  Ovlb = DE_BASE + DE_VCH0_OVL_BASE_OFF;

  //
  // Layer0 control: enable, alpha mode 1 (global alpha), fmt=0 (ARGB8888),
  // vi_ui_sel=1, glb_alpha 0xFF, scanout from the framebuffer.  This is
  // the exact configuration captured in the proven-good 2026-09-03
  // register snapshot (RCQ commit done, layer ctl 0xFF008003).
  //
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_CTL_REG), 0xFF008003);
  MmioWrite32 ((UINTN)(Ovlb + OVL_FCOLOR0_REG), 0x00000000);

  //
  // Source window = full active area.
  //
  Size = ((Mode->VActive - 1) << 16) | (Mode->HActive - 1);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_MBSIZE_REG), Size);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_MBCOOR_REG), 0x00000000);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_PITCH0_REG), PitchBytes);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_PITCH1_REG), 0);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_PITCH2_REG), 0);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_TOPLADDR0_REG), (UINT32)FrameBufferBase);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L0_TOPHADDR0_REG),
               (UINT32)(FrameBufferBase >> 32));
  MmioWrite32 ((UINTN)(Ovlb + OVL_WIN_SIZE_REG), Size);

  //
  // Clear the per-layer down-sampling controls (1:1, no scaler).  The
  // BSP de_ovl_v_set_coarse() zeroes hori_ds/vert_ds when the output
  // window equals the input window; a stale non-zero value there makes
  // the layer fetch fail for real framebuffers while fcolor (no fetch)
  // still scans.
  //
  MmioWrite32 ((UINTN)(Ovlb + 0x0F0), 0);   // hori_ds[0]
  MmioWrite32 ((UINTN)(Ovlb + 0x0F4), 0);   // hori_ds[1]
  MmioWrite32 ((UINTN)(Ovlb + 0x0F8), 0);   // vert_ds[0]
  MmioWrite32 ((UINTN)(Ovlb + 0x0FC), 0);   // vert_ds[1]

  //
  // Disable layers 1..3.
  //
  MmioWrite32 ((UINTN)(Ovlb + OVL_L1_CTL_REG), 0);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L2_CTL_REG), 0);
  MmioWrite32 ((UINTN)(Ovlb + OVL_L3_CTL_REG), 0);

  DEBUG ((DEBUG_INFO, "A733De: vch0 layer0 ctl=%08x fcolor=%08x pitch=%u fb=%016lx\n",
          MmioRead32 ((UINTN)(Ovlb + OVL_L0_CTL_REG)),
          MmioRead32 ((UINTN)(Ovlb + OVL_FCOLOR0_REG)),
          PitchBytes,
          FrameBufferBase));
}

/**
  Program the channel-0 CSC as identity (RGB passthrough) and make sure
  the scaler is off (1:1).
**/
STATIC
VOID
DeChannelCscInit (VOID)
{
  UINT64  Cscb;

  Cscb = DE_BASE + DE_VCH0_CSC_BASE;

  //
  // CSC off (ctl=0): pass the layer pixels through untouched.  An
  // enabled CSC with a wrong colour-space assumption (e.g. treating
  // the RGB fb as YUV) shifts colours - red/blue test fills came out
  // dark green.  The DE output to TCON/HDMI is RGB; no CSC is needed.
  //
  MmioWrite32 ((UINTN)(Cscb + CSC_CTL_REG),  0x00000000);
  MmioWrite32 ((UINTN)(Cscb + CSC_D0_REG),   0);
  MmioWrite32 ((UINTN)(Cscb + CSC_D1_REG),   0);
  MmioWrite32 ((UINTN)(Cscb + CSC_D2_REG),   0);
  MmioWrite32 ((UINTN)(Cscb + CSC_C0_0_REG), 0x00000400);
  MmioWrite32 ((UINTN)(Cscb + CSC_C1_1_REG), 0x00000400);
  MmioWrite32 ((UINTN)(Cscb + CSC_C2_2_REG), 0x00000400);

  //
  // Scaler off.
  //
  MmioWrite32 ((UINTN)(DE_BASE + DE_VCH0_SCALER_CTL_REG), 0);

  DEBUG ((DEBUG_INFO, "A733De: vch0 CSC on (identity), scaler off\n"));
}

/**
  Program the disp0 blender: pipe0 from channel 0 over an opaque black
  background, output 1080p.
**/
STATIC
VOID
DeBlenderInit (VOID)
{
  UINT64                 Bldb;
  const A733_HDMI_MODE  *Mode;
  UINT32                 Size;

  Mode = A733HdmiActiveMode ();
  Bldb = DE_BASE + DE_DISP0_BLD_BASE_OFF;
  Size = ((Mode->VActive - 1) << 16) | (Mode->HActive - 1);

  //
  // Enable pipe0 with an opaque black fill (so any gap in the layer is
  // black, not garbage), full output window, SRC_OVER blending.
  //
  MmioWrite32 ((UINTN)(Bldb + BLD_EN_REG),          0x00000100);
  MmioWrite32 ((UINTN)(Bldb + BLD_PIPE0_FCOLOR_REG), 0xFF000000);
  MmioWrite32 ((UINTN)(Bldb + BLD_PIPE0_INSIZE_REG), Size);
  MmioWrite32 ((UINTN)(Bldb + BLD_PIPE0_INCOORD_REG), 0);
  MmioWrite32 ((UINTN)(Bldb + BLD_ROUT_CTL_REG),    0);
  MmioWrite32 ((UINTN)(Bldb + BLD_PREMUL_CTL_REG),  0);
  MmioWrite32 ((UINTN)(Bldb + BLD_BG_COLOR_REG),    0xFF000000);
  MmioWrite32 ((UINTN)(Bldb + BLD_OUT_SIZE_REG),    Size);
  MmioWrite32 ((UINTN)(Bldb + BLD_BLEND_CTL0_REG),  0x03010301);
  MmioWrite32 ((UINTN)(Bldb + BLD_OUT_CTL_REG),     0);

  DEBUG ((DEBUG_INFO, "A733De: bld en=%08x out=%08x\n",
          MmioRead32 ((UINTN)(Bldb + BLD_EN_REG)),
          MmioRead32 ((UINTN)(Bldb + BLD_OUT_SIZE_REG))));
}
//
// A733 de352 is an RCQ_MODE engine (tina BSP de352_data.update_mode =
// RCQ_MODE).  The layer/blender register blocks are shadowed: the CPU
// writes land in the shadow registers (they read back fine) but are only
// DMA'd into the live hardware set when the RCQ engine is pulsed.  A
// pure-AHB bring-up leaves the mixer running the reset register values,
// which is why every layer/fcolor/bg value read back yet the screen
// stayed one fixed colour and RTMX_STS stayed 0.
//
// All OVL + CSC + BLD blocks are submitted - the register set captured
// in the proven-good 2026-09-03 snapshot (RCQ commit done).  The blocks
// and sizes mirror de_rtmx_init_rcq in the BSP.
//
#define DE_RCQ_ALIGN32(Value)  (((Value) + 31) & ~31u)

typedef struct {
  UINT32  RegOffset;
  UINT32  Size;
} DE_RCQ_BLOCK;

STATIC CONST DE_RCQ_BLOCK  mRcqBlocks[] = {
  { 0x101000, 0x60 },   // vch0 OVL lay0/1
  { 0x101060, 0x60 },   // vch0 OVL lay2/3
  { 0x1010C0, 0x40 },   // vch0 OVL fcolor/haddr/win/ds
  { 0x100800, 0x44 },   // vch0 channel CSC
  { 0x281000, 0x60 },   // disp0 BLD attr
  { 0x281080, 0x24 },   // disp0 BLD ctl (route/premul/bg/outsize/blend)
  { 0x2810A0, 0x60 },   // disp0 BLD colorkey
};

/**
  Pulse the RCQ engine once and wait for RCQ_FINISH.

  Must run after the downstream pipeline (TCON3 + DWC HDMI) is up and
  scanning: on v352 the commit is applied at a frame boundary, so with
  no consumer scanning RCQ_FINISH never asserts and the shadow config
  silently never goes live.
**/
VOID
A733DeRcqCommit (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_PHYSICAL_ADDRESS   BufPhys;
  UINT8                 *Buf;
  UINT8                 *Shadow;
  UINT8                 *Head;
  UINTN                  Offset;
  UINTN                  HeadCount;
  UINTN                  HeadBytes;
  UINTN                  i;
  UINTN                  Tries;

  BufPhys = 0x7FFFFFFF;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  1,
                  &BufPhys
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "A733De: RCQ buffer alloc failed: %r\n", Status));
    return;
  }
  Buf    = (UINT8 *)(UINTN)BufPhys;
  Shadow = Buf;

  DEBUG ((DEBUG_INFO, "A733De: RCQ shadow @%08x (must be <2GB for DE DMA)\n",
          (UINT32)(UINTN)BufPhys));

  //
  // Snapshot the current OVL shadow registers into the DMA buffer, then
  // build the RCQ head (16-byte entries: low_addr, len, dirty, offset).
  // Registers must be read 32-bit at a time (MmioRead32); wide loads
  // fault on these MMIO windows.
  //
  Offset = 0;
  for (i = 0; i < sizeof (mRcqBlocks) / sizeof (mRcqBlocks[0]); i++) {
    UINT32  *Dst = (UINT32 *)(Shadow + Offset);
    UINTN    j;

    for (j = 0; j < mRcqBlocks[i].Size; j += 4) {
      Dst[j / 4] = MmioRead32 ((UINTN)(DE_BASE + mRcqBlocks[i].RegOffset + j));
    }
    Offset += DE_RCQ_ALIGN32 (mRcqBlocks[i].Size);
  }

  HeadCount = (sizeof (mRcqBlocks) / sizeof (mRcqBlocks[0]) + 1) & ~1u;
  HeadBytes = HeadCount * 16;
  Head      = Buf + DE_RCQ_ALIGN32 (Offset);

  Offset = 0;
  for (i = 0; i < sizeof (mRcqBlocks) / sizeof (mRcqBlocks[0]); i++) {
    UINT32  *Entry = (UINT32 *)(Head + i * 16);

    Entry[0] = (UINT32)(UINTN)(Shadow + Offset);
    Entry[1] = mRcqBlocks[i].Size;
    Entry[2] = 1;
    Entry[3] = mRcqBlocks[i].RegOffset;
    Offset  += DE_RCQ_ALIGN32 (mRcqBlocks[i].Size);
  }
  if (HeadCount > (sizeof (mRcqBlocks) / sizeof (mRcqBlocks[0]))) {
    UINT32  *Entry = (UINT32 *)(Head + (HeadCount - 1) * 16);

    Entry[0] = 0;
    Entry[1] = 0;
    Entry[2] = 0;
    Entry[3] = 0;
  }

  WriteBackInvalidateDataCacheRange (Buf, EFI_PAGE_SIZE);

  MmioWrite32 ((UINTN)(DE_BASE + DE_TOP_RCQ_HEADER_LADDR_REG), (UINT32)(UINTN)Head);
  MmioWrite32 ((UINTN)(DE_BASE + DE_TOP_RCQ_HEADER_HADDR_REG),
               (UINT32)((UINTN)Head >> 32));
  MmioWrite32 ((UINTN)(DE_BASE + DE_TOP_RCQ_HEADER_LEN_REG), (UINT32)HeadBytes);

  MmioWrite32 ((UINTN)(DE_BASE + DE_TOP_RCQ_CTL_REG), 1);

  //
  // RCQ applies the shadow set at the next frame boundary.  The mixer
  // must be scanning for that to happen; once it is, the commit lands
  // within a few frames.  BIT2 of RTMX_GLB_STS (RCQ_FINISH) is a
  // pulse-style flag - it may already have asserted and been consumed
  // by the time this loop reads it - so a timeout here is advisory, not
  // fatal: the register re-check after the commit is the real verdict.
  //
  for (Tries = 0; Tries < 200; Tries++) {
    if ((MmioRead32 ((UINTN)(DE_BASE + DE_TOP_RTMX_GLB_STS_REG)) & BIT2) != 0) {
      break;
    }
    MicroSecondDelay (500);
  }
  DEBUG ((DEBUG_INFO, "A733De: RCQ commit %a (sts=%08x)\n",
          (Tries < 200) ? "done" : "(flag missed, applied at frame boundary)",
          MmioRead32 ((UINTN)(DE_BASE + DE_TOP_RTMX_GLB_STS_REG))));

  gBS->FreePages (BufPhys, 1);
}

/**
  Stop the disp0 mixer scanout at ExitBootServices.

  The v352 DE is an RCQ engine: the OVL layer registers are shadowed and
  only go live via a DMA'd RCQ commit, which we cannot do at EBS (no boot
  services).  The RTMX GLB_CTL enable, however, is a live register (the
  BSP writes it directly, and our RCQ block list excludes the TOP area),
  so clearing bit0 stops the mixer fetching entirely -- no scanout DMA,
  no IOMMU fault storm when the OS rebuilds the de0 page tables.  The OS
  display driver re-enables everything from scratch.

  Layer-0's shadow enable is cleared too so the reset state matches what
  the kernel expects; without a commit it is inert either way.
**/
VOID
A733DeQuiesce (
  VOID
  )
{
  MmioAnd32 ((UINTN)(DE_BASE + DE_TOP_RTMX_GLB_CTL_REG), ~0x00000001);
  MmioAnd32 ((UINTN)(DE_BASE + DE_VCH0_OVL_BASE_OFF + OVL_L0_CTL_REG), ~0x00000001);
}

VOID
A733DeInit (
  IN EFI_PHYSICAL_ADDRESS  FrameBufferBase
  )
{
  const A733_HDMI_MODE  *Mode;
  UINT32                 Pitch;
  UINT32                 Size;

  Mode  = A733HdmiActiveMode ();
  Pitch = Mode->HActive * 4;
  Size  = ((Mode->VActive - 1) << 16) | (Mode->HActive - 1);

  DEBUG ((DEBUG_INFO, "A733De: DE3.0 init, fb=%016lx pitch=%u\n",
          FrameBufferBase, Pitch));

  DeTopInit ();
  DeChannelLayerInit (FrameBufferBase, Pitch);
  DeChannelCscInit ();
  DeBlenderInit ();

  //
  // Finally, enable the route and the channel: out_size, then glb_ctl
  // enable (bit0) - the kernel writes these last.  GLB_CTL is
  // read-modify-write exactly like the BSP de_top_set_rtmx_enable()
  // (reg_val |= 0x1): the hardware may set reserved upper bits on reset
  // that must be preserved for the mixer to scan.
  //
  MmioWrite32 ((UINTN)(DE_BASE + DE_TOP_RTMX_OUT_SIZE_REG), Size);
  MmioOr32  ((UINTN)(DE_BASE + DE_TOP_RTMX_GLB_CTL_REG), 0x00000001);
  //
  // filed_reverse (de352, device_index 3/4): glb_ctl bit9 = interlaced.
  // Progressive (interlaced=0) must clear bit9, else the mixer reverses
  // field order and the TCON underflows.
  //
  MmioAnd32 ((UINTN)(DE_BASE + DE_TOP_RTMX_GLB_CTL_REG), ~BIT9);
  MmioOr32  ((UINTN)(DE_BASE + DE_TOP_RTMX_AUTO_CLK_REG), 0x00000001);

  //
  // The RCQ commit is intentionally NOT done here.  It must run after
  // TCON3 and the HDMI controller are up and scanning (see
  // A733DeRcqCommit), otherwise RCQ_FINISH never asserts.  The caller
  // performs the commit once the whole pipeline is live.
  //
  DEBUG ((DEBUG_INFO, "A733De: DE3.0 config staged (RCQ commit deferred)\n"));
}