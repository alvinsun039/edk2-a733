/** @file
  ArmPlatformLib for the Allwinner A733 (sun60iw2p1) on the Radxa Cubie A7A.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/PcdLib.h>

#include <A733.h>
#include <A733Dmac.h>
#include <A733Dwc3.h>
#include <A733Musb.h>
#include <A733Rpio.h>
#include <A733Smhc.h>
#include <A733Spi.h>
#include <A733Twi.h>
#include <A733Usb.h>

//
// Device windows that must be covered by the SEC page tables before any of
// their registers is touched with the MMU on: the console (DebugLib and
// SerialPortLib poke UART0), and the GIC once a driver probes it.  The A733
// puts the GIC distributor at 0x03400000 and the redistributors at
// 0x03460000, so one 8 MiB span covers both.
//
#define A733_GIC_BASE   0x03400000UL
#define A733_GIC_SIZE   SIZE_8MB
// The CCU occupies 0x02002000..0x02005fff per the manual memory map.  Reads
// above +0x2000 come back as a mirror of the first page on this part, which
// is no reason to leave the rest of the window unmapped: a driver touching a
// register the manual places at +0x2xxx would fault instead of reading it.
#define A733_CCMU_BASE  0x02002000UL
#define A733_CCMU_SIZE  (4 * SIZE_4KB)
// The pin controller occupies 0x02000000..0x02001fff per the manual memory map.
// Every base A733GpioLib touches is inside this window: the global power
// registers at +0x40.. and the bank blocks from +0x80 (bank A) to +0x5f0 (bank
// K's secure register), so the 8 KiB span covers the whole library.
#define A733_GPIO_BASE  0x02000000UL
#define A733_GPIO_SIZE  SIZE_8KB
//
// The R_PIO (S_GPIO) domain and S_TWI0 are both in the CPUS window and are
// separate blocks from everything above.
//
// Manual: memory map, CPUS subsystem gives S_GPIO 0x07025000..0x07026FFF
// (8 KB); 18.14.5.2 "S_GPIO Register List" pins its block base at 0x0702_5000
// and lists registers up to PL_SECURE/PM_SECURE at +0x0400, so the 8 KB span
// covers everything A733TwiLib's PL0/PL1 mux touches.  The same map gives
// S_TWI0 0x07083000..0x07083FFF (4 KB); 18.11.8 "S_TWI0 Register List" runs
// from +0x0000 to TWI_RR8_CTL at +0x0380, so the 4 KB span covers the whole
// controller.  Bases and sizes come from the block headers (A733Rpio.h,
// A733Twi.h) so the map and the drivers cannot drift apart.
//
#define A733_RPIO_SIZE    A733_RPIO_WINDOW_SIZE
#define A733_STWI0_BASE   A733_TWI0_BASE
#define A733_STWI0_SIZE   A733_TWI_WINDOW_SIZE
// S_TWI1 (0x07084000) is a block of its own, not part of S_TWI0's: the RTC
// on this board sits behind it.
#define A733_STWI1_BASE   A733_TWI1_BASE
#define A733_STWI1_SIZE   A733_TWI_WINDOW_SIZE
// The CPU PLLs are a separate block: BACK/L/B/DSU at +0x0000/+0x1000/
// +0x2000/+0x3000, plus each PLL's clock-select register at +0x1c.
#define A733_CPUPLL_BASE 0x08870000UL
#define A733_CPUPLL_SIZE (4 * SIZE_4KB)
//
// USB0, the OTG port's window at its manual address.  It holds the MUSB core at
// +0x0000 and then the same four blocks USB1 has, one 0x1000 further in: EHCI0
// at +0x1000, OHCI0 at +0x1400 and the PHY's HCI registers ("pmu0") at +0x1800.
// 64 KB covers all of them, which is what lets A733UsbHostDxe survey EHCI0 and
// the MUSB core without a map of its own; the base comes from the USB header so
// the map and the driver cannot drift apart.
//
#define A733_USB0_SIZE  SIZE_64KB
//
// The system SRAM controller.  Its word at +0x04 is where the USB2.0 DRD's FIFO
// SRAM is handed to the device controller (A733MusbDeviceDxe writes bit 0 of
// it); the register is not in the manual, so only the one 4 KB page that holds
// it is mapped - nothing else in the SRAM controller is touched.
//
#define A733_SRAMC_MAP_BASE  A733_SRAMC_BASE
#define A733_SRAMC_MAP_SIZE  A733_SRAMC_WINDOW_MMIO
//
// The USB2.0 host port is the USB1 window one megabyte up (manual memory map,
// "HIGH-SPEED INTERFACE": USB1 0x04200000---0x042FFFFF).  Its EHCI controller,
// OHCI companion and the PHY's own registers are within the first 4 KB of it,
// so that is all A733UsbHostDxe needs mapped; base and size come from the USB
// header so the map and the driver cannot drift apart.
//
#define A733_USB_HOST_BASE  A733_USB1_MMIO_BASE
#define A733_USB_HOST_SIZE  A733_USB1_MMIO_SIZE
//
// PCK600, the power-domain controller in the CPUS window (manual memory map:
// PPU 0x07060000---0x07078FFF).  A733UsbHostDxe reads the USB2 domain's status
// and switches it on if it is off, and A733MusbDeviceDxe does the same before it
// touches any USB0 register, so the first 128 KB of the block is mapped.
//
// The block's own bus gate is CLK_R_PPU in the always-on R_CCU, and a driver
// that opens that gate has to read the register back to log it.  R_CCU's window
// (0x07010000) is therefore mapped too, one page: leaving it out is what made
// the step-0 read of 0x070101AC a translation fault that took the boot down
// before anything else ran, because an unmapped MMIO read is a page fault and
// not a device that stays quiet.  Base and size come from the USB header so the
// map and the driver cannot drift apart; R_CCU's documented window is 0x400 and
// the gate this platform reads is at +0x1ac, both inside the first page.
//
#define A733_PPU_MAP_BASE    A733_PPU_BASE
#define A733_PPU_MAP_SIZE    A733_PPU_WINDOW_SIZE
#define A733_R_CCU_MAP_BASE  A733_R_CCU_BASE
#define A733_R_CCU_MAP_SIZE  ALIGN_VALUE (A733_R_CCU_SIZE, SIZE_4KB)
//
// The USB2 PHY's own register block at 0x06B00000, the analog half USB0's
// device and host sides share (A733Usb.h).  A733MusbDeviceDxe powers port 0 up
// through it, so the page it lives in is mapped the same way: this is a separate
// window from the HCI registers at 0x04101800 and from every other USB block, and
// a driver reaching it through an unprogrammed entry faults exactly like the
// R_CCU read above did.
//
#define A733_USB2_PHY_MAP_BASE  A733_USB2_PHY_BASE
#define A733_USB2_PHY_MAP_SIZE  ALIGN_VALUE (A733_USB2_PHY_WINDOW_SIZE, SIZE_4KB)
//
// The SD/MMC controllers: SMHC0..SMHC3 at 0x04020000/0x4021000/0x4022000/
// 0x4023000, one 4 KB block each and contiguous, so one 64 KB span covers every
// controller and whichever one a driver brings up finds its registers mapped.
// The board's eMMC is SMHC2 (A733SmhcDxe); SMHC0 is the SD slot.  Base and
// size come from the SMHC driver's own header, so the map and the driver
// cannot drift apart.
#define A733_SMHC_BASE  A733_SMHC_WINDOW_BASE
#define A733_SMHC_SIZE  A733_SMHC_WINDOW_SIZE
//
// SPI0 is the NOR the variable store lives on.  A733FvbDxe reprograms the
// controller after ExitBootServices, so its registers have to be mapped here
// and described to the OS as runtime device memory; base and size come from
// the controller's own header so the map and the driver cannot drift apart.
//
#define A733_SPI0_SIZE  A733_SPI0_WINDOW_SIZE
//
// The system DMA controller, DMAC0 at 0x04601000 (manual 13.1.6 "Register List":
// 0x04601000---0x04602FFF, 8 KB).  A733DmacLib programs a channel's descriptor
// from here and the descriptor's addresses point at any DRAM buffer, so the page
// tables have to cover this block before the first channel is touched; an
// unmapped MMIO read is a page fault and not a device that stays quiet.
//
// DMAC1 at 0x04024000 needs no entry of its own: it lies inside the SMHC window
// mapped below, so the library's init read of that block's security word lands
// on device memory already.
//
#define A733_DMAC_MAP_BASE  A733_DMAC0_BASE
#define A733_DMAC_MAP_SIZE  A733_DMAC_WINDOW_SIZE
//
// The USB3.1 GEN2 DRD (the DWC3 core) and the SerDes subsystem it sits behind,
// for A733Xhci2Dxe.  Both windows are separate blocks from every USB window
// mapped above, and both are touched with the MMU on, so both have to be here
// before the first read:
//
// - 0x06A00000, the whole megabyte the manual gives the controller (18.4.4.1).
//   Its xHCI registers are at the bottom and the DWC3 globals at +0xC100, and
//   the driver reads them both; a half-mapped window would fault on the half
//   that was left out.
// - 0x06C00000, the SerDes subsystem (manual 18.2.5).  The driver writes
//   SUBSYS_USB3P1_BGR at +0x0008, the first 64 KB covers it and the COMB0 link
//   registers at +0x1000.. and the PIPE lane map at +0x6100 that the SuperSpeed
//   bring-up needs, so the entry is sized for the block's documented register
//   list rather than for today's one write.
// - 0x06C80000, COMBPHY0's register file (manual memory map, "COMBPHY0
//   0x06C80000---0x06C9FFFF").  A separate block from the SerDes subsystem, not
//   a part of it: this is the Cadence PMA's own 16-bit register file, and the
//   SuperSpeed bring-up both writes its lane/PLL configuration and polls it for
//   the PMA's ready bit.  The window is 128 KB, which is what the manual's range
//   and the board's device tree (combo-phy0's second reg entry, length 0x20000)
//   both give.
// - 0x07090000, the RTC block, one page: the 24 MHz reference gate for COMB0's
//   PLLs is at +0x16C.  Another block again - not R_CCU at 0x07010000, whose
//   page holds the PPU gate - and an unmapped MMIO write is a page fault, not a
//   device that stays quiet.
//
// The USB3.1 application register window at 0x06B00000 needs no entry of its
// own: it is DWC3 + 0x100000, and it is already mapped as A733_USB2_PHY_MAP_*
// above.
//
#define A733_DWC3_MAP_BASE     A733_DWC3_BASE
#define A733_DWC3_MAP_SIZE     A733_DWC3_WINDOW_SIZE
#define A733_SERDES_MAP_BASE   A733_SERDES_BASE
#define A733_SERDES_MAP_SIZE   A733_SERDES_WINDOW_SIZE
#define A733_COMBPHY0_MAP_BASE A733_COMBPHY0_BASE
#define A733_COMBPHY0_MAP_SIZE A733_COMBPHY0_WINDOW_SIZE
#define A733_RTC_MAP_BASE      A733_RTC_CCU_BASE
#define A733_RTC_MAP_SIZE      A733_RTC_CCU_WINDOW_SIZE
//
// Display.  A733HdmiDxe brings the whole pipeline up from cold inside EDK2
// (DE3.0 mixer -> tcon-top -> DWC-HDMI + SNPS PHY -> TCON3), so every block
// it programs has to be device-mapped before the first read.  They are five
// separate windows, not one, and the gaps between them belong to other
// subsystems, so each gets its own entry:
//
// - 0x05000000, the display engine.  The driver's highest register is the
//   disp0 blender at +0x281000 (A733HdmiDxe.h: DE_DISP0_BLD_BASE), and the
//   block's documented window runs to 0x053FFFFF; 4 MB covers both.
// - 0x05500000, tcon-top.  vo0 is the page at +0x00000 and vo1 at +0x10000;
//   the driver sets the TVE clock source and the HDMI clock gates in both,
//   so the entry spans the two pages (128 KB keeps them one region).
// - 0x05520000, the DWC-HDMI core.  Its registers are byte-addressed and run
//   from +0x0000 to the DDC I2CM block at +0x7E00, so 128 KB covers the
//   whole map with the block's own alignment.
// - 0x05600000, the top-PHY (SNPS PHY configuration at +0x3000).
// - 0x05730000, TCON3 (tcon-tv): its register list ends at +0x340.
//
// An unmapped MMIO access is a translation fault that stops the boot, not a
// device that stays quiet - the same reason the R_CCU page above is mapped.
//
#define A733_DE_MAP_BASE        0x05000000UL
#define A733_DE_MAP_SIZE        (4 * SIZE_1MB)
#define A733_TCON_TOP_MAP_BASE  0x05500000UL
#define A733_TCON_TOP_MAP_SIZE  SIZE_128KB
#define A733_HDMI_MAP_BASE      0x05520000UL
#define A733_HDMI_MAP_SIZE      SIZE_128KB
#define A733_TPHY_MAP_BASE      0x05600000UL
#define A733_TPHY_MAP_SIZE      SIZE_64KB
#define A733_TCON3_MAP_BASE     0x05730000UL
#define A733_TCON3_MAP_SIZE     SIZE_4KB

STATIC ARM_MEMORY_REGION_DESCRIPTOR  mVirtualMemoryTable[] = {
  // System RAM; base and size are read from the PCDs in
  // ArmPlatformGetVirtualMemoryMap() so the map matches the DRAM the PrePi
  // discovered from the device tree.
  {
    .PhysicalBase = 0,
    .VirtualBase  = 0,
    .Length       = 0,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
  },
  {
    .PhysicalBase = A733_UART0_BASE,
    .VirtualBase  = A733_UART0_BASE,
    .Length       = SIZE_4KB,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_GIC_BASE,
    .VirtualBase  = A733_GIC_BASE,
    .Length       = A733_GIC_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_CCMU_BASE,
    .VirtualBase  = A733_CCMU_BASE,
    .Length       = A733_CCMU_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_GPIO_BASE,
    .VirtualBase  = A733_GPIO_BASE,
    .Length       = A733_GPIO_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_RPIO_BASE,
    .VirtualBase  = A733_RPIO_BASE,
    .Length       = A733_RPIO_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_STWI0_BASE,
    .VirtualBase  = A733_STWI0_BASE,
    .Length       = A733_STWI0_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_STWI1_BASE,
    .VirtualBase  = A733_STWI1_BASE,
    .Length       = A733_STWI1_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_CPUPLL_BASE,
    .VirtualBase  = A733_CPUPLL_BASE,
    .Length       = A733_CPUPLL_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_USB0_BASE,
    .VirtualBase  = A733_USB0_BASE,
    .Length       = A733_USB0_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_USB_HOST_BASE,
    .VirtualBase  = A733_USB_HOST_BASE,
    .Length       = A733_USB_HOST_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_SRAMC_MAP_BASE,
    .VirtualBase  = A733_SRAMC_MAP_BASE,
    .Length       = A733_SRAMC_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  //
  // SID efuse window (0x03006000): SunxiGmacDxe reads the chip-unique ID
  // here to derive the station address.
  //
  {
    .PhysicalBase = 0x03006000,
    .VirtualBase  = 0x03006000,
    .Length       = SIZE_4KB,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_R_CCU_MAP_BASE,
    .VirtualBase  = A733_R_CCU_MAP_BASE,
    .Length       = A733_R_CCU_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_PPU_MAP_BASE,
    .VirtualBase  = A733_PPU_MAP_BASE,
    .Length       = A733_PPU_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_USB2_PHY_MAP_BASE,
    .VirtualBase  = A733_USB2_PHY_MAP_BASE,
    .Length       = A733_USB2_PHY_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_SMHC_BASE,
    .VirtualBase  = A733_SMHC_BASE,
    .Length       = A733_SMHC_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_SPI0_BASE,
    .VirtualBase  = A733_SPI0_BASE,
    .Length       = A733_SPI0_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_DMAC_MAP_BASE,
    .VirtualBase  = A733_DMAC_MAP_BASE,
    .Length       = A733_DMAC_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  //
  // GMAC0: the DesignWare MAC window and its separate SYSCON block (RGMII
  // delays).  SunxiGmacDxe touches both before DwEmacSnpDxe binds.
  //
  {
    .PhysicalBase = 0x04500000,
    .VirtualBase  = 0x04500000,
    .Length       = 0x00008000,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = 0x04508000,
    .VirtualBase  = 0x04508000,
    .Length       = SIZE_4KB,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_DWC3_MAP_BASE,
    .VirtualBase  = A733_DWC3_MAP_BASE,
    .Length       = A733_DWC3_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_SERDES_MAP_BASE,
    .VirtualBase  = A733_SERDES_MAP_BASE,
    .Length       = A733_SERDES_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_COMBPHY0_MAP_BASE,
    .VirtualBase  = A733_COMBPHY0_MAP_BASE,
    .Length       = A733_COMBPHY0_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_RTC_MAP_BASE,
    .VirtualBase  = A733_RTC_MAP_BASE,
    .Length       = A733_RTC_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_DE_MAP_BASE,
    .VirtualBase  = A733_DE_MAP_BASE,
    .Length       = A733_DE_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_TCON_TOP_MAP_BASE,
    .VirtualBase  = A733_TCON_TOP_MAP_BASE,
    .Length       = A733_TCON_TOP_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_HDMI_MAP_BASE,
    .VirtualBase  = A733_HDMI_MAP_BASE,
    .Length       = A733_HDMI_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_TPHY_MAP_BASE,
    .VirtualBase  = A733_TPHY_MAP_BASE,
    .Length       = A733_TPHY_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  {
    .PhysicalBase = A733_TCON3_MAP_BASE,
    .VirtualBase  = A733_TCON3_MAP_BASE,
    .Length       = A733_TCON3_MAP_SIZE,
    .Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
  },
  // Terminator: ArmConfigureMmu() stops at the first all-zero entry.
  { 0, 0, 0, 0 }
};

/**
  Return the current Boot Mode

  This function returns the boot reason on the platform

**/
EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

/**
  First platform specific function to be called in the PEI phase

  This function is actually the first function called by the PrePi
  or PrePeiCore modules. It allows to retrieve arguments passed to
  the UEFI firmware through the CPU registers.

**/
VOID
ArmPlatformPeiBootAction (
  VOID
  )
{
}

/**
  Initialize controllers that must setup in the normal world

  This function is called by the ArmPlatformPkg/PrePi or ArmPlatformPkg/PlatformPei
  in the PEI phase.

**/
RETURN_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  // TF-A has already brought up DRAM and the console; nothing to do here.
  return RETURN_SUCCESS;
}

/**
  Return the Virtual Memory Map of your platform

  This Virtual Memory Map is used by MemoryInitPei Module to initialize the MMU on your platform.

  @param[out]   VirtualMemoryMap    Array of ARM_MEMORY_REGION_DESCRIPTOR describing a Physical-to-
                                    Virtual Memory mapping. This array must be ended by a zero-filled
                                    entry

**/
VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  mVirtualMemoryTable[0].PhysicalBase = PcdGet64 (PcdSystemMemoryBase);
  mVirtualMemoryTable[0].VirtualBase  = PcdGet64 (PcdSystemMemoryBase);
  mVirtualMemoryTable[0].Length       = PcdGet64 (PcdSystemMemorySize);

  *VirtualMemoryMap = mVirtualMemoryTable;
}

/**
  Return the Platform specific PPIs

  This function exposes the Platform Specific PPIs. They can be used by any PrePi modules or passed
  to the PeiCore by PrePeiCore.

  @param[out]   PpiListSize         Size in Bytes of the Platform PPI List
  @param[out]   PpiList             Platform PPI List

**/
VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  // Single core: no MP core info PPI to publish.
  *PpiListSize = 0;
  *PpiList     = NULL;
}