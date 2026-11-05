/** @file
 *
 *  DSDT — Allwinner A733 (Radxa Cubie A7A)
 *
 *  Only what ACPI can actually describe on this SoC is here.  The OS this
 *  board ships with boots from the device tree, and most Allwinner blocks
 *  (SMHC, GMAC, display, PMIC) have no ACPI binding in the vendor kernel,
 *  so a full device list would be fiction.  What ACPI does need is the
 *  console: UART0, the same port SPCR and DBG2 point at.
 *
 *  MADT carries the CPUs and the GIC, GTDT the timers, FADT the PSCI
 *  conduit, PPTT the topology - no AML is required for any of those.
 *
 *  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
 *  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

DefinitionBlock (
  "DsdtCommon.aml",
  "DSDT",
  0x02,                                   // AML revision 2 (ACPI 2.0+)
  "A733",                                 // OEM ID (matches AcpiTables.h)
  "A733DSDT",
  0x00000000
  )
{
  Scope (\_SB_)
  {
    //
    // UART0: Synopsys DW APB UART at 0x02500000, registers one 32-bit
    // word apart.  board.dts wires it to GIC SPI 2, i.e. GSIV 34
    // (32 + 2), level-triggered active-high.  APMC0D08 is the ID Linux's
    // 8250_dw driver claims; it programs the port as UPIO_MEM32.
    //
    Device (URT0)
    {
      Name (_HID, "APMC0D08")     // Linux 8250_dw
      Name (_CID, "PNP0501")      // Windows' 16550 driver
      Name (_UID, Zero)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0x02500000, 0x00000400)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 34 }
      })
    }

    //
    // xHCI: the DWC3 host controller at 0x06A00000, GIC SPI 155 (GSIV
    // 187), level-high.  PNP0D10 is the ACPI ID Windows' USBXHCI claims;
    // it gives an ACPI-only OS a USB stack, and with it mass storage and
    // input - the only storage this board can offer a system that has no
    // Allwinner SMHC driver.  Linux reaches the same controller through
    // the device tree and matches no _HID here.
    //
    Device (XHC0)
    {
      Name (_HID, "PNP0D10")
      Name (_UID, Zero)
      //
      // The DWC3's AHB master does not snoop the CPU caches - the same
      // reason A733UsbHostDxe and A733Xhci2Dxe register their MMIO
      // windows as NonCoherent.  Without _CCA the OS assumes coherent
      // DMA and skips the cache maintenance, which corrupts transfers.
      //
      Name (_CCA, Zero)
      Name (_CRS, ResourceTemplate ()
      {
        Memory32Fixed (ReadWrite, 0x06A00000, 0x00100000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 187 }
      })
    }
  }
}