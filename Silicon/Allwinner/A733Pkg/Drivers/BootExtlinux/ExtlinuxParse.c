/** @file
  Parser for the syslinux/extlinux configuration U-Boot reads with
  `sysboot`, reduced to what distro installers actually emit.

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include "Extlinux.h"

STATIC
CHAR8 *
SkipBlanks (
  IN CHAR8  *Text
  )
{
  while ((*Text == ' ') || (*Text == '\t')) {
    Text++;
  }

  return Text;
}

STATIC
UINTN
TokenLength (
  IN CONST CHAR8  *Text
  )
{
  UINTN  Length;

  for (Length = 0; (Text[Length] != '\0') && (Text[Length] != ' ') &&
                    (Text[Length] != '\t'); Length++)
  {
  }

  return Length;
}

STATIC
VOID
TrimTrailingBlanks (
  IN CHAR8  *Text
  )
{
  UINTN  Length;

  Length = AsciiStrLen (Text);
  while (Length > 0) {
    if ((Text[Length - 1] != ' ') && (Text[Length - 1] != '\t') &&
        (Text[Length - 1] != '\r'))
    {
      break;
    }

    Text[--Length] = '\0';
  }
}

STATIC
VOID
CopyToUnicode (
  OUT CHAR16       *Dst,
  IN  UINTN        DstChars,
  IN  CONST CHAR8  *Src
  )
{
  UINTN  Index;

  for (Index = 0; (Index + 1 < DstChars) && (Src[Index] != '\0'); Index++) {
    Dst[Index] = (CHAR16)(UINT8)Src[Index];
  }

  Dst[Index] = L'\0';
}

/**
  Compare the keyword at Text, whose terminator is temporarily turned into
  a NUL, with a lower-case literal.
**/
STATIC
BOOLEAN
KeywordIs (
  IN CHAR8        *Text,
  IN UINTN        Length,
  IN CONST CHAR8  *Literal
  )
{
  CHAR8  Saved;
  INTN   Result;

  if (Length == 0) {
    return FALSE;
  }

  Saved        = Text[Length];
  Text[Length] = '\0';
  Result       = AsciiStriCmp (Text, Literal);
  Text[Length] = Saved;

  return Result == 0;
}

EFI_STATUS
ExtlinuxParse (
  IN  CHAR8            *Text,
  IN  UINTN            Size,
  OUT EXTLINUX_CONFIG  *Config
  )
{
  CHAR8    *Line;
  CHAR8    *Next;
  CHAR8    *Keyword;
  CHAR8    *Rest;
  UINTN    KeywordLength;
  INTN     Current;
  BOOLEAN  SkipHelp;

  if ((Text == NULL) || (Config == NULL) || (Size == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Config, sizeof (*Config));
  Config->Prompt = FALSE;

  Current  = -1;
  SkipHelp = FALSE;
  Line     = Text;

  while (*Line != '\0') {
    //
    // Cut the line at the newline, then trim what syslinux ignores.
    //
    Next = Line;
    while ((*Next != '\0') && (*Next != '\n')) {
      Next++;
    }

    if (*Next == '\n') {
      *Next = '\0';
      Next++;
    }

    TrimTrailingBlanks (Line);
    Keyword = SkipBlanks (Line);
    if ((*Keyword == '\0') || (*Keyword == '#')) {
      Line = Next;
      continue;
    }

    KeywordLength = TokenLength (Keyword);
    Rest          = SkipBlanks (Keyword + KeywordLength);

    if (SkipHelp) {
      //
      // TEXT HELP ... ENDTEXT runs until a line starting with ENDTEXT.
      //
      if (KeywordIs (Keyword, KeywordLength, "endtext")) {
        SkipHelp = FALSE;
      }

      Line = Next;
      continue;
    }

    if (KeywordIs (Keyword, KeywordLength, "label")) {
      if (Config->Count < EXTLINUX_MAX_ENTRIES) {
        Current = (INTN)Config->Count++;
        CopyToUnicode (Config->Entry[Current].Label, EXTLINUX_LABEL_LEN, Rest);
      } else {
        Current = -1;
      }
    } else if (KeywordIs (Keyword, KeywordLength, "menu")) {
      CHAR8  *Sub;
      CHAR8  *SubRest;
      UINTN  SubLength;

      Sub       = Rest;
      SubLength = TokenLength (Sub);
      SubRest   = SkipBlanks (Sub + SubLength);

      if (KeywordIs (Sub, SubLength, "label")) {
        if (Current >= 0) {
          CopyToUnicode (
            Config->Entry[Current].MenuLabel,
            EXTLINUX_MENU_LEN,
            SubRest
            );
        }
      } else if (KeywordIs (Sub, SubLength, "title")) {
        CopyToUnicode (Config->Title, EXTLINUX_MENU_LEN, SubRest);
      } else if (KeywordIs (Sub, SubLength, "default")) {
        CopyToUnicode (Config->Default, EXTLINUX_LABEL_LEN, SubRest);
      }
    } else if (KeywordIs (Keyword, KeywordLength, "default")) {
      CopyToUnicode (Config->Default, EXTLINUX_LABEL_LEN, Rest);
    } else if (KeywordIs (Keyword, KeywordLength, "timeout")) {
      Config->Timeout = AsciiStrDecimalToUintn (Rest);
    } else if (KeywordIs (Keyword, KeywordLength, "prompt")) {
      Config->Prompt = AsciiStrDecimalToUintn (Rest) != 0;
    } else if ((KeywordIs (Keyword, KeywordLength, "linux")) ||
               (KeywordIs (Keyword, KeywordLength, "kernel")))
    {
      if (Current >= 0) {
        CopyToUnicode (Config->Entry[Current].Kernel, EXTLINUX_PATH_LEN, Rest);
      }
    } else if (KeywordIs (Keyword, KeywordLength, "initrd")) {
      if (Current >= 0) {
        CopyToUnicode (Config->Entry[Current].Initrd, EXTLINUX_PATH_LEN, Rest);
      }
    } else if (KeywordIs (Keyword, KeywordLength, "fdt")) {
      if (Current >= 0) {
        CopyToUnicode (Config->Entry[Current].Fdt, EXTLINUX_PATH_LEN, Rest);
      }
    } else if (KeywordIs (Keyword, KeywordLength, "fdtdir")) {
      if (Current >= 0) {
        CopyToUnicode (Config->Entry[Current].FdtDir, EXTLINUX_PATH_LEN, Rest);
      }
    } else if (KeywordIs (Keyword, KeywordLength, "append")) {
      if (Current >= 0) {
        CopyToUnicode (
          Config->Entry[Current].Append,
          EXTLINUX_APPEND_LEN,
          Rest
          );
      }
    } else if (KeywordIs (Keyword, KeywordLength, "text")) {
      //
      // TEXT HELP introduces a block that ENDTEXT closes.
      //
      if (KeywordIs (Rest, TokenLength (Rest), "help")) {
        SkipHelp = TRUE;
      }
    }

    Line = Next;
  }

  return Config->Count == 0 ? EFI_NOT_FOUND : EFI_SUCCESS;
}

INTN
ExtlinuxStriCmp (
  IN CONST CHAR16  *First,
  IN CONST CHAR16  *Second
  )
{
  CHAR16  Left;
  CHAR16  Right;

  for (;;) {
    Left  = *First++;
    Right = *Second++;

    if ((Left >= L'A') && (Left <= L'Z')) {
      Left = (CHAR16)(Left + (L'a' - L'A'));
    }

    if ((Right >= L'A') && (Right <= L'Z')) {
      Right = (CHAR16)(Right + (L'a' - L'A'));
    }

    if (Left != Right) {
      return (INTN)Left - (INTN)Right;
    }

    if (Left == L'\0') {
      return 0;
    }
  }
}

VOID
ExtlinuxCopyString (
  OUT CHAR16        *Dst,
  IN  UINTN         DstChars,
  IN  CONST CHAR16  *Src
  )
{
  UINTN  Index;

  if (DstChars == 0) {
    return;
  }

  for (Index = 0; (Index + 1 < DstChars) && (Src[Index] != L'\0'); Index++) {
    Dst[Index] = Src[Index];
  }

  Dst[Index] = L'\0';
}

UINTN
ExtlinuxDefaultIndex (
  IN CONST EXTLINUX_CONFIG  *Config
  )
{
  UINTN  Index;

  if (Config->Default[0] != L'\0') {
    for (Index = 0; Index < Config->Count; Index++) {
      if (ExtlinuxStriCmp (Config->Default, Config->Entry[Index].Label) == 0) {
        return Index;
      }
    }
  }

  return 0;
}

VOID
ExtlinuxToUefiPath (
  IN  CONST CHAR16  *BaseDir,
  IN  CONST CHAR16  *Path,
  OUT CHAR16        *Out,
  IN  UINTN         OutChars
  )
{
  UINTN  Length;

  if (OutChars == 0) {
    return;
  }

  Length  = 0;
  Out[0]  = L'\0';

  if (Path[0] == L'/') {
    //
    // Absolute in the syslinux sense: the volume root.
    //
    if (Length + 1 < OutChars) {
      Out[Length++] = L'\\';
    }

    Path++;
  } else {
    //
    // Relative: resolve against the configuration directory.
    //
    while ((*BaseDir != L'\0') && (Length + 1 < OutChars)) {
      Out[Length++] = *BaseDir++;
    }

    if ((Length > 0) && (Out[Length - 1] != L'\\') && (Length + 1 < OutChars)) {
      Out[Length++] = L'\\';
    }
  }

  while (*Path != L'\0') {
    CHAR16  Character;

    Character = *Path++;
    if (Character == L'/') {
      Character = L'\\';
    }

    if (Length + 1 >= OutChars) {
      break;
    }

    Out[Length++] = Character;
  }

  Out[Length] = L'\0';
}
