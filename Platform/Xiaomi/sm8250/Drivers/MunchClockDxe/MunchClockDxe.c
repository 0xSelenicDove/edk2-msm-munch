/** @file
  Real date/time for Xiaomi munch, built on the PMIC RTC counter.

  The PM8150 RTC peripheral is a free-running 32-bit seconds counter that keeps
  counting while the phone is off, but it does not hold the real date: Android
  stores its own offset (the counter reads 1970-07-17).  So this driver adds an
  offset of its own, kept in a small text file on the EFI system partition:

      \EFI\munch\clock.txt        "<offset_seconds> <local_minutes_from_utc>"

  utc_seconds = pmic_counter + offset_seconds; the EFI time is utc + the minutes.
  The Windows side (tools/windows/munch-clock-sync.ps1) rewrites the file whenever
  Windows has a good time, so the date survives power-off and restarts.

  At boot (ReadyToBoot, boot services still available) the driver
    1. reads the PMIC counter,
    2. reads clock.txt (if any) and sets the EFI time through SetTime(),
    3. publishes the counter value as the volatile variable MunchRtc so Windows
       can work out the offset later.

  Without clock.txt the time simply stays at the firmware build date.
**/

#include <Uefi.h>

#include <Guid/FileInfo.h>
#include <Protocol/SimpleFileSystem.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

// ---- PMIC arbiter (SPMI), read only --------------------------------------------------
#define PMIC_ARB_CORE_BASE        0x0C440000U
#define PMIC_ARB_CONFIG_BASE      0x0C40A000U
#define PMIC_ARB_OBSERVER_BASE    0x0E600000U
#define PMIC_ARB_FEATURES         0x0004U
#define PMIC_ARB_APID_MAP_BASE    0x0900U
#define PMIC_ARB_MAX_APIDS        512U
#define PMIC_ARB_CMD              0x00U
#define PMIC_ARB_STATUS           0x08U
#define PMIC_ARB_RDATA0           0x18U
#define PMIC_ARB_OP_EXT_READL     1U
#define PMIC_ARB_STATUS_DONE      BIT0
#define PMIC_ARB_STATUS_ERROR     (BIT1 | BIT2 | BIT3)

#define PM8150_SID                0U
#define RTC_CTRL                  0x6046U     // bit7 = RTC enabled
#define RTC_READ                  0x6048U     // 4 bytes, little endian, LSB read first

#define CLOCK_FILE                L"\\EFI\\munch\\clock.txt"

STATIC EFI_GUID  mMunchRtcGuid = { 0x4a1d8b3c, 0x6e52, 0x47f0, { 0x9c, 0x3a, 0x71, 0xe0, 0x5b, 0x28, 0xd4, 0x16 } };

#pragma pack(1)
typedef struct {
  UINT64  PmicSeconds;     // RTC counter when the firmware read it
  UINT64  Counter;         // ARM generic counter at that moment
  UINT64  Frequency;       // ARM generic counter frequency
} MUNCH_RTC_INFO;
#pragma pack()

STATIC
INT32
FindApid (
  IN UINT16  Ppid
  )
{
  UINT32 Count;
  UINT32 Index;

  Count = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_FEATURES) & 0x7FFU;
  if ((Count == 0) || (Count > PMIC_ARB_MAX_APIDS)) {
    Count = PMIC_ARB_MAX_APIDS;
  }

  for (Index = 0; Index < Count; Index++) {
    UINT32 Map;

    Map = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_APID_MAP_BASE + (Index * sizeof (UINT32)));
    if ((Map != 0) && (((Map >> 8) & 0xFFFU) == Ppid)) {
      return (INT32)Index;
    }
  }

  return -1;
}

STATIC
EFI_STATUS
SpmiRead (
  IN  UINT16  Address,
  OUT UINT8   *Value
  )
{
  INT32   Apid;
  UINTN   Channel;
  UINT32  Retry;
  UINT32  Status;

  Apid = FindApid ((UINT16)((PM8150_SID << 8) | (Address >> 8)));
  if (Apid < 0) {
    return EFI_NOT_FOUND;
  }

  Channel = PMIC_ARB_OBSERVER_BASE + (0x80U * (UINT32)Apid);
  MmioWrite32 (Channel + PMIC_ARB_CMD, (PMIC_ARB_OP_EXT_READL << 27) | ((Address & 0xFFU) << 4));
  for (Retry = 0; Retry < 1000; Retry++) {
    Status = MmioRead32 (Channel + PMIC_ARB_STATUS);
    if ((Status & PMIC_ARB_STATUS_DONE) != 0) {
      if ((Status & PMIC_ARB_STATUS_ERROR) != 0) {
        return EFI_DEVICE_ERROR;
      }
      *Value = (UINT8)MmioRead32 (Channel + PMIC_ARB_RDATA0);
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

/**
  Read the 32-bit RTC seconds counter (re-read when the low byte wrapped, like Linux).
**/
STATIC
EFI_STATUS
ReadRtcCounter (
  OUT UINT32  *Seconds
  )
{
  UINT8       B[4];
  UINT8       Again;
  UINT8       Ctrl;
  UINTN       I;
  EFI_STATUS  Status;

  Status = SpmiRead (RTC_CTRL, &Ctrl);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((Ctrl & BIT7) == 0) {
    return EFI_NOT_READY;
  }

  for (I = 0; I < 4; I++) {
    Status = SpmiRead ((UINT16)(RTC_READ + I), &B[I]);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = SpmiRead (RTC_READ, &Again);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Again < B[0]) {
    for (I = 0; I < 4; I++) {
      Status = SpmiRead ((UINT16)(RTC_READ + I), &B[I]);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  *Seconds = (UINT32)B[0] | ((UINT32)B[1] << 8) | ((UINT32)B[2] << 16) | ((UINT32)B[3] << 24);
  return EFI_SUCCESS;
}

// ---- date arithmetic -----------------------------------------------------------------
STATIC
VOID
EpochToTime (
  IN  INT64     Seconds,
  OUT EFI_TIME  *Time
  )
{
  INT64 Days;
  INT64 Rest;
  INT64 Z;
  INT64 Era;
  INT64 Doe;
  INT64 Yoe;
  INT64 Y;
  INT64 Doy;
  INT64 Mp;

  Days = Seconds / 86400;
  Rest = Seconds % 86400;
  if (Rest < 0) {
    Rest += 86400;
    Days--;
  }

  Z   = Days + 719468;
  Era = (Z >= 0 ? Z : Z - 146096) / 146097;
  Doe = Z - Era * 146097;
  Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
  Y   = Yoe + Era * 400;
  Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
  Mp  = (5 * Doy + 2) / 153;

  ZeroMem (Time, sizeof (*Time));
  Time->Day      = (UINT8)(Doy - (153 * Mp + 2) / 5 + 1);
  Time->Month    = (UINT8)(Mp < 10 ? Mp + 3 : Mp - 9);
  Time->Year     = (UINT16)(Y + (Time->Month <= 2 ? 1 : 0));
  Time->Hour     = (UINT8)(Rest / 3600);
  Time->Minute   = (UINT8)((Rest % 3600) / 60);
  Time->Second   = (UINT8)(Rest % 60);
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
}

// ---- clock.txt -----------------------------------------------------------------------
STATIC
BOOLEAN
ParseInt (
  IN OUT CONST CHAR8  **Cursor,
  IN     CONST CHAR8  *End,
  OUT    INT64        *Value
  )
{
  CONST CHAR8  *P;
  BOOLEAN      Neg;
  BOOLEAN      Any;
  INT64        V;

  P = *Cursor;
  while ((P < End) && ((*P == ' ') || (*P == '\t') || (*P == '\r') || (*P == '\n'))) {
    P++;
  }
  Neg = FALSE;
  if ((P < End) && (*P == '-')) {
    Neg = TRUE;
    P++;
  }
  V   = 0;
  Any = FALSE;
  while ((P < End) && (*P >= '0') && (*P <= '9')) {
    V   = V * 10 + (*P - '0');
    Any = TRUE;
    P++;
  }
  if (!Any) {
    return FALSE;
  }
  *Cursor = P;
  *Value  = Neg ? -V : V;
  return TRUE;
}

/**
  Find \EFI\munch\clock.txt on any file system and parse it.
**/
STATIC
BOOLEAN
ReadClockFile (
  OUT INT64  *Offset,
  OUT INT64  *TzMinutes
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            I;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  CHAR8                            Buffer[128];
  UINTN                            Size;
  CONST CHAR8                      *Cursor;
  BOOLEAN                          Ok;

  Ok      = FALSE;
  Handles = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return FALSE;
  }

  for (I = 0; (I < Count) && !Ok; I++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    if (!EFI_ERROR (Root->Open (Root, &File, CLOCK_FILE, EFI_FILE_MODE_READ, 0))) {
      Size = sizeof (Buffer) - 1;
      if (!EFI_ERROR (File->Read (File, &Size, Buffer)) && (Size > 0)) {
        Cursor = Buffer;
        Ok     = ParseInt (&Cursor, Buffer + Size, Offset);
        if (Ok && !ParseInt (&Cursor, Buffer + Size, TzMinutes)) {
          *TzMinutes = 0;
        }
      }
      File->Close (File);
    }
    Root->Close (Root);
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }
  return Ok;
}

STATIC
VOID
EFIAPI
MunchClockReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32          Pmic;
  EFI_STATUS      Status;
  INT64           Offset;
  INT64           Tz;
  EFI_TIME        Time;
  MUNCH_RTC_INFO  Info;
  BOOLEAN         HaveFile;

  gBS->CloseEvent (Event);

  Status = ReadRtcCounter (&Pmic);
  if (EFI_ERROR (Status)) {
    Print (L"Clock: PMIC RTC unavailable (%r)\r\n", Status);
    return;
  }

  Info.PmicSeconds = Pmic;
  Info.Counter     = GetPerformanceCounter ();
  Info.Frequency   = GetPerformanceCounterProperties (NULL, NULL);
  gRT->SetVariable (L"MunchRtc", &mMunchRtcGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                    sizeof (Info), &Info);

  Offset   = 0;
  Tz       = 0;
  HaveFile = ReadClockFile (&Offset, &Tz);
  if (!HaveFile) {
    Print (L"Clock: no clock.txt, PMIC counter %u\r\n", Pmic);
    return;
  }

  EpochToTime ((INT64)Pmic + Offset + Tz * 60, &Time);
  Status = gRT->SetTime (&Time);
  Print (L"Clock: %04u-%02u-%02u %02u:%02u (PMIC RTC) %r\r\n", Time.Year, Time.Month, Time.Day,
         Time.Hour, Time.Minute, Status);
}

EFI_STATUS
EFIAPI
MunchClockDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT Event;

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                MunchClockReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &Event
                );
}
