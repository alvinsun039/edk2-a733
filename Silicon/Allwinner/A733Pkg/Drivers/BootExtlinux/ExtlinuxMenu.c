/** @file
  Text-mode boot menu for BootExtlinux.

  Draws the same information a GRUB menu carries - title, entries, the
  selected line, and the countdown - on whatever ConOut is (graphics
  console or serial).  Attributes are best-effort: a serial terminal
  ignores them, the screen shows the highlight.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextIn.h>
#include "Extlinux.h"

#define EXTLINUX_COLOR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define EXTLINUX_COLOR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLUE)
#define EXTLINUX_COLOR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)

STATIC
VOID
SetAttribute (
  IN UINTN  Attribute
  )
{
  gST->ConOut->SetAttribute (gST->ConOut, Attribute);
}

STATIC
CONST CHAR16 *
EntryLabel (
  IN CONST EXTLINUX_ENTRY  *Entry
  )
{
  return Entry->MenuLabel[0] != L'\0' ? Entry->MenuLabel : Entry->Label;
}

STATIC
VOID
DrawMenu (
  IN CONST EXTLINUX_CONFIG  *Config,
  IN UINTN                  Selected,
  IN UINTN                  SecondsLeft,
  IN BOOLEAN                Countdown
  )
{
  UINTN  Index;

  gST->ConOut->ClearScreen (gST->ConOut);

  SetAttribute (EXTLINUX_COLOR_TITLE);
  Print (L"  edk2-a733  extlinux boot  ");
  if (Config->Title[0] != L'\0') {
    Print (L"| %s", Config->Title);
  }

  SetAttribute (EXTLINUX_COLOR_NORMAL);
  Print (L"\r\n\r\n");

  for (Index = 0; Index < Config->Count; Index++) {
    if (Index == Selected) {
      SetAttribute (EXTLINUX_COLOR_SELECTED);
      Print (L"  > %s", EntryLabel (&Config->Entry[Index]));
      SetAttribute (EXTLINUX_COLOR_NORMAL);
      Print (L"\r\n");
    } else {
      Print (L"    %s\r\n", EntryLabel (&Config->Entry[Index]));
    }
  }

  Print (L"\r\n");
  if (Countdown) {
    Print (
      L"  Booting the default entry in %u s; any key stops the countdown.\r\n",
      SecondsLeft
      );
  } else {
    Print (
      L"  Up/Down select, Enter boot, 1-%u pick, Esc exit.\r\n",
      Config->Count
      );
  }
}

EFI_STATUS
ExtlinuxMenuSelect (
  IN  CONST EXTLINUX_CONFIG  *Config,
  OUT UINTN                  *Selected
  )
{
  UINTN          Selection;
  UINTN          TotalMs;
  UINTN          Elapsed;
  UINTN          Shown;
  UINTN          ShownSelection;
  BOOLEAN        Countdown;
  EFI_STATUS     Status;
  EFI_INPUT_KEY  Key;

  if ((Config == NULL) || (Selected == NULL) || (Config->Count == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Selection = ExtlinuxDefaultIndex (Config);
  TotalMs   = Config->Timeout * 100;
  Elapsed   = 0;
  Shown          = MAX_UINTN;
  ShownSelection = MAX_UINTN;
  Countdown = TotalMs != 0;

  for (;;) {
    UINTN  SecondsLeft;

    SecondsLeft = Countdown ? (TotalMs - Elapsed + 999) / 1000 : 0;
    //
    // The selection is part of what is on screen: after the first key stops the
    // countdown the seconds stop changing, and a redraw keyed on them alone
    // would leave every later Up/Down press invisible.
    //
    if ((SecondsLeft != Shown) || (Selection != ShownSelection)) {
      DrawMenu (Config, Selection, SecondsLeft, Countdown);
      Shown          = SecondsLeft;
      ShownSelection = Selection;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (!EFI_ERROR (Status)) {
      //
      // Any key means the operator is here: stop the countdown, then act.
      //
      Countdown = FALSE;

      if (Key.ScanCode == SCAN_UP) {
        Selection = (Selection + Config->Count - 1) % Config->Count;
      } else if (Key.ScanCode == SCAN_DOWN) {
        Selection = (Selection + 1) % Config->Count;
      } else if (Key.ScanCode == SCAN_ESC) {
        SetAttribute (EXTLINUX_COLOR_NORMAL);
        return EFI_ABORTED;
      } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        *Selected = Selection;
        SetAttribute (EXTLINUX_COLOR_NORMAL);
        return EFI_SUCCESS;
      } else if ((Key.UnicodeChar >= L'1') && (Key.UnicodeChar <= L'9')) {
        UINTN  Candidate;

        Candidate = Key.UnicodeChar - L'1';
        if (Candidate < Config->Count) {
          Selection = Candidate;
        }
      }

      continue;
    }

    if (Countdown) {
      if (Elapsed >= TotalMs) {
        *Selected = Selection;
        SetAttribute (EXTLINUX_COLOR_NORMAL);
        return EFI_SUCCESS;
      }

      gBS->Stall (20 * 1000);
      Elapsed += 20;
    } else {
      gBS->Stall (50 * 1000);
    }
  }
}
