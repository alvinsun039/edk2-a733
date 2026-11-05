/** @file
  Private header for A733HdmiDxe.

  All constants here were transcribed from the Allwinner BSP kernel
  (drivers/clk/sunxi-ng/ccu-sun60iw2.c, bsp/drivers/drm/sunxi_device/)
  and cross-checked against the Radxa Cubie A7A DTS.  The active video
  mode comes from the A733_HDMI_MODE table (A733HdmiMode.c); geometry
  for DE / TCON / GOP is derived from the selected entry.  The DWC frame
  composer and SNPS PHY stages are only measured for VIC 16 (1080p60),
  so A733HdmiDxeEntryPoint rejects any mode without a PHY anchor.

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SUNXI_HDMI_DXE_H_
#define SUNXI_HDMI_DXE_H_

#include <PiDxe.h>
#include <Guid/GlobalVariable.h>

// ---------------------------------------------------------------------------
// Frame geometry (fixed 1080p for v1)
// ---------------------------------------------------------------------------
#define HDMI_HACTIVE          1920
#define HDMI_VACTIVE          1080
#define HDMI_HTOTAL           2200
#define HDMI_VTOTAL           1125
#define HDMI_HSYNC_START      2008
#define HDMI_HSYNC_END        2052
#define HDMI_VSYNC_START      1084
#define HDMI_VSYNC_END        1089
#define HDMI_PIXEL_CLK_KHZ    148500
#define HDMI_VIC              16          // CEA-861 1080p60
#define HDMI_VIC_ANCHOR       16          // only VIC with measured PHY data

#include <Library/A733HdmiModeLib.h>

const A733_HDMI_MODE *
A733HdmiGetMode (
  UINT32  Vic
  );

const A733_HDMI_MODE *
A733HdmiActiveMode (
  VOID
  );

//
// Best mode the sink's EDID advertises, capped at the 1080p anchor
// geometry; the anchor itself when no EDID has been read yet or nothing
// bigger than the anchor is on offer.  See A733HdmiMode.c for the policy.
//
const A733_HDMI_MODE *
A733HdmiSelectBestMode (
  VOID
  );

//
// Read the sink EDID (block 0) through the DDC I2CM master and, when a
// detailed timing descriptor is present, fill a native A733_HDMI_MODE.
// Returns EFI_NOT_FOUND when no EDID/DTD is available (caller falls back
// to the anchor 1080p60 mode).
//
EFI_STATUS
A733HdmiReadEdidNativeMode (
  OUT A733_HDMI_MODE  *Mode
  );

//
// Cache + probe API (see A733HdmiEdid.c).  ProbeEdid must be called
// after the HDMI core clock is up (A733HdmiHwInit).
//
EFI_STATUS
A733HdmiProbeEdid (
  VOID
  );

//
// Full-mode: which of the firmware preset modes the sink EDID advertises
// (bit i set == preset i supported).  Used by the Setup Display page to
// build a filtered mode list.  Returns EFI_NO_MEDIA when the sink has no
// valid EDID - the caller then shows the full preset table.
//
EFI_STATUS
A733HdmiGetSupportedPresets (
  OUT UINT32  *Bitmap
  );

//
// EDID detailed-timing list (modes that may not match a preset, e.g.
// 1920x1080@30).  Fill an array of A733_HDMI_MODE; the first DTD is the
// sink's preferred/native timing.
//
EFI_STATUS
A733HdmiGetEdidModes (
  IN  UINTN           MaxModes,
  OUT A733_HDMI_MODE  *Modes,
  OUT UINTN           *Count
  );

extern A733_HDMI_MODE  mEdidNativeMode;
extern BOOLEAN         mEdidNativeValid;

EFI_STATUS
A733HdmiModeFromEditor (
  IN  CONST VOID      *EditorData,
  OUT A733_HDMI_MODE  *Mode
  );

//
// Register sizes are derived; see the individual blocks. DWC-HDMI registers
// are byte-addressed (dw_write = writeb), everything else is 32-bit.
//

// ---------------------------------------------------------------------------
// CCU (0x02002000).  The driver programs the CCU through A733DisplayClockLib
// (keyed AHB/MBUS gates, PLL_DE/CLK_DE0, the HDMI module gate and its
// resets); the offsets below exist for the read-back in A733DisplayClock.c
// and A733HdmiHw.c's register diagnosis.
// ---------------------------------------------------------------------------
#define CCU_BASE              0x02002000

#define CCU_PLL_DE_REG        0x02E0
#define CCU_DE0_CLK_REG       0x0A00
#define CCU_BUS_DE0_REG       0x0A04
#define CCU_DPSS_TOP1_REG     0x16CC
#define CCU_VIDEO_OUT1_REG    0x16EC
#define CCU_TCONTV0_REG       0x1604
#define CCU_HDMI_REG          0x168C
#define CCU_HDMI_TV_CFG_REG   0x1684      // CLK_HDMI: mux(24,3) div_m(8,5) gate(31)
#define CCU_HDMI_SFR_REG      0x1690
// (optional, skipped): HDCP_ESM 0x1694, HDMI_REF/CEC 0x1680

//
// HDMI TV output clock (0x1684) sources.  A733_PLL_VIDEO0_4X_HZ is the
// parent PLL preset by the boot chain (CCU PLL_VIDEO0 @ 0x0120, 4x tap);
// A733_HDMI_PIXEL_CLOCK_HZ is the CEA-861 pixel clock for HDMI_VIC.
// A733DisplayClockSetHdmiTvRate divides the former to the latter.
//
#define A733_PLL_VIDEO0_4X_HZ   594000000UL   // pll-video0-4x (U-Boot preset)
#define A733_HDMI_PIXEL_CLOCK_HZ  148500000UL // 1080p60 (HDMI_VIC 16)

// ---------------------------------------------------------------------------
// tcon-top vo1 (0x05510000) - internal forwarding to the HDMI path
// ---------------------------------------------------------------------------
#define TCON_TOP_VO0_BASE     0x05500000
#define TCON_TOP_VO1_BASE     0x05510000
#define TCON_TOP_TV_SETUP     0x00
#define   TV0_HDMIPHY_CCU_SEL BIT3          // 0 = top-PHY PLL clock source
#define   TV0_TV_CLK_SRC_TVE  BIT1          // tcon-tv dclk from TVE/top-PHY
#define TCON_TOP_CLK_GATE     0x20
#define   TV0_CLK_GATE        BIT20
#define   TV0_HDMI_GATE       BIT28

// ---------------------------------------------------------------------------
// TCON3 (tcon-tv @ 0x05730000)
// ---------------------------------------------------------------------------
#define TCON3_BASE            0x05730000
#define TCON3_GCTL_REG        0x000
#define TCON3_GINT0_REG       0x004
#define TCON3_SRC_CTL_REG     0x040   // src_sel: 1=colorbar, 8=blue (builtin)
#define   TCON3_SRC_COLORBAR  0x01
#define TCON3_IO_POL_REG      0x088
#define   IO0_INV             BIT24       // vsync polarity (1 = positive)
#define   IO1_INV             BIT25       // hsync polarity (1 = positive)
#define   IO2_INV             BIT26
#define TCON3_IO_TRI_REG      0x08C
#define TCON3_TV_CTL_REG      0x090
#define   TV_EN               BIT31
#define   TV_SRC_SEL_SHIFT    0
#define   TV_START_DELAY_SHIFT 2
// basic0 (0x94) is a discarded/legacy register on sun60iw2; the sun60iw2
// variant packs vt+vic39 into basic1 (0x98) and xo/yo into basic2 (0x9C).
#define TCON3_BASIC1_REG      0x098
#define TCON3_BASIC2_REG      0x09C
#define TCON3_BASIC3_REG      0x0A0
#define TCON3_BASIC4_REG      0x0A4
#define TCON3_BASIC5_REG      0x0A8
#define TCON3_DEBUGC_REG      0x0FC     // tcon_debug: line / fifo underflow
#define TCON3_CEU_CTL_REG     0x100
#define TCON3_CEU_00_10_REG   0x110     // rr
#define TCON3_CEU_01_10_REG   0x114
#define TCON3_CEU_02_10_REG   0x118
#define TCON3_CEU_03_10_REG   0x11C
#define TCON3_CEU_10_10_REG   0x120
#define TCON3_CEU_11_10_REG   0x124     // gg
#define TCON3_CEU_12_10_REG   0x128
#define TCON3_CEU_13_10_REG   0x12C
#define TCON3_CEU_20_10_REG   0x130
#define TCON3_CEU_21_10_REG   0x134
#define TCON3_CEU_22_10_REG   0x138     // bb
#define TCON3_CEU_23_10_REG   0x13C
#define TCON3_CEU_0L_11_REG   0x140     // rv
#define TCON3_CEU_1L_11_REG   0x144     // gv
#define TCON3_CEU_2L_11_REG   0x148     // bv
#define TCON3_CEU_3L_11_REG   0x14C
#define TCON3_FILL_CTL_REG    0x300
#define TCON3_FILL_START0_REG 0x304
#define TCON3_FILL_END0_REG   0x308
#define TCON3_FILL_DATA0_REG  0x30C
#define TCON3_DATA_IO_POL0_REG 0x330
#define TCON3_DATA_IO_POL1_REG 0x334
#define TCON3_DATA_IO_TRI0_REG 0x338
#define TCON3_DATA_IO_TRI1_REG 0x33C
#define TCON3_PIXEL_DEPTH_REG 0x340

// ---------------------------------------------------------------------------
// DE3.0 (0x05000000)
// ---------------------------------------------------------------------------
#define DE_BASE               0x05000000

// DE TOP.  The A7A DTS wires HDMI to disp0 (&disp0_out_tcon3 endpoint
// reg=<4>), and de352-mode2 routes disp0's video chn0 to vch0 (phy chn
// 0).  Register offsets follow the BSP de330/de352 V2 layout:
//   clk/reset   CORE0 at 0x8000/0x8004 bit0 (disp0 = DE_CLK_DISP0) + bit16
//   rtmx        offset = base + disp * 0x40  (disp0 => +0x00)
//   de2tcon     bits[disp<<2 .. +4)  (disp0 => bits[3:0])
//   buf_depth   0x8050 + disp * 0x4
//   vch2core    0x8028 (video), uch2core 0x802C (UI)
//   port2chn    0x8030 + disp * 0x4
#define DE_TOP_RESET_REG      0x8000
#define DE_TOP_CLK_REG        0x8004
#define DE_TOP_MBUS_CLK_REG   0x8008
#define DE_TOP_RESERVE_CTL_REG 0x800C
#define   DE_AHB_READ_MODE_SHIFT 8   // 1 = conflict mode: RCQ DMA may use AHB
#define DE_TOP_URGENCY_REG    0x8080
#define DE_TOP_BUF_DEPTH_REG  0x8050        // disp0 (stride 0x4)
#define DE_TOP_DE2TCON_MUX_REG 0x8010
#define DE_TOP_VCH2CORE_MUX_REG 0x8028      // video chn -> core (4 bits/chn)
#define DE_TOP_UCH2CORE_MUX_REG 0x802C      // UI chn -> core (4 bits/chn)
#define DE_TOP_PORT2CHN_MUX_REG 0x8030      // disp0 port->chn (stride 0x4)
#define DE_TOP_RTMX_GLB_CTL_REG 0x8100      // disp0 (+0x40)
#define DE_TOP_RTMX_GLB_STS_REG 0x8104
#define DE_TOP_RTMX_OUT_SIZE_REG 0x8108
#define DE_TOP_RTMX_AUTO_CLK_REG 0x810C
#define DE_TOP_RCQ_CTL_REG      0x8110
#define DE_TOP_RCQ_HEADER_LADDR_REG 0x8114
#define DE_TOP_RCQ_HEADER_HADDR_REG 0x8118
#define DE_TOP_RCQ_HEADER_LEN_REG   0x811C

// Channel 0 (vch0), the video channel disp0 drives HDMI with.  OVL block
// at chn0 base 0x100000 + 0x1000 = 0x101000; CSC at +0x800; scaler at
// +0x4000.  (de352-mode2 disp0 = "v0v1u0u2": vch0 is its first video chn.)
#define DE_VCH0_OVL_BASE      0x101000
#define   OVL_L0_CTL_REG      0x000
#define   OVL_L0_MBSIZE_REG   0x004
#define   OVL_L0_MBCOOR_REG   0x008
#define   OVL_L0_PITCH0_REG   0x00C
#define   OVL_L0_PITCH1_REG   0x010
#define   OVL_L0_PITCH2_REG   0x014
#define   OVL_L0_TOPLADDR0_REG 0x018
#define   OVL_L1_CTL_REG      0x030
#define   OVL_L2_CTL_REG      0x060
#define   OVL_L3_CTL_REG      0x090
#define   OVL_FCOLOR0_REG     0x0C0
#define   OVL_L0_TOPHADDR0_REG 0x0D0
#define   OVL_WIN_SIZE_REG    0x0E8

// Channel 0 CSC (vch0 = chn0 base 0x100000 + 0x800)
#define DE_VCH0_CSC_BASE      0x100800
#define   CSC_CTL_REG         0x000
#define   CSC_D0_REG          0x004
#define   CSC_D1_REG          0x008
#define   CSC_D2_REG          0x00C
#define   CSC_C0_0_REG        0x010
#define   CSC_C1_1_REG        0x024
#define   CSC_C2_2_REG        0x038

// Channel 0 scaler (disable for 1:1), chn0 base + 0x4000
#define DE_VCH0_SCALER_CTL_REG 0x104000

// Blender for disp0: DISP_BASE_V3XX(0x280000) + disp0*0x20000 + 0x1000
#define DE_DISP0_BLD_BASE     0x281000
#define   BLD_EN_REG          0x000
#define   BLD_PIPE0_FCOLOR_REG 0x004
#define   BLD_PIPE0_INSIZE_REG 0x008
#define   BLD_PIPE0_INCOORD_REG 0x00C
#define   BLD_ROUT_CTL_REG    0x080
#define   BLD_PREMUL_CTL_REG  0x084
#define   BLD_BG_COLOR_REG    0x088
#define   BLD_OUT_SIZE_REG    0x08C
#define   BLD_BLEND_CTL0_REG  0x090
#define   BLD_OUT_CTL_REG     0x0FC

// ---------------------------------------------------------------------------
// DWC HDMI (0x05520000, BYTE-addressed registers)
// ---------------------------------------------------------------------------
#define HDMI_BASE             0x05520000

// MC
#define MC_CLKDIS_REG         0x4001
#define MC_SWRSTZREQ_REG      0x4002
#define MC_FLOWCTRL_REG       0x4004
#define MC_PHYRSTZ_REG        0x4005

// PHY (I2CM slave)
#define PHY_CONF0_REG         0x3000
#define PHY_STAT0_REG         0x3004
//
// PHY_STAT0 bit1 is the live hot-plug detect (the dw_hdmi HDMI_PHY_HPD
// convention; bit0 is the PLL lock the PHY bring-up polls).  The driver's
// hot-plug poll reads it once a second; IH_MUTE masks the interrupt path
// and does not gate this status bit.
//
#define HDMI_PHY_HPD          BIT1
#define PHY_I2CM_SLAVE_REG    0x3020
#define PHY_I2CM_ADDR_REG     0x3021
#define PHY_I2CM_DATAO_1_REG  0x3022
#define PHY_I2CM_DATAO_0_REG  0x3023
#define PHY_I2CM_OPERATION_REG 0x3026
#define JTAG_PHY_CONFIG_REG   0x3034
#define IH_I2CMPHY_STAT0_REG  0x0108

// DDC I2CM master (DWC-HDMI standard block, offset 0x7E00).  Used to read
// the sink EDID at slave address 0x50 (I2CM_SLAVE_DDC_ADDR) - the same
// register set U-Boot's dw_hdmi.c drives for its DDC reads.
#define DDC_I2CM_SLAVE_REG        0x7E00
#define DDC_I2CM_ADDRESS_REG      0x7E01
#define DDC_I2CM_DATAI_REG        0x7E03
#define DDC_I2CM_OPERATION_REG    0x7E04
#define DDC_I2CM_DIV_REG          0x7E07
#define DDC_I2CM_SEGADDR_REG      0x7E08
#define DDC_I2CM_SOFTRSTZ_REG     0x7E09
#define DDC_I2CM_SEGPTR_REG       0x7E0A
#define DDC_I2CM_SS_SCL_HCNT_1_REG 0x7E0B
#define DDC_I2CM_SS_SCL_HCNT_0_REG 0x7E0C
#define DDC_I2CM_SS_SCL_LCNT_1_REG 0x7E0D
#define DDC_I2CM_SS_SCL_LCNT_0_REG 0x7E0E
#define IH_I2CM_STAT0_REG         0x0105

// DDC SCL timing (100 kHz standard mode, sunxi dw-hdmi reference values).
#define DDC_SCL_HCNT              0x00D8
#define DDC_SCL_LCNT              0x00FE

#define DDC_I2CM_SLAVE_ADDR       0x50   // EDID EEPROM on the DDC bus
#define DDC_I2CM_SEGADDR_DDC      0x30
//
// DDC I2CM transfer ops.  Values from the A733 BSP dw_i2cm.h (the
// reference that actually reads this board's EDID): RD8 = 0x04,
// RD8_EXT = 0x08.  The DWC bit layout differs from the generic
// dw_hdmi.h used by U-Boot (RD8 = 0x1) - the wrong opcode makes the
// master never issue the read (IH stays 0x00).
//
//
// Single-byte read into DATAI; RD8 (0x04) reads 8 bytes into
// READ_BUFF0-7 instead - pairing RD8 with per-byte DATAI reads shifts
// every byte (the EEPROM pointer advances 8, we only take 1).
//
#define DDC_I2CM_OP_RD            0x01   // read 1 byte -> DATAI
#define DDC_I2CM_OP_RD8           0x04   // read 8 bytes -> READ_BUFF0-7
#define DDC_I2CM_OP_RD8_EXT       0x08   // extended (block 1+)
#define DDC_I2CM_DIV_STD_MODE     0x0
#define DDC_I2CM_SOFTRSTZ_MASK    0x1

// FC (frame composer)
#define FC_INVIDCONF_REG      0x1000
#define FC_INHACTIVE0_REG     0x1001
#define FC_INHACTIVE1_REG     0x1002
#define FC_INHBLANK0_REG      0x1003
#define FC_INHBLANK1_REG      0x1004
#define FC_INVACTIVE0_REG     0x1005
#define FC_INVACTIVE1_REG     0x1006
#define FC_INVBLANK_REG       0x1007
#define FC_HSYNCINDELAY0_REG  0x1008
#define FC_HSYNCINDELAY1_REG  0x1009
#define FC_HSYNCINWIDTH0_REG  0x100A
#define FC_HSYNCINWIDTH1_REG  0x100B
#define FC_VSYNCINDELAY_REG   0x100C
#define FC_VSYNCINWIDTH_REG   0x100D
#define FC_CTRLDUR_REG        0x1011
#define FC_EXCTRLDUR_REG      0x1012
#define FC_EXCTRLSPAC_REG     0x1013
#define FC_CH0PREAM_REG       0x1014
#define FC_CH1PREAM_REG       0x1015
#define FC_CH2PREAM_REG       0x1016
#define FC_GCP_REG            0x1018
#define FC_AVICONF0_REG       0x1019
#define FC_AVICONF1_REG       0x101A
#define FC_AVICONF2_REG       0x101B
#define FC_AVIVID_REG         0x101C
#define FC_AUDSCONF_REG       0x1063
#define FC_PRSYNC12_REG       0x1074   // (unused, keep 0)
#define FC_DATAUTO0_REG       0x10B3
#define FC_DATAUTO1_REG       0x10B4
#define FC_DATAUTO2_REG       0x10B5
#define FC_PACKET_TX_EN_REG   0x10E3
#define FC_SCRAMBLER_CTRL_REG 0x10E1
#define FC_PR_CTRL_REG        0x10E0
#define FC_VSDEMU_REG         0x10E2   // (unused)

// VP / TX
#define VP_PR_CD_REG          0x0801
#define VP_STUFF_REG          0x0802
#define VP_REMAP_REG          0x0803
#define VP_CONF_REG           0x0804
#define TX_INVID0_REG         0x0200
#define TX_INSTUFFING_REG     0x0201

// IH
#define IH_MUTE_REG           0x01FF
#define IH_MUTE_FC_REG        0x0180
#define IH_MUTE_AS_REG        0x0181
#define IH_MUTE_PHY_REG       0x0182
#define IH_MUTE_PHY_STAT0_REG 0x0183
#define IH_MUTE_I2CM_ABORT_REG 0x0186
#define IH_MUTE_I2CM_STAT0_REG 0x0185   // BSP dw_mc.h: IH_MUTE_I2CM_STAT0
#define IH_MUTE_CPM_REG       0x0188
#define IH_MUTE_VP_REG        0x0189

// ---------------------------------------------------------------------------
// Top PHY PLL (0x05600000) - pixel clock source in phy-PLL mode
// ---------------------------------------------------------------------------
#define HDMI_TOP_PHY_BASE     0x05600000
#define TOP_PHY_REG_0000      0x000
#define TOP_PHY_REG_0020      0x020
#define TOP_PHY_REG_0024      0x024
#define TOP_PHY_REG_0028      0x028
#define TOP_PHY_REG_002C      0x02C
#define TOP_PHY_REG_0030      0x030
#define TOP_PHY_REG_0040      0x040
#define   TOP_PHY_PLL_E1       0xE80F6200  // n=98, p0=15, ldo+en
#define   TOP_PHY_PLL_LOCKED   0xEA0F6200  // + lock_enable
#define   TOP_PHY_PLL_OGATE    0xEB0F6200  // + output gate
#define   TOP_PHY_LDO_E1       0x00035000
#define   TOP_PHY_PAT0_E1      0x80000000
#define   TOP_PHY_PAT1_E1      0x30000000

// ---------------------------------------------------------------------------
// SNPS PHY internal registers (written via I2CM)
// ---------------------------------------------------------------------------
#define SNPS_PHY_MPLL_0       0x06       // mpll reg1
#define SNPS_PHY_MPLL_1       0x10       // mpll reg2
#define SNPS_PHY_MPLL_2       0x11       // mpll reg3
#define SNPS_PHY_DRV_0        0x19       // drive reg1
#define SNPS_PHY_DRV_1        0x0E       // drive reg2
#define SNPS_PHY_DRV_2        0x09       // drive reg3

// 148.5MHz, 8bpc -> MPLL index 0
#define SNPS_MPLL_E1          0x0001
#define SNPS_MPLL_E2          0x2080
#define SNPS_MPLL_E3          0x020A
// drive values from DTS snps_phy band 0 (25000-185625 kHz)
#define SNPS_DRV_E1           0x0007
#define SNPS_DRV_E2           0x8160
#define SNPS_DRV_E3           0x8188

// Board-level HDMI resistor select (SYSCFG)
#define SYSCFG_BASE           0x03000000
#define SYSCFG_HDMI_R1_REG    0x160
#define SYSCFG_HDMI_R2_REG    0x164

// ---------------------------------------------------------------------------
// Module interfaces
// ---------------------------------------------------------------------------
EFI_STATUS
A733DisplayClockInit (VOID);

VOID
A733DeInit (
  IN EFI_PHYSICAL_ADDRESS  FrameBufferBase
  );

VOID
A733DeQuiesce (VOID);

VOID
A733DeRcqCommit (VOID);

VOID
A733TconInit (VOID);

VOID
A733HdmiHwInit (VOID);

VOID
A733HdmiDiag (VOID);

#endif // SUNXI_HDMI_DXE_H_