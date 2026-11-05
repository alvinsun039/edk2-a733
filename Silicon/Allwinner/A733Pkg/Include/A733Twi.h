/** @file
  TWI (two-wire / I2C) register map for the Allwinner A733 (sun60iw2p1).

  Every offset and field below is from the A733 user manual's TWI chapter:
  the register lists in 18.11.7 (TWI0) and 18.11.8 (S_TWI0, "the same with
  TWI0", plus the PMU-offload registers up to TWI_RR8_CTL at +0x0380), and
  the register descriptions in 18.11.9.  The controller is the Marvell-derived
  engine the vendor's mvtwsi.c drives, and the vendor's own driver agrees with
  the manual field for field (control bit 3 is a write-1-to-clear interrupt
  flag on sun6i and newer, which this part is).

  The S_TWI controllers live in the always-on CPUS domain.  S_TWI0 owns 4 KB
  at 0x07083000; S_TWI1 and S_TWI2 follow at +0x1000 each with the same
  registers.  The manual's memory map gives the window, and its register list
  ends at +0x0380, so the 4 KB span covers the whole block.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_TWI_H__
#define A733_TWI_H__

#include <Uefi.h>

//
// Manual: memory map, CPUS subsystem (0x07083000---0x07083FFF, 4K).  Exposed
// for the platform's MMU map so a mistyped size cannot leave the block
// unmapped.
//
#define A733_TWI0_BASE        0x07083000UL
#define A733_TWI1_BASE        0x07084000UL

// R_PIO selector for each instance's pin pair (vendor pinctrl table).
#define A733_TWI_S1_FUNC      3U
#define A733_TWI_WINDOW_SIZE  0x1000U

//
// the A733 user manual, register offsets, relative to a controller's base.
//
#define A733_TWI_ADDR   0x000U
#define A733_TWI_XADDR  0x004U
#define A733_TWI_DATA   0x008U
#define A733_TWI_CNTR   0x00CU
#define A733_TWI_STAT   0x010U
#define A733_TWI_CCR    0x014U
#define A733_TWI_SRST   0x018U
#define A733_TWI_EFR    0x01CU
#define A733_TWI_LCR    0x020U

//
// the A733 user manual, TWI_CNTR.  INT_FLAG is write-1-to-clear: after every
// state the controller holds SCL low until a 1 is written to it, so every
// CNTR write that advances the engine sets it.
//
#define A733_TWI_CNTR_INT_EN    (1U << 7)
#define A733_TWI_CNTR_BUS_EN    (1U << 6)
#define A733_TWI_CNTR_M_STA     (1U << 5)
#define A733_TWI_CNTR_M_STP     (1U << 4)
#define A733_TWI_CNTR_INT_FLAG  (1U << 3)
#define A733_TWI_CNTR_A_ACK     (1U << 2)
//
// The value a CNTR write adds to release the engine from the state it just
// reported.  Named so the "write 1 to clear" rule is visible at every call.
//
#define A733_TWI_CNTR_CLEAR_IFLG  A733_TWI_CNTR_INT_FLAG

//
// the A733 user manual, TWI_SRST.  Write 1 to reset; the bit self-clears.
//
#define A733_TWI_SRST_SOFT_RST  (1U << 0)

//
// the A733 user manual, TWI_CCR: CLK_DUTY_30_EN bit 8, CLK_DUTY bit 7, CLK_M
// [6:3], CLK_N [2:0].  Foscl = Fin / (2^CLK_N * (CLK_M + 1) * 10).
//
#define A733_TWI_CCR_CLK_M_SHIFT  3U
#define A733_TWI_CCR_CLK_M_MASK   0x78U
#define A733_TWI_CCR_CLK_N_SHIFT  0U
#define A733_TWI_CCR_CLK_N_MASK   0x07U

//
// the A733 user manual, TWI_STAT.  Every code is a multiple of 8 and only bits
// [7:3] carry it, so a read is masked to those bits.
//
#define A733_TWI_STAT_MASK         0xF8U
#define A733_TWI_STAT_IDLE         0xF8U
#define A733_TWI_STAT_START        0x08U
#define A733_TWI_STAT_RESTART      0x10U
#define A733_TWI_STAT_ADDR_W_ACK   0x18U
#define A733_TWI_STAT_ADDR_W_NACK  0x20U
#define A733_TWI_STAT_DATA_W_ACK   0x28U
#define A733_TWI_STAT_DATA_W_NACK  0x30U
#define A733_TWI_STAT_ADDR_R_ACK   0x40U
#define A733_TWI_STAT_ADDR_R_NACK  0x48U
#define A733_TWI_STAT_DATA_R_ACK   0x50U
#define A733_TWI_STAT_DATA_R_NACK  0x58U

//
// Bound for one state's poll.  A byte at 100 kHz is 9 SCL periods = 90 us;
// this is two orders of magnitude of headroom and still returns to BDS
// promptly when the bus (or the block's clock gate) is dead.
//
#define A733_TWI_TIMEOUT_US  10000U

#endif