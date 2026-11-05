<!-- Copyright (c) 2026, edk2-a733 contributors. -->
<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# edk2-a733 — UEFI firmware for the Allwinner A733 (Radxa Cubie A7A)

An EDK2 platform and SoC package for the Allwinner A733 (sun60iw2p1), as used
on the Radxa Cubie A7A.  It boots Android from the eMMC's GPT partitions,
serves fastboot over the OTG port, brings up HDMI, and keeps its variables in
the board's SPI NOR.

Not an official Radxa or Allwinner project, and not endorsed by either.

## Status

| Area | State |
|---|---|
| eMMC (SMHC3, 8-bit) | works; DDR52, HS200 and HS400 negotiated per card capability; **180 MB/s** on 256 KiB reads at HS400 |
| microSD (SMHC0, 4-bit, 3.3 V) | controller and SD identification implemented; slot reports "no media" until a card is verified on hardware |
| Block stack | `EmmcDxe` + `SdDxe` from MdeModulePkg, GPT, FAT |
| Console | 16550 UART at 0x02500000, 115200 8N1; ConPlatform binds it, so the UiApp menus take arrow keys |
| Display | works; `A733HdmiDxe` brings DE3.0/TCON3/DWC-HDMI up from cold, reads EDID and publishes the board's GOP |
| Boot manager | UiApp with the board logo and a five-second countdown; boot order is Android, Debian, fastboot, Shell |
| OS hand-off | `BootAndroid` (GKI boot_a/init_boot_a/vendor_boot_a from GPT) and `BootDebian` (EFI-stub `\Image` + initrd); the device tree is published through the EFI configuration table |
| ACPI | FACP/APIC/GTDT/PPTT/SPCR/DBG2/DSDT installed at EndOfDxe; the shipping kernel is DT-only, so this is for ACPI-aware operating systems |
| SMBIOS | Type 0/1/2/3/4/16/17/19/32 with the real board identity |
| RTC | `Hym8563RtcLib` backs GetTime/SetTime with the HYM8563TS on S_TWI1; the chip has no backup cell, so it reports "invalid" after a cold boot |
| System suspend (S3) | works; PSCI hands the platform to the ARISC super-standby and a HYM8563 RTC alarm resumes it.  Repeated `rtcwake -m mem` cycles complete in ~7 s with the wake attributed to the hym8563 IRQ.  The aic USB combo module's firmware resets can abort a cycle, so set `avoid_reset_quirk` on its USB device before measuring |
| USB host (USB1) | works; EHCI + OHCI published, devices behind the on-board hub enumerate (keyboard, mass storage) |
| USB host (USB0, OTG port) | EHCI0 published so its host half gets the controller reset the port needs; the MUSB core then owns the pads |
| USB device (OTG port) | works; `fastboot` enumerates as 1f3a:1010 and downloads at **3.27 MB/s** |
| USB host (USB3, DWC3) | xHCI2 works; the 4-port hub, keyboard, mass storage and the WiFi module enumerate |
| Ethernet (GMAC0) | works; 1000 Mb/s link, DHCP and ping verified from the UEFI Shell |
| PCIe / NVMe | driver ported and wired, but disabled by `PcdSunxiPcieEnable`: the combo SerDes lane is the one USB3 uses |
| Everything else | not implemented (camera, video decode, NPU, thermal/fan control) |

Speed negotiation is data-verified at every step: each mode is entered only
if the card advertises it, the sample/strobe delay is swept and each tap is
judged on the bytes it returns, and any failure reverts to the previous mode
before anything else is tried.  See
`Silicon/Allwinner/A733Pkg/Drivers/A733SmhcDxe/` for the ladder and its
revert paths.

## Drivers

SoC blocks are driven by this tree's code; the class drivers and protocol
stacks above them come from edk2.

| Block | This tree | edk2 side | State |
|---|---|---|---|
| SMHC0/2/3 | `A733SmhcDxe` (EFI_SD_MMC_PASS_THRU) | `EmmcDxe`, `SdDxe`, `DiskIoDxe`, `PartitionDxe`, FAT | works |
| SPI0 NOR | `A733FvbDxe` + `A733SpiNor.c` (JEDEC/SFDP) | `VariableRuntimeDxe`, `FaultTolerantWriteDxe` | works, variables persist |
| DE3.0 / TCON3 / DWC-HDMI | `A733HdmiDxe` | `GraphicsConsoleDxe`, `BootLogoLib` | works |
| USB1 EHCI/OHCI | `A733UsbHostDxe`, `SunxiOhciDxe` | `EhciDxe`, `SunxiOhciDxe`, `UsbBusDxe` | works |
| USB0 MUSB (device) | `A733MusbDeviceDxe` | `AndroidFastbootTransportUsbDxe`, `AndroidFastbootApp` | works |
| USB3 DWC3 | `A733Xhci2Dxe` | `XhciDxe`, `UsbBusDxe` | works |
| GMAC0 | `SunxiGmacDxe`, `DwEmacSnpDxe`, `A733ClockLib` | `SnpDxe`, `MnpDxe`, `ArpDxe`, `Ip4Dxe`, `Dhcp4Dxe`, UDP/TCP, PXE | works; DHCP + ping verified |
| S_TWI0 / S_TWI1 | `A733TwiLib`, `A733PmicLib`, `Hym8563RtcLib` | `RealTimeClockRuntimeDxe` | works |
| ARISC (E902) | `AriscStartupDxe`, `A733PlatformPeiLib` | - | starts boot0's ARISC package, reserves the configuration FDT page boot0 hands over at 0x41200000; the ARISC runs the super-standby flow |
| GPIO / CCU | `A733GpioLib`, `A733CcuLib`, `A733ClockLib` | - | works |
| PCIe | `SunxiPcieDxe` | `NvmExpressDxe` | disabled by PCD |
| ACPI tables | `AcpiPlatformDxe`, `AcpiTables` | `AcpiTableDxe`, `BootGraphicsResourceTableDxe` | installed |
| SMBIOS | `SunxiSmbiosDxe` | `SmbiosDxe` | installed |
| RNG | `SunxiRngDxe` (PRNG, Raw) | `DxeNetLib` dependency | works |
| Boot | `PlatformBootManagerLib`, `BootAndroid`, `BootDebian`, `CubieA7ALogoDxe` | `BdsDxe`, `UiApp`, Shell | works |

`DwEmacSnpDxe`, `SunxiGmacDxe`, `SunxiRngDxe`, `SunxiPcieDxe`, `SunxiSmbiosDxe`,
`SunxiOhciDxe`, `AcpiPlatformDxe`/`AcpiTables`, `A733HdmiDxe` and the
`A733*` display/clock libraries are ports; see Provenance.

## Building

Requirements:

- an **unmodified** edk2 checkout (this tree is a platform/silicon package on
  top of it, not a fork)
- an AArch64 GCC toolchain (`aarch64-linux-gnu-`)

```sh
git clone https://github.com/tianocore/edk2.git ~/edk2
git -C ~/edk2 checkout 4208d71dab          # the commit this tree is verified against
EDK2=~/edk2 ./build.sh
EDK2=~/edk2 ./build.sh --verbose           # add VERBOSE_SERIAL: full DEBUG_INFO
```

That commit is what the firmware in this repository was last built and booted
against.  It matters: `edk2` moves, and this tree names files by path.  Three
paths have already moved once - `ArmTransferListLib` (ArmPkg to MdePkg), the
AArch64 exception handler (ArmPkg's `ArmExceptionLib` to
`UefiCpuPkg/Library/CpuExceptionHandlerLib`, with `DefaultExceptionHandlerLib`
dropped entirely), and `GptLib`, which `PartitionDxe` now expects the platform
to name.  Against an older checkout those three lines in the DSC are wrong,
and against a newer one they may be again.

The quiet build keeps `DEBUG_ERROR|DEBUG_WARN`; `--verbose` adds the INFO
trace, which costs several seconds per boot over a 115200 baud UART.  The
firmware volume lands at
`$WORKSPACE/Build/CubieA7A/DEBUG_GCC/FV/CUBIEA7A_EFI.fd`, with `WORKSPACE`
defaulting to `build-workspace/` beside `build.sh` and overridable:

```sh
EDK2=~/edk2 WORKSPACE=~/build-ws-pristine ./build.sh
```

## Running it on the board

The FD is only the BL33 payload.  A bootable medium also needs components
that are **not** in this repository and are not covered by its licence:
Allwinner BL31/ARISC images, the FES loader, the vendor OP-TEE blob and the
packaged `*.fex` container (the spare head itself is generated).  The two Python tools
in `tools/pack/` assemble them and drive the flasher; see
`tools/pack/README.md` for the input paths, the container layout and the
license boundary.

```sh
EDK2=~/edk2 ./build.sh
tools/pack/pack.sh                         # -> build-workspace/a7a_m1.fex
# fes1 comes from the spl-pub tree: make p=sun60iw2p1 b=a733 fes
sudo python3 tools/pack/flash.py flash 24576 a7a_m1.fex \
        --fes1 fes1_sun60iw2p1.bin
```

Flashing goes through FEL/FES over the OTG connector: the board is put into
FEL (send `2` on the UART while it powers up, or `reboot efex` from a
running system).  `flash.py` loads `fes1_sun60iw2p1.bin` at 0x90000 with
the TFES magic, so fes1 trains the LPDDR5 on a cold FEL and enters the FES
USB stack in the same run; the fex is then written to the boot package LBA
(24576 on eMMC).  `boot0` (LBA 16 on eMMC / 256 on SD) is not part of the
fex.

`flash.py boot <fes1>` stops at FES, `burn <addr> <file>` and
`dump <addr> <len> <file>` talk to it, and `--type dram` switches the
address from 512-byte sectors to a DRAM address.

### Replacing the vendor U-Boot with `dd`

The boot package lives at **LBA 24576 (12 MiB)** on both eMMC and SD.  The
vendor layout reserves the first 16 MiB (`[mbr] size = 16384` in
`sys_partition.fex`), so the package has 4 MiB from 12 MiB up to the first
partition.  `boot0` is outside that area and keeps running: it reads the
package at 24576 and starts what it finds.

There are two ways to do the same write: patch the image file before it is
written to the card, or write the stock image first and then patch the
card.  The result is identical.

Patching the image file - Radxa ships the Cubie A7A image as `.img.xz`, so
decompress it first.  `conv=notrunc` is required, or dd truncates the image
right after the fex:

```sh
xz -dk radxa-cubie-a7a.img.xz            # -> radxa-cubie-a7a.img
cp radxa-cubie-a7a.img cubie-a7a-edk2.img
dd if=build-workspace/a7a_m1.fex of=cubie-a7a-edk2.img \
        bs=512 seek=24576 conv=notrunc
# write cubie-a7a-edk2.img to the card with dd, Etcher or Radxa's flasher
```

Patching the card or the eMMC directly, after the stock image is on it.
Unmount the medium first, or do it from another machine:

```sh
lsblk                                    # the whole disk, e.g. /dev/sdX
sudo dd if=/dev/sdX of=vendor-boot-package.bin bs=512 skip=24576 count=8192
sudo dd if=build-workspace/a7a_m1.fex of=/dev/sdX bs=512 seek=24576 conv=fsync
```

`count=8192` backs up the whole 4 MiB area, which is more than the fex
needs.  The fex must stay under 4 MiB.  Read the header back, check the
magic, then the length boot0 will use:

```sh
# on the card
sudo dd if=/dev/sdX bs=512 skip=24576 count=1 2>/dev/null | xxd -l 48
# or in the image file
xxd -s $((24576*512)) -l 48 cubie-a7a-edk2.img
# 0x00 "sunxi-package", 0x10 0x89119800, 0x24 valid_len == fex size
```

A shorter fex leaves the old tail in place; boot0 stops at `valid_len`, so
those bytes are ignored.  Zero them only if the leftover matters.

This swaps the entire boot package - the EDK2 BL33, BL31, the ARISC
firmware and its DTB - and nothing else.  `boot0` and the partitions are
untouched, so the image must still contain what the EDK2 boot manager can
start: the Android `boot_a`/`init_boot_a`/`vendor_boot_a` GPT partitions,
or an EFI-stub `\Image` plus initrd on a FAT ESP.  A stock U-Boot image
that boots from extlinux files will stop in the EDK2 Boot Manager instead.

Serial console: UART0 at 0x02500000, 115200 8N1, no flow control.  On boot
the UiApp shows the logo and counts down five seconds; ESC enters the
Boot Manager, ENTER boots the first option at once, and with no key the
first `BootOrder` entry (Android) starts.

If the boot chain is damaged, the board still comes up in FEL: flashing the
fex again (and `boot0` if it was touched) is the recovery path.

## The rest of the boot chain

The FD is BL33 only.  Everything before it, and the two blobs that sit beside
it in the boot package, come from trees that are not part of this repository:

| component | where it comes from | how it is produced |
|---|---|---|
| BL31 (TF-A monitor) | the vendor TF-A fork, `sun60iw2p1` platform | `tf-a/build.sh` (`make PLAT=sun60iw2p1 SPD=opteed bl31`) -> `build/sun60iw2p1/release/bl31.bin` |
| ARISC firmware (`scp.bin`) | the vendor ARISC tree | `arisc/build.sh`; the ARISC is a T-HEAD XuanTie RISC-V core whose `crt0.S` needs the T-HEAD GCC 8.4.0 toolchain (`ARISC_TOOLS`), stock GCC rejects its CSRs |
| OP-TEE (BL32) | vendor blob, `device-a733/bin/optee_sun60iw2p1.bin` | not built here |
| FES loader (`fes1_sun60iw2p1.bin`) | `fes/` in the `u-boot-aw2501/spl-pub` tree: this project's entry/shims plus the vendor `sunxi_usb` stack, built in-tree | `make p=sun60iw2p1 b=a733 fes` -> `out/sun60iw2p1/bin/fes1_sun60iw2p1.bin`; needs the vendor `dramlib` object (`FES_DRAM_LIB`, default `../dramlib/sun60iw2p1/spl_libdram/libdram`) |
| U-Boot spare head | built into `tools/pack/pack.py` | `SPARE_HEAD_WORDS` plus the jump, lengths and checksum it computes; byte-identical to the vendor image's first 0x640 bytes, so it is not an input at all |
| `boot0` | vendor binary | written to LBA 16 (eMMC) / 256 (SD), outside the fex |

Those trees usually sit next to each other in one vendor workspace, whose
top-level `build.sh` runs the whole chain in order - BL31, ARISC, the EDK2 FD,
then the pack step.  This repository's `tools/pack/pack.sh` does the same
packing with the same inputs; `BL31` is required and `--scp`, `--scp-dts`,
`--optee` are optional (an omitted stage stays a zero-length TOC entry).
Pointing it at a freshly built BL31 and `scp.bin` is enough to turn this
tree's FD into a flashable package.

None of that vendor material is redistributed here; see `NOTICE` and
`tools/pack/README.md` for the boundary.

## Layout

| path | contents |
|---|---|
| `Platform/Radxa/CubieA7APkg/` | DSC/FDF, platform libraries, boot manager, logo |
| `Silicon/Allwinner/A733Pkg/` | SoC package: drivers, libraries, ACPI tables, include |
| `tools/pack/` | fex builder and FEL/FES flasher |
| `build.sh` | build wrapper around an external edk2 checkout |

## Provenance

The code was written for this port with AI assistance and validated on the
board after each step (build, flash, boot, measure).  Register offsets, bit
fields, command codes and timing parameters were taken as *facts* from public
documentation and from vendor sources that were read for reference; the
implementation is independent — no vendor source file was copied, and a
similarity check against the Allwinner kernel/U-Boot MMC and USB drivers found
no shared expression beyond stock EDK2 boilerplate.  Where a value is not what
the manual gives (the SPI-NOR backend's, and a list of them in the USB path)
the comment next to it says what it is, what failed without it, and which
firmware on this board confirmed it.  See `NOTICE` for the sources consulted
and `CONTRIBUTORS` for authorship.

Several subsystems are ports from the upstream
[edk2-a733](https://github.com/vehoelite/edk2-a733) reference implementation,
which is under the same licence.  That project is separate and is not
authored by this port: the HDMI display stack and its clock/power/mode
libraries, the GMAC bring-up and DesignWare SNP driver, the network-stack
configuration driver, the minimal RNG protocol, the PCIe root-complex
driver, the OHCI core, the SMBIOS records, and the ACPI platform driver with
its tables are ported with attribution.  Each ported file carries the
derivation line in its header and `NOTICE` lists them with their upstream
copyrights.  Everything else - the SMHC ladder, the two USB stacks, the
MUSB device core, fastboot, the boot manager, the NOR variable store and the
TWI/RTC support - is original code for this port.

## References

- This repository: <https://github.com/alvinsun039/edk2-a733>
- Upstream edk2-a733 reference port (separate project, not this port's
  author): <https://github.com/vehoelite/edk2-a733>
- RK3588 EDK2 port: <https://github.com/edk2-porting/edk2-rk3588>
- TianoCore EDK2: <https://github.com/tianocore/edk2>
- edk2-rockchip ACPI/DT templates:
  <https://github.com/edk2-rockchip/edk2-rockchip>

## Licence

BSD-2-Clause-Patent; see `LICENSE`.  Contributions are accepted under the
same licence with a `Signed-off-by` (DCO), see `CONTRIBUTORS`.