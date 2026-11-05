/** @file
  AriscStartupDxe - start the A733 ARISC (E906) PM co-processor.

  The A733 BL31 (closed bl31.bin and the open port alike) exposes the
  ARISC loader as SIP FID 0x8000FF10: it copies the firmware image that
  boot0 placed at 0x48100000 (from the boot-package "scp" item) into the
  E902 SRAM, releases the core, writes the boot argument into RTC+0x10C
  (the ARISC's "dtb base store" register) and waits for the 0x90 startup
  notification.

  Boot argument: the physical address of the ARISC configuration FDT
  ("scp dts").  The ARISC parses it lazily (only /dram here:
    - standby_dts_parse():  /standby_param - INTENTIONALLY ABSENT: each
      mask bit power-offs one AXP power-tree output, and the Cubie A7A has
      no such table (the production card ships no scp dts either).  With
      a foreign table (Cubie A7Z) the ARISC powered off wrong outputs and
      the USB VBUS became the only power input.
    - dram_dts_parse():     /dram  (dram_para00..95 = the 96-word struct
      dram_para: 32 named fields in order + the dram_add[64] tail) for the
      DRAM power-save/up sequence.
  With a zero argument those parses fail, the super-standby aborts and the
  AP domain is never powered down (observed: fan keeps spinning, no wake).

  boot0 delivers that FDT itself.  The boot package built by
  tools/pack/pack.sh compiles Platform/Radxa/CubieA7APkg/scp_dts.dts and
  carries the resulting DTB as its "dtb" item; boot0 copies the item to
  uboot_base + SUNXI_DTB_OFFSET (2 MB), i.e. to 0x41200000 with the
  firmware itself at 0x41000000.

  That address is inside the DRAM range the DXE core may allocate from, so
  A733PlatformPeiLib reserves the page with a memory allocation HOB while
  the HOB list is built - before any allocation can happen - and marks it
  EfiReservedMemoryType in the map the OS receives.  This driver can
  therefore rely on boot0's copy being intact here and after
  ExitBootServices (the E902 parses the tree again at every super-standby).

  boot0 also leaves the DRAM parameters it trained in the u-boot spare
  head at uboot_base + 0x30.  They are injected into /dram, replacing the
  static values the packed file carries - the same fixup the vendor U-Boot
  performs in update_fdt_dram_para().  The packed values can be stale:
  on this board dram_para07 differs from what boot0 trained.

  The FDT page is turned into EfiReservedMemoryType: the E902 parses it
  again at the first super-standby, long after ExitBootServices, so it
  must not be handed to the OS allocator.

  The Allwinner BSP U-Boot performs this role from sunxi_arisc_probe();
  the Radxa EDK2 BL33 never did (stock gap: the ARISC - and with it the
  PSCI reset/off and suspend paths - was dead on the SD/UEFI chain).

  Copyright (c) 2026, edk2-a733 contributors.
  Portions derived from edk2-a733 (github.com/vehoelite/edk2-a733),
  Copyright (c) 2026 carpi-os contributors, BSD-2-Clause-Patent.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define A733_SIP_ARISC_STARTUP  0x8000FF10ULL

//
// Where boot0 puts the boot-package items: "u-boot" (the firmware itself)
// at the start of the BL33 window.  0x41000030 is the spare head boot0
// fills with the DRAM parameters it trained.
//
#define A733_UBOOT_BASE       0x41000000UL
//
// boot0 copies the boot package's "dtb" item (the ARISC configuration FDT)
// two megabytes past the firmware's load address.  A733PlatformPeiLib
// reserves this page before the DXE core can allocate it.
//
#define A733_ARISC_DTB_BASE   (A733_UBOOT_BASE + 0x200000UL)

//
// spare_boot_data_head.dram_para[32], right after the 0x30-byte ctrl head.
//
#define A733_UBOOT_DRAMPARA   (A733_UBOOT_BASE + 0x30)
#define A733_DRAMPARA_WORDS   32

STATIC
UINT64
SunxiSiP (
  IN UINT64  Fid,
  IN UINT64  Arg1
  )
{
  register UINT64  X0 __asm__ ("x0") = Fid;
  register UINT64  X1 __asm__ ("x1") = Arg1;

  __asm__ volatile ("smc #0" : "+r" (X0), "+r" (X1) :: "memory");
  return X0;
}

STATIC
VOID
CacheCleanToPoc (
  IN VOID   *Addr,
  IN UINTN  Len
  )
{
  UINTN  a = (UINTN)Addr & ~(UINTN)0x3F;
  UINTN  e = ((UINTN)Addr + Len + 0x3F) & ~(UINTN)0x3F;

  for (; a < e; a += 0x40) {
    __asm__ volatile ("dc civac, %0" :: "r" (a) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
  __asm__ volatile ("isb" ::: "memory");
}

/**
  Replace the /dram values with the parameters boot0 trained on this board.

  The packed FDT carries the values of the board it was captured on; the
  spare head carries what this DRAM actually needs.  Properties keep their
  four-byte length, so this only overwrites data in place.

  @param[in]  Fdt   Configuration FDT to fix up (the reserved private copy).
**/
STATIC
VOID
InjectDramPara (
  IN VOID  *Fdt
  )
{
  STATIC CONST CHAR8  mName[] = "dram_para00";
  volatile UINT32     *Para;
  CHAR8               Prop[sizeof (mName)];
  INT32               Node;
  UINTN               i;

  Para = (volatile UINT32 *)(UINTN)A733_UBOOT_DRAMPARA;

  //
  // boot0 stores the head through the cache; clean before the first read so
  // this CPU cannot pick up a stale line, then keep the values.
  //
  CacheCleanToPoc ((VOID *)(UINTN)A733_UBOOT_DRAMPARA,
                   A733_DRAMPARA_WORDS * sizeof (UINT32));

  Node = FdtSubnodeOffset (Fdt, 0, "dram");
  if (Node < 0) {
    DEBUG ((DEBUG_WARN, "AriscStartupDxe: no /dram node in the config FDT\n"));
    return;
  }

  for (i = 0; i < A733_DRAMPARA_WORDS; i++) {
    UINT32  Value;

    CopyMem (Prop, mName, sizeof (Prop));
    Prop[9]  = (CHAR8)('0' + (i / 10));
    Prop[10] = (CHAR8)('0' + (i % 10));
    Value    = SwapBytes32 (Para[i]);
    if (FdtSetProp (Fdt, Node, Prop, &Value, sizeof (Value)) != 0) {
      DEBUG ((DEBUG_WARN, "AriscStartupDxe: cannot set %a\n", Prop));
    }
  }
}

EFI_STATUS
EFIAPI
AriscStartupDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_PHYSICAL_ADDRESS   DtbPage;
  VOID                   *Fdt;
  UINT32                 FdtSize;
  UINT64                 Ret;

  Fdt = (VOID *)(UINTN)A733_ARISC_DTB_BASE;
  if (FdtCheckHeader (Fdt) != 0) {
    DEBUG ((DEBUG_WARN, "AriscStartupDxe: no config FDT at 0x%lx\n", (UINT64)A733_ARISC_DTB_BASE));
    Fdt = NULL;
  }

  FdtSize = 0;
  if (Fdt != NULL) {
    //
    // The page is already EfiReservedMemoryType: A733PlatformPeiLib built a
    // memory allocation HOB for it before the DXE core existed.  Allocating
    // it again is only a belt-and-braces check for builds that dropped the
    // override; EFI_NOT_FOUND/EFI_ACCESS_DENIED mean it is reserved.
    //
    DtbPage = A733_ARISC_DTB_BASE;
    Status  = gBS->AllocatePages (AllocateAddress, EfiReservedMemoryType, 1, &DtbPage);
    if (EFI_ERROR (Status) &&
        (Status != EFI_NOT_FOUND) && (Status != EFI_ACCESS_DENIED)) {
      DEBUG ((DEBUG_WARN, "AriscStartupDxe: cannot reserve the config page (%r)\n", Status));
    }

    FdtSize = SwapBytes32 (*(UINT32 *)((UINTN)Fdt + 4));
    if (FdtSize > EFI_PAGE_SIZE) {
      FdtSize = EFI_PAGE_SIZE;
    }
  }

  if (Fdt != NULL) {
    if (FdtSubnodeOffset (Fdt, 0, "standby_param") >= 0) {
      DEBUG ((DEBUG_INFO, "AriscStartupDxe: /standby_param present\n"));
    } else {
      DEBUG ((DEBUG_WARN, "AriscStartupDxe: /standby_param MISSING\n"));
    }

    InjectDramPara (Fdt);
    CacheCleanToPoc (Fdt, FdtSize);
  }

  DEBUG ((
    DEBUG_INFO,
    "AriscStartupDxe: scp dts @ %lx (%u bytes, from boot0), starting ARISC\n",
    (UINT64)A733_ARISC_DTB_BASE,
    FdtSize
    ));

  Ret = SunxiSiP (A733_SIP_ARISC_STARTUP, (UINT64)(UINTN)Fdt);
  if (Ret == 0) {
    DEBUG ((DEBUG_INFO, "AriscStartupDxe: ARISC (E906) started\n"));
  } else {
    DEBUG ((
      DEBUG_WARN,
      "AriscStartupDxe: ARISC startup failed (%Lx) - PSCI reset/off unavailable\n",
      Ret
      ));
  }

  return EFI_SUCCESS;
}