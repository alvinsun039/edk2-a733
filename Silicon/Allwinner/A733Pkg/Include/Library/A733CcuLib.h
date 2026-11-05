/** @file
  Clock control unit library for the Allwinner A733 (sun60iw2p1).

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_CCU_LIB_H__
#define A733_CCU_LIB_H__

#include <Uefi.h>

#include <A733Ccu.h>

/**
  Raw 32-bit access to a CCU register.

  The module API below covers the blocks the CCU describes with a CLK_REG and
  a BGR_REG.  A block whose clock registers do not fit that model (the
  display pipeline's PLL_DE, the tcon-top crossing, the HDMI TV divider)
  programs its own offsets; these accessors give it the same base and the
  same register width without a second copy of the base address.

  @param[in] RegOffset  Offset from the CCU base (PcdA733CcuBase).

  @return A733CcuRead returns the register value; the writers return the
          value written for convenience.
**/
UINT32
EFIAPI
A733CcuRead (
  IN UINT32  RegOffset
  );

VOID
EFIAPI
A733CcuWrite (
  IN UINT32  RegOffset,
  IN UINT32  Value
  );

VOID
EFIAPI
A733CcuSetBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  );

VOID
EFIAPI
A733CcuClearBits (
  IN UINT32  RegOffset,
  IN UINT32  Bits
  );

/**
  Give a module a running clock: de-assert its reset and open its gates.

  Touches the BGR_REG sub-block 0 (reset 0 = asserted, clock gate 0 = off)
  and, where the module has one, the CLK_REG gate.

  @param[in] Module  Module to enable.

  @retval EFI_SUCCESS            The module's gates are open.
  @retval EFI_INVALID_PARAMETER  Module is out of range.
  @retval EFI_UNSUPPORTED        Module has neither a CLK_REG nor a BGR_REG.
**/
EFI_STATUS
EFIAPI
A733CcuEnableModule (
  IN A733_CCU_MODULE  Module
  );

/**
  Stop a module's clock.

  Gates off the BGR_REG sub-block 0 clock and the CLK_REG gate.  Reset is
  left de-asserted on purpose: a gated module keeps its register state, and
  a caller that wants it cleared asks for A733CcuResetModule.

  @param[in] Module  Module to disable.

  @retval EFI_SUCCESS            The module's gates are closed.
  @retval EFI_INVALID_PARAMETER  Module is out of range.
  @retval EFI_UNSUPPORTED        Module has neither a CLK_REG nor a BGR_REG.
**/
EFI_STATUS
EFIAPI
A733CcuDisableModule (
  IN A733_CCU_MODULE  Module
  );

/**
  Pulse a module's reset: assert it, hold it, then de-assert it.

  The module must have a BGR_REG, which is where the manual puts reset.

  @param[in] Module  Module to reset.

  @retval EFI_SUCCESS            The reset was asserted and released.
  @retval EFI_INVALID_PARAMETER  Module is out of range.
  @retval EFI_UNSUPPORTED        Module has no BGR_REG.
**/
EFI_STATUS
EFIAPI
A733CcuResetModule (
  IN A733_CCU_MODULE  Module
  );

/**
  Set a module's clock parent and both dividers.

  The parent is named by frequency class; the module's own table maps the
  class to the selector value that module expects and gives the field its
  width (GIC's selector is 2 bits, every other module's is 3).  The manual
  states the module clock is "Clock Source / M / N", so both factors are
  written here in one read-modify-write of the CLK_REG: selector, then
  FACTOR_M = DivM - 1 (4:0) and FACTOR_N = DivN - 1 (12:8).  The gate and
  every other field are kept as they were.

  @param[in] Module  Module to program.
  @param[in] Source  Clock parent, see A733_CCU_SRC.
  @param[in] DivM    Divider M, 1..32 (FACTOR_M is 5 bits).
  @param[in] DivN    Divider N, 1..32 (FACTOR_N is 5 bits).

  @retval EFI_SUCCESS            The clock source was written.
  @retval EFI_INVALID_PARAMETER  Module, Source, DivM or DivN is out of range.
  @retval EFI_UNSUPPORTED        The module has no CLK_REG (or no parent
                                 selector), or its own table does not list
                                 Source.  UART0 and UART1 are clocked by the
                                 shared APB_UART gate and have no per-module
                                 source or divider.
**/
EFI_STATUS
EFIAPI
A733CcuSetModuleClockDivider (
  IN A733_CCU_MODULE  Module,
  IN A733_CCU_SRC     Source,
  IN UINT32           DivM,
  IN UINT32           DivN
  );

/**
  Set a module's clock parent and its M divider.

  The M-only wrapper around A733CcuSetModuleClockDivider: it calls that with
  DivN = 1, so a module's clock is parent / M exactly as before.  FACTOR_N is
  therefore written to 0 (divider 1) explicitly rather than left at whatever
  residue the register held - the N field is no longer preserved across a call.

  @param[in] Module  Module to program.
  @param[in] Source  Clock parent, see A733_CCU_SRC.
  @param[in] DivM    Divider, M = DivM, 1..32 (FACTOR_M is 5 bits).

  @retval EFI_SUCCESS            The clock source was written.
  @retval EFI_INVALID_PARAMETER  Module, Source or DivM is out of range.
  @retval EFI_UNSUPPORTED        The module has no parent selector, or its own
                                 table does not list Source, or it has no
                                 CLK_REG.  See A733CcuSetModuleClockDivider.
**/
EFI_STATUS
EFIAPI
A733CcuSetModuleClock (
  IN A733_CCU_MODULE  Module,
  IN A733_CCU_SRC     Source,
  IN UINT32           DivM
  );

/**
  Report the frequency a module's clock input runs at.

  The raw selector is decoded through the module's own table, so the same
  frequency class comes back whatever value that module stores it as.

  @param[in] Module  Module to read.

  @return The parent rate divided by M and then by N (a factor field of 0 is
          the hardware's divider 1), or 0 when the clock is gated off, when the
          module has no parent selector or no CLK_REG (UART0/UART1: the shared
          APB_UART gate; DMA0: no clock divider at all), when the selector
          holds a value the module's table does not list, or when the selected
          class's rate is unknown (every PLL-derived class, whose rate this port
          could not verify).  A UINT64 result cannot carry
          EFI_UNSUPPORTED - A733CcuSetModuleClock is where the caller gets that
          status.
**/
UINT64
EFIAPI
A733CcuGetModuleClockHz (
  IN A733_CCU_MODULE  Module
  );

/**
  Report a PLL's rate.

  CPU PLLs return the VCO (DCXO * N / M0), which is what the CPU clock mux
  divides.  The CCU's peripheral PLLs return the manual's
  PERI0PLL2X-style formula applied to the fields decoded at 22:20 / 19:16 /
  15:8: the number is derived from the manual, never measured, because no
  PERI0 output could be reconciled against the silicon.

  @param[in] Pll  PLL to read.

  @return The rate in Hz, or 0 when Pll is out of range or its fields are
          all zero.
**/
UINT64
EFIAPI
A733CcuGetPllHz (
  IN A733_CCU_PLL  Pll
  );

/**
  Report the core clock (core 0..5, the CPU_L clock mux).

  @return The core rate in Hz, or 0 when the mux selects a parent the port
          cannot name.
**/
UINT64
EFIAPI
A733CcuGetCpuHz (
  VOID
  );

/**
  Report the DSU clock (CPU_DSU clock mux).

  @return The DSU rate in Hz, or 0 when the mux selects a parent the port
          cannot name.
**/
UINT64
EFIAPI
A733CcuGetDsuHz (
  VOID
  );

/**
  Print the clock tree through DebugLib, one line per node: every module in
  the module table, every PLL, and the CPU/DSU muxes.  A module whose CLK_REG
  exists shows its divider as /M/N.  Peripheral PLL lines print the raw
  register value and show the derived rate as unverified.
**/
VOID
EFIAPI
A733CcuDumpTree (
  VOID
  );

#endif