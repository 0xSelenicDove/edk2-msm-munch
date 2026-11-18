/** @file
  Volume/power key input for Xiaomi munch, read directly from the PM8150.

  Qualcomm's ButtonsDxe depends on PmicDxe, which is built for a different
  product and does not work on munch.  This driver reads the keys through the
  PMIC arbiter (read-only) and exposes them as a console input device:

    Volume Up   (PM8150 GPIO6, active low)   -> Up arrow
    Volume Down (PM8150 PON RESIN)           -> Down arrow
    Power       (PM8150 PON KPDPWR)          -> Enter

  The input handle uses a fixed vendor device path that PlatformBootManagerLib
  adds to ConIn.
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

#define PMIC_ARB_CORE_BASE      0x0C440000U
#define PMIC_ARB_OBSERVER_BASE  0x0E600000U
#define PMIC_ARB_VERSION        0x0000U
#define PMIC_ARB_FEATURES       0x0004U
#define PMIC_ARB_APID_MAP_BASE  0x0900U
#define PMIC_ARB_MAX_APIDS      512U

#define PMIC_ARB_CMD            0x00U
#define PMIC_ARB_STATUS         0x08U
#define PMIC_ARB_RDATA0         0x18U
#define PMIC_ARB_OP_EXT_READL   1U
#define PMIC_ARB_STATUS_DONE    BIT0
#define PMIC_ARB_STATUS_ERROR   (BIT1 | BIT2 | BIT3)

// PM8150 is SID 0.  PPID = (SID << 8) | (register address >> 8).
#define PON_PPID                0x008U
#define PON_RT_STS_REG          0x10U    // offset inside the PON peripheral
#define PON_KPDPWR_BIT          BIT0
#define PON_RESIN_BIT           BIT1

#define GPIO6_PPID              0x0C5U   // PM8150 GPIO6 peripheral 0xC500
#define GPIO_STATUS_REG         0x08U
#define GPIO_STATUS_LEVEL_BIT   BIT0

#define POLL_PERIOD_100NS       200000U  // 20 ms
#define QUEUE_SIZE              16U
#define MAX_NOTIFY              8U

#define BUTTON_VOL_UP           0
#define BUTTON_VOL_DOWN         1
#define BUTTON_POWER            2
#define BUTTON_COUNT            3

typedef struct {
  BOOLEAN  InUse;
  EFI_KEY_DATA                     KeyData;
  EFI_KEY_NOTIFY_FUNCTION          Function;
} NOTIFY_ENTRY;

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} MUNCH_KEYPAD_DEVICE_PATH;

STATIC MUNCH_KEYPAD_DEVICE_PATH mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    // Keep in sync with mMunchKeypad in PlatformBm.c.
    { 0x3f6b1c92, 0x5a07, 0x4e4d, { 0x9b, 0x21, 0x6c, 0x0d, 0x41, 0xa8, 0x7e, 0x15 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL     mTextIn;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  mTextInEx;
STATIC EFI_HANDLE                         mHandle;
STATIC EFI_EVENT                          mTimer;
STATIC EFI_EVENT                          mExitBsEvent;

STATIC INT32    mPonApid  = -1;
STATIC INT32    mGpioApid = -1;
STATIC BOOLEAN  mGpioOk   = FALSE;

STATIC EFI_INPUT_KEY  mQueue[QUEUE_SIZE];
STATIC UINTN          mHead;
STATIC UINTN          mTail;
STATIC NOTIFY_ENTRY   mNotify[MAX_NOTIFY];

STATIC BOOLEAN  mPressed[BUTTON_COUNT];
STATIC BOOLEAN  mLastRaw[BUTTON_COUNT];
STATIC UINT32   mHeldTicks[BUTTON_COUNT];

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

    Map = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_APID_MAP_BASE +
                      (Index * sizeof (UINT32)));
    if ((Map != 0) && (((Map >> 8) & 0xFFFU) == Ppid)) {
      return (INT32)Index;
    }
  }

  return -1;
}

STATIC
EFI_STATUS
SpmiReadReg (
  IN  INT32   Apid,
  IN  UINT8   Offset,
  OUT UINT8   *Value
  )
{
  UINTN   Channel;
  UINT32  Retry;
  UINT32  Status;

  Channel = PMIC_ARB_OBSERVER_BASE + (0x80U * (UINT32)Apid);
  MmioWrite32 (Channel + PMIC_ARB_CMD,
               (PMIC_ARB_OP_EXT_READL << 27) | ((UINT32)Offset << 4));

  for (Retry = 0; Retry < 1000; Retry++) {
    Status = MmioRead32 (Channel + PMIC_ARB_STATUS);
    if ((Status & PMIC_ARB_STATUS_DONE) != 0) {
      if ((Status & PMIC_ARB_STATUS_ERROR) != 0) {
        return EFI_DEVICE_ERROR;
      }
      *Value = (UINT8)MmioRead32 (Channel + PMIC_ARB_RDATA0);
      return EFI_SUCCESS;
    }
    gBS->Stall (1);
  }

  return EFI_TIMEOUT;
}

STATIC
VOID
QueuePush (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  UINTN  Next;
  UINTN  Index;

  Next = (mTail + 1) % QUEUE_SIZE;
  if (Next != mHead) {
    mQueue[mTail].ScanCode    = ScanCode;
    mQueue[mTail].UnicodeChar = UnicodeChar;
    mTail = Next;
  }

  for (Index = 0; Index < MAX_NOTIFY; Index++) {
    if (mNotify[Index].InUse &&
        (mNotify[Index].KeyData.Key.ScanCode == ScanCode) &&
        (mNotify[Index].KeyData.Key.UnicodeChar == UnicodeChar)) {
      EFI_KEY_DATA Data;

      ZeroMem (&Data, sizeof (Data));
      Data.Key.ScanCode    = ScanCode;
      Data.Key.UnicodeChar = UnicodeChar;
      Data.KeyState.KeyShiftState  = EFI_SHIFT_STATE_VALID;
      Data.KeyState.KeyToggleState = EFI_TOGGLE_STATE_VALID;
      mNotify[Index].Function (&Data);
    }
  }
}

STATIC
VOID
ButtonEvent (
  IN UINTN  Button
  )
{
  switch (Button) {
    case BUTTON_VOL_UP:
      QueuePush (SCAN_UP, CHAR_NULL);
      break;
    case BUTTON_VOL_DOWN:
      QueuePush (SCAN_DOWN, CHAR_NULL);
      break;
    default:
      QueuePush (SCAN_NULL, CHAR_CARRIAGE_RETURN);
      break;
  }
}

STATIC
VOID
EFIAPI
PollTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  BOOLEAN  Raw[BUTTON_COUNT];
  UINT8    Value;
  UINTN    Index;

  Raw[BUTTON_VOL_UP]   = FALSE;
  Raw[BUTTON_VOL_DOWN] = FALSE;
  Raw[BUTTON_POWER]    = FALSE;

  if (mPonApid >= 0 && !EFI_ERROR (SpmiReadReg (mPonApid, PON_RT_STS_REG, &Value))) {
    Raw[BUTTON_VOL_DOWN] = (Value & PON_RESIN_BIT) != 0;
    Raw[BUTTON_POWER]    = (Value & PON_KPDPWR_BIT) != 0;
  }

  if (mGpioOk && !EFI_ERROR (SpmiReadReg (mGpioApid, GPIO_STATUS_REG, &Value))) {
    // Active low with pull-up: level 0 means pressed.
    Raw[BUTTON_VOL_UP] = (Value & GPIO_STATUS_LEVEL_BIT) == 0;
  }

  for (Index = 0; Index < BUTTON_COUNT; Index++) {
    // Two identical consecutive samples (40 ms) before a state change.
    if (Raw[Index] == mLastRaw[Index]) {
      if (Raw[Index] != mPressed[Index]) {
        mPressed[Index]   = Raw[Index];
        mHeldTicks[Index] = 0;
        if (mPressed[Index]) {
          ButtonEvent (Index);
        }
      } else if (mPressed[Index] && Index != BUTTON_POWER) {
        // Auto-repeat the volume keys while held: 500 ms delay, then 150 ms.
        mHeldTicks[Index]++;
        if (mHeldTicks[Index] >= 25 && ((mHeldTicks[Index] - 25) % 8) == 0) {
          ButtonEvent (Index);
        }
      }
    }
    mLastRaw[Index] = Raw[Index];
  }
}

//
// Simple Text Input
//
STATIC
EFI_STATUS
EFIAPI
TextInReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mHead = 0;
  mTail = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TextInReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_TPL  OldTpl;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mHead == mTail) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }

  *Key  = mQueue[mHead];
  mHead = (mHead + 1) % QUEUE_SIZE;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
WaitForKeyNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mHead != mTail) {
    gBS->SignalEvent (Event);
  }
}

//
// Simple Text Input Ex
//
STATIC
EFI_STATUS
EFIAPI
TextInExReset (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  return TextInReset (&mTextIn, ExtendedVerification);
}

STATIC
EFI_STATUS
EFIAPI
TextInExReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  EFI_STATUS  Status;

  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (KeyData, sizeof (*KeyData));
  Status = TextInReadKeyStroke (&mTextIn, &KeyData->Key);
  KeyData->KeyState.KeyShiftState  = EFI_SHIFT_STATE_VALID;
  KeyData->KeyState.KeyToggleState = EFI_TOGGLE_STATE_VALID;
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
TextInExSetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_KEY_TOGGLE_STATE               *KeyToggleState
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TextInExRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                               **NotifyHandle
  )
{
  UINTN    Index;
  EFI_TPL  OldTpl;

  if ((KeyData == NULL) || (KeyNotificationFunction == NULL) || (NotifyHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  for (Index = 0; Index < MAX_NOTIFY; Index++) {
    if (!mNotify[Index].InUse) {
      mNotify[Index].InUse    = TRUE;
      mNotify[Index].KeyData  = *KeyData;
      mNotify[Index].Function = KeyNotificationFunction;
      *NotifyHandle = &mNotify[Index];
      gBS->RestoreTPL (OldTpl);
      return EFI_SUCCESS;
    }
  }
  gBS->RestoreTPL (OldTpl);
  return EFI_OUT_OF_RESOURCES;
}

STATIC
EFI_STATUS
EFIAPI
TextInExUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN VOID                               *NotificationHandle
  )
{
  UINTN  Index;

  if (NotificationHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < MAX_NOTIFY; Index++) {
    if (NotificationHandle == &mNotify[Index]) {
      mNotify[Index].InUse = FALSE;
      return EFI_SUCCESS;
    }
  }
  return EFI_INVALID_PARAMETER;
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  // Leave the PMIC alone once the OS takes over.
  if (mTimer != NULL) {
    gBS->SetTimer (mTimer, TimerCancel, 0);
  }
}

EFI_STATUS
EFIAPI
MunchButtonsDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Version;
  UINT8       Probe;

  Version = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_VERSION);
  if ((Version & 0xF0000000U) != 0x50000000U) {
    DEBUG ((DEBUG_ERROR, "MunchButtonsDxe: unsupported PMIC arbiter 0x%08x\n", Version));
    return EFI_UNSUPPORTED;
  }

  mPonApid = FindApid (PON_PPID);
  if (mPonApid < 0 || EFI_ERROR (SpmiReadReg (mPonApid, PON_RT_STS_REG, &Probe))) {
    DEBUG ((DEBUG_ERROR, "MunchButtonsDxe: PON peripheral not readable\n"));
    return EFI_NOT_FOUND;
  }

  mGpioApid = FindApid (GPIO6_PPID);
  mGpioOk   = (mGpioApid >= 0) &&
              !EFI_ERROR (SpmiReadReg (mGpioApid, GPIO_STATUS_REG, &Probe));

  mTextIn.Reset         = TextInReset;
  mTextIn.ReadKeyStroke = TextInReadKeyStroke;
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, WaitForKeyNotify,
                             NULL, &mTextIn.WaitForKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mTextInEx.Reset               = TextInExReset;
  mTextInEx.ReadKeyStrokeEx     = TextInExReadKeyStrokeEx;
  mTextInEx.SetState            = TextInExSetState;
  mTextInEx.RegisterKeyNotify   = TextInExRegisterKeyNotify;
  mTextInEx.UnregisterKeyNotify = TextInExUnregisterKeyNotify;
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, WaitForKeyNotify,
                             NULL, &mTextInEx.WaitForKeyEx);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHandle,
                  &gEfiDevicePathProtocolGuid,         &mDevicePath,
                  &gEfiSimpleTextInProtocolGuid,       &mTextIn,
                  &gEfiSimpleTextInputExProtocolGuid,  &mTextInEx,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, OnExitBootServices,
                               NULL, &gEfiEventExitBootServicesGuid, &mExitBsEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             PollTimer, NULL, &mTimer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // A key that is already held at start-up counts as one press, so that
  // holding a key during power-on still reaches the boot menu.
  Status = gBS->SetTimer (mTimer, TimerPeriodic, POLL_PERIOD_100NS);
  DEBUG ((DEBUG_INFO, "MunchButtonsDxe: ready (gpio6 %a)\n", mGpioOk ? "ok" : "n/a"));
  return Status;
}
