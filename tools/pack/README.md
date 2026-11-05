<!-- Copyright (c) 2026, edk2-a733 contributors. -->
<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# tools/pack

Build the Allwinner boot package (`boot_package` / fex) and drive the FEL/FES
flasher.  Everything here is this repository's own code
(BSD-2-Clause-Patent); the vendor payloads it consumes are not checked in.

## Layout

| file | role |
|---|---|
| `pack.py` | TOC1 container builder (replaces `update_uboot` and `dragonsecboot`; does not need `script`) |
| `pack.sh` | wrapper: FD + payloads -> `a7a_m1.fex` |
| `flash.py` | FEL/FES host flasher (usbfs via ctypes, no libusb, no sunxi-fel) |

## Inputs

| input | default | provenance |
|---|---|---|
| `FD` (required) | `build-workspace/Build/CubieA7A/DEBUG_GCC/FV/CUBIEA7A_EFI.fd` | this tree, `./build.sh` |
| `BL31` (required) | `$EDK2_AW/tf-a/build/sun60iw2p1/release/bl31.bin` | TF-A, `tf-a/build.sh` |
| `SCP` (optional) | `$UBOOT_AW/arisc/ar100s/scp.bin` | ARISC E906 firmware, `edk2-aw/arisc/build.sh` |
| `SCP_DTS` (optional) | `build-workspace/scp_dts.dtb` (generated from `Platform/Radxa/CubieA7APkg/scp_dts.dts`) | ARISC config FDT; boot0 copies this item to 0x41200000, where `AriscStartupDxe` picks it up |
| `OPTEE` (optional) | `$EDK2_AW/device-a733/bin/optee_sun60iw2p1.bin` | vendor OP-TEE 3.7 blob (not in this repo) |

`EDK2_AW` defaults to `~/workdir/edk2-aw`, `UBOOT_AW` to
`~/workdir/u-boot-aw2501`; both can be overridden.  `pack.sh` compiles
`SCP_DTS_SRC` with `dtc` and needs it in `PATH`; setting `SCP_DTS` to an
existing file skips the compile and uses that prebuilt blob.

`FD` and `BL31` are required; `SCP`, `SCP_DTS` and `OPTEE` are optional and
become `--scp`, `--scp-dts` and `--optee` on the `pack.py` command line.
Omitting one (or pointing the variable at a missing file when using
`pack.sh`) leaves that TOC entry zero-length, which is what the vendor
container does when a stage is not shipped - OP-TEE is the usual case.

The spare head is not an input either: `pack.py` builds it from
`SPARE_HEAD_WORDS` and fills the jump, lengths, load address and checksum
itself, byte-identical to a head taken from the vendor U-Boot image.

## Usage

```sh
# build the FD first
EDK2=~/workdir/edk2-aw/edk2 ./build.sh

# then pack (output defaults to build-workspace/a7a_m1.fex)
tools/pack/pack.sh
tools/pack/pack.sh /path/to/out.fex

# or call the packer directly; the last three are optional
tools/pack/pack.py FD BL31 OUT.fex --scp scp.bin --scp-dts build-workspace/scp_dts.dtb --optee optee.bin
```

The container layout is fixed by the vendor boot0/TOC1:

| entry | offset | payload |
|---|---|---|
| u-boot | 0x800 | spare head + EDK2 FD, padded to a 4 KB boundary; the length is recorded in the TOC and the spare head |
| monitor | 0x184800 | BL31, wrapped in a 0x1000 header |
| scp | 0x197c00 | ARISC firmware |
| dtb | 0x1b5400 | ARISC configuration FDT |
| optee | 0x1b5800 | OP-TEE |

scp/dtb/optee keep these vendor offsets while the wrapped monitor fits;
a larger u-boot segment or monitor moves the later entries forward with
0x100 alignment, and the TOC carries the offsets boot0 uses.

## Flashing

`flash.py` drives the whole flow itself, no sunxi-fel:

```sh
# board already in FEL
sudo python3 flash.py boot  <fes1.bin>              # load + train + enter FES
sudo python3 flash.py burn  24576 a7a_m1.fex        # FES write, default flash
sudo python3 flash.py dump  24576 65536 out.bin     # FES read
sudo python3 flash.py flash 24576 a7a_m1.fex --fes1 <fes1.bin>   # one shot
```

`<fes1.bin>` is `out/sun60iw2p1/bin/fes1_sun60iw2p1.bin` from the
`u-boot-aw2501/spl-pub` tree (`make p=sun60iw2p1 b=a733 fes`): the vendor
USB/EFEX stack is built there in-tree, and the image trains the DRAM on a
cold FEL before entering FES (TFES magic, one execute).  `--chunk` is
capped at 128 KiB; larger data phases hang the device.  `--type dram` makes
the address a DRAM address instead of a sector.  `boot0` lives at LBA 16
(eMMC) / 256 (SD) and is not part of the fex.

## License boundary

The packer, wrapper and flasher are BSD-2-Clause-Patent.  The payloads from
`device-a733` and `u-boot-aw2501` (OP-TEE, ARISC firmware, configuration FDT,
boot0) are vendor material and stay outside this repository; the spare head
is generated here.  `edk2-aw/tools/pack` still carries an
older copy of the two Python tools for the legacy `CubieA7ABootPkg` build;
this directory is the canonical one for `edk2-a733`.