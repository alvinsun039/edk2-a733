/** @file
  SunxiGmacDxe — Allwinner A733 GMAC0 board bring-up for the Cubie A7A.

  The A733 GMAC0 is a Synopsys DesignWare MAC 5.20 ("sunxi-gmac-210",
  "snps,dwmac-5.20") at 0x04500000.  The actual MAC + PHY driver is
  DwEmacSnpDxe, which binds a NonDiscoverable MMIO device.  This driver's
  only job is the board-level bring-up that DwEmacSnpDxe expects to be
  already done:

    * clock + de-reset GMAC0 in the CCU (module gate + AXI/AHB reset)
    * 25 MHz PHY reference clock on the GMAC0_PHY mux
    * mux PH0..PH15 to alternate function 0x5 (rgmii0)
    * take the Maxio PHY (MDIO addr 1) out of reset via PH16 (active low)
    * register the GMAC0 MMIO window as a NonDiscoverable device so
      DwEmacSnpDxe's DriverBinding can attach.

  Register layout (all offsets from SUNXI_CCU_BASE = 0x02002000):
    GMAC_PTP_CLK   0x1400  — PTP ref clock gate (bit31)
    GMAC0_PHY_CLK  0x1410  — PHY ref clock: gate bit31, M div [4:0]
    GMAC0_BGR      0x141C  — module gate bit0, reset bit16, AXI rst bit17
    MBUS gate 0x05E0/0x05E4 already carry GMAC0_MCLK (bit11) from the
    SunxiUsbDxe bring-up, so no MBUS write is needed here.

  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/A733ClockLib.h>
#include <Library/A733GpioLib.h>
#include <Library/A733PmicLib.h>
#include <Library/A733TwiLib.h>
#include <Protocol/NonDiscoverableDevice.h>

//
// CCU / PIO base addresses come from Library/A733ClockLib.h and
// Library/A733GpioLib.h; register access goes through their helpers.
//

//
// GMAC0 CCU clocks/resets are driven through A733ClockLib
// (SunxiClockInitGmac0: module gate + RST + AXI_RST at 0x141C, PHY 25 MHz
// reference clock at 0x1410, PTP gate at 0x1400).

//
// GMAC0 MMIO window.
//
#define GMAC0_BASE                0x04500000ULL
#define GMAC0_SIZE                0x00008000ULL

//
// GMAC0 SYSCON (devicetree reg[1] = 0x04508000, 0x1000).
// dwmac210 CFG @0x00 (U-Boot dwc_eth_qos_aw.c):
//   ETCS[1:0] = 2 INT_GMII, EPIT bit2, ETXDC_H[17:16], ETXDC_L[12:10],
//   ERXDC[9:5].  Board DTS: tx-delay=12, rx-delay=10.
//
#define GMAC0_SYSCON_BASE         0x04508000ULL
#define GMAC0_SYSCON_CFG_REG      0x00
#define SYSCON_ETCS_INT_GMII      (0x2U << 0)
#define SYSCON_EPIT               BIT2
#define SYSCON_ETXDC_H_MSK        (0x3U << 16)
#define SYSCON_ETXDC_L_MSK        (0x7U << 10)
#define SYSCON_ERXDC_MSK          (0x1FU << 5)
// TX/RX delay chains and PHY reset GPIO are BOARD properties (RGMII trace
// lengths / PHY wiring) - configured per board via the PCDs below
// (defaults = Cubie A7A).

//
// PH16 PHY reset (board DTS gmac0_phy0 reset-gpios PH16) - board-level,
// kept here rather than in A733GpioLib.  Bank/function constants for PH /
// rgmii0 come from A733GpioLib (A733_PIO_BANK_H / A733_PIO_FUNC_RGMII0).
//
#define PHY_RESET_PIN             16      // PH16 (default board wiring)
//
// PH16 reset timing (Maxio 7b74:4412).  Back to the 4b73bc3-era values
// (1ms/30ms): the 29071ff control build with these exact values restores
// the network (2026-09-09), so the reset timing was never the problem.
//
#define PHY_RESET_ASSERT_US       1000
#define PHY_RESET_DEASSERT_US     30000

/**
  Bring GMAC0 up: clocks + reset, PHY ref clock, PH0..15 rgmii0 mux,
  PH16 PHY reset release.
**/
STATIC
VOID
Gmac0BoardInit (
  VOID
  )
{
  UINT32   V;
  UINTN    Pin;
  UINT32   ResetGpio;
  UINT32   ResetBank;
  UINT32   ResetPin;
  BOOLEAN  ActiveHigh;
  UINT32   TxDelay;
  UINT32   RxDelay;

  //
  // 1-3. GMAC0 clock + de-reset, 25 MHz PHY reference clock, PTP gate.
  //
  SunxiClockInitGmac0 ();
  DEBUG ((DEBUG_VERBOSE, "SunxiGmac: CCU BGR(0x141C)=0x%08x PHY(0x1410)=0x%08x\n",
          SunxiCcuRead (0x141C), SunxiCcuRead (0x1410)));

  //
  // 4. Mux PH0..PH15 to rgmii0 (selector 5), with the board's pull-up
  //    and drive strength.
  //
  ResetGpio = FixedPcdGet32 (PcdGmacPhyResetGpio);
  ResetBank = (ResetGpio >> 8) & 0xFF;
  ResetPin  = ResetGpio & 0xFF;
  ActiveHigh = ((ResetGpio & BIT17) != 0);

  for (Pin = 0; Pin < 16; Pin++) {
    A733GpioSetFunction (A733_GPIO_PIN (A733_GPIO_BANK_H, Pin), A733_GPIO_FUNC_ALT5);
    A733GpioSetPull (A733_GPIO_PIN (A733_GPIO_BANK_H, Pin), A733_GPIO_PULL_UP);
    A733GpioSetDrive (A733_GPIO_PIN (A733_GPIO_BANK_H, Pin), 1);
  }
  DEBUG ((DEBUG_VERBOSE, "SunxiGmac: PIO PH0..15 -> rgmii0 (func %d)\n",
          A733_GPIO_FUNC_ALT5));

  //
  // 5. SYSCON @0x04508000: RGMII interface + TX/RX delay chains
  //    (dwmac210 CFG, mirrors U-Boot set_syscon + set_delaychain).
  //
  TxDelay = FixedPcdGet8 (PcdGmacRgmiiTxDelay);
  RxDelay = FixedPcdGet8 (PcdGmacRgmiiRxDelay);

  V  = SYSCON_ETCS_INT_GMII | SYSCON_EPIT;
  V |= (TxDelay >> 3) << 16;                           // ETXDC_H
  V |= (TxDelay & 0x7) << 10;                          // ETXDC_L
  V |= (RxDelay & 0x1F) << 5;                          // ERXDC
  MmioWrite32 (GMAC0_SYSCON_BASE + GMAC0_SYSCON_CFG_REG, V);
  DEBUG ((DEBUG_VERBOSE, "SunxiGmac: SYSCON 0x04508000=0x%08x (RGMII tx_delay=%d rx_delay=%d)\n",
    (UINT32)MmioRead32 (GMAC0_SYSCON_BASE + GMAC0_SYSCON_CFG_REG),
    TxDelay, RxDelay));

  //
  // 6. Power the PHY.  On this board phy3v3 comes from the AXP8191
  //    SWOUT1 rail (U-Boot dts: phy-supply = <&reg_swout1>); nothing in
  //    boot0/BL31/ARISC turns it on, so a cold boot reaches the PHY probe
  //    with the rail off and every MDIO read returns 0xffff.  The PMIC
  //    sits at 0x36 on R_TWI0 (PL0/PL1); A733TwiInit() gates the bus and
  //    muxes the pins.  AXP8191 DCDC_CTL2 (0x11) bit3 = SWOUT1 enable.
  //
  {
    UINT8  Reg;

    if (!EFI_ERROR (A733PmicReadReg (0x11, &Reg))) {
      if ((Reg & BIT3) == 0) {
        A733PmicWriteReg (0x11, (UINT8)(Reg | BIT3));
      }
      DEBUG ((DEBUG_INFO, "SunxiGmac: AXP8191 SWOUT1 (PHY 3V3) enabled (0x%02x)\n", Reg));
      MicroSecondDelay (1000);
    } else {
      DEBUG ((DEBUG_WARN, "SunxiGmac: AXP8191 SWOUT1 enable failed\n"));
    }
  }

  //
  // 7. Take the Maxio PHY out of reset: PH16 as output (active-low reset,
  //    per the board DTS) with a pull-up, then assert low and release.
  //    Input mode would not drive the pad, leaving the PHY in reset.
  //
  A733GpioSetFunction (A733_GPIO_PIN (ResetBank, ResetPin), A733_GPIO_FUNC_OUTPUT);
  A733GpioSetPull (A733_GPIO_PIN (ResetBank, ResetPin), A733_GPIO_PULL_UP);
  //
  // Polarity: assert drives the ACTIVE level (high if BIT17 active-high,
  // low if active-low), release drives the opposite.  The old code had
  // these swapped - an active-low PHY reset (this board) was held
  // DEasserted during 'assert' and asserted during 'release', keeping the
  // PHY in reset forever so MDIO never answered.
  //
  A733GpioWrite (A733_GPIO_PIN (ResetBank, ResetPin), ActiveHigh);     // assert
  MicroSecondDelay (PHY_RESET_ASSERT_US);
  A733GpioWrite (A733_GPIO_PIN (ResetBank, ResetPin), (BOOLEAN)!ActiveHigh);  // release
  MicroSecondDelay (PHY_RESET_DEASSERT_US);

  DEBUG ((DEBUG_INFO, "SunxiGmac: PH16 PHY reset released\n"));
}

//
// Per-chip station address, derived exactly as U-Boot does on this SoC
// (board/sunxi/board.c setup_environment -> get_unique_sid).  The A733
// SID efuse holds a chip-unique 16-byte ID; the single words 1..3 share
// many bits across dies, so U-Boot crc32s them together to get a unique
// value, then builds a locally-administered MAC (bit1 of the first octet
// set, so no OUI is needed).  Using the same derivation gives EDK2 the
// same MAC that U-Boot/Linux put on the wire, so DHCP leases and switch
// tables do not change when the firmware hands off.
//
// SID efuse data window: SIDC @ 0x03006000, efuse at +0x200.
//
#define A733_SID_EFUSE_BASE      0x03006200UL

/**
  Return the GMAC0 station address as a 48-bit big-endian UINT64 (byte 0
  of the MAC in bits 47:40), the layout DwEmacSnpDxe expects in
  Resources[1].AddrRangeMin.

  Falls back to PcdGmac0MacAddress when the SID is not readable (all
  zero), so the platform PCD remains the override of last resort.
**/
STATIC
UINT64
SunxiGmacDeriveStationMac (
  VOID
  )
{
  UINT32  Sid[4];
  UINT64  Mac;
  UINTN   Index;

  for (Index = 0; Index < 4; Index++) {
    Sid[Index] = MmioRead32 (A733_SID_EFUSE_BASE + Index * 4);
  }

  if (Sid[0] == 0) {
    DEBUG ((DEBUG_WARN, "SunxiGmac: SID unreadable, using PCD MAC\n"));
    return FixedPcdGet64 (PcdGmac0MacAddress);
  }

  Sid[3] = CalculateCrc32 (&Sid[1], 12);
  if ((Sid[3] & 0xFFFFFF) == 0) {
    Sid[3] |= 0x800000;
  }

  Mac  = ((UINT64)0x02                     << 40); // locally administered
  Mac |= ((UINT64)(Sid[0] & 0xFF)          << 32);
  Mac |= ((UINT64)((Sid[3] >> 24) & 0xFF)  << 24);
  Mac |= ((UINT64)((Sid[3] >> 16) & 0xFF)  << 16);
  Mac |= ((UINT64)((Sid[3] >>  8) & 0xFF)  <<  8);
  Mac |= ((UINT64)( Sid[3]        & 0xFF));

  DEBUG ((DEBUG_INFO, "SunxiGmac: station MAC %02x:%02x:%02x:%02x:%02x:%02x (from SID)\n",
          (UINT8)(Mac >> 40), (UINT8)(Mac >> 32), (UINT8)(Mac >> 24),
          (UINT8)(Mac >> 16), (UINT8)(Mac >>  8), (UINT8)Mac));

  return Mac;
}

/**
  SunxiGmacDxe entry point.  Board bring-up then register GMAC0 as a
  NonDiscoverable MMIO device for DwEmacSnpDxe to bind.
**/
EFI_STATUS
EFIAPI
SunxiGmacDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT64      StationMac;

  Gmac0BoardInit ();

  StationMac = SunxiGmacDeriveStationMac ();

  //
  // Register the GMAC0 MMIO window.  DwEmacSnpDxe's DriverSupported only
  // checks for the NonDiscoverable protocol (not the type GUID), and reads
  // Resources[0].AddrRangeMin as the MAC base — exactly this window.
  // Resources[1] carries the station address, which DriverBinding reads
  // back and byte-swaps into CurrentAddress.
  //
  Status = RegisterNonDiscoverableMmioDevice (
             NonDiscoverableDeviceTypeAmba,   // no Gmac type in the enum; GUID is what binds
             NonDiscoverableDeviceDmaTypeCoherent,
             NULL,
             NULL,
             2,
             GMAC0_BASE,
             GMAC0_SIZE,
             StationMac,
             6
             );
  DEBUG ((DEBUG_INFO, "SunxiGmac: GMAC0 NonDiscoverable register: %r\n", Status));

  return Status;
}
