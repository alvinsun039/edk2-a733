/** @file
  EmacDxeUtil.h

  Copyright (c) 2026, carpi-os contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef EMAC_DXE_UTIL_H__
#define EMAC_DXE_UTIL_H__

#include <Protocol/SimpleNetwork.h>

// Most common CRC32 Polynomial for little endian machines
#define CRC_POLYNOMIAL                                            0xEDB88320
#define RX_MAX_PACKET                                             1600

// A733 GMAC-210 = dwmac-5.20 (DWC_ether_qos) register map:
//   MAC  regs base 0x000
//   MTL  regs base 0xD00
//   DMA  regs base 0x1000 (channel 0)
//   SYSCON (separate window 0x04508000) configures RGMII/RMII + delays.
#define EQOS_MAC_REGS_BASE                             (0x000)
#define EQOS_MTL_REGS_BASE                             (0xD00)
#define EQOS_DMA_REGS_BASE                             (0x1000)

// MAC configuration (bits are NOT the old dwmac3 layout!)
//   RE=bit0, TE=bit1, PS=bit15, FES=bit14, DM=bit13, LM=bit12,
//   JE=bit16, JD=bit17, WD=bit19, ACS=bit20, CST=bit21, GPSLCE=bit23
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_OFST         (0x000)
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_RE_SET_MSK    BIT0
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_TE_SET_MSK    BIT1
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_RE_CLR_MSK    ~BIT0
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_TE_CLR_MSK    ~BIT1
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_PS_SET_MSK    BIT15
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_FES_SET_MSK   BIT14
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_DM_SET_MSK    BIT13
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_ACS_SET_MSK   BIT20
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_CST_SET_MSK   BIT21

// MAC RX queue ctrl0 @0x0A0: enable RXQ0 (required before RX DMA runs)
#define EQOS_MAC_RXQ_CTRL0_OFST                         (0x0A0)
#define EQOS_MAC_RXQ_CTRL0_RXQ0EN_ENABLED               (0x2 << 0)

// MAC 0x0A4: multicast + broadcast packet queue enable
#define EQOS_MAC_MULTICAST_BROADCAST_Q_ENABLE           (0x00100000)
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_JE_SET_MSK    BIT16
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_JD_SET_MSK    BIT17
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_WD_SET_MSK    BIT19
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_ACS_SET_MSK   BIT20
#define DW_EMAC_GMACGRP_MAC_CONFIGURATION_CST_SET_MSK   BIT21

// MAC address0 registers (EQoS: 0x300/0x304, not old dwmac3 0x40/0x44)
#define DW_EMAC_GMACGRP_MAC_ADDRESS0_HIGH_OFST         (0x300)
#define DW_EMAC_GMACGRP_MAC_ADDRESS0_LOW_OFST          (0x304)

// MAC packet filter (EQoS bit positions ~ old dwmac3 kept for RxFilters)
#define DW_EMAC_GMACGRP_MAC_FRAME_FILTER_OFST          (0x008)
#define DW_EMAC_GMACGRP_MAC_FRAME_FILTER_RESET         (0x00000000)
#define DW_EMAC_GMACGRP_MAC_FRAME_FILTER_HMC_SET_MSK    BIT2
#define DW_EMAC_GMACGRP_MAC_FRAME_FILTER_DBF_SET_MSK    BIT5
#define DW_EMAC_GMACGRP_MAC_FRAME_FILTER_PR_SET_MSK     BIT0
#define DW_EMAC_GMACGRP_MAC_FRAME_FILTER_PM_SET_MSK     BIT4
#define HASH_TABLE_REG(n)                              (0x900 + (0x4 * (n)))

// MTL (EQoS): TXQ0 op mode @0xD00, RXQ0 op mode @0xD30
#define EQOS_MTL_TXQ0_OPERATION_MODE_OFST              (0xD00)
#define EQOS_MTL_TXQ0_OPERATION_MODE_TQS_SHIFT         16
#define EQOS_MTL_TXQ0_OPERATION_MODE_TQS_MASK          (0x1FF << 16)
#define EQOS_MTL_TXQ0_OPERATION_MODE_TXQEN_SHIFT       2
#define EQOS_MTL_TXQ0_OPERATION_MODE_TXQEN_ENABLED     (0x2 << 2)
#define EQOS_MTL_TXQ0_OPERATION_MODE_TSF               BIT1
#define EQOS_MTL_RXQ0_OPERATION_MODE_OFST              (0xD30)
#define EQOS_MTL_RXQ0_OPERATION_MODE_RQS_SHIFT         20
#define EQOS_MTL_RXQ0_OPERATION_MODE_RQS_MASK          (0x3FF << 20)
#define EQOS_MTL_RXQ0_OPERATION_MODE_RSF               BIT5
#define EQOS_MTL_RXQ0_OPERATION_MODE_EHFC              BIT7
#define EQOS_MTL_RXQ0_OPERATION_MODE_RFD_SHIFT         14
#define EQOS_MTL_RXQ0_OPERATION_MODE_RFA_SHIFT         8

// DMA (EQoS): mode @0x1000, ch0 regs @0x1100+
#define DW_EMAC_DMAGRP_MODE_OFST                       (0x1000)
#define DW_EMAC_DMAGRP_MODE_SWR_SET_MSK                BIT0
#define DW_EMAC_DMAGRP_MODE_SWR_GET(value)             (((value) & BIT0) >> 0)
#define EQOS_DMA_SYSBUS_MODE_OFST                      (0x1004)
#define EQOS_DMA_SYSBUS_MODE_BLEN16                    BIT3
#define EQOS_DMA_SYSBUS_MODE_BLEN8                     BIT2
#define EQOS_DMA_SYSBUS_MODE_BLEN4                     BIT1
#define EQOS_DMA_SYSBUS_MODE_EAME                      BIT11
#define EQOS_DMA_SYSBUS_MODE_RD_OSR_LMT_SHIFT          16
#define EQOS_DMA_CH0_CONTROL_OFST                      (0x1100)
#define EQOS_DMA_CH0_CONTROL_PBLX8                     BIT16
#define EQOS_DMA_CH0_CONTROL_DSL_SHIFT                 18
#define EQOS_DMA_CH0_CONTROL_DSL_MASK                  (0x7 << 18)
#define EQOS_DMA_CH0_TX_CONTROL_OFST                   (0x1104)
#define EQOS_DMA_CH0_TX_CONTROL_ST                     BIT0
#define EQOS_DMA_CH0_TX_CONTROL_OSP                    BIT4
#define EQOS_DMA_CH0_TX_CONTROL_TXPBL_SHIFT            16
#define EQOS_DMA_CH0_TX_CONTROL_TXPBL_MASK             (0x3F << 16)
#define EQOS_DMA_CH0_RX_CONTROL_OFST                   (0x1108)
#define EQOS_DMA_CH0_RX_CONTROL_SR                     BIT0
#define EQOS_DMA_CH0_RX_CONTROL_RBSZ_SHIFT             1
#define EQOS_DMA_CH0_RX_CONTROL_RBSZ_MASK              (0x3FFF << 1)
#define EQOS_DMA_CH0_RX_CONTROL_RXPBL_SHIFT            16
#define EQOS_DMA_CH0_RX_CONTROL_RXPBL_MASK             (0x3F << 16)
#define EQOS_DMA_CH0_TXDESC_LIST_ADDRESS_OFST          (0x1114)
#define EQOS_DMA_CH0_RXDESC_LIST_ADDRESS_OFST          (0x111C)
#define EQOS_DMA_CH0_TXDESC_TAIL_POINTER_OFST          (0x1120)
#define EQOS_DMA_CH0_RXDESC_TAIL_POINTER_OFST          (0x1128)
#define EQOS_DMA_CH0_TXDESC_RING_LENGTH_OFST           (0x112C)
#define EQOS_DMA_CH0_RXDESC_RING_LENGTH_OFST           (0x1130)

// EQoS descriptor control bits (Des3)
#define TDES0_OWN                                      BIT31
#define TDES0_TXINT                                    BIT30
#define TDES0_TXLAST                                   BIT29
#define TDES0_TXFIRST                                  BIT28
#define TDES0_TXCRCDIS                                 BIT27
#define EQOS_DESC3_FD                                  BIT29
#define EQOS_DESC3_LD                                  BIT28
#define EQOS_DESC3_BUF1V                               BIT24
#define EQOS_DESC3_OWN                                 BIT31
#define RX_LENGTH_MASK                                 0x7FFF

// RX status bits (Des3 of RX descriptor after reception)
#define RDES0_CE                                       BIT1
#define RDES0_DBE                                      BIT2
#define RDES0_RE                                       BIT3
#define RDES0_RWT                                      BIT4
#define RDES0_LC                                       BIT6
#define RDES0_GF                                       BIT7
#define RDES0_OE                                       BIT11
#define RDES0_LE                                       BIT12
#define RDES0_SAF                                      BIT13
#define RDES0_DE                                       BIT14
#define RDES0_ES                                       BIT15
#define RDES0_AFM                                      BIT30
#define RDES0_OWN                                      BIT31

// Descriptor ring sizes (matching the SNP driver's EMAC_DRIVER)
#define CONFIG_TX_DESCR_NUM                            2
#define CONFIG_RX_DESCR_NUM                            2
#define CONFIG_ETH_BUFSIZE                             2048
#define TX_TOTAL_BUFSIZE                               (CONFIG_ETH_BUFSIZE * CONFIG_TX_DESCR_NUM)
#define RX_TOTAL_BUFSIZE                               (CONFIG_ETH_BUFSIZE * CONFIG_RX_DESCR_NUM)

// BSP descriptor size: dwmac-5.20 on A733 (axi_bus_width=8) computes
  // desc_step = 16 + DSL_MASK*8 = 72 > ARCH_DMA_MINALIGN(64), so each
  // descriptor occupies 64 bytes and DSL = (64-16)/8 = 6.  Descriptor N
  // lives at base + N*64, NOT base + N*16 - the DMA engine walks the ring
  // in 64-byte steps.  U-Boot dwc_eth_qos does exactly this (eqos_setup
  // / eqos_probe_resources_core).  Using 16-byte-spaced descriptors with
  // DSL=0 left the ring at 16-byte pitch, so the engine read garbage at
  // base+16 and never consumed OWN (TX dead).
  #define EQOS_DESC_SIZE                                (64)
  #define EQOS_DMA_CH0_CONTROL_DSL_OFST                  (6 << 18)
  #define EQOS_DMA_CH0_CONTROL_DSL_MASK                  (0x7 << 18)

typedef struct {
  UINT32 Des0;   // buffer address (TX/RX)
  UINT32 Des1;   // unused (0)
  UINT32 Des2;   // TX: length; RX: 0
  UINT32 Des3;   // OWN bit31, FD bit29, LD bit28, BUF1V bit24; RX len in [14:0]
} DESIGNWARE_HW_DESCRIPTOR;

typedef struct {
  EFI_PHYSICAL_ADDRESS        AddrMap;
  void                        *Mapping;
} MAP_INFO;

typedef struct {
  DESIGNWARE_HW_DESCRIPTOR    *TxdescRing[CONFIG_TX_DESCR_NUM];
  DESIGNWARE_HW_DESCRIPTOR    *RxdescRing[CONFIG_RX_DESCR_NUM];
  CHAR8                       TxBuffer[TX_TOTAL_BUFSIZE];
  CHAR8                       RxBuffer[RX_TOTAL_BUFSIZE];
  MAP_INFO                    TxdescRingMap[CONFIG_TX_DESCR_NUM ];
  MAP_INFO                    RxdescRingMap[CONFIG_RX_DESCR_NUM ];
  MAP_INFO                    RxBufNum[CONFIG_TX_DESCR_NUM];
  UINT32                      TxCurrentDescriptorNum;
  UINT32                      TxNextDescriptorNum;
  UINT32                      RxCurrentDescriptorNum;
  UINT32                      RxNextDescriptorNum;
} EMAC_DRIVER;

VOID
EFIAPI
EmacSetMacAddress (
  IN  EFI_MAC_ADDRESS         *MacAddress,
  IN  UINTN                   MacBaseAddress
  );

VOID
EFIAPI
EmacReadMacAddress (
  OUT EFI_MAC_ADDRESS         *MacAddress,
  IN  UINTN                   MacBaseAddress
  );

EFI_STATUS
EFIAPI
EmacDxeInitialization (
  IN  EMAC_DRIVER             *EmacDriver,
  IN  UINTN                   MacBaseAddress
  );

EFI_STATUS
EFIAPI
EmacDmaInit (
  IN  EMAC_DRIVER             *EmacDriver,
  IN  UINTN                   MacBaseAddress
  );

EFI_STATUS
EFIAPI
EmacSetupTxdesc (
  IN  EMAC_DRIVER             *EmacDriver,
  IN  UINTN                   MacBaseAddress
 );

EFI_STATUS
EFIAPI
EmacSetupRxdesc (
  IN  EMAC_DRIVER             *EmacDriver,
  IN  UINTN                   MacBaseAddress
  );

VOID
EFIAPI
EmacStartTransmission (
  IN  UINTN                   MacBaseAddress
  );

EFI_STATUS
EFIAPI
EmacRxFilters (
  IN  UINT32                  ReceiveFilterSetting,
  IN  BOOLEAN                 Reset,
  IN  UINTN                   NumMfilter             OPTIONAL,
  IN  EFI_MAC_ADDRESS         *Mfilter               OPTIONAL,
  IN  UINTN                   MacBaseAddress
  );

UINT32
EFIAPI
GenEtherCrc32 (
  IN  EFI_MAC_ADDRESS         *Mac,
  IN  UINT32 AddrLen
  );

UINT8
EFIAPI
BitReverse (
  UINT8                       Value
  );

VOID
EFIAPI
EmacStopTxRx (
  IN  UINTN                   MacBaseAddress
  );

EFI_STATUS
EFIAPI
EmacDmaStart (
  IN  UINTN                   MacBaseAddress
  );


VOID
EFIAPI
EmacGetDmaStatus (
  OUT UINT32                  *IrqStat  OPTIONAL,
  IN  UINTN                   MacBaseAddress
  );

VOID
EFIAPI
EmacGetStatistic (
  IN  EFI_NETWORK_STATISTICS *Stats,
  IN  UINTN                  MacBaseAddress
  );

VOID
EFIAPI
EmacConfigAdjust (
  IN  UINT32                 Speed,
  IN  UINT32                 Duplex,
  IN  UINTN                  MacBaseAddress
  );

#endif // EMAC_DXE_UTIL_H__
