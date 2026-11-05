<!-- Copyright (c) 2026, edk2-a733 contributors -->
<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->
# FES flashing protocol (derived from A733 measurements)

Version 1.1 / 2026-11-05 / edk2-a733 contributors (from usb_efex.c and board measurements)

## 1. Transport

| item | value |
|---|---|
| device VID/PID | 1f3a:efe8 |
| bulk OUT / IN | **0x02 / 0x81** (kernel sysfs; awusb hard-codes 1/2 and does not match the device) |
| usbfs prerequisite | `CLAIMINTERFACE(0)` in the process (kernel 5.13+ hardening), or every bulk transfer returns ENOENT |
| EP0 vendor | `0xA1:0xFE` -> 1-byte MAX_LUN (0x00); raw usbfs `0xC0185500` control transfer |

## 2. CBW/CSW layout (32 B / 13 B)

```
CBW: magic="AWUC"(0x43555741) tag(4) data_transfer_len(4) resv1(2) resv2(1)
     cmd_len(1) package{direction(1) resv(1) dataLen(4) resv2(10)}
CSW: magic="AWUS"(0x53555741) tag(4) residue(4) status(1)
direction: TL_CMD_RECEIVE=0x12 (host -> device) TL_CMD_TRANSMIT=0x11 (device -> host)
```

## 3. State machine (from usb_efex.c): one CBW per phase

```
IDLE --CBW--> SETUP --app_step switch--> command/data/status phases
SETUP(app_step==APPS_CMD)      : command phase: start a 16 B receive, parse app_cmd
SETUP(app_step==RECEIVE_DATA)  : data phase: start the DMA receive only if direction==RECEIVE
SETUP(app_step==APPS_STATUS)   : status phase: direction must be TRANSMIT, or the
                                 device hangs on "INVALID direction" (reproduced 12+ times)
top-level STATUS state          : unconditional __sunxi_efex_send_status (automatic CSW)
```

**Key rule: every phase must be followed by a status CBW (direction=TRANSMIT, len=13)**
- clears app_step (APPS_STATUS -> IDLE);
- triggers/finishes the CSW (both an automatic CSW and a STATUS-CBW CSW can appear,
  so read 2-3 times to cover both);
- omitting it makes the next transaction's command CBW fail in the 2141 branch
  (INVALID direction loop).

## 4. Transaction template (client command sequence)

```
fes_down (128 KiB per transaction; the device splits by medium):
  per block:
    CBW(RECEIVE,16) + 16B{fes_trans_t: app_cmd=0x0206 tag addr len type=0x7f00}
    -> STATUS-CBW(TRANSMIT,13) -> read CSW
    -> CBW(RECEIVE,128K) + data
    -> STATUS-CBW(TRANSMIT,13) -> read CSW
fes_up (read back): same command with app_cmd=0x0207; the data-phase CBW uses
**direction=TRANSMIT** (only this makes the device enter SEND_DATA); the host reads bulk IN
fes_trans_t: app_cmd(u16) tag(u16) addr(u32) len(u32) type(u32) - 16 B
type: 0x7f00 = DRAM (addr = target memory) / anything else = flash
      (addr = first sector, len = bytes)
**FINISH_TAG=0x10000**: the last DRAM block must use type=0x7f00|0x10000=0x17f00,
otherwise the data stays in the receive buffer and is not copied to DRAM
(read-back returned 0xff in that case).
  single-block model: every block is FINISH (memcpy immediately);
  streaming model: normal blocks accumulate, the last FINISH copies them.
```

The client defaults to `FES_CHUNK = 128 KiB`; a larger data phase (256 KiB) hangs
the device until power-cycled.  The device splits one `fes_trans` by medium: the
eMMC IDMAC write path uses 64 KiB, SD uses 128 KiB.

## 5. Traps (all hit during bring-up)

| # | trap | symptom | countermeasure |
|---|---|---|---|
| 1 | CBW payload direction=0 | INVALID direction and exit usb (looks like a dead device) | always set 0x12/0x11 explicitly |
| 2 | missing STATUS-CBW | app_step stuck in APPS_STATUS, later commands rejected | send a TRANSMIT CBW after every phase |
| 3 | data phase above 128 KiB | device hangs, needs a power cycle | stay at 128 KiB; eMMC writes split at 64 KiB |
| 4 | stale write_enable | data phase completes after a false 3 ms | fixed (cleared in the 2196 branch) |
| 5 | CSW read timing | automatic CSW and STATUS-CBW CSW can both appear | read 2-3 times |
| 6 | resuming a dirty session | the client misbehaves on leftover state | confirm device state / re-gate before each transaction |
| 7 | awusb module | udev binds the interface, CLAIM fails | blacklist and unbind (or unload) |

## 6. Command codes (usb_efex.h)

```
APP:    VERIFY_DEV=0x0001 SWITCH_ROLE=0x0002 IS_READY=0x0003 GET_CMD_SET_VER=0x0004
FEX_CMD: fes_trans=0x0201 run=0x0202 down=0x0206 up=0x0207 verify=0x0208
         query_storage=0x0209 flash_set_on=0x020A off=0x020B ... force_erase=0x021?
(new tools also send 0x102; it is not in the 2018 command table and the device
rejects it cleanly)
```

## 7. Tools

- `tools/pack/flash.py`: the reference implementation of this protocol
  (boot/burn/dump/flash).
- Write validation: `flash.py burn` followed by `flash.py dump` and a
  byte-for-byte comparison; the test regions were restored afterwards.
- device state machine: `fes/usb/vendor/usb_efex.c` (SETUP/data/STATUS).

## 8. Current state (2026-11-05)

1. OK: full consecutive-block transactions pass after the STATUS-CBW fix.
2. OK: fes_down flash mode reaches the real backend (eMMC/SD).
3. OK: eMMC/SD read and write drivers (PIO and IDMAC; eMMC HS200 at 200 MHz).
4. TODO: receives above 128 KiB (chained DMA measured slower and was dropped).
5. TODO: TX-direction DMA (the TX FIFO conflicts with the status phase; limited gain).

## 9. eMMC write backend (conclusions, 2026-10-13)

### 9.1 Local data commands (fes_mmc_write.c)

- Initialization still uses the library (sunxi_mmc_init(dev2) -> find -> mmc_init;
  FES_MMC_DEV=2 = SMHC2 = eMMC).
- **Data commands are fully local** (the library data path never fills the FIFO:
  cmd25/18 end in a data timeout, error 200):
  write = GCTRL(AHB)|BLKSZ|BYTCNT|ARG|clear RINT|command|push 32 bits while not
  FIFO_FULL|CMD_DONE|DATA_OVER|wait for card busy;
  read is symmetric; the FIFO read waits for **FIFO_EMPTY**, not RXWL.

### 9.2 Command bits (A733, from u-boot sunxi_mmc.h)

```
START=31 RESP_EXPIRE=6 CHK_CRC=8 DATA_EXPIRE=9 WRITE=10
AUTO_STOP=12 WAIT_PRE_OVER=13
multi-block (blocks>1) requires AUTO_STOP (automatic CMD12 ends the open transfer)
completion: multi-block -> RINT_AUTO_CMD_DONE(bit14); single -> RINT_DATA_OVER(bit3)
RINT_ERR = 0xBFC2 (u-boot error mask; includes bit10 VOLTAGE_CHANGE_DONE)
STATUS: FIFO_EMPTY=bit2 FIFO_FULL=bit3 CARD_PRESENT=bit8
        CARD_DATA_BUSY=bit9 DATA_FSM_BUSY=bit10 (A733; do not copy bit11)
```

### 9.3 Two root causes (one test round each)

1. **Missing AUTO_STOP**: the open multi-block write was never terminated by CMD12,
   the card stayed in the write session and the next CMD18 got RESP_TIMEOUT
   (rint=0x104); the apparent "success" was an unterminated transfer.
2. **Inverted shim return value**: callers test `if (!sunxi_flash_write())` for
   failure (**nonzero = success**), while the shim returned 0 on success.  That
   looked like a failure, set csw.status=-1 (host st=255) and left the session in
   APPS_IDLE (-> exit usb -> second training -> DEV_GONE).  A failed write
   (negative) happened to pass the test: write failure st=0, write success st=255.

### 9.4 Verification loop

```
down 64 KB (USB -> eMMC LBA 0x7a2c00, empty partition) -> device wb-verify MATCH
(reads back and compares after the write)
-> up 64 KB (eMMC -> USB) -> host FLASH VERIFY MATCH -> all CSWs st=0 -> DEV_ALIVE
firmware 7f07acaa (fes1.bin); host tool fes_burn.py
```
