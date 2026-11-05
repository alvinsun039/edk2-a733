#!/usr/bin/env bash
# Copyright (c) 2026, edk2-a733 contributors.
# SPDX-License-Identifier: BSD-2-Clause-Patent
# pack.sh - build an Allwinner boot_package (TOC1 fex) from the EDK2 FD plus
# the monitor, ARISC and OP-TEE payloads.  The spare head is generated.
#
# Usage: tools/pack/pack.sh [out.fex]
#
# Required inputs (env-overridable):
#   FD        EDK2 FD      build-workspace/Build/CubieA7A/DEBUG_GCC/FV/CUBIEA7A_EFI.fd
#   BL31      TF-A BL31    $EDK2_AW/tf-a/build/sun60iw2p1/release/bl31.bin
#
# Optional inputs (env-overridable; omitted from the package when the file
# is absent, which leaves that TOC entry zero-length):
#   SCP       ARISC fw     $UBOOT_AW/arisc/ar100s/scp.bin
#   SCP_DTS_SRC ARISC DT source  Platform/Radxa/CubieA7APkg/scp_dts.dts
#   SCP_DTS   ARISC DT     build-workspace/scp_dts.dtb (generated from
#                          SCP_DTS_SRC with dtc; override to use a prebuilt
#                          blob)
#   OPTEE     OP-TEE       $EDK2_AW/device-a733/bin/optee_sun60iw2p1.bin
# The vendor payloads are not part of this repository; see README.md.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
EDK2_AW=${EDK2_AW:-$HOME/workdir/edk2-aw}
UBOOT_AW=${UBOOT_AW:-$HOME/workdir/u-boot-aw2501}

FD=${FD:-$REPO/build-workspace/Build/CubieA7A/DEBUG_GCC/FV/CUBIEA7A_EFI.fd}
BL31=${BL31:-$EDK2_AW/tf-a/build/sun60iw2p1/release/bl31.bin}
SCP=${SCP:-$UBOOT_AW/arisc/ar100s/scp.bin}
SCP_DTS_SRC=${SCP_DTS_SRC:-$REPO/Platform/Radxa/CubieA7APkg/scp_dts.dts}
OPTEE=${OPTEE:-$EDK2_AW/device-a733/bin/optee_sun60iw2p1.bin}
OUT=${1:-$REPO/build-workspace/a7a_m1.fex}

# The ARISC FDT is built from its DTS at pack time.  An explicit SCP_DTS
# environment setting is taken as a prebuilt blob and is not regenerated.
if [ -z "${SCP_DTS:-}" ]; then
  SCP_DTS=$REPO/build-workspace/scp_dts.dtb
  if [ -f "$SCP_DTS_SRC" ]; then
    command -v dtc >/dev/null 2>&1 || {
      echo "ERROR: dtc not found (needed to build $SCP_DTS_SRC)" >&2
      exit 2
    }
    mkdir -p "$(dirname "$SCP_DTS")"
    dtc -q -I dts -O dtb -o "$SCP_DTS" "$SCP_DTS_SRC"
  fi
fi

for f in "$FD" "$BL31"; do
  [ -f "$f" ] || {
    echo "ERROR: input not found: $f" >&2
    echo "       override FD/BL31; see README.md" >&2
    exit 2
  }
done

ARGS=("$FD" "$BL31" "$OUT")
for name in SCP SCP_DTS OPTEE; do
  eval "path=\$$name"
  if [ -f "$path" ]; then
    case "$name" in
      SCP)     ARGS+=(--scp "$path") ;;
      SCP_DTS) ARGS+=(--scp-dts "$path") ;;
      OPTEE)   ARGS+=(--optee "$path") ;;
    esac
  else
    echo "pack.sh: $name not found ($path), leaving that entry empty" >&2
  fi
done

python3 "$HERE/pack.py" "${ARGS[@]}"