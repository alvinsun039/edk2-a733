/** @file
  Memory map for the Allwinner A733 (sun60iw2p1) as used by this platform.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_H__
#define A733_H__

// UART0: the board console (PB9/PB10).  The port itself is driven by
// MdeModulePkg's BaseSerialPortLib16550, which takes its base, clock, baud rate,
// register stride and access width from the platform's PCDs; this is only the
// map entry that makes the page device memory.
#define A733_UART0_BASE  0x02500000UL

#endif