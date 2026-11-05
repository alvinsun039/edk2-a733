/** @file

  Copyright (c) 2011 - 2019, Intel Corporaton. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

  The original software modules are licensed as follows:

  Copyright (c) 2012 - 2014, ARM Limited. All rights reserved.
  Copyright (c) 2004 - 2010, Intel Corporation. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "EmacDxeUtil.h"
#include "PhyDxeUtil.h"

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>

VOID
EFIAPI
EmacSetMacAddress (
  IN  EFI_MAC_ADDRESS   *MacAddress,
  IN  UINTN             MacBaseAddress
  )
{
  DEBUG ((DEBUG_INFO, "SNP:MAC: %a ()\r\n", __func__));

  // Note: This MAC_ADDR0 registers programming sequence cannot be swap:
  // Must program HIGH Offset first before LOW Offset
  // because synchronization is triggered when MAC Address0 Low Register are written.
  MmioWrite32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_ADDRESS0_HIGH_OFST,
               (UINT32)(MacAddress->Addr[4] & 0xFF) |
                      ((MacAddress->Addr[5] & 0xFF) << 8)
                    );
  // MacAddress->Addr[0,1,2] is the 3 bytes OUI
  MmioWrite32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_ADDRESS0_LOW_OFST,
                       (MacAddress->Addr[0] & 0xFF) |
                      ((MacAddress->Addr[1] & 0xFF) << 8) |
                      ((MacAddress->Addr[2] & 0xFF) << 16) |
                      ((MacAddress->Addr[3] & 0xFF) << 24)
                    );

  DEBUG ((DEBUG_INFO, "SNP:MAC: gmacgrp_mac_address0_low  = 0x%08X \r\n",
          MmioRead32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_ADDRESS0_LOW_OFST)));
  DEBUG ((DEBUG_INFO, "SNP:MAC: gmacgrp_mac_address0_high = 0x%08X \r\n",
          MmioRead32 (MacBaseAddress +DW_EMAC_GMACGRP_MAC_ADDRESS0_HIGH_OFST)));
}


VOID
EFIAPI
EmacReadMacAddress (
  OUT  EFI_MAC_ADDRESS   *MacAddress,
  IN   UINTN             MacBaseAddress
  )
{
  UINT32          MacAddrHighValue;
  UINT32          MacAddrLowValue;

  DEBUG ((DEBUG_INFO, "SNP:MAC: %a ()\r\n", __func__));

  // Read the Mac Addr high register
  MacAddrHighValue = (MmioRead32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_ADDRESS0_HIGH_OFST) & 0xFFFF);
  // Read the Mac Addr low register
  MacAddrLowValue = MmioRead32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_ADDRESS0_LOW_OFST);

  SetMem (MacAddress, sizeof(*MacAddress), 0);
  MacAddress->Addr[0] = (MacAddrLowValue & 0xFF);
  MacAddress->Addr[1] = (MacAddrLowValue & 0xFF00) >> 8;
  MacAddress->Addr[2] = (MacAddrLowValue & 0xFF0000) >> 16;
  MacAddress->Addr[3] = (MacAddrLowValue & 0xFF000000) >> 24;
  MacAddress->Addr[4] = (MacAddrHighValue & 0xFF);
  MacAddress->Addr[5] = (MacAddrHighValue & 0xFF00) >> 8;

  DEBUG ((DEBUG_INFO, "SNP:MAC: MAC Address = %02X:%02X:%02X:%02X:%02X:%02X\r\n",
    MacAddress->Addr[0], MacAddress->Addr[1], MacAddress->Addr[2],
    MacAddress->Addr[3], MacAddress->Addr[4], MacAddress->Addr[5]
  ));
}

EFI_STATUS
EFIAPI
EmacDxeInitialization (
  IN  EMAC_DRIVER   *EmacDriver,
  IN  UINTN         MacBaseAddress
  )
{
  DEBUG ((DEBUG_INFO, "SNP:MAC: %a ()\r\n", __func__));

  // Init EMAC DMA
  EmacDmaInit (EmacDriver, MacBaseAddress);

  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
EmacDmaInit (
  IN  EMAC_DRIVER   *EmacDriver,
  IN  UINTN         MacBaseAddress
  )
{
  UINT32  Val;
  UINT32  Txfifo, Rxfifo, Tqs, Rqs;

  DEBUG ((DEBUG_INFO, "SNP:MAC: %a ()\r\n", __func__));

  //
  // EQoS (dwmac-5.20) init, in the order the Synopsys DW driver does it:
  //   1. software reset (DMA_MODE.SWR @0x1000)
  //   2. MTL TX/RX queue enable (0xD00/0xD30)
  //   3. DMA channel 0 control + TX/RX control + sysbus mode (0x1100+/0x1004)
  //   4. descriptor ring base + ring length (0x1114/0x111C/0x112C/0x1130)
  //   5. start RX/TX (ch0_rx_control.SR, ch0_tx_control.ST, MAC cfg RE/TE)
  //

  // 1. Software reset, wait for SWR to self-clear.
  MmioOr32 (MacBaseAddress + DW_EMAC_DMAGRP_MODE_OFST, DW_EMAC_DMAGRP_MODE_SWR_SET_MSK);
  {
    UINTN  SwrCount;
    for (SwrCount = 0; SwrCount < 500000; SwrCount++) {
      if (!DW_EMAC_DMAGRP_MODE_SWR_GET (MmioRead32 (MacBaseAddress + DW_EMAC_DMAGRP_MODE_OFST))) {
        break;
      }
    }
  }

  // 2. MTL - read FIFO sizes from HW feature registers to size the queues.
  Val    = MmioRead32 (MacBaseAddress + 0x0120);   // hw_feature1
  Txfifo = 128U << ((Val >> 6) & 0x1F);            // TXFIFOSIZE field
  Rxfifo = 128U << ((Val >> 0) & 0x1F);            // RXFIFOSIZE field
  Tqs    = Txfifo / 256 - 1;
  Rqs    = Rxfifo / 256 - 1;

  // TX queue: enable, store-and-forward, TQS.
  MmioOr32 (MacBaseAddress + EQOS_MTL_TXQ0_OPERATION_MODE_OFST,
            EQOS_MTL_TXQ0_OPERATION_MODE_TSF |
            EQOS_MTL_TXQ0_OPERATION_MODE_TXQEN_ENABLED);
  MmioAndThenOr32 (MacBaseAddress + EQOS_MTL_TXQ0_OPERATION_MODE_OFST,
                   ~EQOS_MTL_TXQ0_OPERATION_MODE_TQS_MASK,
                   (Tqs << EQOS_MTL_TXQ0_OPERATION_MODE_TQS_SHIFT));

  // RX queue: store-and-forward + RQS.
  MmioOr32 (MacBaseAddress + EQOS_MTL_RXQ0_OPERATION_MODE_OFST,
            EQOS_MTL_RXQ0_OPERATION_MODE_RSF);
  MmioAndThenOr32 (MacBaseAddress + EQOS_MTL_RXQ0_OPERATION_MODE_OFST,
                   ~EQOS_MTL_RXQ0_OPERATION_MODE_RQS_MASK,
                   (Rqs << EQOS_MTL_RXQ0_OPERATION_MODE_RQS_SHIFT));

  // 3a. RX buffer size must be a multiple of bus width.
  MmioAndThenOr32 (MacBaseAddress + EQOS_DMA_CH0_RX_CONTROL_OFST,
                   ~EQOS_DMA_CH0_RX_CONTROL_RBSZ_MASK,
                   (CONFIG_ETH_BUFSIZE << EQOS_DMA_CH0_RX_CONTROL_RBSZ_SHIFT));

  // 3b. Channel control: PBLx8, DSL=6 (64-byte descriptor pitch on A733,
  //     see EQOS_DESC_SIZE; 16-byte pitch DSL=0 makes the DMA engine read
  //     garbage at base+16 and never consume OWN).
  MmioWrite32 (MacBaseAddress + EQOS_DMA_CH0_CONTROL_OFST,
               EQOS_DMA_CH0_CONTROL_PBLX8 |
               EQOS_DMA_CH0_CONTROL_DSL_OFST);

  // 3c. TX/RX PBL (burst length). EQoS: TXPBL/RXPBL.
  MmioAndThenOr32 (MacBaseAddress + EQOS_DMA_CH0_TX_CONTROL_OFST,
                   ~EQOS_DMA_CH0_TX_CONTROL_TXPBL_MASK,
                   (1U << EQOS_DMA_CH0_TX_CONTROL_TXPBL_SHIFT));
  MmioAndThenOr32 (MacBaseAddress + EQOS_DMA_CH0_RX_CONTROL_OFST,
                   ~EQOS_DMA_CH0_RX_CONTROL_RXPBL_MASK,
                   (8U << EQOS_DMA_CH0_RX_CONTROL_RXPBL_SHIFT));

  // 3d. AXI sysbus mode: fixed burst, OSR limit.
  MmioWrite32 (MacBaseAddress + EQOS_DMA_SYSBUS_MODE_OFST,
               (2U << EQOS_DMA_SYSBUS_MODE_RD_OSR_LMT_SHIFT) |
               EQOS_DMA_SYSBUS_MODE_EAME |
               EQOS_DMA_SYSBUS_MODE_BLEN16 |
               EQOS_DMA_SYSBUS_MODE_BLEN8 |
               EQOS_DMA_SYSBUS_MODE_BLEN4);

  // 4. Set up the descriptor rings.
  EmacSetupTxdesc (EmacDriver, MacBaseAddress);
  EmacSetupRxdesc (EmacDriver, MacBaseAddress);

  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
EmacSetupTxdesc (
  IN  EMAC_DRIVER   *EmacDriver,
  IN  UINTN         MacBaseAddress
 )
{
  INTN                       Index;
  DESIGNWARE_HW_DESCRIPTOR   *TxDescriptor;

  for (Index = 0; Index < CONFIG_TX_DESCR_NUM; Index++) {
    TxDescriptor = (VOID *)(UINTN)EmacDriver->TxdescRingMap[Index].AddrMap;
    ZeroMem (TxDescriptor, sizeof (DESIGNWARE_HW_DESCRIPTOR));
    TxDescriptor->Des0 = (UINT32)(UINTN)&EmacDriver->TxBuffer[Index * CONFIG_ETH_BUFSIZE];
  }

  // Write the address + ring length of the tx descriptor list.
  MmioWrite32 (MacBaseAddress + 0x1110, 0);   // TXDESC list HADDR <- 0 (DMA base < 4GB)
  MmioWrite32 (MacBaseAddress + EQOS_DMA_CH0_TXDESC_LIST_ADDRESS_OFST,
               (UINT32)(UINTN)EmacDriver->TxdescRingMap[0].AddrMap);
  MmioWrite32 (MacBaseAddress + EQOS_DMA_CH0_TXDESC_RING_LENGTH_OFST,
               CONFIG_TX_DESCR_NUM - 1);

  // Initialize the descriptor number
  EmacDriver->TxCurrentDescriptorNum = 0;
  EmacDriver->TxNextDescriptorNum = 0;

  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
EmacSetupRxdesc (
  IN  EMAC_DRIVER   *EmacDriver,
  IN  UINTN         MacBaseAddress
  )
{
  INTN                        Index;
  DESIGNWARE_HW_DESCRIPTOR    *RxDescriptor;

  for (Index = 0; Index < CONFIG_RX_DESCR_NUM; Index++) {
    RxDescriptor = (VOID *)(UINTN)EmacDriver->RxdescRingMap[Index].AddrMap;
    ZeroMem (RxDescriptor, sizeof (DESIGNWARE_HW_DESCRIPTOR));
    RxDescriptor->Des0 = (UINT32)EmacDriver->RxBufNum[Index].AddrMap;
    RxDescriptor->Des3 = EQOS_DESC3_OWN | EQOS_DESC3_BUF1V;
  }

  // Write the address + ring length of the rx descriptor list.
  MmioWrite32 (MacBaseAddress + 0x1118, 0);   // RXDESC list HADDR <- 0 (DMA base < 4GB)
  MmioWrite32 (MacBaseAddress + EQOS_DMA_CH0_RXDESC_LIST_ADDRESS_OFST,
               (UINT32)(UINTN)EmacDriver->RxdescRingMap[0].AddrMap);
  MmioWrite32 (MacBaseAddress + EQOS_DMA_CH0_RXDESC_RING_LENGTH_OFST,
               CONFIG_RX_DESCR_NUM - 1);

  // RX tail pointer -> last descriptor.  EQoS treats "tail == head" as no
  // descriptors available, so pointing it at the last slot makes all the
  // others available at startup (matches U-Boot eqos_start()).  Without
  // this the DMA engine never fills the ring and RX is dead.
  MmioWrite32 (MacBaseAddress + EQOS_DMA_CH0_RXDESC_TAIL_POINTER_OFST,
               (UINT32)(UINTN)EmacDriver->RxdescRingMap[CONFIG_RX_DESCR_NUM - 1].AddrMap);

  // Initialize the descriptor number
  EmacDriver->RxCurrentDescriptorNum = 0;
  EmacDriver->RxNextDescriptorNum = 0;

  return EFI_SUCCESS;
}


VOID
EFIAPI
EmacStartTransmission (
  IN  UINTN   MacBaseAddress
  )
{
  DEBUG ((DEBUG_INFO, "SNP:MAC: %a ()\r\n", __func__));

  // Enable the TX/RX datapaths in the MAC and start the DMA channel,
  // in the same order / with the same bits as U-Boot eqos_start():
  //   - MAC config: CST (core speed test) + ACS (auto pad/CRC strip)
  //   - MAC RXQ ctrl0: RXQ0EN (queue must be enabled before RX DMA)
  //   - MAC 0x0A4: multicast/broadcast queue enable
  //   - DMA ch0 TX control: OSP (operate on second packet)
  //   - MAC RE|TE, DMA ST|SR
  MmioOr32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_CONFIGURATION_OFST,
            DW_EMAC_GMACGRP_MAC_CONFIGURATION_CST_SET_MSK |
            DW_EMAC_GMACGRP_MAC_CONFIGURATION_ACS_SET_MSK);
  MmioOr32 (MacBaseAddress + EQOS_MAC_RXQ_CTRL0_OFST,
            EQOS_MAC_RXQ_CTRL0_RXQ0EN_ENABLED);
  MmioOr32 (MacBaseAddress + 0x0A4, EQOS_MAC_MULTICAST_BROADCAST_Q_ENABLE);
  MmioOr32 (MacBaseAddress + EQOS_DMA_CH0_TX_CONTROL_OFST,
            EQOS_DMA_CH0_TX_CONTROL_OSP);
  MmioOr32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_CONFIGURATION_OFST,
            DW_EMAC_GMACGRP_MAC_CONFIGURATION_RE_SET_MSK |
            DW_EMAC_GMACGRP_MAC_CONFIGURATION_TE_SET_MSK);
  MmioOr32 (MacBaseAddress + EQOS_DMA_CH0_TX_CONTROL_OFST, EQOS_DMA_CH0_TX_CONTROL_ST);
  MmioOr32 (MacBaseAddress + EQOS_DMA_CH0_RX_CONTROL_OFST, EQOS_DMA_CH0_RX_CONTROL_SR);
}


EFI_STATUS
EFIAPI
EmacRxFilters (
  IN  UINT32            ReceiveFilterSetting,
  IN  BOOLEAN           Reset,
  IN  UINTN             NumMfilter          OPTIONAL,
  IN  EFI_MAC_ADDRESS   *Mfilter            OPTIONAL,
  IN  UINTN             MacBaseAddress
  )
{
  UINT32  MacFilter;
  UINT32  Crc;
  UINT32  Count;
  UINT32  HashReg;
  UINT32  HashBit;
  UINT32  Reg;
  UINT32  Val;

  // If reset then clear the filter registers
  if (Reset) {
    for (Count = 0; Count < NumMfilter; Count++)
    {
      MmioWrite32 (MacBaseAddress + HASH_TABLE_REG(Count), 0x00000000);
    }
  }

  // Set MacFilter to the reset value of the  DW_EMAC_GMACGRP_MAC_FRAME_FILTER register.
  MacFilter =  DW_EMAC_GMACGRP_MAC_FRAME_FILTER_RESET;

  if (ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_MULTICAST) {
    MacFilter |=  DW_EMAC_GMACGRP_MAC_FRAME_FILTER_HMC_SET_MSK;

    // Set the hash tables
    if ((NumMfilter > 0) && (!Reset)) {
      // Go through each filter address and set appropriate bits on hash table
      for (Count = 0; Count < NumMfilter; Count++) {
        // Generate a 32-bit CRC
        Crc = GenEtherCrc32 (&Mfilter[Count], 6);
        // reserve CRC + take upper 8 bit = take lower 8 bit and reverse it
        Val = BitReverse(Crc & 0xff);
        // The most significant bits determines the register to be used (Hash Table Register X),
        // and the least significant five bits determine the bit within the register.
        // For example, a hash value of 8b'10111111 selects Bit 31 of the Hash Table Register 5.
        HashReg = (Val >> 5);
        HashBit = (Val & 0x1f);

        Reg = MmioRead32 (MacBaseAddress + HASH_TABLE_REG(HashReg));
        // set 1 to HashBit of HashReg
        // for example, set 1 to bit 31 to Reg 5 as in above example
        Reg |= (1 << HashBit);
        MmioWrite32(MacBaseAddress + HASH_TABLE_REG(HashReg), Reg);
      }
    }
  }

  if ((ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST) == 0) {
    MacFilter |=  DW_EMAC_GMACGRP_MAC_FRAME_FILTER_DBF_SET_MSK;
  }

  if (ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS) {
    MacFilter |=  DW_EMAC_GMACGRP_MAC_FRAME_FILTER_PR_SET_MSK;
  }

  if (ReceiveFilterSetting & EFI_SIMPLE_NETWORK_RECEIVE_PROMISCUOUS_MULTICAST) {
    MacFilter |= ( DW_EMAC_GMACGRP_MAC_FRAME_FILTER_PM_SET_MSK);
  }

  // Set MacFilter to EMAC register
  MmioWrite32 (MacBaseAddress +  DW_EMAC_GMACGRP_MAC_FRAME_FILTER_OFST, MacFilter);
  return EFI_SUCCESS;
}


UINT32
EFIAPI
GenEtherCrc32 (
  IN  EFI_MAC_ADDRESS   *Mac,
  IN  UINT32            AddrLen
  )
{
  INT32   Iter;
  UINT32  Remainder;
  UINT8   *Ptr;

  Iter = 0;
  Remainder = 0xFFFFFFFF;    // 0xFFFFFFFF is standard seed for Ethernet

  // Convert Mac Address to array of bytes
  Ptr = (UINT8 *)Mac;

  // Generate the Crc bit-by-bit (LSB first)
  while (AddrLen--) {
    Remainder ^= *Ptr++;
    for (Iter = 0; Iter < 8; Iter++) {
      // Check if exponent is set
      if (Remainder & 1) {
        Remainder = (Remainder >> 1) ^ CRC_POLYNOMIAL;
      } else {
        Remainder = (Remainder >> 1) ^ 0;
      }
    }
  }

  return (~Remainder);
}


STATIC CONST UINT8 NibbleTab[] = {
    /* 0x0 0000 -> 0000 */  0x0,
    /* 0x1 0001 -> 1000 */  0x8,
    /* 0x2 0010 -> 0100 */  0x4,
    /* 0x3 0011 -> 1100 */  0xc,
    /* 0x4 0100 -> 0010 */  0x2,
    /* 0x5 0101 -> 1010 */  0xa,
    /* 0x6 0110 -> 0110 */  0x6,
    /* 0x7 0111 -> 1110 */  0xe,
    /* 0x8 1000 -> 0001 */  0x1,
    /* 0x9 1001 -> 1001 */  0x9,
    /* 0xa 1010 -> 0101 */  0x5,
    /* 0xb 1011 -> 1101 */  0xd,
    /* 0xc 1100 -> 0011 */  0x3,
    /* 0xd 1101 -> 1011 */  0xb,
    /* 0xe 1110 -> 0111 */  0x7,
    /* 0xf 1111 -> 1111 */  0xf
};

UINT8
EFIAPI
BitReverse (
  UINT8   Value
  )
{
  return (NibbleTab[Value & 0xf] << 4) | NibbleTab[Value >> 4];
}


VOID
EFIAPI
EmacStopTxRx (
   IN  UINTN   MacBaseAddress
  )
{
  DEBUG ((DEBUG_INFO, "SNP:MAC: %a ()\r\n", __func__));

  // Stop DMA channels + MAC datapath.
  MmioAnd32 (MacBaseAddress + EQOS_DMA_CH0_TX_CONTROL_OFST, ~EQOS_DMA_CH0_TX_CONTROL_ST);
  MmioAnd32 (MacBaseAddress + EQOS_DMA_CH0_RX_CONTROL_OFST, ~EQOS_DMA_CH0_RX_CONTROL_SR);
  MmioAnd32 (MacBaseAddress + DW_EMAC_GMACGRP_MAC_CONFIGURATION_OFST,
             DW_EMAC_GMACGRP_MAC_CONFIGURATION_RE_CLR_MSK &
             DW_EMAC_GMACGRP_MAC_CONFIGURATION_TE_CLR_MSK);
}


EFI_STATUS
EFIAPI
EmacDmaStart (
  IN  UINTN   MacBaseAddress
  )
{
  // EQoS: kick the TX channel via the desc tail pointer of the next free slot.
  // The SNP driver's Transmit path sets the OWN bit and we simply poke the
  // channel to re-evaluate the ring (write tail pointer = next descriptor).
  return EFI_SUCCESS;
}


VOID
EFIAPI
EmacGetDmaStatus (
  OUT  UINT32   *IrqStat  OPTIONAL,
  IN   UINTN    MacBaseAddress
  )
{
  // EQoS (dwmac-5.20) has no interrupt-driven status in the SNP polling
  // model; RX availability is detected by descriptor OWN-bit polling in
  // SnpReceive. Report no pending interrupts.
  if (IrqStat != NULL) {
    *IrqStat = 0;
  }
}


VOID
EFIAPI
EmacGetStatistic (
  OUT  EFI_NETWORK_STATISTICS   *Statistic,
  IN   UINTN                     MacBaseAddress
  )
{
  EFI_NETWORK_STATISTICS   *Stats;

  DEBUG ((DEBUG_INFO, "SNP:MAC: %a ()\r\n", __func__));

  Stats = AllocateZeroPool (sizeof (EFI_NETWORK_STATISTICS));
  if (Stats == NULL) {
    return;
  }

  // EQoS MMC: RX/TX octet + frame counters in the MAC MMC block (0x800 baseregion).
  Stats->RxTotalBytes = (UINT64)MmioRead32 (MacBaseAddress + 0x0184) |   // rxoctetcount_gb
                        ((UINT64)MmioRead32 (MacBaseAddress + 0x0188) << 32);
  Stats->RxGoodFrames = MmioRead32 (MacBaseAddress + 0x0180);            // rxframe_count_gb
  Stats->TxTotalBytes = (UINT64)MmioRead32 (MacBaseAddress + 0x0114) |   // txoctetcount_gb
                        ((UINT64)MmioRead32 (MacBaseAddress + 0x0118) << 32);
  Stats->TxTotalFrames = MmioRead32 (MacBaseAddress + 0x0110);           // txframe_count_g

  CopyMem (Statistic, Stats, sizeof (EFI_NETWORK_STATISTICS));
  FreePool (Stats);
}


VOID
EFIAPI
EmacConfigAdjust (
  IN  UINT32   Speed,
  IN  UINT32   Duplex,
  IN  UINTN    MacBaseAddress
  )
{
  UINT32   Config;

  Config = 0;
  if (Speed != SPEED_1000) {
   Config |= DW_EMAC_GMACGRP_MAC_CONFIGURATION_PS_SET_MSK;
  }
  if (Speed == SPEED_100) {
    Config |= DW_EMAC_GMACGRP_MAC_CONFIGURATION_FES_SET_MSK;
  }
  if (Duplex == DUPLEX_FULL) {
    Config |= DW_EMAC_GMACGRP_MAC_CONFIGURATION_DM_SET_MSK;
  }

  //
  // RMW: PS/FES/DM are set-or-clear speed fields; a plain OR would
  // leave the previous speed latched when the link re-negotiates
  // 1000 <-> 100 Mbps.
  //
  MmioAndThenOr32 (MacBaseAddress +
                     DW_EMAC_GMACGRP_MAC_CONFIGURATION_OFST,
                   ~(DW_EMAC_GMACGRP_MAC_CONFIGURATION_PS_SET_MSK |
                     DW_EMAC_GMACGRP_MAC_CONFIGURATION_FES_SET_MSK |
                     DW_EMAC_GMACGRP_MAC_CONFIGURATION_DM_SET_MSK),
                   Config);
}
