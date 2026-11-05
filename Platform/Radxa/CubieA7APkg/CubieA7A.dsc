## @file
#  Radxa Cubie A7A platform: minimal build (serial + DXE core).
#
#  Copyright (c) 2026, edk2-a733 contributors.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME           = CubieA7A
  PLATFORM_GUID           = b2c3d4e5-6f70-4182-93a4-a7a000000002
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS           = DEBUG
  SKUID_IDENTIFIER        = DEFAULT
  FLASH_DEFINITION        = Platform/Radxa/CubieA7APkg/CubieA7A.fdf

[LibraryClasses.common]
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  ArmCcaInitPeiLib|ArmVirtPkg/Library/ArmCcaInitPeiLibNull/ArmCcaInitPeiLibNull.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  # Moved from ArmPkg/Library to MdePkg/Library upstream; same class.
  ArmTransferListLib|MdePkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  ArmPlatformLib|Silicon/Allwinner/A733Pkg/Library/A733PlatformLib/A733PlatformLib.inf
  # A733 CCU: module gates/resets, clock parents, PLL and CPU clocks.
  A733CcuLib|Silicon/Allwinner/A733Pkg/Library/A733CcuLib/A733CcuLib.inf
  # A733 clock/reset abstraction shared with the GMAC bring-up.
  A733ClockLib|Silicon/Allwinner/A733Pkg/Library/A733ClockLib/A733ClockLib.inf
  # A733 GPIO / pin controller: mux, pull, drive, level, bank dump.
  A733GpioLib|Silicon/Allwinner/A733Pkg/Library/A733GpioLib/A733GpioLib.inf
  # A733 TWI: polled I2C master on S_TWI0.
  A733TwiLib|Silicon/Allwinner/A733Pkg/Library/A733TwiLib/A733TwiLib.inf
  # A733 system DMA controller: channels, descriptors, one-package transfers.
  # No consumer today: both DMACs read as secure (SEC = 0) from the normal
  # world, so no transfer can actually be programmed - A733MusbDeviceDxe
  # documents the failed attempt it was written for.  Kept because the block
  # exists and the day BL31 opens it, the driver is already written.
  A733DmacLib|Silicon/Allwinner/A733Pkg/Library/A733DmacLib/A733DmacLib.inf
  # A733 PMIC: AXP8191 CLDO rails over S_TWI0.
  A733PmicLib|Silicon/Allwinner/A733Pkg/Library/A733PmicLib/A733PmicLib.inf
  # A733 PCK600 power domains: DE_SYS / VO1 rails for the display engine.
  A733PowerLib|Silicon/Allwinner/A733Pkg/Library/A733PowerLib/A733PowerLib.inf
  # A733 display clocks: keyed AHB/MBUS gates, DE0, HDMI/vo1, HDMI TV rate.
  A733DisplayClockLib|Silicon/Allwinner/A733Pkg/Library/A733DisplayClockLib/A733DisplayClockLib.inf
  # A733 HDMI preset-mode table (shared with the Display Mode selection).
  A733HdmiModeLib|Silicon/Allwinner/A733Pkg/Library/A733HdmiModeLib/A733HdmiModeLib.inf
  # BGRT driver (BootGraphicsResourceTableDxe) consumes this to decode the
  # logo BMP for the Boot Graphics Resource Table, and publishes
  # gEdkiiBootLogo2ProtocolGuid, which BootLogoLib's progress bar draws through.
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  # This board's SMHC controller table: which controllers exist, and the base,
  # interrupt, CCU module, pad group and card type of each.  A733SmhcDxe is
  # driven from it, so a different board changes this library and nothing else.
  A733SmhcBoardLib|Platform/Radxa/CubieA7APkg/Library/A733SmhcBoardLib/A733SmhcBoardLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  # AArch64 exception handling moved out of ArmPkg into UefiCpuPkg's
  # CpuExceptionHandlerLib; only DxeCore and ArmPkg/Drivers/CpuDxe consume the
  # class here, and both are DXE, so the DXE instance is the one to use.
  # DefaultExceptionHandlerLib went with it - nothing in the current tree
  # references the class any more.
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf
  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  NULL|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  # PartitionDxe's GPT code is a library of its own from this EDK2 on.
  GptLib|MdeModulePkg/Library/GptLib/GptLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsicArmVirt.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf
  BaseUcs2Utf8Lib|RedfishPkg/Library/BaseUcs2Utf8Lib/BaseUcs2Utf8Lib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  PostCodeLib|MdePkg/Library/BasePostCodeLibDebug/BasePostCodeLibDebug.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  PlatformBootManagerLib|Platform/Radxa/CubieA7APkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  ReportStatusCodeLib|MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLibNull.inf
  # NS16550 serial port.  The PCDs below are the whole difference between
  # this SoC's UART0 and a PC's COM1; there is no A733-specific code in it.
  SerialPortLib|MdeModulePkg/Library/BaseSerialPortLib16550/BaseSerialPortLib16550.inf
  # BaseSerialPortLib16550's constructor chain asks for the platform hook; this
  # platform's hook has nothing to do (the pad mux and the module clock are the
  # BL31/DTB's, not the firmware's).
  PlatformHookLib|MdeModulePkg/Library/BasePlatformHookLibNull/BasePlatformHookLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf

  AcpiLib|EmbeddedPkg/Library/AcpiLib/AcpiLib.inf
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  ArmSvcLib|MdePkg/Library/ArmSvcLib/ArmSvcLib.inf
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  BmpSupportLib|MdeModulePkg/Library/BaseBmpSupportLib/BaseBmpSupportLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
  DpcLib|NetworkPkg/Library/DxeDpcLib/DxeDpcLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  IpIoLib|NetworkPkg/Library/DxeIpIoLib/DxeIpIoLib.inf
  LzmaDecompressLib|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  NetLib|NetworkPkg/Library/DxeNetLib/DxeNetLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  PciCf8Lib|MdePkg/Library/BasePciCf8Lib/BasePciCf8Lib.inf
  PciLib|MdePkg/Library/BasePciLibCf8/BasePciLibCf8.inf
  PeiServicesLib|MdePkg/Library/PeiServicesLib/PeiServicesLib.inf
  PlatformPeiLib|ArmPlatformPkg/PlatformPei/PlatformPeiLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  #
  # Real time: the board's HYM8563TS on S_TWI1.  The virtual library this
  # used to use only remembered a value in a variable; the chip is the only
  # clock the board has.
  #
  RealTimeClockLib|Silicon/Allwinner/A733Pkg/Library/Hym8563RtcLib/Hym8563RtcLib.inf
  # PSCI SYSTEM_RESET/SYSTEM_OFF into BL31 (SMC; the ArmMonitorLib conduit
  # defaults to SMC on this SoC) - consumed by ResetSystemRuntimeDxe.
  ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  RngLib|MdePkg/Library/BaseRngLib/BaseRngLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  UdpIoLib|NetworkPkg/Library/DxeUdpIoLib/DxeUdpIoLib.inf
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf
  #
  # Runtime drivers keep their DEBUG calls but must stop making them once the OS
  # owns the console: this instance registers an ExitBootServices event and
  # returns before touching SerialPortLib afterwards.  Without it a runtime
  # driver's diagnostic line is a raw UART write racing the OS's serial driver -
  # and with a 16550 SerialPortLib whose THRE wait has no bound, a UART the OS
  # gated would hang the runtime call.  The variable store's own paths are
  # additionally gated in code (A733_NV_LOG), but this mapping is what covers
  # every runtime driver and library they call, without each having to remember.
  #
  DebugLib|MdePkg/Library/DxeRuntimeDebugLibSerialPort/DxeRuntimeDebugLibSerialPort.inf

[LibraryClasses.common.DXE_CORE]
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  PerformanceLib|MdeModulePkg/Library/DxeCorePerformanceLib/DxeCorePerformanceLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

[LibraryClasses.common.SEC]
  MemoryInitPeiLib|ArmPlatformPkg/MemoryInitPei/MemoryInitPeiLib.inf
  #
  # Platform override: the default only publishes the FV HOB.  This board's
  # boot0 hands the ARISC configuration FDT over at 0x41200000, inside the
  # system memory the DXE core allocates from; the override reserves that
  # page while the HOB list is still being built (see the library header).
  #
  PlatformPeiLib|Platform/Radxa/CubieA7APkg/Library/A733PlatformPeiLib/A733PlatformPeiLib.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf

[PcdsFeatureFlag.common]
  #
  # PrePi publishes the memory type information HOB, so DxeCore lays out its
  # per-type page bins from a fixed size instead of discovering them; that keeps
  # the memory map identical from boot to boot.  Without it DxeCore and
  # MemoryBin both warn that the HOB is missing and S4 could not resume.
  #
  gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob|TRUE

[PcdsFixedAtBuild.common]
  #
  # OS config-table hand-off read by AcpiPlatformDxe: bit0 installs ACPI,
  # bit1 keeps the device tree (0x3 = both).  The device tree stays the
  # default hand-off; the ACPI tables ride along for an OS that asks for
  # them (arm64 Linux needs acpi=force when a DT is also present).
  #
  gA733TokenSpaceGuid.PcdConfigTableMode|0x3

  # Five seconds of BdsWait: the logo stays up while the progress bar runs
  # (PlatformBootManagerWaitCallback), ESC drops into the Boot Manager and
  # ENTER boots the first BootOrder entry right away.  The default 0xFFFF
  # would park BDS in BdsWait for 18 hours; 0 would skip the window entirely.
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|5

  #
  # UART0 (0x02500000), 24 MHz input clock, 115200 8N1, registers one word
  # apart.  Every one of these is read by BaseSerialPortLib16550.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x02500000
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialClockRate|24000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialBaudRate|115200
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterStride|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseMmio|TRUE
  # 32, not the library's 8-bit default: this controller is a 32-bit register
  # block behind a 4-byte stride, and the library then accesses it with
  # MmioRead32/MmioWrite32, the width every other driver here uses on it.
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterAccessWidth|32
  # LCR: 8 data bits, no parity, one stop bit.  This is the console's contract.
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialLineControl|0x03
  # FCR: FIFO enable, clear both queues - the library's default, stated because
  # its transmit loop sizes its burst from this.
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialFifoControl|0x07
  #
  # The serial console is a VT100 terminal (tmux/telnet, minicom, ...):
  # TerminalDxe must speak VT100 so UiApp's forms and the Shell's cursor
  # keys work over the UART, not the default PC-ANSI mapping.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|1
  # The board wires TX/RX only; flow control would make every write wait on a
  # modem status line that is never asserted.
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseHardwareFlowControl|FALSE
  # PcdSerialPciDeviceInfo is deliberately left at its default ({0xFF}), which is
  # what makes the library use PcdSerialRegisterBase as a fixed address instead
  # of walking a PCI bus this SoC does not have.

  # Console level, and why it is a build flag.  The DXE dispatcher, the
  # platform drivers and BDS all print their bring-up trace at DEBUG_INFO:
  # ~2200 lines over UART at 115200 baud, and SerialPortWrite blocks, so the
  # trace alone costs several seconds of every boot (measured: ~12 s to
  # fastboot-visible with it, and the same boot loses multiple seconds with
  # it masked, measured: ~12 s to fastboot-visible with the trace and ~4.8 s
  # with the default below).  The quiet default needs the SMHC driver's own
  # post-tuning readiness wait (see A733SmhcCard.c): with the trace masked the
  # boot reached the partition scan before the card had finished programming,
  # and the GPT-entry read timed out on every block device.  -D VERBOSE_SERIAL
  # gives ERROR|INIT|WARN|INFO|VERBOSE (0x8000004F) for bring-up debugging.
  # MdePkg.dec default is 0x80000000 (errors only); do not go below
  # ERROR|WARN or asserts get unreadable again.
  #
  !ifdef VERBOSE_SERIAL
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F
  !else
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000002
  !endif
  # ASSERT enabled (0x01) with the breakpoint (0x20) and dead-loop (0x40)
  # bits off, so a failed assertion prints its file and line over the serial
  # console and execution continues - on a headless board a dead loop in the
  # middle of a flash cycle costs a power cycle and hides everything after it.
  # ASSERT was disabled here while the platform had no forms driver, because
  # HiiDatabase's ReadyToBoot callback asserts on an empty HII database;
  # SetupBrowserDxe and DisplayEngineDxe are in the image now, so the reason is
  # gone and the assertions are worth having back.
  # Bits: 0x01 ASSERT, 0x02 PRINT, 0x04 DEBUG_CODE, 0x08 CLEAR_STACK_ON_SWITCH.
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x0F

  # The CCU sits at its manual address; the CPU PLL block is separate.
  gA733TokenSpaceGuid.PcdA733CcuBase|0x02002000
  gA733TokenSpaceGuid.PcdA733CpuPllBase|0x08870000

  # Debug knob: set TRUE to print the whole clock tree once from
  # PlatformBootManagerAfterConsole (one DEBUG_INFO line per node).
  gA733TokenSpaceGuid.PcdA733CcuDumpEnable|FALSE

  # The pin controller sits at its manual address (8 KB window).
  gA733TokenSpaceGuid.PcdA733GpioBase|0x02000000

  #
  # Fastboot's USB identity: the pair the board's BSP presents on the OTG port,
  # consumed by EmbeddedPkg's fastboot USB transport for its device descriptor.
  # With these the host sees 1f3a:1010 and "fastboot devices" needs no rule.
  #
  gEmbeddedTokenSpaceGuid.PcdAndroidFastbootUsbVendorId|0x1F3A
  gEmbeddedTokenSpaceGuid.PcdAndroidFastbootUsbProductId|0x1010

  #
  # Boot the fastboot application before the Shell, so the OTG port is live as
  # soon as the firmware is up (the application's prompt takes RETURN or SPACE
  # and BDS then boots the Shell).  FALSE for a Shell-first board.
  #
  gCubieA7APkgTokenSpaceGuid.PcdA733FastbootBoot|TRUE

  #
  # Mirror the NV variable set to \EFI\A7A\vars.bin on the SD card (or the
  # first FAT volume without one) and restore it onto a store that has never
  # been written.  The NOR store stays authoritative.
  #
  gCubieA7APkgTokenSpaceGuid.PcdA733VarSnapshotEnable|TRUE

  #
  # 0 = microSD build, 1 = eMMC build.  Selects which volume class
  # \EFI\A7A\vars.bin lives on.  Override on the build command line:
  #   build ... --pcd gCubieA7APkgTokenSpaceGuid.PcdA733BootMedium=1
  #
  gCubieA7APkgTokenSpaceGuid.PcdA733BootMedium|0

  # Debug knob: set TRUE to print every GPIO bank's configured pins once from
  # PlatformBootManagerAfterConsole, next to the clock tree.
  gA733TokenSpaceGuid.PcdA733GpioDumpEnable|FALSE

  # Debug knob: set TRUE to print one DEBUG_INFO line per eMMC command, with the
  # command's raw interrupt status and its response words.
  gA733TokenSpaceGuid.PcdA733SmhcDumpEnable|FALSE

  # S_TWI0 base and its 24 MHz APBS1 input clock; R_PIO base, which holds the
  # PL0/PL1 S-TWI0 pins the TWI library muxes.
  gA733TokenSpaceGuid.PcdA733TwiBase|0x07083000
  gA733TokenSpaceGuid.PcdA733TwiClockHz|24000000
  gA733TokenSpaceGuid.PcdA733RpioBase|0x07025000

  # Debug knob: set TRUE to trace every PMIC register read and write from
  # A733PmicLib.  The chip-ID and CLDO5 result line prints either way.
  gA733TokenSpaceGuid.PcdA733PmicVerbose|FALSE

  #
  # OS hand-off command lines.
  #
  # BootDebian appends PcdBootDebianCmdline to \Image's LoadOptions: the
  # console wiring this board has (UART0 at 0x02500000), and panic=10 so a
  # faulting boot comes back to the menu instead of hanging.
  #
  gA733TokenSpaceGuid.PcdBootDebianCmdline|L"console=ttyS0,115200 earlycon=uart,mmio32,0x02500000 root=PARTUUID=00000000-02 rootwait rw panic=10"

  # BootExtlinux resolves an extlinux FDTDIR entry to this board file before
  # falling back to the first .dtb in that directory.  The distro tree
  # (linux-image-*) names the Cubie A7A device tree after the board.
  #
  gA733TokenSpaceGuid.PcdBootExtlinuxFdtFile|L"sun60i-a733-cubie-a7a.dtb"

  # BootAndroid appends PcdBootAndroidExtraCmdline to the vendor_boot
  # cmdline.  androidboot.slot_suffix=_a boots the A slot and drives the
  # /first_stage_ramdisk/fstab.<hardware> lookup; without fstab_suffix and
  # hardware the first-stage mount fails and init reboots to the
  # bootloader.  clk_ignore_unused keeps the display clocks the kernel's
  # driver does not claim alive across the hand-off.
  #
  gA733TokenSpaceGuid.PcdBootAndroidExtraCmdline|L"console=ttyAS0,115200 androidboot.slot_suffix=_a androidboot.verifiedbootstate=orange androidboot.fstab_suffix=sun60iw2p1 androidboot.hardware=sun60iw2p1 earlycon=uart,mmio32,0x02500000 clk_ignore_unused"

  # FD is loaded by TF-A at PRELOADED_BL33_BASE.
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x41001000
  gArmTokenSpaceGuid.PcdFdSize|0x00180000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x41002000

  #
  # Non-volatile variables.  These name a DRAM window, not flash: the variable
  # driver reads its store as plain memory and only uses the block protocol to
  # erase and program, so A733FvbDxe reserves this window as
  # EfiRuntimeServicesData, fills it from the board's SPI NOR and keeps the two
  # in step.  On the flash side the window is the last 192 KB of the part: chip
  # offset 0x7D0000 on the 8 MB W25Q64FW the board carries, 0xF00000 on a
  # 16 MB one.  Either way the low part - where a NOR boot image would live -
  # is untouched, and the eMMC is not involved at all.
  #
  # The address is fixed at 0x43000000 because the variable driver only ever
  # looks at what these PCDs say: it has to be known before anything is
  # allocated.  It sits above the FD (0x41001000 + 0x180000 ends at 0x42801000)
  # and below the BL31 image at 0x48000000, and no driver in this build asks
  # for memory there.
  #
  # Variable, FTW working and FTW spare are contiguous, one 64 KB region each,
  # which is the NOR window's own layout.  The block size is 4 KB throughout
  # (the NOR's sector erase unit).
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0x43000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase|0x43000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x00010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase64|0x43010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase|0x43010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x00010000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase64|0x43020000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase|0x43020000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x00010000
  # The store is the NOR's, not RAM's: variables survive a power cycle, and an
  # OS SetVariable is served at runtime by A733FvbDxe.
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|FALSE

  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x200000000

  #
  # GICv3 (GIC-600).  Distributor at 0x03400000; the redistributor region
  # starts at 0x03460000 and holds one 0x20000 frame per core (the DTB's
  # interrupt-controller@3400000 declares reg = 0x3400000/0x10000,
  # 0x3460000/0xFF004, i.e. 8 contiguous frames).  ArmGicDxe derives the
  # frame for the running core as base + Aff1 * 0x20000, SGI_base =
  # frame + 0x10000, which matches the A733's flat DynamIQ layout.
  #
  # BDS looks this up in the FV to create the boot-manager-menu option;
  # the MdeModulePkg default is a placeholder that no image carries.
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0x21, 0xaa, 0x2c, 0x46, 0x14, 0x76, 0x03, 0x45, 0x83, 0x6e, 0x8a, 0xb6, 0xf4, 0x66, 0x23, 0x31 }
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x03400000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x03460000

  #
  # 8 Cortex-A55 in one flat DynamIQ cluster with the core index in Aff1
  # (Aff0 always 0), so the boot core is Aff1=0/Aff0=0.  PcdArmPrimaryCore
  # and the cluster count are unused until an MP-services driver is added,
  # but they are the platform's statement of the topology.
  #
  gArmTokenSpaceGuid.PcdArmPrimaryCore|0x0000
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|8

  #
  # The generic timer's frequency is *not* a PCD in this edk2 revision:
  # ArmGenericTimerPhyCounterLib reads CNTFRQ_EL0, which BL31 programs to
  # 24 MHz on the A733 (the DTB's timer_arch node declares
  # clock-frequency = 0x16E3600 = 24000000).  The PPI numbers TimerDxe uses
  # come from the ArmPkg.dec defaults (Sec 29 / NS 30 / Virt 27 / Hyp 26)
  # and match the same DTB node.
  #

[PcdsDynamicDefault.common]
  #
  # Console geometry.  PcdConOut* are PatchableInModule-or-later in
  # MdeModulePkg.dec, so the platform must fix them here for ConSplitterDxe
  # to see anything but the DEC default; 100x31 is what the A733 port uses
  # on this edk2 revision.  This is also what makes Pcd.inf (and the DXE PCD
  # database) part of the image: BdsDxe reads PcdTestKeyUsed, which is
  # declared Dynamic-only.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|100
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|31
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|100
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|31

[PcdsDynamicHII.common]
  #
  # Display Mode selection.  A733HdmiDxe reads the "DisplayModePreset"
  # variable directly and falls back to the 1080p60 anchor when it is
  # absent, so these two entries exist to give the CUSTOM timing path a
  # home (and the variable a documented namespace); with no Setup
  # application in this tree nothing writes them.
  #
  gA733TokenSpaceGuid.PcdDisplayModePreset|L"DisplayModePreset"|gA733SetupFormSetGuid|0x0|{ 0x0 }
  gA733TokenSpaceGuid.PcdDisplayModeCustom|L"DisplayModeCustom"|gA733SetupFormSetGuid|0x0|{ 0x0 }

[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER]
  #
  # DxeCore protects EfiRuntimeServicesCode images only when their sections
  # are aligned to RUNTIME_PAGE_ALLOCATION_GRANULARITY, which AArch64 sets to
  # 64 KB.  The tool chain default is 4 KB, so every runtime driver (Runtime,
  # ResetSystem, Variable, Fvb, RTC, ...) was loaded, found unalignable and
  # left RWX with "Image Section Alignment(0x1000) does not match Required
  # Alignment (0x10000)".  Link those images 64 KB aligned; DXE_DRIVER and
  # the applications stay at 4 KB (their requirement is EFI_PAGE_SIZE).
  #
  GCC:*_GCC_*_DLINK_FLAGS = -z common-page-size=0x10000

[Components.AARCH64]
  #
  # SEC / PrePi: entry from TF-A BL31, straight into DXE (no PEI).
  #
  ArmVirtPkg/PrePi/ArmVirtPrePiUniCoreRelocatable.inf

  #
  # DXE core, plus the PCD driver that backs the Dynamic PCDs above and the
  # DxePcdLib users (BdsDxe, ConSplitterDxe, HiiDatabaseDxe) below.
  #
  MdeModulePkg/Core/Dxe/DxeMain.inf
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  #
  # Architectural protocols.  DxeCore asserts unless every one of these is
  # installed before the dispatch loop drains.
  #
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf {
    <LibraryClasses>
      ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  }
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf

  #
  # Variable services.  The store is the board's SPI NOR, reached through
  # A733FvbDxe: it reserves a DRAM window at the PCD addresses above, fills it
  # from the flash, and publishes the firmware volume block protocol the
  # variable driver erases and programs through.
  #
  # The order below is the FV's dispatch order, and the two depexes make it a
  # requirement rather than a habit: FaultTolerantWriteDxe waits for
  # gEfiFirmwareVolumeBlockProtocolGuid, and VariableRuntimeDxe waits for
  # gEdkiiNvVarStoreFormattedGuid through the NULL library instance, which
  # A733FvbDxe installs once the store's headers are in place.
  #
  Silicon/Allwinner/A733Pkg/Drivers/A733FvbDxe/A733FvbDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      # Makes the variable driver wait for gEdkiiNvVarStoreFormattedGuid
      # instead of reading a store whose headers may not be there yet.
      NULL|EmbeddedPkg/Library/NvVarStoreFormattedLib/NvVarStoreFormattedLib.inf
  }
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf

  #
  # Display: A733HdmiDxe drives the whole pipeline from cold (DE3.0 ->
  # TCON3 -> DWC-HDMI) and publishes the only GOP this board has - there is
  # no pre-initialised onboard panel for a SimpleFB scanout to take over.
  # The entry point only arms a VariableWrite notification, so the pipeline
  # comes up as soon as the variable store is usable, before BDS builds the
  # console.  It reads dynamic PCDs and therefore carries the DxePcdLib
  # override like BdsDxe/UiApp.
  #
  Silicon/Allwinner/A733Pkg/Drivers/A733HdmiDxe/A733HdmiDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  Platform/Radxa/CubieA7APkg/Drivers/CubieA7ALogoDxe/CubieA7ALogoDxe.inf

  #
  # ARISC: the A733's E906 PM co-processor is started by SIP FID 0x8000FF10
  # with the address of its configuration FDT.  BL31 only stages the firmware
  # from the boot package; without this call the core never runs and PSCI
  # SYSTEM_RESET/SYSTEM_OFF wait forever on the unanswered SYS_OP - which is
  # what a `reboot` from Linux looks like: "Restarting system" and nothing.
  #
  Platform/Radxa/CubieA7APkg/Drivers/AriscStartupDxe/AriscStartupDxe.inf

  #
  # ACPI: AcpiTableDxe publishes gEfiAcpiTableProtocol, and the BGRT driver
  # uses it at ReadyToBoot to install the Boot Graphics Resource Table - and,
  # what this tree actually wants from it, produces gEdkiiBootLogo2Protocol
  # so BootLogoLib's EnableLogo/UpdateProgress work as designed.  Nothing
  # else installs ACPI tables: the OS boots from the device tree, so the
  # root table it eventually sees carries only the BGRT.
  #
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  MdeModulePkg/Universal/Acpi/BootGraphicsResourceTableDxe/BootGraphicsResourceTableDxe.inf
  #
  # Installs the FV ACPI tables (AcpiTables.inf, FILE_GUID 7E374E25) at
  # EndOfDxe, gated on PcdConfigTableMode bit0.  With bit0 set - the default
  # here is 0x3, ACPI plus FDT - an OS that asks for ACPI finds a real DSDT;
  # arm64 Linux with a populated device tree keeps using the DT unless
  # acpi=force, so the Android path is unchanged.
  #
  Silicon/Allwinner/A733Pkg/Drivers/AcpiPlatformDxe/AcpiPlatformDxe.inf

  #
  # The tables it installs: ASL built into one FFS by the ACPITABLE rule in
  # CubieA7A.fdf.inc.
  #
  Silicon/Allwinner/A733Pkg/AcpiTables/AcpiTables.inf

  #
  # SMBIOS: the generic producer plus this SoC's Type 0/1/2/3/4/7/16/17/19/32
  # records.  Without them the firmware UI shows the placeholder model.
  #
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  Silicon/Allwinner/A733Pkg/Drivers/SunxiSmbiosDxe/SunxiSmbiosDxe.inf

  #
  # Console over UART0 and, once the GOP is up, over HDMI.
  #
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf {
    <LibraryClasses>
      # Reads the dynamic geometry/video PCDs at console init; with the
      # global BasePcdLibNull this trips LibPcdGet32's ASSERT.
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf

  #
  # Device path services, and the two stacks the UEFI Shell cannot start
  # without: HiiDatabaseDxe produces the HII String/Database/ConfigRouting
  # protocols that UefiHiiServicesLib's constructor locates (Shell.c returns
  # EFI_NOT_STARTED if its HiiAddPackages fails), and EnglishDxe provides
  # EFI_UNICODE_COLLATION2_PROTOCOL, which Shell.c requires at entry.
  #
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf {
    <LibraryClasses>
      # HiiDatabaseDxe reads the DynamicEx PcdNvStoreDefaultValueBuffer.
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  #
  # eMMC and the microSD slot.  A733SmhcDxe is the host: it brings up each
  # controller the board's table declares - SMHC3 for the eMMC, SMHC0 for the
  # slot - and publishes one EFI_SD_MMC_PASS_THRU_PROTOCOL handle per controller.
  # EmmcDxe and SdDxe each bind the handle whose device path node is theirs, and
  # PartitionDxe and Fat are the stock stack above both, which is what turns the
  # handles into the Shell's fs0: and fs1:.
  #
  Silicon/Allwinner/A733Pkg/Drivers/A733SmhcDxe/A733SmhcDxe.inf
  MdeModulePkg/Bus/Sd/EmmcDxe/EmmcDxe.inf
  MdeModulePkg/Bus/Sd/SdDxe/SdDxe.inf
  # Produces EFI_DISK_IO_PROTOCOL on top of BlockIo.  PartitionDxe and Fat
  # both declare it as a dependency, and without it the eMMC's BlockIo
  # handles are never looked at: no partitions, no fs0:.
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
  Ext4Pkg/Ext4Dxe/Ext4Dxe.inf

  #
  # USB, on the board's USB2.0 host port.  A733UsbHostDxe is the SoC glue -
  # PCK600 domain, CCU gates, PHY, port power - and publishes the controller as
  # a non-discoverable PCI device.  Everything above it is stock: the PCI I/O
  # emulation that gives that device a config space with the EHCI class code,
  # EhciDxe that binds to it, UsbBusDxe that enumerates the root hub and the
  # hub behind it, and then the two class drivers the port is for - a keyboard
  # as a console input, a stick as BlockIo, which DiskIoDxe/PartitionDxe/Fat
  # above turn into the Shell's fsN:.
  #
  # OHCI0/OHCI1 are the full/low-speed companions in the next 0x400 block of
  # each USB window; A733UsbHostDxe publishes them and SunxiOhciDxe (ported
  # from edk2-a733) binds them over the same non-discoverable PCI path.
  # Devices behind the board's high-speed hub still reach the EHCI through
  # the hub's transaction translator.
  #
  Silicon/Allwinner/A733Pkg/Drivers/A733UsbHostDxe/A733UsbHostDxe.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  MdeModulePkg/Bus/Pci/EhciDxe/EhciDxe.inf
  Silicon/Allwinner/A733Pkg/Drivers/SunxiOhciDxe/SunxiOhciDxe.inf

  #
  # The USB3.1 GEN2 DRD (DWC3, xHCI2 at 0x06A00000).  A733Xhci2Dxe is that
  # controller's SoC glue - its clock group, the SerDes subsystem it sits
  # behind, its USB2.0 port's PHY, and the core's own reset and host-mode
  # setup - and publishes it the same way.  XhciDxe is the stock xHCI driver
  # that binds to it; UsbBusDxe above then enumerates the root hub, the 4-port
  # hub behind it and whatever the board's three Type-A ports have in them.
  #
  # This is the controller the board's user-reachable USB2 ports are on: the
  # on-board hub's upstream is its USB2.0 port, and the SuperSpeed lane goes to
  # the USB3 receptacle.  The two EHCI controllers above have no part in it -
  # their root ports are empty, which is what their ccs=0 lines in the log say.
  #
  Silicon/Allwinner/A733Pkg/Drivers/A733Xhci2Dxe/A733Xhci2Dxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  #
  # Android fastboot over the OTG port, i.e. the same USB0 window's *device*
  # half (manual 18.5, MUSB at 0x04100000).  A733MusbDeviceDxe owns that core
  # and produces EFI_USB_DEVICE_PROTOCOL; the transport and command engine above
  # it are EmbeddedPkg's stock ones, unchanged, and A733FastbootPlatformDxe is
  # this board's answer to the three questions they ask (partitions, writes,
  # getvar).  Order matters at the top only: the transport locates the device
  # protocol once, in its entry point, and does not retry.
  #
  # The application is reached as a boot option: PlatformBootManagerLib
  # registers it, and PcdA733FastbootBoot decides whether BDS boots it before
  # the Shell.  An application that lives in a firmware volume has no device
  # path to boot it by, which is why it is named there by FILE_GUID.
  #
  Silicon/Allwinner/A733Pkg/Drivers/A733MusbDeviceDxe/A733MusbDeviceDxe.inf
  EmbeddedPkg/Drivers/AndroidFastbootTransportUsbDxe/FastbootTransportUsbDxe.inf
  Silicon/Allwinner/A733Pkg/Drivers/A733FastbootPlatformDxe/A733FastbootPlatformDxe.inf
  EmbeddedPkg/Application/AndroidFastboot/AndroidFastbootApp.inf

  #
  # OS hand-off.  Both are UEFI applications registered as boot options
  # (PlatformBootManagerLib names BootAndroid by FILE_GUID for each disk
  # that carries an Android boot_a; BootDebian is reached from the Shell or
  # a hand-made boot option).  Each loads an EFI-stub kernel and hands it
  # the device tree from the FDT configuration table plus a LoadFile2
  # initrd - no GRUB, no U-Boot.
  #
  Silicon/Allwinner/A733Pkg/Drivers/BootAndroid/BootAndroid.inf
  Silicon/Allwinner/A733Pkg/Drivers/BootDebian/BootDebian.inf
  Silicon/Allwinner/A733Pkg/Drivers/BootExtlinux/BootExtlinux.inf

  #
  # Ethernet (GMAC0): SunxiGmacDxe does the board bring-up (CCU clock/
  # reset, PH0..15 rgmii0 mux, PHY reset) and registers the MAC as a
  # NonDiscoverable device; DwEmacSnpDxe drives the MAC + Maxio PHY and
  # publishes EFI_SIMPLE_NETWORK.  The stack above it is the stock
  # NetworkPkg set used for DHCP/PXE boot.
  #
  NetworkPkg/DpcDxe/DpcDxe.inf
  #
  # DxeNetLib's [Depex] requires gEfiRngProtocolGuid, and the board has no
  # hardware TRNG wired up, so the network stack never dispatches without
  # this.  It only backs DHCP transaction IDs and TCP ISNs.
  #
  Silicon/Allwinner/A733Pkg/Drivers/SunxiRngDxe/SunxiRngDxe.inf
  Silicon/Allwinner/A733Pkg/Drivers/SunxiGmacDxe/SunxiGmacDxe.inf
  Silicon/Allwinner/A733Pkg/Drivers/DwEmacSnpDxe/DwEmacSnpDxe.inf

  #
  # PCIe root complex and the NVMe driver behind it.  SunxiPcieDxe brings
  # the DesignWare RC up from cold and publishes the endpoint it finds as a
  # non-discoverable PCI device, which is how NvmExpressDxe binds without a
  # PciHostBridge/PciBus stack.  Gated by PcdSunxiPcieEnable, FALSE here:
  # the combo SerDes lane is the one A733Xhci2Dxe drives for USB3.
  #
  Silicon/Allwinner/A733Pkg/Drivers/SunxiPcieDxe/SunxiPcieDxe.inf
  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf
  Silicon/Allwinner/A733Pkg/Drivers/NetworkStackConfigDxe/NetworkStackConfigDxe.inf
  NetworkPkg/SnpDxe/SnpDxe.inf {
    <LibraryClasses>
      NULL|Silicon/Allwinner/A733Pkg/Drivers/NetworkStackConfigDxe/NetworkStackEnabledLib.inf
  }
  NetworkPkg/MnpDxe/MnpDxe.inf {
    <LibraryClasses>
      NULL|Silicon/Allwinner/A733Pkg/Drivers/NetworkStackConfigDxe/NetworkStackEnabledLib.inf
  }
  NetworkPkg/ArpDxe/ArpDxe.inf
  NetworkPkg/Dhcp4Dxe/Dhcp4Dxe.inf
  NetworkPkg/Ip4Dxe/Ip4Dxe.inf {
    <LibraryClasses>
      NULL|Silicon/Allwinner/A733Pkg/Drivers/NetworkStackConfigDxe/NetworkStackIpv4EnabledLib.inf
  }
  NetworkPkg/Udp4Dxe/Udp4Dxe.inf
  NetworkPkg/TcpDxe/TcpDxe.inf
  NetworkPkg/Mtftp4Dxe/Mtftp4Dxe.inf
  NetworkPkg/UefiPxeBcDxe/UefiPxeBcDxe.inf {
    <LibraryClasses>
      NULL|Silicon/Allwinner/A733Pkg/Drivers/NetworkStackConfigDxe/NetworkStackPxeBootEnabledLib.inf
  }

  #
  # The form stack UiApp needs.  UiApp's front page is a HII form: it locates
  # EFI_FORM_BROWSER2_PROTOCOL and calls SendForm on it, and without the browser
  # that pointer stays NULL and the first thing the Boot Manager Menu does is
  # dereference it - a data abort at FAR=0 in CallFrontPage (FrontPage.c:329).
  # SetupBrowserDxe provides the protocol, DisplayEngineDxe provides the
  # gEdkiiFormDisplayEngineProtocolGuid the browser draws through; both read
  # Dynamic PCDs, so both need the protocol-backed PcdLib like the other HII
  # consumers here.  Adding these is also what the HiiDatabase note above was
  # waiting for - its ReadyToBoot callback asserts on an empty HII database.
  #
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }

  #
  # Boot manager and the Shell it starts.
  #
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
  }
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf {
    <LibraryClasses>
      # BdsDxe reads PcdTestKeyUsed, which MdeModulePkg.dec declares
      # Dynamic-only, so PcdLib must be the protocol-backed instance.
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellNetwork1CommandsLib/UefiShellNetwork1CommandsLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
    <PcdsFixedAtBuild>
      # The Shell installs its own parameter/environment protocols; the
      # UefiShellLib constructor must not hunt for them at app entry.
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }