/** @file
  HYM8563 real time clock backing for the EFI runtime services.

  The Cubie A7A carries a HYM8563TS on S_TWI1 (PL12/PL13, function 3) at
  address 0x51, with a 32.768 kHz crystal on OSCI/OSCO - schematic page 19,
  U11.  It is the only clock the board has: the SoC's own RTC block is
  disabled in the device tree, and its VDD comes from the PMIC's ALDO1, so
  the time survives only while the board is powered unless a backup source
  is fitted.

  This library replaces the virtual clock this tree used to ship, so
  GetTime/SetTime reach real hardware.  The read is polled over the same
  TWI engine the PMIC uses, on the second controller instance; see
  A733TwiInitOnBus().

  Copyright (c) 2026, edk2-a733 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Pi/PiDxeCis.h>

#include <Library/A733TwiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

#include <A733Rpio.h>
#include <A733Twi.h>

//
// Device: HYM8563TS on S_TWI1, address 0x51.
//
#define HYM8563_SLAVE_ADDR  0x51U

#define HYM8563_REG_CTRL1    0x00U
#define HYM8563_REG_CTRL2    0x01U
#define HYM8563_REG_SECONDS  0x02U
#define HYM8563_REG_MINUTES  0x03U
#define HYM8563_REG_HOURS    0x04U
#define HYM8563_REG_DAYS     0x05U
#define HYM8563_REG_WEEKDAY  0x06U
#define HYM8563_REG_MONTHS   0x07U
#define HYM8563_REG_YEARS    0x08U

#define HYM8563_SECONDS_VL    BIT7   // voltage-low: the time is not valid
#define HYM8563_MONTHS_CENTURY BIT7  // only meaningful on some parts

//
// Bus: S_TWI1 at 0x07084000, pins PL12/PL13 function 3 (the vendor pinctrl
// table's "s_twi1").  Its gate and reset live in the always-on R_CCU.
//
#define S_TWI1_BASE        A733_TWI1_BASE
#define R_CCU_BASE         0x07010000U
#define R_CCU_TWI1_REG     0x019CU
#define R_CCU_TWI1_CLK     BIT1
#define R_CCU_TWI1_RST     BIT17
#define HYM8563_BUS_HZ     100000U

STATIC BOOLEAN  mHym8563Present;

//
// The bus address the runtime services use.  It starts as the physical base
// and is converted when the OS installs its virtual address map; the runtime
// code must reach the controller through the converted address, because the
// kernel's EFI page tables map the block at the virtual address it assigned,
// not at the physical one.
//
STATIC UINTN  mHym8563TwiBase = S_TWI1_BASE;

STATIC
UINT8
BcdToBin (
  IN UINT8  Value
  )
{
  return (UINT8)(((Value >> 4) & 0x0F) * 10 + (Value & 0x0F));
}

STATIC
UINT8
BinToBcd (
  IN UINT8  Value
  )
{
  return (UINT8)(((Value / 10) << 4) | (Value % 10));
}

/**
  Day of week for a date, 0 = Sunday (Sakamoto's method).

  The HYM8563 keeps a weekday register but EFI_TIME has no such field, so
  it is derived from the date whenever the clock is set.
**/
STATIC
UINT8
WeekdayFromDate (
  IN UINT16  Year,
  IN UINT8   Month,
  IN UINT8   Day
  )
{
  STATIC CONST UINT8  Offsets[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

  if (Month < 3) {
    Year--;
  }

  return (UINT8)((Year + Year / 4 - Year / 100 + Year / 400 +
                  Offsets[Month - 1] + Day) % 7);
}

STATIC
EFI_STATUS
Hym8563Read (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{
  return A733TwiReadReg (mHym8563TwiBase, HYM8563_SLAVE_ADDR, Reg, Value);
}

STATIC
EFI_STATUS
Hym8563Write (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  return A733TwiWriteReg (mHym8563TwiBase, HYM8563_SLAVE_ADDR, Reg, Value);
}

/**
  Open the S_TWI1 gate/reset and bring the controller up.

  @retval EFI_SUCCESS  The bus is usable.
  @retval Others       The controller did not come up.
**/
STATIC
EFI_STATUS
Hym8563BusInit (
  VOID
  )
{
  UINT32      Value;
  EFI_STATUS  Status;

  //
  // R_CCU: open the clock gate and release the reset.  Both bits are
  // active high on this part - S_TWI0 boots with bits 0 and 16 set (gate
  // open, reset released), and the PMIC on it works - so the reset bit is
  // set and left set.  Clearing it holds the block in reset, which reads
  // back as a status register of 0x00.
  //
  Value  = MmioRead32 (R_CCU_BASE + R_CCU_TWI1_REG);
  Value |= R_CCU_TWI1_CLK | R_CCU_TWI1_RST;
  MmioWrite32 (R_CCU_BASE + R_CCU_TWI1_REG, Value);
  MicroSecondDelay (10);
  DEBUG ((
    DEBUG_INFO,
    "Hym8563: R_CCU+0x19C = 0x%08x, S_TWI1 STATUS = 0x%02x\n",
    MmioRead32 (R_CCU_BASE + R_CCU_TWI1_REG),
    MmioRead32 (S_TWI1_BASE + 0x010U) & 0xFFU
    ));

  Status = A733TwiInitS1 (HYM8563_BUS_HZ);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Hym8563: S_TWI1 did not come up (%r)\n", Status));
  }

  return Status;
}

/**
  Returns the current time and date from the HYM8563.

  @param  Time         Receives the snapshot.
  @param  Capabilities Optional; resolution is one second.

  @retval EFI_SUCCESS      The time was read.
  @retval EFI_DEVICE_ERROR The chip did not answer, or its time is invalid
                           (VL set after a power loss).
**/
EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME                *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  EFI_STATUS  Status;
  UINT8       Regs[7];
  UINT8       Index;
  UINT8       Seconds;
  UINT8       Minutes;
  UINT8       Hours;
  UINT8       Days;
  UINT8       Months;
  UINT8       Years;

  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < 7; Index++) {
    Status = Hym8563Read ((UINT8)(HYM8563_REG_SECONDS + Index), &Regs[Index]);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }
  }

  Seconds = BcdToBin (Regs[0] & 0x7F);
  Minutes = BcdToBin (Regs[1] & 0x7F);
  Hours   = BcdToBin (Regs[2] & 0x3F);
  Days    = BcdToBin (Regs[3] & 0x3F);
  Months  = BcdToBin (Regs[5] & 0x1F);
  Years   = BcdToBin (Regs[6]);

  if ((Regs[0] & HYM8563_SECONDS_VL) != 0) {
    //
    // The chip lost its supply; the registers still read, but they are not
    // a time.  Reported once per boot to keep the log readable.
    //
    if (mHym8563Present) {
      DEBUG ((DEBUG_WARN, "Hym8563: VL set, the stored time is invalid\n"));
      mHym8563Present = FALSE;
    }

    return EFI_DEVICE_ERROR;
  }

  if ((Months == 0) || (Months > 12) || (Days == 0) || (Days > 31) ||
      (Hours > 23) || (Minutes > 59) || (Seconds > 59))
  {
    DEBUG ((DEBUG_WARN, "Hym8563: implausible time %02u:%02u:%02u\n", Hours, Minutes, Seconds));
    return EFI_DEVICE_ERROR;
  }

  ZeroMem (Time, sizeof (*Time));
  Time->Year       = (UINT16)(2000 + Years);
  Time->Month      = Months;
  Time->Day        = Days;
  Time->Hour       = Hours;
  Time->Minute     = Minutes;
  Time->Second     = Seconds;
  Time->Nanosecond = 0;
  Time->TimeZone   = EFI_UNSPECIFIED_TIMEZONE;
  Time->Daylight   = 0;

  if (Capabilities != NULL) {
    Capabilities->Resolution = 1;
    Capabilities->Accuracy   = 0;
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

/**
  Sets the HYM8563 to the supplied time.

  Writing the seconds register also clears the VL flag.

  @param  Time  The time to set.

  @retval EFI_SUCCESS           The time was written.
  @retval EFI_INVALID_PARAMETER A field is out of range.
  @retval EFI_DEVICE_ERROR      The chip did not answer.
**/
EFI_STATUS
EFIAPI
LibSetTime (
  IN EFI_TIME  *Time
  )
{
  EFI_STATUS  Status;
  UINT8       Regs[7];

  if ((Time == NULL) ||
      (Time->Year < 2000) || (Time->Year > 2099) ||
      (Time->Month < 1) || (Time->Month > 12) ||
      (Time->Day < 1) || (Time->Day > 31) ||
      (Time->Hour > 23) || (Time->Minute > 59) || (Time->Second > 59))
  {
    return EFI_INVALID_PARAMETER;
  }

  Regs[0] = BinToBcd ((UINT8)Time->Second);            // also clears VL
  Regs[1] = BinToBcd ((UINT8)Time->Minute);
  Regs[2] = BinToBcd ((UINT8)Time->Hour);
  Regs[3] = BinToBcd ((UINT8)Time->Day);
  Regs[4] = WeekdayFromDate (Time->Year, (UINT8)Time->Month, (UINT8)Time->Day);
  Regs[5] = BinToBcd ((UINT8)Time->Month);
  Regs[6] = BinToBcd ((UINT8)(Time->Year - 2000));

  for (UINT8 Index = 0; Index < 7; Index++) {
    Status = Hym8563Write ((UINT8)(HYM8563_REG_SECONDS + Index), Regs[Index]);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

/**
  Wakeup alarms are not wired: RTC_INT_L goes to PL3 but nothing consumes it.

  @retval EFI_UNSUPPORTED  Always.
**/
EFI_STATUS
EFIAPI
LibGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Wakeup alarms are not wired.

  @retval EFI_UNSUPPORTED  Always.
**/
EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN    Enabled,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Describe the TWI block as runtime memory.

  LibGetTime and LibSetTime run in the operating system's EFI runtime
  environment, after boot services are gone.  The kernel maps only memory the
  firmware marked EFI_MEMORY_RUNTIME, so the TWI registers this library polls
  have to carry that attribute; without it the first command-register write
  faults (the Debian kernel's rtc-efi driver does exactly that a few seconds
  into boot).
**/
STATIC
VOID
Hym8563MarkBusRuntime (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Base;
  UINT64                Length;

  Base   = A733_TWI1_BASE;
  Length = SIZE_4KB;

  Status = gDS->AddMemorySpace (
                 EfiGcdMemoryTypeMemoryMappedIo,
                 Base,
                 Length,
                 EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                 );
  if (EFI_ERROR (Status) && (Status != EFI_ACCESS_DENIED)) {
    DEBUG ((
      DEBUG_WARN,
      "Hym8563: cannot describe the TWI block as runtime memory: %r\n",
      Status
      ));
    return;
  }

  Status = gDS->SetMemorySpaceAttributes (
                 Base,
                 Length,
                 EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                 );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Hym8563: cannot mark the TWI block runtime: %r\n", Status));
  }
}

/**
  Move the TWI base to the address the OS mapped it at.

  Runs on the virtual address change event, which is the one moment
  ConvertPointer may be used.  Everything after this point (GetTime/SetTime)
  reaches the controller through the converted address.
**/
STATIC
VOID
EFIAPI
Hym8563VirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EfiConvertPointer (0x0, (VOID **)&mHym8563TwiBase);
}

/**
  Probe the chip and leave the bus ready.

  Returns success even when the chip does not answer, so the runtime
  services stay installed; GetTime reports the failure instead of the whole
  driver disappearing.

  @param  ImageHandle  The driver's image handle (unused).
  @param  SystemTable  The system table (unused).

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
LibRtcInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT8       Seconds;
  EFI_EVENT   Event;

  Hym8563MarkBusRuntime ();

  Status = gBS->CreateEventEx (
                  EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE,
                  TPL_NOTIFY,
                  Hym8563VirtualAddressChange,
                  NULL,
                  NULL,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "Hym8563: cannot register the virtual address change event: %r\n",
      Status
      ));
  }

  Status = Hym8563BusInit ();
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  Status = Hym8563Read (HYM8563_REG_SECONDS, &Seconds);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Hym8563: no answer at 0x%02x on S_TWI1\n", HYM8563_SLAVE_ADDR));
    return EFI_SUCCESS;
  }

  mHym8563Present = TRUE;
  DEBUG ((
    DEBUG_INFO,
    "Hym8563: present on S_TWI1, seconds=0x%02x%s\n",
    Seconds,
    ((Seconds & HYM8563_SECONDS_VL) != 0) ? " (VL set: time lost)" : ""
    ));

  return EFI_SUCCESS;
}