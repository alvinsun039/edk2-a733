#!/usr/bin/env bash
# Copyright (c) 2026, edk2-a733 contributors.
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# Build the Cubie A7A firmware against an unmodified edk2 checkout.
#
#   --verbose  bring-up firmware: -D VERBOSE_SERIAL -> the full DEBUG_INFO
#              trace over the serial console (defaults are ERROR|WARN only;
#              the INFO trace costs several seconds of every boot over a
#              115200 baud UART).
#   --sd       microSD build: PcdA733BootMedium=0, so the variable snapshot
#              lives on the card's FAT volume.
#   --emmc     eMMC build: PcdA733BootMedium=1, so it lives on the eMMC's.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDK2="${EDK2:-$HERE/edk2}"

BUILD_FLAGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --verbose) BUILD_FLAGS+=("-D" "VERBOSE_SERIAL") ;;
    --sd)      BUILD_FLAGS+=("--pcd" "gCubieA7APkgTokenSpaceGuid.PcdA733BootMedium=0") ;;
    --emmc)    BUILD_FLAGS+=("--pcd" "gCubieA7APkgTokenSpaceGuid.PcdA733BootMedium=1") ;;
    *) BUILD_FLAGS+=("$1") ;;
  esac
  shift
done

[ -d "$EDK2" ] || { echo "edk2 not found at $EDK2; set EDK2=<path>" >&2; exit 2; }

# A pristine EDK2 checkout has no BaseTools binaries.  build.py expects
# libCommon.a and the C tools to exist; build them once here.
if [ ! -f "$EDK2/BaseTools/Source/C/libs/libCommon.a" ]; then
  echo "== building EDK2 BaseTools"
  make -C "$EDK2/BaseTools" -j"$(nproc)"
fi

# WORKSPACE is overridable so a pristine checkout can be built beside the
# working one instead of on top of it.
export WORKSPACE="${WORKSPACE:-$HERE/build-workspace}"
mkdir -p "$WORKSPACE/Conf"
# A pristine checkout has no Conf/*.txt: edksetup.sh copies them out of
# BaseTools/Conf/*.template on its first run.  Fall back to the template so
# the build does not depend on edksetup having been run in that tree already.
for f in target.txt tools_def.txt build_rule.txt; do
  [ -f "$WORKSPACE/Conf/$f" ] && continue
  if [ -f "$EDK2/Conf/$f" ]; then
    cp "$EDK2/Conf/$f" "$WORKSPACE/Conf/$f"
  else
    cp "$EDK2/BaseTools/Conf/${f%.txt}.template" "$WORKSPACE/Conf/$f"
  fi
done
# $HERE first: a shared edk2 checkout may also carry a copy of this platform
# from another port, and the platform path alone would be ambiguous.
export PACKAGES_PATH="$HERE:$EDK2"
export EDK_TOOLS_PATH="$EDK2/BaseTools"
export PYTHONPATH="$EDK_TOOLS_PATH/Source/Python"
export PYTHON_COMMAND="${PYTHON_COMMAND:-python3}"
export GCC_AARCH64_PREFIX="${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}"

set +u
. "$EDK2/edksetup.sh" BaseTools >/dev/null
set -u

build -p Platform/Radxa/CubieA7APkg/CubieA7A.dsc -a AARCH64 -t GCC -b DEBUG "${BUILD_FLAGS[@]}"
