/** @file
  Data model and helpers for the syslinux/extlinux configuration format
  that U-Boot consumes through `sysboot`.

  The subset here is the one distro installers write (Debian's
  u-boot-update, Fedora's grubby extlinux mode, Buildroot, ...):
  DEFAULT / PROMPT / TIMEOUT / MENU TITLE, and per LABEL: MENU LABEL,
  LINUX or KERNEL, INITRD, FDT or FDTDIR, APPEND.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef EXTLINUX_H_
#define EXTLINUX_H_

#include <Uefi.h>

#define EXTLINUX_MAX_ENTRIES  16
#define EXTLINUX_LABEL_LEN    64
#define EXTLINUX_MENU_LEN     96
#define EXTLINUX_PATH_LEN     260
#define EXTLINUX_APPEND_LEN   512

typedef struct {
  CHAR16    Label[EXTLINUX_LABEL_LEN];
  CHAR16    MenuLabel[EXTLINUX_MENU_LEN];
  CHAR16    Kernel[EXTLINUX_PATH_LEN];
  CHAR16    Initrd[EXTLINUX_PATH_LEN];
  CHAR16    Fdt[EXTLINUX_PATH_LEN];
  CHAR16    FdtDir[EXTLINUX_PATH_LEN];
  CHAR16    Append[EXTLINUX_APPEND_LEN];
} EXTLINUX_ENTRY;

typedef struct {
  CHAR16            Title[EXTLINUX_MENU_LEN];
  CHAR16            Default[EXTLINUX_LABEL_LEN];
  UINTN             Timeout;      ///< Tenths of a second, as in syslinux. 0 = wait.
  BOOLEAN           Prompt;
  UINTN             Count;
  EXTLINUX_ENTRY    Entry[EXTLINUX_MAX_ENTRIES];
} EXTLINUX_CONFIG;

/**
  Parse an extlinux.conf image.  Text must be NUL terminated.

  @param[in]   Text    Configuration text.
  @param[in]   Size    Size of Text, in bytes.
  @param[out]  Config  Parsed configuration.

  @retval EFI_SUCCESS    At least one LABEL entry was found.
  @retval EFI_NOT_FOUND  No usable entry.
**/
EFI_STATUS
ExtlinuxParse (
  IN  CHAR8            *Text,
  IN  UINTN            Size,
  OUT EXTLINUX_CONFIG  *Config
  );

/**
  Index of the entry DEFAULT names, or 0 when it names nothing.

  @param[in]  Config  Parsed configuration.

  @return Entry index.
**/
UINTN
ExtlinuxDefaultIndex (
  IN CONST EXTLINUX_CONFIG  *Config
  );

/**
  Case-insensitive comparison of two Unicode strings.

  BaseLib only publishes the ASCII case-insensitive compare; the entry
  labels and file names this driver matches are ASCII in practice but are
  stored as CHAR16, so this is the Unicode counterpart it lacks.

  @return 0 when equal, <0 or >0 as StrCmp otherwise.
**/
INTN
ExtlinuxStriCmp (
  IN CONST CHAR16  *First,
  IN CONST CHAR16  *Second
  );

/**
  Copy a Unicode string into a fixed-size buffer, always terminating it.

  @param[out]  Dst       Destination.
  @param[in]   DstChars  Capacity of Dst in CHAR16 units.
  @param[in]   Src       Source.
**/
VOID
ExtlinuxCopyString (
  OUT CHAR16        *Dst,
  IN  UINTN         DstChars,
  IN  CONST CHAR16  *Src
  );

/**
  Show the entry list and wait for a selection.

  @param[in]   Config    Parsed configuration.
  @param[out]  Selected  Index the operator chose.

  @retval EFI_SUCCESS  Selected was filled in.
  @retval EFI_ABORTED  Esc was pressed.
**/
EFI_STATUS
ExtlinuxMenuSelect (
  IN  CONST EXTLINUX_CONFIG  *Config,
  OUT UINTN                  *Selected
  );

/**
  Turn a syslinux path into an EFI_FILE_PROTOCOL path.

  Absolute syslinux paths ("/boot/vmlinuz") map to the volume root
  ("\\boot\\vmlinuz"); relative ones resolve against the directory the
  configuration file lives in.  '/' becomes '\\'.

  @param[in]   BaseDir   Configuration directory, e.g. L"\\boot\\extlinux".
  @param[in]   Path      Path as written in the configuration.
  @param[out]  Out       Resulting UEFI path.
  @param[in]   OutChars  Size of Out in CHAR16 units.
**/
VOID
ExtlinuxToUefiPath (
  IN  CONST CHAR16  *BaseDir,
  IN  CONST CHAR16  *Path,
  OUT CHAR16        *Out,
  IN  UINTN         OutChars
  );

#endif
