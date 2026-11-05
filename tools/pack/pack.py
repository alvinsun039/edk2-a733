#!/usr/bin/env python3
# Copyright (c) 2026, edk2-a733 contributors.
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""pack.py - Allwinner boot_package (TOC1) builder.

Replaces the three Allwinner closed-source tools the vendor flow uses:
  - script          (sys_config.fex -> sys_config.bin)  -> not used here
  - update_uboot    (spare-head checksum/length patch)  -> inlined
  - dragonsecboot   (TOC1 sunxi-package container)      -> inlined

Layout (vendor offsets; scp/dtb/optee move forward with 0x100 alignment
when the wrapped monitor outgrows its slot):
  u-boot   @0x800    len 0x184000  (0x640 spare head, pad to 0x1000, EDK2 FD)
  monitor  @0x184800 len = BL31 wrapped in the 0x1000 monitor container
  scp      @0x197c00 len = scp.bin  (ARISC E906 firmware)
  dtb      @0x1b5400 len = scp_dts.dtb (ARISC config FDT, 984 B)
  optee    @0x1b5800 len = optee.bin (BL32; zero-length entry if omitted)

Every entry is always written; pass a zero-length file to leave one empty.
boot0 validates the TOC1 magic 0x89119800 and the container add_sum; the
spare-head checksum is filled the same way update_uboot fills it.

Usage:
  pack.py <edk2_fd> <bl31.bin> <out.fex> [--scp scp.bin]
          [--scp-dts scp_dts.dtb] [--optee optee.bin]

The three -- options are optional: omitting one leaves its TOC1 entry
zero-length, which is what the vendor container does when a stage is not
shipped (OP-TEE is the usual case).

  edk2_fd        EDK2 FD (CUBIEA7A_EFI.fd, Platform/Radxa/CubieA7APkg)
  bl31.bin       bare BL31 (wrapped here) or an already wrapped monitor
  scp.bin        ARISC firmware
  scp_dts.dtb    ARISC configuration FDT (generated from scp_dts.dts)
"""
import argparse
import struct
import sys

STAMP = 0x5F0A6C39          # spare-head checksum stamp
PAYLOAD_BASE = 0x41000000   # where boot0 copies the u-boot segment
FD_BASE = 0x41001000        # EDK2 FD load address (FDF BaseAddress)
HEAD_SIZE = 0x640           # spare_boot_ctrl_head + spare_boot_data_head

#
# The spare head is a 0x640-byte structure of which only these words are
# non-zero in the vendor image.  The jump at 0x00, the lengths at 0x14/0x18,
# the load address at 0x2C and the checksum at 0x0C are written by
# make_uboot_segment(); the rest is fixed data (magic, version strings and
# four values boot0 reads back but does not derive).  Keeping them here
# removes the vendor U-Boot binary from the pack inputs.
#
SPARE_HEAD_WORDS = {
    0x004: b'uboot\x00\x00\x00',
    0x010: struct.pack('<I', 0x00004000),
    0x01C: b'4.0.0\x00\x00\x00',
    0x024: b'2.0.0\x00\x00\x00',
    0x0B0: struct.pack('<I', 0x000003F0),
    0x0B4: struct.pack('<I', 0x000004B0),
    0x4EC: struct.pack('<I', 0x00008020),
    0x508: struct.pack('<I', 0x4A0BFDE4),
}


def make_head_template():
    """Build the 0x640-byte spare head without the vendor U-Boot image."""
    head = bytearray(HEAD_SIZE)
    for offset, data in SPARE_HEAD_WORDS.items():
        head[offset:offset + len(data)] = data
    return bytes(head)
UBOOT_SLOT = 0x800          # TOC1 entry offset (absolute in package)
MONITOR_OFF = 0x184800      # vendor default; moved if the monitor outgrows it
SCP_OFF = 0x197C00          # vendor default; moved after a larger monitor
DTB_OFF = 0x1B5400          # vendor default; moved after a larger scp


def make_uboot_segment(fd_data):
    """Spare head (0x640) + pad to 0x1000 + FD, padded to a 4 KB boundary.

    The length is what the TOC entry and the spare head record - it does not
    have to be the vendor's 0x184000, only large enough for the FD and small
    enough to stop before the monitor.  boot0 copies exactly that many bytes
    and jumps, and reads the monitor's own offset from the TOC.
    """
    head = bytearray(make_head_template())
    # jump_instruction: head -> FD+4 (the FD trampoline's branch)
    struct.pack_into('<I', head, 0x00,
                     struct.unpack('<I', beq_to(FD_BASE + 4 - PAYLOAD_BASE))[0])
    seg = bytes(head) + b'\x00' * (0x1000 - HEAD_SIZE) + fd_data
    length = (len(seg) + 0xFFF) & ~0xFFF
    seg = bytearray(seg)
    # length / uboot_length: whole segment (update_uboot pads to 0x184000)
    struct.pack_into('<I', seg, 0x14, length)
    struct.pack_into('<I', seg, 0x18, length)
    struct.pack_into('<I', seg, 0x2C, PAYLOAD_BASE)
    if len(seg) < length:
        seg += b'\xff' * (length - len(seg))
    # Match the storage fields the vendor update_uboot writes; boot0
    # overwrites the runtime-relevant ones itself.
    if len(seg) > 0x3E8: struct.pack_into('<I', seg, 0x3E8, 0xFFFFFFFF)
    if len(seg) > 0x3EC: struct.pack_into('<I', seg, 0x3EC, 0xFFFFFFFF)
    if len(seg) > 0x504: struct.pack_into('<I', seg, 0x504, 0x01000000)
    # checksum: STAMP-in-place, sum all 32-bit words up to length
    b = bytearray(seg[:length])
    struct.pack_into('<I', b, 0x0C, STAMP)
    acc = 0
    for i in range(0, length & ~3, 4):
        acc += struct.unpack_from('<I', b, i)[0]
    struct.pack_into('<I', seg, 0x0C, acc & 0xFFFFFFFF)
    return bytes(seg)


def beq_to(off_bytes):
    """AArch64 `B <off_bytes>` from the head start."""
    assert off_bytes % 4 == 0 and 0 <= off_bytes < 128 * 1024 * 1024
    return struct.pack('<I', 0x14000000 | (off_bytes >> 2))


MONITOR_LOAD_BASE = 0x48000000      # boot0 copies the monitor segment here
MONITOR_IMG_BASE = MONITOR_LOAD_BASE + 0x1000
MONITOR_VERSION = b"v0.1-A7A-open"  # informational version field


def ensure_monitor_container(bl31):
    """Wrap a raw TF-A bl31.bin in the Allwinner "monitor" container that
    boot0 expects for the TOC1 monitor segment (copy to 0x48000000, jump).

    A bare bl31.bin (raw binary, ARM code at offset 0) has no container
    header and boot0's jump lands mid-image - the board stalls with no
    output.  The shipped closed-source bl31.bin already carries the header
    (magic "monitor" at +0x04); detect it and pass through unchanged.

    Container page 0 (0x1000 bytes): b 0x1000 @0x00, "monitor" @0x04,
    "CHKv1.0" @0x1C, version @0xC8, zero pad; raw image follows.
    """
    if bl31[4:12] == b"monitor\x00":
        return bl31
    hdr = bytearray(0x1000)
    struct.pack_into('<I', hdr, 0x00, struct.unpack('<I', beq_to(MONITOR_IMG_BASE - MONITOR_LOAD_BASE))[0])
    hdr[0x04:0x0C] = b"monitor\x00"
    hdr[0x1C:0x24] = b"CHKv1.0\x00"
    hdr[0x24:0x2C] = b"monitor\x00"
    # +0x2C: load base; the closed container carries the same value
    struct.pack_into('<I', hdr, 0x2C, MONITOR_LOAD_BASE)
    hdr[0xC8:0xC8 + len(MONITOR_VERSION)] = MONITOR_VERSION
    wrapped = bytes(hdr) + bl31
    print(f"monitor: wrapped bare BL31 {len(bl31)}B -> {len(wrapped)}B container")
    return wrapped


def verify_addsum(pkg, valid_len):
    """Recompute the TOC add_sum with the STAMP word in place."""
    vl = valid_len & ~3
    b = bytearray(pkg[:vl])
    saved = struct.unpack_from('<I', b, 0x14)[0]
    struct.pack_into('<I', b, 0x14, STAMP)
    acc = 0
    for i in range(0, vl, 4):
        acc += struct.unpack_from('<I', b, i)[0]
    return saved, acc & 0xFFFFFFFF


def read_optional(path):
    """Payload of an optional stage: empty when it is not shipped."""
    if path is None:
        return b''
    return open(path, 'rb').read()


def main():
    ap = argparse.ArgumentParser(
        description='Build an Allwinner boot_package (TOC1) from an EDK2 FD.')
    ap.add_argument('fd', help='EDK2 FD (CUBIEA7A_EFI.fd)')
    ap.add_argument('bl31', help='TF-A BL31 monitor')
    ap.add_argument('out', help='output .fex')
    ap.add_argument('--scp', help='ARISC firmware (optional)')
    ap.add_argument('--scp-dts', dest='scp_dts',
                    help='ARISC configuration FDT (optional)')
    ap.add_argument('--optee', help='OP-TEE BL32 (optional)')
    args = ap.parse_args()

    fd = open(args.fd, 'rb').read()
    mon = ensure_monitor_container(open(args.bl31, 'rb').read())
    scp = read_optional(args.scp)
    dtb = read_optional(args.scp_dts)
    optee = read_optional(args.optee)
    out = args.out

    uboot = make_uboot_segment(fd)
    print(f"u-boot: {len(uboot):#x}B")

    # Layout: u-boot @0x800 and monitor @0x184800 are fixed (matched against
    # the vendor output for the closed BL31).  scp/dtb/optee follow the real
    # monitor size, packed with 0x100 alignment, so a larger open BL31 (debug
    # build with USB efex stack) never overlaps the next segment.  When the
    # fixed vendor offsets still fit they are kept byte-identical.
    mon_off = max(MONITOR_OFF, (UBOOT_SLOT + len(uboot) + 0xFF) & ~0xFF)
    mon_end = mon_off + len(mon)
    scp_off = SCP_OFF if mon_end <= SCP_OFF else ((mon_end + 0xFF) & ~0xFF)
    scp_end = scp_off + len(scp)
    dtb_off = DTB_OFF if scp_end <= DTB_OFF else ((scp_end + 0xFF) & ~0xFF)
    dtb_end = dtb_off + len(dtb)
    optee_off = ((dtb_end + 0xFF) & ~0xFF)
    print(f"layout: monitor@{mon_off:#x}->{mon_end:#x}, "
          f"scp@{scp_off:#x}->{scp_end:#x}, dtb@{dtb_off:#x}->{dtb_end:#x}, "
          f"optee@{optee_off:#x}->{optee_off + len(optee):#x}")

    sections = [('u-boot', UBOOT_SLOT, uboot),
                ('monitor', mon_off, mon),
                ('scp', scp_off, scp),
                ('dtb', dtb_off, dtb),
                ('optee', optee_off, optee)]
    TBL = 0x40 + len(sections) * 0x170
    entries = []
    blob = bytearray()
    for name, fixed, data in sections:
        pos = fixed - TBL
        assert pos >= 0, f"{name}: offset {fixed:#x} < table {TBL:#x}"
        if len(blob) < pos:
            blob += b'\x00' * (pos - len(blob))   # vendor pads gaps with 0x00
        blob += data
        entries.append((name, fixed, len(data)))

    items_nr = len(entries)
    head = bytearray(0x40)
    head[0:16] = b'sunxi-package\x00\x00\x00'
    struct.pack_into('<I', head, 0x10, 0x89119800)
    struct.pack_into('<I', head, 0x20, items_nr)
    struct.pack_into('<I', head, 0x24, len(blob) + 0x40 + items_nr * 0x170)
    head[0x3C:0x40] = b'MIE;'

    table = bytearray()
    for name, off, ln in entries:
        e = bytearray(0x170)
        e[0:len(name)] = name.encode()
        struct.pack_into('<I', e, 0x40, off)
        struct.pack_into('<I', e, 0x44, ln)
        struct.pack_into('<I', e, 0x4C, 3)           # type=3: bin
        struct.pack_into('<I', e, 0x16C, 0x3B454949)  # end marker
        table += e

    pkg = bytearray(bytes(head) + bytes(table) + bytes(blob))
    # valid_len: end of the last section, zero-padded to a 0x4000 boundary
    # the way the vendor dragonsecboot does (0x1b8000 when optee is empty).
    body_end = 0x40 + items_nr * 0x170 + len(blob)
    valid = (body_end + 0x3FFF) & ~0x3FFF
    if len(pkg) < valid:
        pkg += b'\x00' * (valid - len(pkg))
    struct.pack_into('<I', pkg, 0x24, valid)

    # add_sum (verify_addsum: +0x14 STAMP -> whole-package word sum)
    b = bytearray(pkg[:valid])
    struct.pack_into('<I', b, 0x14, STAMP)
    acc = 0
    for i in range(0, valid & ~3, 4):
        acc += struct.unpack_from('<I', b, i)[0]
    struct.pack_into('<I', pkg, 0x14, acc & 0xFFFFFFFF)

    saved, calc = verify_addsum(bytes(pkg), valid)
    ok = saved == calc
    print(f"valid_len={valid:#x} pkg={len(pkg)} add_sum={saved:#x} verify={calc:#x} "
          f"{'OK' if ok else 'FAIL'}")
    if not ok:
        return 1
    for name, off, ln in entries:
        print(f"  {name:8s} @{off:#x} len={ln:#x}")

    pad = (512 - len(pkg) % 512) % 512
    with open(out, 'wb') as f:
        f.write(bytes(pkg) + b'\x00' * pad)
    print(f"wrote {out} ({len(pkg) + pad}B, 512-aligned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
