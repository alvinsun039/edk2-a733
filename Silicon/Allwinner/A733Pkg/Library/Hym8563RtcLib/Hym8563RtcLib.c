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

  The alarm registers back GetWakeupTime/SetWakeupTime.  RTC_INT_L is on
  PL3, which the ARISC super-standby monitors, so an alarm can resume the
  board from S3.

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
#define HYM8563_REG_ALARM_MIN     0x09U
#define HYM8563_REG_ALARM_HOUR    0x0AU
#define HYM8563_REG_ALARM_DAY     0x0BU
#define HYM8563_REG_ALARM_WEEKDAY 0x0CU

#define HYM8563_SECONDS_VL    BIT7   // voltage-low: the time is not valid
#define HYM8563_MONTHS_CENTURY BIT7  // only meaningful on some parts

// Control_2: bit 1 enables the alarm interrupt, bit 3 is the alarm flag
// (write 0 to clear it).  Bit 7 of each alarm register ignores that field.
#define HYM8563_CTRL2_AIE     BIT1
#define HYM8563_CTRL2_AF      BIT3
#define HYM8563_ALARM_DISABLE BIT7

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
  Number of days in a month.

  The alarm matches a day of month in every month, so the firmware has to
  know which months actually contain that day before it can tell the caller
  when the alarm will fire.
**/
STATIC
UINT8
DaysInMonth (
  IN UINT16  Year,
  IN UINT8   Month
  )
{
  STATIC CONST UINT8  MonthDays[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  UINT8               Days;

  Days = MonthDays[Month - 1];
  if ((Month == 2) &&
      ((Year % 4) == 0) &&
      (((Year % 100) != 0) || ((Year % 400) == 0)))
  {
    Days++;
  }

  return Days;
}

/**
  Next month and year in which the chip's day-of-month alarm matches.

  The chip stores minute, hour and day of month only.  A day that does not
  exist in the next month (29-31) makes it skip to a later month, which is
  what this function computes.  Callers use the result to reject an alarm
  date the hardware cannot represent without firing early.
**/
STATIC
VOID
NextAlarmDate (
  IN  CONST EFI_TIME  *Now,
  IN  UINT8           Day,
  IN  UINT8           Hour,
  IN  UINT8           Minute,
  OUT UINT16          *Year,
  OUT UINT8           *Month
  )
{
  BOOLEAN  NextMonth;
  UINT16   CandidateYear;
  UINT8    CandidateMonth;
  UINTN    Index;

  if (Day > Now->Day) {
    NextMonth = FALSE;
  } else if (Day < Now->Day) {
    NextMonth = TRUE;
  } else {
    NextMonth = (Hour < Now->Hour) ||
                ((Hour == Now->Hour) && (Minute <= Now->Minute));
  }

  CandidateYear  = Now->Year;
  CandidateMonth = Now->Month;
  if (NextMonth) {
    if (CandidateMonth == 12) {
      CandidateYear++;
      CandidateMonth = 1;
    } else {
      CandidateMonth++;
    }
  }

  for (Index = 0; Index < 12; Index++) {
    if (Day <= DaysInMonth (CandidateYear, CandidateMonth)) {
      break;
    }

    if (CandidateMonth == 12) {
      CandidateYear++;
      CandidateMonth = 1;
    } else {
      CandidateMonth++;
    }
  }

  *Year  = CandidateYear;
  *Month = CandidateMonth;
}

/**
  Returns the HYM8563 alarm as the EFI wakeup time.

  Month and year are reconstructed as the next occurrence of the stored day
  of month, because the chip does not keep them.  A weekday alarm set by
  another driver has no EFI_TIME equivalent and is reported as unsupported
  while enabled.

  @param  Enabled  Receives TRUE when the alarm interrupt is enabled.
  @param  Pending  Receives TRUE when the alarm flag is set.
  @param  Time     Receives the alarm time, at minute resolution.

  @retval EFI_SUCCESS           The alarm state was read.
  @retval EFI_INVALID_PARAMETER An output pointer is NULL.
  @retval EFI_DEVICE_ERROR      The chip did not answer, or the clock is
                                invalid so the date cannot be reconstructed.
  @retval EFI_UNSUPPORTED       The enabled alarm matches a weekday, which
                                EFI_TIME cannot express.
**/
EFI_STATUS
EFIAPI
LibGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  EFI_STATUS  Status;
  EFI_TIME    Now;
  UINT8       Ctrl2;
  UINT8       AlarmMin;
  UINT8       AlarmHour;
  UINT8       AlarmDay;
  UINT8       AlarmWeekday;
  UINT8       Minute;
  UINT8       Hour;
  UINT8       Day;
  UINT16      Year;
  UINT8       Month;
  INT16       TimeZone;
  UINT8       Daylight;

  if ((Enabled == NULL) || (Pending == NULL) || (Time == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = Hym8563Read (HYM8563_REG_CTRL2, &Ctrl2);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Read (HYM8563_REG_ALARM_MIN, &AlarmMin);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Read (HYM8563_REG_ALARM_HOUR, &AlarmHour);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Read (HYM8563_REG_ALARM_DAY, &AlarmDay);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Read (HYM8563_REG_ALARM_WEEKDAY, &AlarmWeekday);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  *Enabled = (Ctrl2 & HYM8563_CTRL2_AIE) != 0;
  *Pending = (Ctrl2 & HYM8563_CTRL2_AF) != 0;

  Status = LibGetTime (&Now, NULL);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  if (((AlarmDay & HYM8563_ALARM_DISABLE) != 0) &&
      ((AlarmWeekday & HYM8563_ALARM_DISABLE) == 0))
  {
    //
    // A weekday-only alarm (day of month ignored) has no EFI_TIME
    // equivalent.  With the alarm disabled the date is irrelevant, so
    // report the current one.
    //
    if (*Enabled) {
      return EFI_UNSUPPORTED;
    }

    Minute = Now.Minute;
    Hour   = Now.Hour;
    Day    = Now.Day;
    Year   = Now.Year;
    Month  = Now.Month;
  } else {
    //
    // A field whose disable bit is set is ignored by the chip; use the
    // current clock value so the result is still a complete EFI_TIME.
    //
    Minute = (AlarmMin & HYM8563_ALARM_DISABLE) != 0
               ? Now.Minute
               : BcdToBin (AlarmMin & 0x7F);
    Hour   = (AlarmHour & HYM8563_ALARM_DISABLE) != 0
               ? Now.Hour
               : BcdToBin (AlarmHour & 0x3F);
    Day    = (AlarmDay & HYM8563_ALARM_DISABLE) != 0
               ? Now.Day
               : BcdToBin (AlarmDay & 0x3F);

    if ((Minute > 59) || (Hour > 23) || (Day == 0) || (Day > 31)) {
      if (*Enabled) {
        return EFI_DEVICE_ERROR;
      }

      Minute = Now.Minute;
      Hour   = Now.Hour;
      Day    = Now.Day;
    }

    NextAlarmDate (&Now, Day, Hour, Minute, &Year, &Month);
  }

  //
  // Keep the time zone and daylight fields the runtime driver placed in the
  // output structure before calling this library.
  //
  TimeZone = Time->TimeZone;
  Daylight = Time->Daylight;

  ZeroMem (Time, sizeof (*Time));
  Time->Year       = Year;
  Time->Month      = Month;
  Time->Day        = Day;
  Time->Hour       = Hour;
  Time->Minute     = Minute;
  Time->Second     = 0;
  Time->Nanosecond = 0;
  Time->TimeZone   = TimeZone;
  Time->Daylight   = Daylight;

  return EFI_SUCCESS;
}

/**
  Sets the HYM8563 alarm used as the EFI wakeup time.

  The chip compares minute, hour and day of month; it has no month or year
  register, so it fires on the next occurrence of that day.  Only that next
  occurrence is accepted, because any later date would fire a month early.
  Seconds are not stored and the alarm fires at second 0 of the minute.

  @param  Enabled  Enable or disable the alarm.
  @param  Time     Alarm time when Enabled is TRUE; ignored otherwise.

  @retval EFI_SUCCESS           The alarm was programmed or disabled.
  @retval EFI_INVALID_PARAMETER Time is missing, out of range, or not the
                                next date the chip can match.
  @retval EFI_DEVICE_ERROR      The chip did not answer.
**/
EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN    Enabled,
  OUT EFI_TIME  *Time
  )
{
  EFI_STATUS  Status;
  EFI_TIME    Now;
  UINT8       Ctrl2;
  UINT16      Year;
  UINT8       Month;

  if (!Enabled) {
    Status = Hym8563Read (HYM8563_REG_CTRL2, &Ctrl2);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    Ctrl2 &= ~(HYM8563_CTRL2_AIE | HYM8563_CTRL2_AF);
    Status = Hym8563Write (HYM8563_REG_CTRL2, Ctrl2);
    return EFI_ERROR (Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  }

  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Time->Year < 2000) || (Time->Year > 2099) ||
      (Time->Month < 1) || (Time->Month > 12) ||
      (Time->Day < 1) || (Time->Day > 31) ||
      (Time->Hour > 23) || (Time->Minute > 59) || (Time->Second > 59))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = LibGetTime (&Now, NULL);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  NextAlarmDate (
    &Now,
    (UINT8)Time->Day,
    (UINT8)Time->Hour,
    (UINT8)Time->Minute,
    &Year,
    &Month
    );
  if ((Time->Year != Year) || (Time->Month != Month)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Disable the interrupt and clear a stale flag before changing the
  // comparison registers, so a half-updated alarm cannot fire.
  //
  Status = Hym8563Read (HYM8563_REG_CTRL2, &Ctrl2);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Ctrl2 &= ~(HYM8563_CTRL2_AIE | HYM8563_CTRL2_AF);
  Status = Hym8563Write (HYM8563_REG_CTRL2, Ctrl2);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Write (
             HYM8563_REG_ALARM_MIN,
             BinToBcd ((UINT8)Time->Minute)
             );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Write (
             HYM8563_REG_ALARM_HOUR,
             BinToBcd ((UINT8)Time->Hour)
             );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Write (
             HYM8563_REG_ALARM_DAY,
             BinToBcd ((UINT8)Time->Day)
             );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Compare both the day of the month and the weekday.  This chip does not
  // honour the alarm-enable bit as "ignore" in the weekday register, so a
  // 0x80 there leaves the alarm matching weekday 0 and it never fires.
  //
  Status = Hym8563Write (
             HYM8563_REG_ALARM_WEEKDAY,
             WeekdayFromDate (
               (UINT16)Time->Year,
               (UINT8)Time->Month,
               (UINT8)Time->Day
               )
             );
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Status = Hym8563Read (HYM8563_REG_CTRL2, &Ctrl2);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  Ctrl2 &= ~HYM8563_CTRL2_AF;
  Ctrl2 |= HYM8563_CTRL2_AIE;
  Status = Hym8563Write (HYM8563_REG_CTRL2, Ctrl2);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
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
  UINT8       Ctrl2;
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

  //
  // A previous OS may have left its alarm armed.  It has already done its
  // job; clearing the enable and the flag keeps a stale match from waking
  // the ARISC on the next standby.
  //
  Status = Hym8563Read (HYM8563_REG_CTRL2, &Ctrl2);
  if (!EFI_ERROR (Status)) {
    Ctrl2 &= ~(HYM8563_CTRL2_AIE | HYM8563_CTRL2_AF);
    Hym8563Write (HYM8563_REG_CTRL2, Ctrl2);
  }

  DEBUG ((
    DEBUG_INFO,
    "Hym8563: present on S_TWI1, seconds=0x%02x%s\n",
    Seconds,
    ((Seconds & HYM8563_SECONDS_VL) != 0) ? " (VL set: time lost)" : ""
    ));

  return EFI_SUCCESS;
}