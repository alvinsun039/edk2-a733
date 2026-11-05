/** @file
  Clock control unit library for the Allwinner A733 (sun60iw2p1).

  Register layouts and field meanings are in <A733Ccu.h>; this file adds the
  module table and the arithmetic.  Where the A733 manual's field positions
  could not be reconciled against a measured clock, the code says so instead
  of presenting a guess as a measurement.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/A733CcuLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>

// A module that has no register of a given kind ("-" in the manual's table).
#define A733_CCU_NO_REG  0xFFFFU

// Long enough for a clocked block to see the asserted reset; the manual
// gives no minimum, so this is a safe lower bound rather than a tuned value.
#define A733_CCU_RESET_PULSE_US  10U

typedef struct {
  UINT8          Sel;
  A733_CCU_SRC   Class;
} A733_CCU_SRC_MAP;

typedef struct {
  CONST CHAR8    *Name;
  UINT16         ClkOffset;
  UINT16         BgrOffset;
  UINT8          SrcShift;
  UINT8          SrcWidth;
  CONST A733_CCU_SRC_MAP  *Sources;
  UINTN          SourceCount;
} A733_CCU_MODULE_DESC;

//
// Manual: CCU chapter, module register table, and the vendor register header's
// per-module parent tables.  Offsets are relative to the CCU base;
// A733_CCU_NO_REG means the manual lists no such register.  Each row maps the
// value a module's selector can hold to the frequency class it names: the
// mapping is per module (SMHC0 sel 1 is PERI0_400M where SMHC2 sel 1 is
// PERI0_800M), so every module that has a selector carries its own table.
// A value the table does not list is reserved: A733CcuSetModuleClock refuses
// it and A733CcuGetModuleClockHz reports it as undecoded rather than guessing.
//
// SrcWidth is 2 for GIC (25:24) and 3 for everyone else, which is why the
// width lives here instead of in a test on the module.  The divider is the
// manual's M*N for every module with a CLK_REG (FACTOR_M at 4:0, FACTOR_N at
// 12:8), so both factors are written and read everywhere and no row needs to
// record whether N is in play.
//
// A few modules have a CLK_REG whose 26:24 field is not a parent selector at
// all (USB0's 12 MHz selector, GMAC0's plain divider), and UART0/UART1 are
// clocked by the shared APB_UART gate; those carry no table and report
// EFI_UNSUPPORTED from A733CcuSetModuleClock.
//
STATIC CONST A733_CCU_SRC_MAP  mGicSources[] = {
  { 0, A733_CCU_SRC_HOSC },
  { 1, A733_CCU_SRC_CLK32K },
  { 2, A733_CCU_SRC_PERI0_600M },
  { 3, A733_CCU_SRC_PERI0_480M },
};

STATIC CONST A733_CCU_SRC_MAP  mSmhc01NandSources[] = {
  { 0, A733_CCU_SRC_HOSC },
  { 1, A733_CCU_SRC_PERI0_400M },
  { 2, A733_CCU_SRC_PERI0_300M },
  { 3, A733_CCU_SRC_PERI1_400M },
  { 4, A733_CCU_SRC_PERI1_300M },
};

STATIC CONST A733_CCU_SRC_MAP  mSmhc23Sources[] = {
  { 0, A733_CCU_SRC_HOSC },
  { 1, A733_CCU_SRC_PERI0_800M },
  { 2, A733_CCU_SRC_PERI0_600M },
  { 3, A733_CCU_SRC_PERI1_800M },
  { 4, A733_CCU_SRC_PERI1_600M },
};

STATIC CONST A733_CCU_SRC_MAP  mSpi0Sources[] = {
  { 0, A733_CCU_SRC_SYS_CLK24M },
  { 1, A733_CCU_SRC_PERI0_300M },
  { 2, A733_CCU_SRC_PERI0_200M },
  { 3, A733_CCU_SRC_PERI1_300M },
  { 4, A733_CCU_SRC_PERI1_200M },
  { 5, A733_CCU_SRC_PERI0_160M },
  { 6, A733_CCU_SRC_PERI1_160M },
  { 7, A733_CCU_SRC_HOSC },
};

STATIC CONST A733_CCU_SRC_MAP  mDe0Sources[] = {
  { 0, A733_CCU_SRC_DEPLL3X },
  { 1, A733_CCU_SRC_DEPLL4X },
  { 2, A733_CCU_SRC_PERI0_480M },
  { 3, A733_CCU_SRC_PERI0_400M },
  { 4, A733_CCU_SRC_PERI0_300M },
  { 5, A733_CCU_SRC_VIDEO0PLL4X },
  { 6, A733_CCU_SRC_VIDEO3PLL4X },
};

STATIC CONST A733_CCU_SRC_MAP  mDsi0Sources[] = {
  { 0, A733_CCU_SRC_HOSC },
  { 1, A733_CCU_SRC_PERI0_200M },
  { 2, A733_CCU_SRC_PERI0_150M },
};

//
// TCONLCD0 and HDMI share one selector table: both name the four VIDEO*PLL4X
// outputs and then PERI0's 2X tap, with the same values.  The vendor register
// header carries the same set in VO0_TCONLCD0_CLK_REG (0x1500) and
// HDMI_PRE_CLK_REG (0x1684), which is why the library reads HDMI's selector
// from the latter rather than from the HDMI BGR.
//
STATIC CONST A733_CCU_SRC_MAP  mTconHdmiSources[] = {
  { 0, A733_CCU_SRC_VIDEO0PLL4X },
  { 1, A733_CCU_SRC_VIDEO1PLL4X },
  { 2, A733_CCU_SRC_VIDEO2PLL4X },
  { 3, A733_CCU_SRC_VIDEO3PLL4X },
  { 4, A733_CCU_SRC_PERI0PLL2X },
};

STATIC CONST A733_CCU_SRC_MAP  mNpuSources[] = {
  { 0, A733_CCU_SRC_NPUPLL },
  { 1, A733_CCU_SRC_PERI0_800M },
  { 2, A733_CCU_SRC_PERI0_600M },
  { 3, A733_CCU_SRC_PERI0_480M },
  { 4, A733_CCU_SRC_CCIPLL },
  { 5, A733_CCU_SRC_VE0PLL },
  { 6, A733_CCU_SRC_VE1PLL },
  { 7, A733_CCU_SRC_DEPLL3X },
};

//
// GPU0's selector is its own set: GPUPLL, then the PERI0 taps as the vendor
// header's GPU0_CLK_REG (0x0B20) lists them, stopping at PERI0_200M.
//
STATIC CONST A733_CCU_SRC_MAP  mGpuSources[] = {
  { 0, A733_CCU_SRC_GPUPLL },
  { 1, A733_CCU_SRC_PERI0_800M },
  { 2, A733_CCU_SRC_PERI0_600M },
  { 3, A733_CCU_SRC_PERI0_400M },
  { 4, A733_CCU_SRC_PERI0_300M },
  { 5, A733_CCU_SRC_PERI0_200M },
};

STATIC CONST A733_CCU_SRC_MAP  mVeDecSources[] = {
  { 0, A733_CCU_SRC_VE1PLL },
  { 1, A733_CCU_SRC_VE0PLL },
  { 2, A733_CCU_SRC_PERI0_800M },
  { 3, A733_CCU_SRC_PERI0_600M },
  { 4, A733_CCU_SRC_PERI0_480M },
  { 5, A733_CCU_SRC_DEPLL3X },
  { 6, A733_CCU_SRC_NPUPLL },
};

STATIC CONST A733_CCU_SRC_MAP  mVeEnc0Sources[] = {
  { 0, A733_CCU_SRC_VE0PLL },
  { 1, A733_CCU_SRC_VE1PLL },
  { 2, A733_CCU_SRC_PERI0_800M },
  { 3, A733_CCU_SRC_PERI0_600M },
  { 4, A733_CCU_SRC_PERI0_480M },
  { 5, A733_CCU_SRC_DEPLL3X },
  { 6, A733_CCU_SRC_NPUPLL },
};

//
// One row per module, in A733_CCU_MODULE order.  SrcShift/SrcWidth describe the
// module's own selector field (GIC's is 2 bits wide at 25:24); SrcWidth 0 means
// the module has no parent selector, and its Sources/SourceCount are then empty.
//
STATIC CONST A733_CCU_MODULE_DESC  mModules[A733_CCU_MODULE_MAX] = {
  { "GIC",      A733_CCU_GIC_CLK,       A733_CCU_NO_REG,        A733_CCU_CLK_SRC_SHIFT, 2U, mGicSources,      ARRAY_SIZE (mGicSources)      },
  { "DMA0",     A733_CCU_NO_REG,        A733_CCU_DMA0_BGR,      0U,                     0U, NULL,             0                             },
  { "UART0",    A733_CCU_NO_REG,        A733_CCU_UART0_BGR,     0U,                     0U, NULL,             0                             },
  { "UART1",    A733_CCU_NO_REG,        A733_CCU_UART1_BGR,     0U,                     0U, NULL,             0                             },
  { "SMHC0",    A733_CCU_SMHC0_CLK,     A733_CCU_SMHC0_BGR,     A733_CCU_CLK_SRC_SHIFT, 3U, mSmhc01NandSources, ARRAY_SIZE (mSmhc01NandSources) },
  { "SMHC1",    A733_CCU_SMHC1_CLK,     A733_CCU_SMHC1_BGR,     A733_CCU_CLK_SRC_SHIFT, 3U, mSmhc01NandSources, ARRAY_SIZE (mSmhc01NandSources) },
  { "SMHC2",    A733_CCU_SMHC2_CLK,     A733_CCU_SMHC2_BGR,     A733_CCU_CLK_SRC_SHIFT, 3U, mSmhc23Sources,   ARRAY_SIZE (mSmhc23Sources)   },
  { "SMHC3",    A733_CCU_SMHC3_CLK,     A733_CCU_SMHC3_BGR,     A733_CCU_CLK_SRC_SHIFT, 3U, mSmhc23Sources,   ARRAY_SIZE (mSmhc23Sources)   },
  { "NAND0",    A733_CCU_NAND0_CLK,     A733_CCU_NAND0_BGR,     A733_CCU_CLK_SRC_SHIFT, 3U, mSmhc01NandSources, ARRAY_SIZE (mSmhc01NandSources) },
  { "SPI0",     A733_CCU_SPI0_CLK,      A733_CCU_SPI0_BGR,      A733_CCU_CLK_SRC_SHIFT, 3U, mSpi0Sources,     ARRAY_SIZE (mSpi0Sources)     },
  { "USB0",     A733_CCU_USB0_CLK,      A733_CCU_USB0_BGR,      0U,                     0U, NULL,             0                             },
  { "USB1",     A733_CCU_USB1_CLK,      A733_CCU_USB1_BGR,      0U,                     0U, NULL,             0                             },
  { "GMAC0",    A733_CCU_GMAC0_CLK,     A733_CCU_GMAC0_BGR,     0U,                     0U, NULL,             0                             },
  { "DE0",      A733_CCU_DE0_CLK,       A733_CCU_DE0_BGR,       A733_CCU_CLK_SRC_SHIFT, 3U, mDe0Sources,      ARRAY_SIZE (mDe0Sources)      },
  { "TCONLCD0", A733_CCU_TCONLCD0_CLK,  A733_CCU_TCONLCD0_BGR,  A733_CCU_CLK_SRC_SHIFT, 3U, mTconHdmiSources, ARRAY_SIZE (mTconHdmiSources) },
  { "DSI0",     A733_CCU_DSI0_CLK,      A733_CCU_DSI0_BGR,      A733_CCU_CLK_SRC_SHIFT, 3U, mDsi0Sources,     ARRAY_SIZE (mDsi0Sources)     },
  { "HDMI",     A733_CCU_HDMI_CLK,      A733_CCU_HDMI_BGR,      A733_CCU_CLK_SRC_SHIFT, 3U, mTconHdmiSources, ARRAY_SIZE (mTconHdmiSources) },
  { "GPU0",     A733_CCU_GPU0_CLK,      A733_CCU_NO_REG,        A733_CCU_CLK_SRC_SHIFT, 3U, mGpuSources,      ARRAY_SIZE (mGpuSources)      },
  { "NPU",      A733_CCU_NPU_CLK,       A733_CCU_NPU_BGR,       A733_CCU_CLK_SRC_SHIFT, 3U, mNpuSources,      ARRAY_SIZE (mNpuSources)      },
  { "VE_DEC",   A733_CCU_VE_DEC_CLK,    A733_CCU_VE_BGR,        A733_CCU_CLK_SRC_SHIFT, 3U, mVeDecSources,    ARRAY_SIZE (mVeDecSources)    },
  { "VE_ENC0",  A733_CCU_VE_ENC0_CLK,   A733_CCU_VE_BGR,        A733_CCU_CLK_SRC_SHIFT, 3U, mVeEnc0Sources,   ARRAY_SIZE (mVeEnc0Sources)   },
};

// Manual: CCU chapter, PLL register table, indexed by A733_CCU_PLL.
STATIC CONST UINT16  mCcuPllOffset[] = {
  A733_CCU_PLL_DDR_REG,
  A733_CCU_PLL_PERI0_REG,
  A733_CCU_PLL_PERI1_REG,
  A733_CCU_PLL_GPU0_REG,
  A733_CCU_PLL_VIDEO0_REG,
  A733_CCU_PLL_VIDEO1_REG,
  A733_CCU_PLL_VIDEO2_REG,
};

// Manual: CPU PLL chapter, indexed by (A733_CCU_PLL - A733_CCU_PLL_CPU_BACK).
STATIC CONST UINT16  mCpuPllOffset[] = {
  A733_CPU_PLL_BACK_REG,
  A733_CPU_PLL_L_REG,
  A733_CPU_PLL_B_REG,
  A733_CPU_PLL_DSU_REG,
};

STATIC CONST CHAR8  *mPllName[A733_CCU_PLL_MAX] = {
  "PLL_DDR",    "PLL_PERI0",  "PLL_PERI1",   "PLL_GPU0",
  "PLL_VIDEO0", "PLL_VIDEO1", "PLL_VIDEO2",  "CPU_BACK_PLL",
  "CPU_L_PLL",  "CPU_B_PLL",  "CPU_DSU_PLL",
};

//
// The enum, the module table and the PLL table are transcribed from the same
// manual tables by hand, so pin their sizes to their enums.
//
STATIC_ASSERT (ARRAY_SIZE (mModules) == A733_CCU_MODULE_MAX, "module table and A733_CCU_MODULE out of step");
STATIC_ASSERT (
  ARRAY_SIZE (mCcuPllOffset) + ARRAY_SIZE (mCpuPllOffset) == A733_CCU_PLL_MAX,
  "PLL tables and A733_CCU_PLL out of step"
  );

/**
  Address a CCU register.

  @param[in] Offset  Register offset inside the 8 KB CCU window.

  @return The MMIO address of the register.
**/
STATIC
UINTN
CcuReg (
  IN UINT16  Offset
  )
{
  return (UINTN)PcdGet64 (PcdA733CcuBase) + Offset;
}

/**
  Raw 32-bit access to a CCU register, for blocks that program their own
  offsets instead of using the module table.

  @param[in] RegOffset  Offset from the CCU base.

  @return The register value (read); the writers return the written value.
**/
UINT32
EFIAPI
A733CcuRead (
  IN UINT32  RegOffset
  )
{
  return MmioRead32 (CcuReg ((UINT16)RegOffset));
}

VOID
EFIAPI
A733CcuWrite (
  IN UINT32  RegOffset,
  IN UINT32  Value
  )
{
  MmioWrite32 (CcuReg ((UINT16)RegOffset), Value);
}

VOID
EFIAPI
A733CcuSetBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  )
{
  MmioOr32 (CcuReg ((UINT16)RegOffset), Bits);
}

VOID
EFIAPI
A733CcuClearBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  )
{
  MmioAnd32 (CcuReg ((UINT16)RegOffset), ~Bits);
}

/**
  Address a register of the CPU PLL block.

  @param[in] Offset  Register offset inside the CPU PLL block.

  @return The MMIO address of the register.
**/
STATIC
UINTN
CpuPllReg (
  IN UINT16  Offset
  )
{
  return (UINTN)PcdGet64 (PcdA733CpuPllBase) + Offset;
}

/**
  Read a CPU PLL's VCO.

  Manual: CPU PLL chapter: VCO = DCXO * N / M0, with N stored as-is for
  L/B/DSU and M0 = field + 1.  That reproduces the 988 MHz (N = 38) and
  1014 MHz (N = 39) this port measures, which is why the PLL_CTRL P field is
  not applied here - the clock mux's own P divider is what produces the
  494 MHz core clock.

  @param[in] Offset  PLL control register offset inside the CPU PLL block.

  @return The VCO in Hz, or 0 when the register reads back as all-zero.
**/
STATIC
UINT64
CpuPllVcoHz (
  IN UINT16  Offset
  )
{
  UINT32  Value;
  UINT32  N;
  UINT32  M0;

  Value = MmioRead32 (CpuPllReg (Offset));

  N  = BitFieldRead32 (Value, A733_CPU_PLL_N_SHIFT, A733_CPU_PLL_N_SHIFT + A733_CPU_PLL_N_WIDTH - 1);
  M0 = BitFieldRead32 (Value, A733_CPU_PLL_M0_SHIFT, A733_CPU_PLL_M0_SHIFT + A733_CPU_PLL_M0_WIDTH - 1) + 1;

  if (N == 0) {
    return 0;
  }

  return MultU64x32 (A733_CCU_DCXO_HZ, N) / M0;
}

/**
  Derive a CCU peripheral PLL's rate from the manual's formula.

  Manual: PLL_PERI0/PERI1 chapter, PERI0PLL2X = f*N/(M1*P0).  The manual
  names the fields but never ties P0/M1/N to bit positions that we could
  check: nothing we could measure on this board is documented as a PERI0
  output.  The decode below is therefore a transcription of the manual, with
  each field read as value + 1 (the convention the CPU PLLs use), and the
  result is an estimate - A733CcuDumpTree prints the raw register next to it
  so the two can be compared on real hardware.

  @param[in] Offset  PLL control register offset inside the CCU window.

  @return The estimated rate in Hz.
**/
STATIC
UINT64
PeripheralPllEstimateHz (
  IN UINT16  Offset
  )
{
  UINT32  Value;
  UINT32  P0;
  UINT32  M1;
  UINT32  N;

  Value = MmioRead32 (CcuReg (Offset));

  P0 = BitFieldRead32 (Value, A733_CCU_PLL_P0_SHIFT, A733_CCU_PLL_P0_SHIFT + A733_CCU_PLL_P0_WIDTH - 1) + 1;
  M1 = BitFieldRead32 (Value, A733_CCU_PLL_M1_SHIFT, A733_CCU_PLL_M1_SHIFT + A733_CCU_PLL_M1_WIDTH - 1) + 1;
  N  = BitFieldRead32 (Value, A733_CCU_PLL_N_SHIFT, A733_CCU_PLL_N_SHIFT + A733_CCU_PLL_N_WIDTH - 1) + 1;

  return MultU64x32 (A733_CCU_DCXO_HZ, N) / ((UINT64)M1 * P0);
}

//
// Names and rates of the A733_CCU_SRC classes, indexed by the enum.  The
// PERI0/PERI1 entries are the manual's nominal taps.  The PLL-derived classes
// have no rate this port could verify, so their entry is 0 and every caller
// treats that as "unknown", never as a measurement: see A733_CCU_SRC_HOSC_HZ
// on why HOSC and the 26 MHz DCXO are different clocks.
//
STATIC CONST CHAR8  *mSrcName[A733_CCU_SRC_MAX] = {
  "HOSC",        "SYS_CLK24M",  "CLK32K",
  "PERI0_800M",  "PERI0_600M",  "PERI0_480M",  "PERI0_400M",  "PERI0_300M",
  "PERI0_200M",  "PERI0_160M",  "PERI0_150M",
  "PERI1_800M",  "PERI1_600M",  "PERI1_480M",  "PERI1_400M",  "PERI1_300M",
  "PERI1_200M",  "PERI1_160M",
  "DEPLL3X",     "DEPLL4X",     "VIDEO0PLL4X", "VIDEO1PLL4X",
  "VIDEO2PLL4X", "VIDEO3PLL4X", "PERI0PLL2X",
  "NPUPLL",      "GPUPLL",      "CCIPLL",      "VE0PLL",      "VE1PLL",
};

STATIC CONST UINT64  mSrcHz[A733_CCU_SRC_MAX] = {
  A733_CCU_SRC_HOSC_HZ,
  A733_CCU_SRC_SYS_CLK24M_HZ,
  A733_CCU_SRC_CLK32K_HZ,
  A733_CCU_SRC_PERI0_800M_HZ,
  A733_CCU_SRC_PERI0_600M_HZ,
  A733_CCU_SRC_PERI0_480M_HZ,
  A733_CCU_SRC_PERI0_400M_HZ,
  A733_CCU_SRC_PERI0_300M_HZ,
  A733_CCU_SRC_PERI0_200M_HZ,
  A733_CCU_SRC_PERI0_160M_HZ,
  A733_CCU_SRC_PERI0_150M_HZ,
  A733_CCU_SRC_PERI1_800M_HZ,
  A733_CCU_SRC_PERI1_600M_HZ,
  A733_CCU_SRC_PERI1_480M_HZ,
  A733_CCU_SRC_PERI1_400M_HZ,
  A733_CCU_SRC_PERI1_300M_HZ,
  A733_CCU_SRC_PERI1_200M_HZ,
  A733_CCU_SRC_PERI1_160M_HZ,
  0,  // DEPLL3X     - rate not verified on this port
  0,  // DEPLL4X     - rate not verified on this port
  0,  // VIDEO0PLL4X - rate not verified on this port
  0,  // VIDEO1PLL4X - rate not verified on this port
  0,  // VIDEO2PLL4X - rate not verified on this port
  0,  // VIDEO3PLL4X - rate not verified on this port
  0,  // PERI0PLL2X  - PERI0's 2X tap; no verified absolute rate, not assumed 1.2 GHz
  0,  // NPUPLL      - rate not verified on this port
  0,  // GPUPLL      - rate not verified on this port
  0,  // CCIPLL      - rate not verified on this port
  0,  // VE0PLL      - rate not verified on this port
  0,  // VE1PLL      - rate not verified on this port
};

STATIC_ASSERT (ARRAY_SIZE (mSrcName) == A733_CCU_SRC_MAX, "source name table and A733_CCU_SRC out of step");
STATIC_ASSERT (ARRAY_SIZE (mSrcHz) == A733_CCU_SRC_MAX, "source rate table and A733_CCU_SRC out of step");

/**
  Name of a source class, for the dump.

  @param[in] Class  A733_CCU_SRC class.

  @return The class's name, or "-" when Class is out of range.
**/
STATIC
CONST CHAR8 *
CcuSourceName (
  IN A733_CCU_SRC  Class
  )
{
  if (Class >= A733_CCU_SRC_MAX) {
    return "-";
  }

  return mSrcName[Class];
}

/**
  Rate of a source class.

  @param[in] Class  A733_CCU_SRC class.

  @return The rate in Hz, or 0 when Class is out of range or the class's rate
          has not been verified (every PLL-derived class).
**/
STATIC
UINT64
CcuSourceHz (
  IN A733_CCU_SRC  Class
  )
{
  if (Class >= A733_CCU_SRC_MAX) {
    return 0;
  }

  return mSrcHz[Class];
}

/**
  Find the selector value a module uses for a source class.

  @param[in] Desc   The module's row.
  @param[in] Class  The source class wanted.

  @return The row's mapping, or NULL when this module cannot name that class.
**/
STATIC
CONST A733_CCU_SRC_MAP *
FindSourceByClass (
  IN CONST A733_CCU_MODULE_DESC  *Desc,
  IN A733_CCU_SRC                Class
  )
{
  UINTN  Index;

  for (Index = 0; Index < Desc->SourceCount; Index++) {
    if (Desc->Sources[Index].Class == Class) {
      return &Desc->Sources[Index];
    }
  }

  return NULL;
}

/**
  Find the source class a module's raw selector value names.

  @param[in] Desc  The module's row.
  @param[in] Sel   The raw selector field, already masked to SrcWidth bits.

  @return The row's mapping, or NULL when this module does not decode Sel.
**/
STATIC
CONST A733_CCU_SRC_MAP *
FindSourceByValue (
  IN CONST A733_CCU_MODULE_DESC  *Desc,
  IN UINT32                      Sel
  )
{
  UINTN  Index;

  for (Index = 0; Index < Desc->SourceCount; Index++) {
    if ((UINT32)Desc->Sources[Index].Sel == Sel) {
      return &Desc->Sources[Index];
    }
  }

  return NULL;
}

/**
  Look up a module's registers.

  @param[in] Module  Module to look up.

  @return The module's table row, or NULL when Module is out of range.
**/
STATIC
CONST A733_CCU_MODULE_DESC *
ModuleDesc (
  IN A733_CCU_MODULE  Module
  )
{
  if (Module >= A733_CCU_MODULE_MAX) {
    return NULL;
  }

  return &mModules[Module];
}

/**
  Give a module a running clock: de-assert its reset and open its gates.

  @param[in] Module  Module to enable.

  @retval EFI_SUCCESS            The module's gates are open.
  @retval EFI_INVALID_PARAMETER  Module is out of range.
  @retval EFI_UNSUPPORTED        Module has neither a CLK_REG nor a BGR_REG.
**/
EFI_STATUS
EFIAPI
A733CcuEnableModule (
  IN A733_CCU_MODULE  Module
  )
{
  CONST A733_CCU_MODULE_DESC  *Desc;
  UINT32                      Value;

  Desc = ModuleDesc (Module);
  if (Desc == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Desc->ClkOffset == A733_CCU_NO_REG) && (Desc->BgrOffset == A733_CCU_NO_REG)) {
    return EFI_UNSUPPORTED;
  }

  if (Desc->BgrOffset != A733_CCU_NO_REG) {
    //
    // De-assert reset (bit 16 = 1), then open the gate: the block must leave
    // reset with its bus clock already planned, not race a release against a
    // rising gate.
    //
    Value  = MmioRead32 (CcuReg (Desc->BgrOffset));
    Value |= A733_CCU_BGR_SUB_RST (A733_CCU_BGR_MAIN_SUB);
    MmioWrite32 (CcuReg (Desc->BgrOffset), Value);

    Value |= A733_CCU_BGR_SUB_CLK (A733_CCU_BGR_MAIN_SUB);
    MmioWrite32 (CcuReg (Desc->BgrOffset), Value);
  }

  if (Desc->ClkOffset != A733_CCU_NO_REG) {
    Value  = MmioRead32 (CcuReg (Desc->ClkOffset));
    Value |= A733_CCU_CLK_GATING;
    MmioWrite32 (CcuReg (Desc->ClkOffset), Value);
  }

  return EFI_SUCCESS;
}

/**
  Stop a module's clock.

  @param[in] Module  Module to disable.

  @retval EFI_SUCCESS            The module's gates are closed.
  @retval EFI_INVALID_PARAMETER  Module is out of range.
  @retval EFI_UNSUPPORTED        Module has neither a CLK_REG nor a BGR_REG.
**/
EFI_STATUS
EFIAPI
A733CcuDisableModule (
  IN A733_CCU_MODULE  Module
  )
{
  CONST A733_CCU_MODULE_DESC  *Desc;
  UINT32                      Value;

  Desc = ModuleDesc (Module);
  if (Desc == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Desc->ClkOffset == A733_CCU_NO_REG) && (Desc->BgrOffset == A733_CCU_NO_REG)) {
    return EFI_UNSUPPORTED;
  }

  if (Desc->ClkOffset != A733_CCU_NO_REG) {
    Value  = MmioRead32 (CcuReg (Desc->ClkOffset));
    Value &= ~A733_CCU_CLK_GATING;
    MmioWrite32 (CcuReg (Desc->ClkOffset), Value);
  }

  if (Desc->BgrOffset != A733_CCU_NO_REG) {
    //
    // Gate only.  Reset stays released: a gated block keeps its registers,
    // and clearing them is a separate request (A733CcuResetModule).
    //
    Value  = MmioRead32 (CcuReg (Desc->BgrOffset));
    Value &= ~A733_CCU_BGR_SUB_CLK (A733_CCU_BGR_MAIN_SUB);
    MmioWrite32 (CcuReg (Desc->BgrOffset), Value);
  }

  return EFI_SUCCESS;
}

/**
  Pulse a module's reset: assert it, hold it, then de-assert it.

  @param[in] Module  Module to reset.

  @retval EFI_SUCCESS            The reset was asserted and released.
  @retval EFI_INVALID_PARAMETER  Module is out of range.
  @retval EFI_UNSUPPORTED        Module has no BGR_REG.
**/
EFI_STATUS
EFIAPI
A733CcuResetModule (
  IN A733_CCU_MODULE  Module
  )
{
  CONST A733_CCU_MODULE_DESC  *Desc;
  UINT32                      Value;

  Desc = ModuleDesc (Module);
  if (Desc == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Desc->BgrOffset == A733_CCU_NO_REG) {
    return EFI_UNSUPPORTED;
  }

  Value  = MmioRead32 (CcuReg (Desc->BgrOffset));
  Value &= ~A733_CCU_BGR_SUB_RST (A733_CCU_BGR_MAIN_SUB);
  MmioWrite32 (CcuReg (Desc->BgrOffset), Value);

  MicroSecondDelay (A733_CCU_RESET_PULSE_US);

  Value |= A733_CCU_BGR_SUB_RST (A733_CCU_BGR_MAIN_SUB);
  MmioWrite32 (CcuReg (Desc->BgrOffset), Value);

  return EFI_SUCCESS;
}

/**
  Set a module's clock parent and both dividers.

  The parent is named by frequency class; this module's own table decides which
  selector value realizes it, and the field is written at the module's own
  width (GIC's selector is 2 bits, every other module's is 3), so no caller has
  to know the module's encoding.  Manual: the module clock is
  "Clock Source / M / N", so FACTOR_M (4:0) and FACTOR_N (12:8) are both
  written, each as divider - 1.

  @param[in] Module  Module to program.
  @param[in] Source  Clock parent, see A733_CCU_SRC.
  @param[in] DivM    Divider M, 1..32 (FACTOR_M is 5 bits).
  @param[in] DivN    Divider N, 1..32 (FACTOR_N is 5 bits).

  @retval EFI_SUCCESS            The clock source was written.
  @retval EFI_INVALID_PARAMETER  Module, Source, DivM or DivN is out of range.
  @retval EFI_UNSUPPORTED        The module has no CLK_REG or no parent
                                 selector, or its table has no entry for
                                 Source.
**/
EFI_STATUS
EFIAPI
A733CcuSetModuleClockDivider (
  IN A733_CCU_MODULE  Module,
  IN A733_CCU_SRC     Source,
  IN UINT32           DivM,
  IN UINT32           DivN
  )
{
  CONST A733_CCU_MODULE_DESC  *Desc;
  CONST A733_CCU_SRC_MAP      *Map;
  UINT32                      Mask;
  UINT32                      Value;

  if ((Module >= A733_CCU_MODULE_MAX) || (Source >= A733_CCU_SRC_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((DivM < A733_CCU_CLK_FACTOR_MIN) || (DivM > A733_CCU_CLK_FACTOR_MAX) ||
      (DivN < A733_CCU_CLK_FACTOR_MIN) || (DivN > A733_CCU_CLK_FACTOR_MAX))
  {
    return EFI_INVALID_PARAMETER;
  }

  Desc = &mModules[Module];
  if ((Desc->ClkOffset == A733_CCU_NO_REG) || (Desc->SrcWidth == 0)) {
    //
    // UART0/UART1 are clocked by the shared APB_UART gate (0x0538) and have
    // no per-module parent or divider to write; DMA0 has no clock divider at
    // all, only the BGR gate.  The other table-less modules (USB0, GMAC0) are
    // refused here too: this port programs a clock through a selector it has
    // modelled, and none is modelled for them.
    //
    DEBUG ((
      DEBUG_ERROR,
      "A733Ccu: %a has no clock parent selector to set\n",
      Desc->Name
      ));
    return EFI_UNSUPPORTED;
  }

  Map = FindSourceByClass (Desc, Source);
  if (Map == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Ccu: %a cannot select %a\n",
      Desc->Name,
      CcuSourceName (Source)
      ));
    return EFI_UNSUPPORTED;
  }

  //
  // One read-modify-write of the CLK_REG: clear the selector and both factor
  // fields, put back the selector, then M, then N, and write once.  The gate
  // and every field this library does not model are read back and kept.  The
  // selector is still written before the factors, exactly as the M-only
  // version did, so a call that only sets M and N = 1 leaves the register in
  // the shape it always did.
  //
  Mask   = ((1U << Desc->SrcWidth) - 1U) << Desc->SrcShift;
  Value  = MmioRead32 (CcuReg (Desc->ClkOffset));
  Value &= ~(Mask | A733_CCU_CLK_FACTOR_N_MASK | A733_CCU_CLK_FACTOR_M_MASK);
  Value |= ((UINT32)Map->Sel << Desc->SrcShift) & Mask;
  Value |= DivM - 1;
  Value |= (DivN - 1) << A733_CCU_CLK_FACTOR_N_SHIFT;
  MmioWrite32 (CcuReg (Desc->ClkOffset), Value);

  return EFI_SUCCESS;
}

/**
  Set a module's clock parent and its M divider.

  The M-only wrapper: it is A733CcuSetModuleClockDivider with DivN = 1, so the
  module clock stays parent / M and FACTOR_N is written to 0 (divider 1)
  explicitly rather than left as whatever residue the register held.

  @param[in] Module  Module to program.
  @param[in] Source  Clock parent, see A733_CCU_SRC.
  @param[in] DivM    Divider, M = DivM, 1..32 (FACTOR_M is 5 bits).

  @retval EFI_SUCCESS            The clock source was written.
  @retval EFI_INVALID_PARAMETER  Module, Source or DivM is out of range.
  @retval EFI_UNSUPPORTED        The module has no parent selector, or its
                                 table has no entry for Source.
**/
EFI_STATUS
EFIAPI
A733CcuSetModuleClock (
  IN A733_CCU_MODULE  Module,
  IN A733_CCU_SRC     Source,
  IN UINT32           DivM
  )
{
  return A733CcuSetModuleClockDivider (Module, Source, DivM, A733_CCU_CLK_FACTOR_MIN);
}

/**
  Report the frequency a module's clock input runs at.

  The raw selector is decoded through the module's own table, so the same
  frequency class comes back whatever value that module stores it as.  The
  divider is the manual's "Clock Source / M / N", so both FACTOR_M and FACTOR_N
  are applied.  A class whose rate this port has not verified (every PLL-derived
  class) yields 0 and a debug line rather than a guessed number.

  @param[in] Module  Module to read.

  @return The parent rate divided by M and N, or 0 when the clock is gated off,
          when the module has no parent selector or no CLK_REG (UART0/UART1:
          the shared APB_UART gate; DMA0: not present), when the selector holds
          a value the module's table does not list, or when the selected class's
          rate is unknown.  A UINT64 result cannot carry EFI_UNSUPPORTED -
          A733CcuSetModuleClock is where the caller gets that status.
**/
UINT64
EFIAPI
A733CcuGetModuleClockHz (
  IN A733_CCU_MODULE  Module
  )
{
  CONST A733_CCU_MODULE_DESC  *Desc;
  CONST A733_CCU_SRC_MAP      *Map;
  UINT32                      Value;
  UINT32                      Sel;
  UINT64                      ParentHz;
  UINT32                      DivM;
  UINT32                      DivN;

  Desc = ModuleDesc (Module);
  if ((Desc == NULL) || (Desc->ClkOffset == A733_CCU_NO_REG) || (Desc->SrcWidth == 0)) {
    return 0;
  }

  Value = MmioRead32 (CcuReg (Desc->ClkOffset));
  if ((Value & A733_CCU_CLK_GATING) == 0) {
    return 0;
  }

  Sel = BitFieldRead32 (Value, Desc->SrcShift, Desc->SrcShift + Desc->SrcWidth - 1);
  Map = FindSourceByValue (Desc, Sel);
  if (Map == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Ccu: %a selector %u is not in its table\n",
      Desc->Name,
      Sel
      ));
    return 0;
  }

  ParentHz = CcuSourceHz (Map->Class);
  if (ParentHz == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Ccu: %a source %a has no verified rate (rate unknown), reporting 0\n",
      Desc->Name,
      CcuSourceName (Map->Class)
      ));
    return 0;
  }

  //
  // Both factors are "divider = field + 1", so an all-zero field reads back as
  // divider 1 - which is how the hardware treats it - and the + 1 also keeps a
  // zero field from dividing by zero.
  //
  DivM = BitFieldRead32 (
           Value,
           A733_CCU_CLK_FACTOR_M_SHIFT,
           A733_CCU_CLK_FACTOR_M_SHIFT + A733_CCU_CLK_FACTOR_M_WIDTH - 1
           ) + 1;
  DivN = BitFieldRead32 (
           Value,
           A733_CCU_CLK_FACTOR_N_SHIFT,
           A733_CCU_CLK_FACTOR_N_SHIFT + A733_CCU_CLK_FACTOR_N_WIDTH - 1
           ) + 1;

  return ParentHz / ((UINT64)DivM * DivN);
}

/**
  Report a PLL's rate.

  @param[in] Pll  PLL to read.

  @return The rate in Hz, or 0 when Pll is out of range.
**/
UINT64
EFIAPI
A733CcuGetPllHz (
  IN A733_CCU_PLL  Pll
  )
{
  if (Pll >= A733_CCU_PLL_MAX) {
    return 0;
  }

  if (Pll >= A733_CCU_PLL_CPU_BACK) {
    return CpuPllVcoHz (mCpuPllOffset[Pll - A733_CCU_PLL_CPU_BACK]);
  }

  return PeripheralPllEstimateHz (mCcuPllOffset[Pll]);
}

/**
  Rate of a CPU clock mux output.

  Manual: CPU PLL chapter, CTRL + 0x1C.  Only the DSU PLL parent is a
  verified encoding, so an unnamed parent yields 0 rather than a wrong rate.

  @param[in] MuxName  Register name, for the debug message.
  @param[in] Offset   Mux register offset inside the CPU PLL block.

  @return The rate in Hz, or 0 when the mux selects an unnamed parent.
**/
STATIC
UINT64
CpuMuxHz (
  IN CONST CHAR8  *MuxName,
  IN UINT16       Offset
  )
{
  UINT32  Value;
  UINT32  Sel;
  UINT32  P;

  Value = MmioRead32 (CpuPllReg (Offset));
  Sel   = BitFieldRead32 (Value, A733_CPU_CLK_SEL_SHIFT, A733_CPU_CLK_SEL_SHIFT + A733_CPU_CLK_SEL_WIDTH - 1);
  P     = 1U << BitFieldRead32 (Value, A733_CPU_CLK_P_SHIFT, A733_CPU_CLK_P_SHIFT + A733_CPU_CLK_P_WIDTH - 1);

  if (Sel != A733_CPU_CLK_SEL_DSU_PLL) {
    DEBUG ((DEBUG_ERROR, "A733Ccu: %a selects unnamed CPU parent %u\n", MuxName, Sel));
    return 0;
  }

  return CpuPllVcoHz (A733_CPU_PLL_DSU_REG) / P;
}

/**
  Report the core clock (core 0..5, the CPU_L clock mux).

  @return The core rate in Hz, or 0 when the mux selects an unnamed parent.
**/
UINT64
EFIAPI
A733CcuGetCpuHz (
  VOID
  )
{
  return CpuMuxHz ("CPU_L_CLK", A733_CPU_PLL_L_REG + A733_CPU_CLK_SEL_OFFSET);
}

/**
  Report the DSU clock (CPU_DSU clock mux).

  @return The DSU rate in Hz, or 0 when the mux selects an unnamed parent.
**/
UINT64
EFIAPI
A733CcuGetDsuHz (
  VOID
  )
{
  return CpuMuxHz ("CPU_DSU_CLK", A733_CPU_PLL_DSU_REG + A733_CPU_CLK_SEL_OFFSET);
}

/**
  Print the clock tree through DebugLib, one line per node.

  The CPU and CCU PLL lines print the raw register so a line's arithmetic can
  be checked against the silicon; peripheral PLL rates are marked unverified
  because the manual's field positions could not be confirmed.
**/
VOID
EFIAPI
A733CcuDumpTree (
  VOID
  )
{
  UINTN   Index;
  UINT16  ClkOffset;
  UINT16  BgrOffset;
  UINT32  ClkValue;
  UINT32  BgrValue;
  UINT32  Value;
  UINT64  Hz;
  UINT32  PllOffset;

  CONST A733_CCU_MODULE_DESC  *Desc;
  CONST A733_CCU_SRC_MAP      *Map;
  CONST CHAR8                 *SrcName;
  CONST CHAR8                 *Reason;
  UINT32                      DivM;
  UINT32                      DivN;

  DEBUG ((
    DEBUG_INFO,
    "A733Ccu: CCU base 0x%08x (offsets below are relative to it), CPU PLL base 0x%08x\n",
    (UINT32)PcdGet64 (PcdA733CcuBase),
    (UINT32)PcdGet64 (PcdA733CpuPllBase)
    ));

  DEBUG ((
    DEBUG_INFO,
    "A733Ccu: window 0x%x, module offset 0xffff means the module has no such register\n",
    A733_CCU_WINDOW_SIZE
    ));

  for (Index = 0; Index < A733_CCU_MODULE_MAX; Index++) {
    Desc      = &mModules[Index];
    ClkOffset = Desc->ClkOffset;
    BgrOffset = Desc->BgrOffset;

    //
    // A module with no register of that kind is shown as offset 0xffff with a
    // zero reading, so a real "reads back as 0" register cannot be confused
    // with an absent one.
    //
    ClkValue = (ClkOffset == A733_CCU_NO_REG) ? 0 : MmioRead32 (CcuReg (ClkOffset));
    BgrValue = (BgrOffset == A733_CCU_NO_REG) ? 0 : MmioRead32 (CcuReg (BgrOffset));

    //
    // Decode the selector through this module's own table and name the reason
    // whenever the rate is 0: no register, a closed gate, no selector at all,
    // a value the table does not list, and an unverified class are five
    // different things that a bare "0 kHz" would flatten into one.
    //
    SrcName = "-";
    Reason  = "";
    DivM    = 0;
    DivN    = 0;

    if (ClkOffset == A733_CCU_NO_REG) {
      Reason = "no CLK_REG";
    } else {
      DivM = BitFieldRead32 (
               ClkValue,
               A733_CCU_CLK_FACTOR_M_SHIFT,
               A733_CCU_CLK_FACTOR_M_SHIFT + A733_CCU_CLK_FACTOR_M_WIDTH - 1
               ) + 1;
      DivN = BitFieldRead32 (
               ClkValue,
               A733_CCU_CLK_FACTOR_N_SHIFT,
               A733_CCU_CLK_FACTOR_N_SHIFT + A733_CCU_CLK_FACTOR_N_WIDTH - 1
               ) + 1;

      if (Desc->SrcWidth == 0) {
        Reason = "no source selector";
      } else if ((ClkValue & A733_CCU_CLK_GATING) == 0) {
        Reason = "gated";
      } else {
        Map     = FindSourceByValue (Desc, BitFieldRead32 (ClkValue, Desc->SrcShift, Desc->SrcShift + Desc->SrcWidth - 1));
        SrcName = (Map == NULL) ? "?" : CcuSourceName (Map->Class);
        if (Map == NULL) {
          Reason = "selector not in table";
        } else if (CcuSourceHz (Map->Class) == 0) {
          Reason = "rate unknown";
        }
      }
    }

    Hz = A733CcuGetModuleClockHz ((A733_CCU_MODULE)Index);

    //
    // The divider label is /M/N for every module that has a CLK_REG, since the
    // manual's module clock is "Clock Source / M / N" and both factors feed the
    // rate.  A module with no CLK_REG has no divider to label.
    //
    if (ClkOffset == A733_CCU_NO_REG) {
      DEBUG ((
        DEBUG_INFO,
        "A733Ccu:   %-8a CLK[%04x]=%08x BGR[%04x]=%08x %-12a %-6a %9u kHz %a\n",
        Desc->Name,
        ClkOffset,
        ClkValue,
        BgrOffset,
        BgrValue,
        SrcName,
        "-",
        (UINT32)(Hz / 1000),
        Reason
        ));
    } else {
      DEBUG ((
        DEBUG_INFO,
        "A733Ccu:   %-8a CLK[%04x]=%08x BGR[%04x]=%08x %-12a /%u/%u  %9u kHz %a\n",
        Desc->Name,
        ClkOffset,
        ClkValue,
        BgrOffset,
        BgrValue,
        SrcName,
        DivM,
        DivN,
        (UINT32)(Hz / 1000),
        Reason
        ));
    }
  }

  for (Index = 0; Index < A733_CCU_PLL_MAX; Index++) {
    if (Index < A733_CCU_PLL_CPU_BACK) {
      PllOffset = mCcuPllOffset[Index];
      Value     = MmioRead32 (CcuReg (mCcuPllOffset[Index]));
      Hz        = A733CcuGetPllHz ((A733_CCU_PLL)Index);
      DEBUG ((
        DEBUG_INFO,
        "A733Ccu:   %-11a [%04x]=%08x P0=%u M1=%u N=%u %9u kHz (manual formula, not measured)\n",
        mPllName[Index],
        PllOffset,
        Value,
        BitFieldRead32 (Value, A733_CCU_PLL_P0_SHIFT, A733_CCU_PLL_P0_SHIFT + A733_CCU_PLL_P0_WIDTH - 1) + 1,
        BitFieldRead32 (Value, A733_CCU_PLL_M1_SHIFT, A733_CCU_PLL_M1_SHIFT + A733_CCU_PLL_M1_WIDTH - 1) + 1,
        BitFieldRead32 (Value, A733_CCU_PLL_N_SHIFT, A733_CCU_PLL_N_SHIFT + A733_CCU_PLL_N_WIDTH - 1) + 1,
        (UINT32)(Hz / 1000)
        ));
    } else {
      PllOffset = mCpuPllOffset[Index - A733_CCU_PLL_CPU_BACK];
      Value     = MmioRead32 (CpuPllReg ((UINT16)PllOffset));
      Hz        = A733CcuGetPllHz ((A733_CCU_PLL)Index);
      DEBUG ((
        DEBUG_INFO,
        "A733Ccu:   %-11a [%04x]=%08x EN=%u N=%u M0=%u P=%u %9u kHz (VCO)\n",
        mPllName[Index],
        PllOffset,
        Value,
        (Value & A733_CPU_PLL_EN) >> 31,
        BitFieldRead32 (Value, A733_CPU_PLL_N_SHIFT, A733_CPU_PLL_N_SHIFT + A733_CPU_PLL_N_WIDTH - 1),
        BitFieldRead32 (Value, A733_CPU_PLL_M0_SHIFT, A733_CPU_PLL_M0_SHIFT + A733_CPU_PLL_M0_WIDTH - 1) + 1,
        BitFieldRead32 (Value, A733_CPU_PLL_P_SHIFT, A733_CPU_PLL_P_SHIFT + A733_CPU_PLL_P_WIDTH - 1) + 1,
        (UINT32)(Hz / 1000)
        ));
    }
  }

  //
  // The DSU mux carries three more dividers (M, M1, N) that feed the AXI,
  // GIC and APB clocks.  All three read 2 in the port's known-good value
  // 0x03010105, so they are printed but not attributed to an output.
  //
  Value = MmioRead32 (CpuPllReg (A733_CPU_PLL_L_REG + A733_CPU_CLK_SEL_OFFSET));
  DEBUG ((
    DEBUG_INFO,
    "A733Ccu:   %-11a [%04x]=%08x SEL=%u P=%u %9u kHz (core 0-5)\n",
    "CPU_L_CLK",
    A733_CPU_PLL_L_REG + A733_CPU_CLK_SEL_OFFSET,
    Value,
    BitFieldRead32 (Value, A733_CPU_CLK_SEL_SHIFT, A733_CPU_CLK_SEL_SHIFT + A733_CPU_CLK_SEL_WIDTH - 1),
    1U << BitFieldRead32 (Value, A733_CPU_CLK_P_SHIFT, A733_CPU_CLK_P_SHIFT + A733_CPU_CLK_P_WIDTH - 1),
    (UINT32)(A733CcuGetCpuHz () / 1000)
    ));

  Value = MmioRead32 (CpuPllReg (A733_CPU_PLL_DSU_REG + A733_CPU_CLK_SEL_OFFSET));
  DEBUG ((
    DEBUG_INFO,
    "A733Ccu:   %-11a [%04x]=%08x SEL=%u P=%u M=%u M1=%u N=%u %9u kHz (DSU)\n",
    "CPU_DSU_CLK",
    A733_CPU_PLL_DSU_REG + A733_CPU_CLK_SEL_OFFSET,
    Value,
    BitFieldRead32 (Value, A733_CPU_CLK_SEL_SHIFT, A733_CPU_CLK_SEL_SHIFT + A733_CPU_CLK_SEL_WIDTH - 1),
    1U << BitFieldRead32 (Value, A733_CPU_CLK_P_SHIFT, A733_CPU_CLK_P_SHIFT + A733_CPU_CLK_P_WIDTH - 1),
    BitFieldRead32 (Value, A733_CPU_CLK_M_SHIFT, A733_CPU_CLK_M_SHIFT + A733_CPU_CLK_M_WIDTH - 1) + 1,
    BitFieldRead32 (Value, A733_CPU_CLK_M1_SHIFT, A733_CPU_CLK_M1_SHIFT + A733_CPU_CLK_M1_WIDTH - 1) + 1,
    BitFieldRead32 (Value, A733_CPU_CLK_N_SHIFT, A733_CPU_CLK_N_SHIFT + A733_CPU_CLK_N_WIDTH - 1) + 1,
    (UINT32)(A733CcuGetDsuHz () / 1000)
    ));
}