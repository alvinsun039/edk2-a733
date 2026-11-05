#!/usr/bin/env python3
# Copyright (c) 2026, edk2-a733 contributors.
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""flash.py - self-contained FEL/FES flasher for Allwinner sunxi.

It drives /dev/bus/usb through the usbfs ioctls with ctypes, so it needs
no libusb, no pyusb and no vendor tool: the same file runs on the host or
inside the privileged bring-up container.

Two protocol layers, one USB link:

  FEL  16-byte aw_fel_request + "AWUC"/"AWUS" bulk framing (sunxi-tools
       fel_lib.c): VERSION 0x001, WRITE 0x101, EXEC 0x102, READ 0x103.
  FES  32-byte CBW / 13-byte CSW carrying the 16-byte fes_trans command
       (FES-PROTOCOL.md): down 0x0206, up 0x0207;
       type 0x7f00 = DRAM (last block 0x17f00 = FINISH), other = flash
       (addr = first 512-byte sector, len = bytes).

Subcommands
  boot    <fes1.bin> [--addr A] [--timeout S]
  burn    <addr> <file> [--type flash|dram] [--chunk N<=128K] [--no-verify]
  dump    <addr> <bytes> <file> [--type flash|dram] [--chunk N]
  flash   <addr> <file> --fes1 <bin> [--type ...] [--chunk N]
  fel ver | write <addr> <file> | read <addr> <len> <file> |
      writel <addr> <value> | exec <addr>

`boot` writes fes1 at 0x90000, sets the RTC magic to "TFES" and executes
it once: fes1 trains the DRAM on a cold FEL and then enters the FES USB
stack in the same run.  "FES1" (instead of "TFES") makes the entry skip
training for a board boot0 already trained (detected_r); TFES is also
safe there, it just retrains.

`burn`/`dump` talk to the FES USB stack (endpoints 0x02/0x81); `flash`
runs `boot` and then `burn`.

`fel` is the raw transport for debugging.  Its write/exec run the
sunxi-tools icache-fix stub first, because the BROM exec does not
invalidate the instruction cache.

The board must already be in FEL before `boot`/`flash`; entering FEL
(power cycle, button, serial flood) is the caller's job.

The FES stage-1 load address (0x90000) and the RTC magic (0x07090114)
are the sun60iw2p1 values.

Exit status: 0 on success, 1 on transfer/verify failure, 2 on usage.
"""

import argparse
import ctypes
import glob
import os
import struct
import sys
import time

# --------------------------------------------------------------------------
# usbfs plumbing (raw USBDEVFS ioctls; no libusb, no pyusb)
# --------------------------------------------------------------------------

USBDEVFS_CONTROL = 0xC0185500
USBDEVFS_BULK = 0xC0185502
USBDEVFS_CLAIMINTERFACE = 0x8004550F
USBDEVFS_RELEASEINTERFACE = 0x80045510

USB_TIMEOUT_MS = 15000

libc = ctypes.CDLL(None, use_errno=True)
libc.ioctl.restype = ctypes.c_int


class CtrlTransfer(ctypes.Structure):
    _fields_ = [
        ("bRequestType", ctypes.c_uint8),
        ("bRequest", ctypes.c_uint8),
        ("wValue", ctypes.c_uint16),
        ("wIndex", ctypes.c_uint16),
        ("wLength", ctypes.c_uint16),
        ("timeout", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
    ]


class BulkTransfer(ctypes.Structure):
    _fields_ = [
        ("ep", ctypes.c_uint),
        ("len", ctypes.c_uint),
        ("timeout", ctypes.c_uint),
        ("data", ctypes.c_void_p),
    ]


def _err(msg):
    e = ctypes.get_errno()
    raise RuntimeError(f"{msg}: errno={e} ({os.strerror(e)})")


class UsbDev:
    """One open usbfs device with its bulk endpoints already claimed."""

    def __init__(self, bus, dev, ep_out, ep_in, unbind=True):
        self.bus = bus
        self.dev = dev
        self.ep_out = ep_out
        self.ep_in = ep_in
        self.path = f"/dev/bus/usb/{bus:03d}/{dev:03d}"
        if unbind:
            unbind_kernel_driver(bus, dev)
        self.fd = os.open(self.path, os.O_RDWR)
        self._claim(0)

    def _claim(self, iface):
        n = ctypes.c_uint(iface)
        if libc.ioctl(self.fd, USBDEVFS_CLAIMINTERFACE, ctypes.byref(n)) < 0:
            err = ctypes.get_errno()
            os.close(self.fd)
            ctypes.set_errno(err)
            _err(f"claim interface {iface} on {self.path}")
        self.iface = iface

    def close(self):
        try:
            n = ctypes.c_uint(self.iface)
            libc.ioctl(self.fd, USBDEVFS_RELEASEINTERFACE, ctypes.byref(n))
        except OSError:
            pass
        os.close(self.fd)

    def bulk_out(self, data, timeout=USB_TIMEOUT_MS):
        data = bytes(data)
        buf = ctypes.create_string_buffer(data, len(data))
        bt = BulkTransfer(self.ep_out, len(data), timeout,
                          ctypes.cast(buf, ctypes.c_void_p).value)
        if libc.ioctl(self.fd, USBDEVFS_BULK, ctypes.byref(bt)) != len(data):
            _err(f"bulk OUT ep=0x{self.ep_out:02x} {len(data)}B")

    def bulk_in(self, size, timeout=USB_TIMEOUT_MS):
        buf = ctypes.create_string_buffer(size)
        bt = BulkTransfer(self.ep_in, size, timeout,
                          ctypes.cast(buf, ctypes.c_void_p).value)
        n = libc.ioctl(self.fd, USBDEVFS_BULK, ctypes.byref(bt))
        if n < 0:
            _err(f"bulk IN ep=0x{self.ep_in:02x} {size}B")
        return buf.raw[:n]


# --------------------------------------------------------------------------
# device discovery / kernel-driver detach
# --------------------------------------------------------------------------

AWUSB_VID = 0x1F3A
AWUSB_PID = 0xEFE8


def _usb_devices():
    for path in glob.glob("/sys/bus/usb/devices/*"):
        try:
            vid = int(open(os.path.join(path, "idVendor")).read(), 16)
            pid = int(open(os.path.join(path, "idProduct")).read(), 16)
            bus = int(open(os.path.join(path, "busnum")).read())
            dev = int(open(os.path.join(path, "devnum")).read())
        except (OSError, ValueError):
            continue
        yield path, vid, pid, bus, dev


def find_awusb():
    """Return (bus, devnum, sysfs_path) for the first 1f3a:efe8 device."""
    for path, vid, pid, bus, dev in _usb_devices():
        if (vid, pid) == (AWUSB_VID, AWUSB_PID):
            return bus, dev, path
    return None


def endpoints_of(sysfs_path):
    """Return (ep_out, ep_in) bulk addresses of the first interface."""
    eps = {}
    for ep_dir in glob.glob(os.path.join(sysfs_path, "*", "ep_*")):
        try:
            addr = int(open(os.path.join(ep_dir, "bEndpointAddress")).read(), 16)
            etype = open(os.path.join(ep_dir, "type")).read().strip()
        except (OSError, ValueError):
            continue
        if etype != "Bulk":
            continue
        eps["in" if addr & 0x80 else "out"] = addr
    return eps.get("out"), eps.get("in")


def unbind_kernel_driver(bus, dev):
    """Detach whatever kernel driver owns the interface (allwinner, usbfs)."""
    base = None
    for path, _vid, _pid, b, d in _usb_devices():
        if (b, d) == (bus, dev):
            base = path
            break
    if base is None:
        return
    for ifdir in glob.glob(base + ":*"):
        link = os.path.join(ifdir, "driver")
        if not os.path.islink(link):
            continue
        drv = os.path.basename(os.readlink(link))
        node = os.path.basename(ifdir)
        try:
            with open(f"/sys/bus/usb/drivers/{drv}/unbind", "w") as f:
                f.write(node)
        except OSError:
            pass  # not rooted / already unbound


def open_awusb(unbind=True, timeout=10.0, want=None):
    """Wait for a 1f3a:efe8 device and open it.

    want="fel" requires the FEL endpoint pair (OUT 0x01), want="fes" the
    FES pair (OUT 0x02), so a still-rebooting device is not mistaken for
    the other stage.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = find_awusb()
        if found is None:
            time.sleep(0.25)
            continue
        bus, dev, path = found
        ep_out, ep_in = endpoints_of(path)
        if ep_out is None or ep_in is None:
            time.sleep(0.25)
            continue
        if want == "fel" and ep_out != 0x01:
            time.sleep(0.25)
            continue
        if want == "fes" and ep_out != 0x02:
            time.sleep(0.25)
            continue
        node = f"/dev/bus/usb/{bus:03d}/{dev:03d}"
        for _ in range(20):
            if os.path.exists(node):
                break
            time.sleep(0.1)
        try:
            return UsbDev(bus, dev, ep_out, ep_in, unbind=unbind)
        except (OSError, RuntimeError):
            time.sleep(0.25)
    raise RuntimeError("no 1f3a:efe8 device in FEL/FES mode found")


# --------------------------------------------------------------------------
# FEL layer (sunxi-tools fel_lib.c)
# --------------------------------------------------------------------------

FEL_VERSION = 0x001
FEL_WRITE = 0x101
FEL_EXEC = 0x102
FEL_READ = 0x103

AW_USB_READ = 0x11
AW_USB_WRITE = 0x12

FEL_CHUNK = 0x10000

# sunxi-tools' icache_fix stub (A733 and other ARMv7 SoCs): clear SCTLR.I,
# invalidate the I-cache, ISB, return.  The BROM FEL exec does not invalidate
# the instruction cache, so a second payload at the same address can execute
# the previous payload's cached lines.
FEL_ICACHE_STUB = (
    0xEE110F10,  # mrc 15, 0, r0, cr1, cr0, {0}   ; SCTLR
    0xE3C00A01,  # bic r0, r0, #0x1000            ; clear I
    0xEE010F10,  # mcr 15, 0, r0, cr1, cr0, {0}
    0xEE070F15,  # mcr 15, 0, r0, cr7, cr5, {0}   ; ICIALLU
    0xF57FF06F,  # isb sy
    0xE12FFF1E,  # bx lr
)
FEL_SCRATCH_FALLBACK = 0xEE800   # sun60iw2p1 BROM scratchpad from `fel ver`


def _aw_frame(typ, length):
    # struct aw_usb_request: signature[8], length, unknown1=0x0c000000,
    # request(u16), length2, pad[10]  (32 bytes, packed)
    return (b"AWUC\0\0\0\0"
            + struct.pack("<II", length, 0x0C000000)
            + struct.pack("<H", typ)
            + struct.pack("<I", length)
            + b"\0" * 10)


def _aw_response(usb):
    resp = usb.bulk_in(13)
    if not resp.startswith(b"AWUS"):
        raise RuntimeError(f"FEL: bad AWUS response {resp!r}")


def _fel_data_write(usb, data):
    """One AWUC data stage, payload possibly split into bulk bursts."""
    usb.bulk_out(_aw_frame(AW_USB_WRITE, len(data)))
    off = 0
    while off < len(data):
        usb.bulk_out(data[off:off + FEL_CHUNK])
        off += FEL_CHUNK
    _aw_response(usb)


def _fel_data_read(usb, length):
    usb.bulk_out(_aw_frame(AW_USB_READ, length))
    got = b""
    while len(got) < length:
        part = usb.bulk_in(length - len(got))
        if not part:
            raise RuntimeError("FEL: short read")
        got += part
    _aw_response(usb)
    return got


def _fel_status(usb):
    # sunxi-tools aw_read_fel_status(): its own AWUC/AWUS read stage
    _fel_data_read(usb, 8)


def _fel_request(usb, cmd, addr, length):
    # aw_send_fel_request() uses aw_usb_write(), i.e. the 16-byte request
    # itself travels inside an AWUC/AWUS stage.
    payload = struct.pack("<IIII", cmd, addr, length, 0)
    _fel_data_write(usb, payload)


def fel_version(usb):
    _fel_request(usb, FEL_VERSION, 0, 0)
    data = _fel_data_read(usb, 32)
    _fel_status(usb)
    if not data.startswith(b"AWUSBFEX"):
        raise RuntimeError(f"FEL: bad version magic {data[:8]!r}")
    return data


def _fel_write_raw(usb, addr, data):
    data = bytes(data)
    off = 0
    while off < len(data):
        chunk = data[off:off + FEL_CHUNK]
        _fel_request(usb, FEL_WRITE, addr + off, len(chunk))
        _fel_data_write(usb, chunk)
        _fel_status(usb)
        off += len(chunk)


def _fel_exec_raw(usb, addr):
    _fel_request(usb, FEL_EXEC, addr, 0)
    _fel_status(usb)


def fel_prepare(usb):
    """Disable the I-cache once per FEL session before new code is used.

    The BROM FEL exec jumps without invalidating the instruction cache, so
    a payload executed from an address another payload used earlier can run
    stale lines.  Mirrors sunxi-tools' soc_info.icache_fix: run a small stub
    from the BROM scratchpad that clears SCTLR.I and issues ICIALLU.
    """
    if getattr(usb, "icache_off", False):
        return
    try:
        scratch = struct.unpack_from("<I", fel_version(usb), 20)[0]
    except RuntimeError:
        scratch = 0
    if not scratch:
        scratch = FEL_SCRATCH_FALLBACK
    _fel_write_raw(usb, scratch, struct.pack("<6I", *FEL_ICACHE_STUB))
    _fel_exec_raw(usb, scratch)
    usb.icache_off = True


def fel_write(usb, addr, data):
    data = bytes(data)
    fel_prepare(usb)
    off = 0
    while off < len(data):
        chunk = data[off:off + FEL_CHUNK]
        _fel_request(usb, FEL_WRITE, addr + off, len(chunk))
        _fel_data_write(usb, chunk)
        _fel_status(usb)
        off += len(chunk)


def fel_read(usb, addr, length):
    out = bytearray()
    off = 0
    while off < length:
        chunk = min(FEL_CHUNK, length - off)
        _fel_request(usb, FEL_READ, addr + off, chunk)
        out += _fel_data_read(usb, chunk)
        _fel_status(usb)
        off += chunk
    return bytes(out)


def fel_exec(usb, addr):
    fel_prepare(usb)
    _fel_exec_raw(usb, addr)


def fel_writel(usb, addr, value):
    fel_write(usb, addr, struct.pack("<I", value))


# --------------------------------------------------------------------------
# FES layer (Allwinner FES protocol: 32-byte CBW / 13-byte CSW)
# --------------------------------------------------------------------------

CBW_MAGIC = 0x43555741  # "AWUC"
CSW_MAGIC = 0x53555741  # "AWUS"
TL_CMD_RECEIVE = 0x12
TL_CMD_TRANSMIT = 0x11

FES_DOWN = 0x0206
FES_UP = 0x0207

TYPE_DRAM = 0x7F00
TYPE_FLASH = 0x0000
FINISH_TAG = 0x10000

FES_CHUNK = 0x20000  # 128 KiB per fes_trans; the device splits media I/O at 64 KiB
FES_CHUNK_MAX = 0x20000  # a larger data phase leaves the device stuck
NIBBLE = 512


def _cbw(tag, direction, dlen):
    return (struct.pack("<II", CBW_MAGIC, tag) + struct.pack("<I", dlen)
            + struct.pack("<H", 0) + bytes([0, 0])
            + struct.pack("<BB", direction, 0) + struct.pack("<I", dlen)
            + bytes(10))


def _read_csw(usb):
    for _ in range(3):
        resp = usb.bulk_in(13)
        if len(resp) >= 13:
            magic, tag, _residue, status = struct.unpack("<IIiB", resp[:13])
            if magic == CSW_MAGIC:
                if status != 0:
                    raise RuntimeError(f"FES: CSW status {status} tag=0x{tag:x}")
                return tag
    raise RuntimeError("FES: no CSW")


def _fes_down(usb, tag, addr, chunk, ftype):
    usb.bulk_out(_cbw(tag, TL_CMD_RECEIVE, 16))
    usb.bulk_out(struct.pack("<HHIII", FES_DOWN, 0x0002, addr, len(chunk), ftype))
    _read_csw(usb)
    usb.bulk_out(_cbw(tag + 1, TL_CMD_RECEIVE, len(chunk)))
    usb.bulk_out(chunk)
    _read_csw(usb)
    usb.bulk_out(_cbw(tag + 2, TL_CMD_TRANSMIT, 13))
    _read_csw(usb)


def _fes_up(usb, tag, addr, size, ftype):
    usb.bulk_out(_cbw(tag, TL_CMD_RECEIVE, 16))
    usb.bulk_out(struct.pack("<HHIII", FES_UP, 0x0004, addr, size, ftype))
    _read_csw(usb)
    usb.bulk_out(_cbw(tag + 1, TL_CMD_TRANSMIT, size))
    got = b""
    while len(got) < size:
        part = usb.bulk_in(size - len(got))
        if not part:
            break
        got += part
    _read_csw(usb)
    usb.bulk_out(_cbw(tag + 2, TL_CMD_TRANSMIT, 13))
    _read_csw(usb)
    return got


def _fes_addr(base, off, ftype):
    return base + off if ftype == TYPE_DRAM else base + off // NIBBLE


def fes_burn(usb, image, base, ftype=TYPE_FLASH, chunk=FES_CHUNK, verify=True):
    if chunk > FES_CHUNK_MAX:
        raise ValueError(f"chunk {chunk:#x} exceeds {FES_CHUNK_MAX:#x}")
    data = open(image, "rb").read()
    pad = (-len(data)) % NIBBLE
    if pad:
        data += b"\xff" * pad
    chunk = max(NIBBLE, (chunk // NIBBLE) * NIBBLE)
    nblk = (len(data) + chunk - 1) // chunk
    t0 = time.time()
    for i in range(nblk):
        off = i * chunk
        blk = data[off:off + chunk]
        addr = _fes_addr(base, off, ftype)
        ftype_blk = ftype
        if ftype == TYPE_DRAM:
            # The device commits DRAM data per fes_trans: a block without
            # FINISH only accumulates in its staging buffer, and the FINISH
            # block copies just its own length from the buffer start to the
            # address it carries.  Mark every block so each one lands at its
            # own address (verified with 1 MiB/4 MiB read-back).
            ftype_blk |= FINISH_TAG
        tag = 0x02000000 | ((i & 0xFF) << 8)
        _fes_down(usb, tag, addr, blk, ftype_blk)
        if verify:
            got = _fes_up(usb, tag + 0x4000, addr, len(blk), ftype_blk)
            if got[:len(blk)] != blk:
                bad = next((j for j in range(min(len(got), len(blk)))
                            if got[j] != blk[j]), -1)
                raise RuntimeError(
                    f"FES: verify mismatch block {i} off={bad:#x} "
                    f"got={len(got)}B (addr {addr:#x})")
        done = off + len(blk)
        print(f"  block {i + 1}/{nblk}: addr {addr:#x} {len(blk)}B "
              f"{done / 1048576 / max(time.time() - t0, 1e-6):.2f} MB/s",
              flush=True)
    return len(data)


def fes_dump(usb, base, length, path, ftype=TYPE_FLASH, chunk=FES_CHUNK):
    if chunk > FES_CHUNK_MAX:
        raise ValueError(f"chunk {chunk:#x} exceeds {FES_CHUNK_MAX:#x}")
    chunk = max(NIBBLE, (chunk // NIBBLE) * NIBBLE)
    out = bytearray()
    off = 0
    i = 0
    while off < length:
        n = min(chunk, length - off)
        addr = _fes_addr(base, off, ftype)
        tag = 0x03000000 | ((i & 0xFF) << 8)
        out += _fes_up(usb, tag, addr, n, ftype)[:n]
        off += n
        i += 1
        print(f"  read {off}/{length} B", flush=True)
    open(path, "wb").write(out)
    return bytes(out)


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

FES1_ADDR = 0x90000        # sun60iw2p1 SRAM window the BROM loads fes1 to
FES1_MAGIC_ADDR = 0x07090114  # RTC+0x114, boot0/fes1 entry handshake
FES1_MAGIC = 0x46455331    # "FES1": entry skips training, FES only
FES1_MAGIC_TRAIN = 0x53454654  # "TFES": train, then FES in the same run


def cmd_fel_ver(args):
    usb = open_awusb(unbind=not args.no_unbind, want="fel")
    try:
        data = fel_version(usb)
    finally:
        usb.close()
    soc = struct.unpack("<I", data[8:12])[0] >> 8
    print(f"FEL {usb.bus}:{usb.dev} magic={data[:8].decode(errors='replace')} "
          f"soc=0x{soc:08x}")


def cmd_fel_write(args):
    data = open(args.file, "rb").read()
    usb = open_awusb(unbind=not args.no_unbind, want="fel")
    try:
        fel_write(usb, args.addr, data)
        if not args.no_verify:
            if fel_read(usb, args.addr, len(data)) != data:
                raise RuntimeError("FEL: write verify mismatch")
    finally:
        usb.close()
    print(f"FEL write {len(data)}B -> 0x{args.addr:x} ok")


def cmd_fel_read(args):
    usb = open_awusb(unbind=not args.no_unbind, want="fel")
    try:
        data = fel_read(usb, args.addr, args.length)
    finally:
        usb.close()
    open(args.file, "wb").write(data)
    print(f"FEL read {len(data)}B <- 0x{args.addr:x} -> {args.file}")


def cmd_fel_writel(args):
    usb = open_awusb(unbind=not args.no_unbind, want="fel")
    try:
        fel_writel(usb, args.addr, args.value)
    finally:
        usb.close()
    print(f"FEL writel 0x{args.value:08x} -> 0x{args.addr:x}")


def cmd_fel_exec(args):
    usb = open_awusb(unbind=not args.no_unbind, want="fel")
    try:
        fel_exec(usb, args.addr)
    finally:
        usb.close()
    print(f"FEL exec 0x{args.addr:x}")


def _load_fes1(usb, path, addr, verify):
    data = open(path, "rb").read()
    fel_write(usb, addr, data)
    if verify and fel_read(usb, addr, len(data)) != data:
        raise RuntimeError("FEL: fes1 read-back mismatch")
    return data


def boot_to_fes(args, load_addr=FES1_ADDR):
    """Load fes1, set the train+FES magic, execute once and wait for FES.

    The image trains the DRAM and calls the FES stack in the same run, so
    there is no return to the BROM FEL and no second execute.  TFES is
    also safe on a board boot0 already trained; it just retrains.
    """
    usb = open_awusb(unbind=not args.no_unbind, want="fel")
    try:
        fel_version(usb)
        fes1 = _load_fes1(usb, args.fes1, load_addr, not args.no_verify)
        fel_writel(usb, FES1_MAGIC_ADDR, FES1_MAGIC_TRAIN)
        fel_exec(usb, load_addr)
    finally:
        usb.close()
    print(f"== FEL: fes1 {len(fes1)}B at 0x{load_addr:x}, training")
    usb = open_awusb(unbind=not args.no_unbind, want="fes",
                     timeout=args.timeout)
    usb.close()
    print("== FES mode: endpoints 0x02 OUT / 0x81 IN")


def cmd_boot(args):
    boot_to_fes(args, args.addr)


def cmd_burn(args):
    usb = open_awusb(unbind=not args.no_unbind, want="fes")
    try:
        n = fes_burn(usb, args.file, args.addr, args.type, args.chunk,
                     verify=not args.no_verify)
    finally:
        usb.close()
    print(f"FES burn {n}B -> 0x{args.addr:x} ok")


def cmd_dump(args):
    usb = open_awusb(unbind=not args.no_unbind, want="fes")
    try:
        fes_dump(usb, args.addr, args.length, args.file, args.type, args.chunk)
    finally:
        usb.close()
    print(f"FES dump {args.length}B <- 0x{args.addr:x} -> {args.file}")


def cmd_flash(args):
    boot_to_fes(args)
    usb = open_awusb(unbind=not args.no_unbind, want="fes", timeout=args.timeout)
    try:
        print(f"== FES: burn {args.file} at 0x{args.addr:x}")
        n = fes_burn(usb, args.file, args.addr, args.type, args.chunk,
                     verify=not args.no_verify)
    finally:
        usb.close()
    print(f"done: {n}B written at 0x{args.addr:x}")


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _int0(s):
    return int(s, 0)


def _chunk(s):
    value = _int0(s)
    if value <= 0:
        raise argparse.ArgumentTypeError("chunk must be positive")
    if value > FES_CHUNK_MAX:
        raise argparse.ArgumentTypeError(
            f"chunk {value:#x} exceeds {FES_CHUNK_MAX:#x}; larger FES "
            "data phases hang the device")
    return value


def main(argv=None):
    p = argparse.ArgumentParser(description="Allwinner FEL/FES flasher",
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--no-unbind", action="store_true",
                   help="do not detach the kernel driver from the interface")
    sub = p.add_subparsers(dest="cmd", required=True)

    fel = sub.add_parser("fel", help="FEL-stage commands")
    fsub = fel.add_subparsers(dest="fel_cmd", required=True)

    fsub.add_parser("ver", help="read the FEL version block").set_defaults(
        func=cmd_fel_ver)

    x = fsub.add_parser("write", help="write a file to DRAM")
    x.add_argument("addr", type=_int0)
    x.add_argument("file")
    x.add_argument("--no-verify", action="store_true")
    x.set_defaults(func=cmd_fel_write)

    x = fsub.add_parser("read", help="read DRAM to a file")
    x.add_argument("addr", type=_int0)
    x.add_argument("length", type=_int0)
    x.add_argument("file")
    x.set_defaults(func=cmd_fel_read)

    x = fsub.add_parser("writel", help="write one 32-bit word")
    x.add_argument("addr", type=_int0)
    x.add_argument("value", type=_int0)
    x.set_defaults(func=cmd_fel_writel)

    x = fsub.add_parser("exec", help="call an address")
    x.add_argument("addr", type=_int0)
    x.set_defaults(func=cmd_fel_exec)

    x = sub.add_parser("boot", help="load fes1 and enter FES (trains DRAM)")
    x.add_argument("fes1")
    x.add_argument("--addr", type=_int0, default=FES1_ADDR)
    x.add_argument("--timeout", type=float, default=60.0,
                   help="seconds to wait for FES re-enumeration")
    x.add_argument("--no-verify", action="store_true")
    x.set_defaults(func=cmd_boot)

    x = sub.add_parser("burn", help="write a file through the FES stack")
    x.add_argument("addr", type=_int0,
                   help="DRAM address, or first 512B sector for flash")
    x.add_argument("file")
    x.add_argument("--type", choices=["flash", "dram"], default="flash")
    x.add_argument("--chunk", type=_chunk, default=FES_CHUNK)
    x.add_argument("--no-verify", action="store_true")
    x.set_defaults(func=cmd_burn)

    x = sub.add_parser("dump", help="read through the FES stack")
    x.add_argument("addr", type=_int0,
                   help="DRAM address, or first 512B sector for flash")
    x.add_argument("length", type=_int0)
    x.add_argument("file")
    x.add_argument("--type", choices=["flash", "dram"], default="flash")
    x.add_argument("--chunk", type=_chunk, default=FES_CHUNK)
    x.set_defaults(func=cmd_dump)

    x = sub.add_parser("flash", help="boot fes1 into FES, then burn (one shot)")
    x.add_argument("addr", type=_int0,
                   help="DRAM address, or first 512B sector for flash")
    x.add_argument("file")
    x.add_argument("--fes1", required=True)
    x.add_argument("--type", choices=["flash", "dram"], default="flash")
    x.add_argument("--chunk", type=_chunk, default=FES_CHUNK)
    x.add_argument("--timeout", type=float, default=60.0)
    x.add_argument("--no-verify", action="store_true")
    x.set_defaults(func=cmd_flash)

    args = p.parse_args(argv)
    if getattr(args, "type", None) == "dram":
        args.type = TYPE_DRAM
    elif getattr(args, "type", None) == "flash":
        args.type = TYPE_FLASH
    try:
        args.func(args)
    except (RuntimeError, OSError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())