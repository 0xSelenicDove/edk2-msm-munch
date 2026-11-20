/** @file
 *
 *  Software EFI RealTimeClock for platforms without a usable hardware RTC.
 *
 *  The time is kept as "base time + ARM generic timer ticks since it was set".
 *  It starts at the firmware build date; a platform driver (for example
 *  MunchClockDxe, which reads the PMIC RTC counter) sets the real time during boot
 *  through SetTime(), and the OS may call SetTime() too.  Nothing is stored in
 *  hardware: the value only lives until the next reset.
 *
 *  Copyright (c), 2018, Andrei Warkentin <andrey.warkentin@gmail.com>
 *  Copyright (c) Microsoft Corporation. All rights reserved.
 *
 *  This program and the accompanying materials
 *  are licensed and made available under the terms and conditions of the BSD
 *License which accompanies this distribution.  The full text of the license may
 *be found at http://opensource.org/licenses/bsd-license.php
 *
 *  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR
 *IMPLIED.
 *
 **/

#include <PiDxe.h>

#include <Library/ArmGenericTimerCounterLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimerLib.h>

#define SECONDS_PER_DAY  86400ULL

// Seconds since 1970-01-01 of the base time, and the counter value it was set at.
STATIC UINT64 mBaseSeconds;
STATIC UINT64 mBaseCounter;

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm).
STATIC
VOID
DaysToCivil (
  IN  INT64   Days,
  OUT UINT16  *Year,
  OUT UINT8   *Month,
  OUT UINT8   *Day
  )
{
  INT64 Z;
  INT64 Era;
  INT64 Doe;
  INT64 Yoe;
  INT64 Y;
  INT64 Doy;
  INT64 Mp;

  Z   = Days + 719468;
  Era = (Z >= 0 ? Z : Z - 146096) / 146097;
  Doe = Z - Era * 146097;
  Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
  Y   = Yoe + Era * 400;
  Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
  Mp  = (5 * Doy + 2) / 153;
  *Day   = (UINT8)(Doy - (153 * Mp + 2) / 5 + 1);
  *Month = (UINT8)(Mp < 10 ? Mp + 3 : Mp - 9);
  *Year  = (UINT16)(Y + (*Month <= 2 ? 1 : 0));
}

STATIC
INT64
CivilToDays (
  IN UINT16  Year,
  IN UINT8   Month,
  IN UINT8   Day
  )
{
  INT64 Y;
  INT64 Era;
  INT64 Yoe;
  INT64 Doy;
  INT64 Doe;

  Y   = (INT64)Year - (Month <= 2 ? 1 : 0);
  Era = (Y >= 0 ? Y : Y - 399) / 400;
  Yoe = Y - Era * 400;
  Doy = (153 * (Month > 2 ? Month - 3 : Month + 9) + 2) / 5 + Day - 1;
  Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
  return Era * 146097 + Doe - 719468;
}

// Firmware build date ("Mmm dd yyyy" / "hh:mm:ss") as the initial time.
STATIC
UINT64
BuildTimeSeconds (
  VOID
  )
{
  STATIC CONST CHAR8 Months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  CONST CHAR8        *D       = __DATE__;
  CONST CHAR8        *T       = __TIME__;
  UINT8              Month;
  UINT8              Day;
  UINT16             Year;
  UINTN              I;

  Month = 1;
  for (I = 0; I < 12; I++) {
    if ((D[0] == Months[I * 3]) && (D[1] == Months[I * 3 + 1]) && (D[2] == Months[I * 3 + 2])) {
      Month = (UINT8)(I + 1);
      break;
    }
  }

  Day  = (UINT8)(((D[4] == ' ') ? 0 : (D[4] - '0')) * 10 + (D[5] - '0'));
  Year = (UINT16)((D[7] - '0') * 1000 + (D[8] - '0') * 100 + (D[9] - '0') * 10 + (D[10] - '0'));

  return (UINT64)CivilToDays (Year, Month, Day) * SECONDS_PER_DAY +
         (UINT64)((T[0] - '0') * 10 + (T[1] - '0')) * 3600 +
         (UINT64)((T[3] - '0') * 10 + (T[4] - '0')) * 60 +
         (UINT64)((T[6] - '0') * 10 + (T[7] - '0'));
}

/**
  Returns the current time and date information, and the capabilities of the
  software RTC.
**/
EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  UINT32  Freq;
  UINT64  Seconds;
  UINT64  Days;
  UINT64  Rest;

  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Freq = ArmGenericTimerGetTimerFreq ();
  ASSERT (Freq != 0);
  if (Freq == 0) {
    return EFI_DEVICE_ERROR;
  }

  if (Capabilities != NULL) {
    Capabilities->Accuracy   = 0;
    Capabilities->Resolution = 1;
    Capabilities->SetsToZero = FALSE;
  }

  Seconds = mBaseSeconds + (GetPerformanceCounter () - mBaseCounter) / Freq;
  Days    = Seconds / SECONDS_PER_DAY;
  Rest    = Seconds % SECONDS_PER_DAY;

  ZeroMem (Time, sizeof (*Time));
  DaysToCivil ((INT64)Days, &Time->Year, &Time->Month, &Time->Day);
  Time->Hour       = (UINT8)(Rest / 3600);
  Time->Minute     = (UINT8)((Rest % 3600) / 60);
  Time->Second     = (UINT8)(Rest % 60);
  Time->TimeZone   = EFI_UNSPECIFIED_TIMEZONE;
  Time->Daylight   = 0;

  return EFI_SUCCESS;
}

/**
  Sets the current time.  The value is kept in RAM only.
**/
EFI_STATUS
EFIAPI
LibSetTime (
  IN EFI_TIME  *Time
  )
{
  if ((Time == NULL) || (Time->Year < 1970) || (Time->Month < 1) || (Time->Month > 12) ||
      (Time->Day < 1) || (Time->Day > 31) || (Time->Hour > 23) || (Time->Minute > 59) ||
      (Time->Second > 59)) {
    return EFI_INVALID_PARAMETER;
  }

  mBaseSeconds = (UINT64)CivilToDays (Time->Year, Time->Month, Time->Day) * SECONDS_PER_DAY +
                 (UINT64)Time->Hour * 3600 + (UINT64)Time->Minute * 60 + Time->Second;
  mBaseCounter = GetPerformanceCounter ();
  return EFI_SUCCESS;
}

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

EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN    Enabled,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibRtcInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  mBaseSeconds = BuildTimeSeconds ();
  mBaseCounter = GetPerformanceCounter ();
  return EFI_SUCCESS;
}

VOID
EFIAPI
LibRtcVirtualNotifyEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  return;
}
