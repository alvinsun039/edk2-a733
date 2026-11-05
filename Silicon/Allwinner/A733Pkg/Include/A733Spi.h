/** @file
  SPI0 register map for the Allwinner A733 (sun60iw2p1).

  Every offset and field below is from the A733 user manual's SPI chapter
  (18.8.7 register list, 18.8.8 register descriptions).  The manual states that
  SPI2, SPI3 and S_SPI0 have the same registers as SPI0, and the memory map
  gives SPI0 the 0x02540000..0x02540FFF window.  The bit fields the driver
  programs are the manual's; where a field's *value* had to be found on
  hardware rather than read off the manual, the driver says so next to it.

  Three things are spelled out here rather than left to the reader, because a
  driver written against a generic SPI controller would guess them wrong:

  - The chip select has an automatic mode and a software mode.  In software
    mode (SPI_TCR.SS_OWNER set) the pin follows SPI_TCR.SS_LEVEL and nothing
    else drives it, which is what makes a command's chip-select window two
    register writes wide instead of one burst wide.

  - A burst's length goes in three registers before SPI_TCR.XCH starts it:
    MBC (total bursts), BCC.STC (single-mode transmit bursts) and MTC.MWTC
    (bursts taken from the FIFO before the controller inserts dummy bursts).
    There is no dummy burst unless BCC.DBC says so.

  - One command is two bursts.  A transmit burst sets MBC, MTC and BCC.STC to
    the length; a receive burst sets MBC only, leaving MTC and BCC.STC at 0 so
    the controller has nothing of its own to send and clocks MISO in instead.
    Both live inside one software chip-select window.  SPI_TCR.DHB discards
    the burst rather than handing it to the hash engine.

  - The clock is CCU SPI0_CLK_REG's source / M / N, with both factors stored as
    value + 1 (FACTOR_M at 4:0, FACTOR_N at 12:8), so a factor field of 0 is a
    divider of 1.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_SPI_H__
#define A733_SPI_H__

#include <Uefi.h>

//
// Manual: memory map, SPI0 window (0x02540000..0x02540FFF).
//
#define A733_SPI0_BASE         0x02540000UL
#define A733_SPI0_WINDOW_SIZE  SIZE_4KB

//
// Manual: SPI chapter, 18.8.7 register list.  Offsets are relative to the
// controller's base.  SPI_MBC is documented separately (18.8.8.16) from the
// list that names SPI_MTC and SPI_BCC, which is why it reads out of order
// here.
//
#define A733_SPI_GCR   0x0004U  // Global control
#define A733_SPI_TCR   0x0008U  // Transfer control
#define A733_SPI_IER   0x0010U  // Interrupt enable
#define A733_SPI_ISR   0x0014U  // Interrupt status
#define A733_SPI_FCR   0x0018U  // FIFO control
#define A733_SPI_CCR   0x0024U  // Clock control: the SPI clock's own divider
#define A733_SPI_SAMP_DL 0x0028U // Sample delay
#define A733_SPI_FSR   0x001CU  // FIFO status
#define A733_SPI_WCR   0x0020U  // Wait clock counter
#define A733_SPI_MBC   0x0030U  // Master burst counter
#define A733_SPI_MTC   0x0034U  // Master transmit counter
#define A733_SPI_BCC   0x0038U  // Master burst control counter
#define A733_SPI_TXD   0x0200U  // TX data
#define A733_SPI_RXD   0x0300U  // RX data

//
// Manual: 18.8.8.1 SPI_GCR.
//
#define A733_SPI_GCR_EN        BIT0
#define A733_SPI_GCR_MODE      BIT1   // 1 = master, 0 = slave
#define A733_SPI_GCR_MODE_SEL  BIT2   // 0 = old sample timing (18.8.6.3)
#define A733_SPI_GCR_TP_EN     BIT7
//
// The live values the controller needs for a 25-series part on this board:
// CCR selects the divided clock the controller actually shifts with (2 is the
// value every working driver on this SoC writes), SAMP_DL sets the sample
// delay, and FCR carries the FIFO thresholds plus its two reset bits clear.
// The three are the difference between "the module clock is on" and "the part
// sees a clock".
//
#define A733_SPI_CCR_VALUE     0x00000002U
#define A733_SPI_SAMP_DL_VALUE 0x00005000U
#define A733_SPI_FCR_VALUE     0x00200140U
#define A733_SPI_GCR_SRST      BIT31  // write 1: reset the whole controller

//
// Manual: 18.8.8.2 SPI_TCR.
//
#define A733_SPI_TCR_CPHA      BIT0
#define A733_SPI_TCR_CPOL      BIT1
#define A733_SPI_TCR_SPOL      BIT2   // 1 = chip select active low
#define A733_SPI_TCR_SSCTL     BIT3   // 0 = keep CS asserted between bursts
#define A733_SPI_TCR_SS_SEL    (3U << 4)
#define A733_SPI_TCR_SS_OWNER  BIT6   // 1 = software drives SS_LEVEL
#define A733_SPI_TCR_SS_LEVEL  BIT7   // level used when SS_OWNER is 1
#define A733_SPI_TCR_DHB       BIT8
#define A733_SPI_TCR_DDB       BIT9
#define A733_SPI_TCR_RPSM      BIT10
#define A733_SPI_TCR_SDC       BIT11
#define A733_SPI_TCR_FBS       BIT12  // 0 = MSB first
#define A733_SPI_TCR_SDM       BIT13  // 1 = normal sample, 0 = delay sample
#define A733_SPI_TCR_SDDM      BIT14
#define A733_SPI_TCR_SDC1      BIT15
#define A733_SPI_TCR_XCH       BIT31  // write 1: start the burst

//
// Manual: 18.8.8.4 SPI_ISR.  Every bit here is write-1-to-clear, including the
// FIFO level flags, so the register is read for state and written to clear.
//
#define A733_SPI_ISR_RX_RDY   BIT0
#define A733_SPI_ISR_RX_EMP   BIT1
#define A733_SPI_ISR_RX_FULL  BIT2
#define A733_SPI_ISR_TX_RDY   BIT4
#define A733_SPI_ISR_TX_EMP   BIT5
#define A733_SPI_ISR_TX_FULL  BIT6
#define A733_SPI_ISR_RX_OVF   BIT8
#define A733_SPI_ISR_RX_UDF   BIT9
#define A733_SPI_ISR_TF_OVF   BIT10
#define A733_SPI_ISR_TF_UDF   BIT11
#define A733_SPI_ISR_TC       BIT12
#define A733_SPI_ISR_SSI      BIT13

//
// The four error conditions of 18.8.5.6.  Any of them ends the transfer, so a
// burst that sets one has to be reported rather than waited out.
//
#define A733_SPI_ISR_ERRORS  (A733_SPI_ISR_RX_OVF | A733_SPI_ISR_RX_UDF | \
                              A733_SPI_ISR_TF_OVF | A733_SPI_ISR_TF_UDF)

//
// Manual: 18.8.8.5 SPI_FCR.
//
#define A733_SPI_FCR_RX_TRIG_LEVEL  (0xFFU << 0)
#define A733_SPI_FCR_RF_RST         BIT15
#define A733_SPI_FCR_TX_TRIG_LEVEL  (0xFFU << 16)
#define A733_SPI_FCR_TF_RST         BIT31

//
// Manual: 18.8.8.6 SPI_FSR.  RF_CNT counts the RX FIFO *and* the 128-entry RX
// buffer (0xc0 is the largest value the manual lists); TF_CNT counts the TX
// FIFO.  The FIFOs themselves are 64 entries of 8 bits (18.8.1).
//
#define A733_SPI_FSR_RF_CNT  (0xFFU << 0)
#define A733_SPI_FSR_TF_CNT  (0xFFU << 16)

#endif