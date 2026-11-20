/** @file
  Bluetooth chip-alive test for Xiaomi munch (SM8250): does the QCA6390 answer an HCI command?

  Nothing in Windows is involved.  This driver proves the hardware side before any Windows
  work is started (see .dl/notes/bluetooth.md).  From the device tree (bt_qca6390,
  qupv3_se6_4uart):
    * chip      : Qualcomm QCA6390 Wi-Fi/Bluetooth combo, Bluetooth on a UART (H4 HCI)
    * UART      : QUP wrapper 0, serial engine 6 (0x998000), GPIO 16 CTS, 17 RTS, 18 TX,
                  19 RX (function qup6, TLMM north tile), 115200 8N1 with RTS/CTS at start-up
    * BT_EN     : GPIO 21 (north tile, active high); SW_CTRL: GPIO 124 (input)
    * supplies  : pm8150 s4 1.8 V, s6 0.95 V (aon + dig), s5 1.9 V (rfa1), pm8150a s8 1.35 V
                  (rfa2), pm8150 l16 3.0 V (asd); all through RPMh
    * no sleep clock is needed

  Steps, each printed on the early console:
    1. enable the QUP clocks (wrapper 0 AHB, core and SE6 branch; same RPMh QUP0/CN0 bus
       vote as MunchTouchDxe if the clocks do not start), and program the SE6 clock to
       7.3728 MHz (19.2 MHz * 48 / 125) so that 115200 baud divides exactly
    2. initialise the serial engine as a UART and run an internal loopback self test.  The
       pins are not muxed yet, so nothing leaves the SoC; this checks clock, GENI firmware
       and the FIFO paths on their own
    3. mux the UART pins, hold BT_EN low, power the five rails, raise BT_EN
    4. HCI Reset (01 03 0c 00) and the Qualcomm "read patch version" vendor command
       (01 00 fc 01 19), first with hardware flow control, then with CTS ignored

  The rails and BT_EN are left on at the end so a later Windows driver can use the chip.
  If a step fails, the driver prints why and stops; it never touches anything else.
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextOut.h>

// How long the result stays on screen before the boot continues.
#define MUNCH_BT_HOLD_MS      6000U

// Baud rate for the first contact (the QCA6390 ROM starts at 115200).
#define BT_BAUD               115200U

// Set to FALSE to switch BT_EN and the rails back off at the end of the test.
#define MUNCH_BT_LEAVE_ON     TRUE

// ---------------------------------------------------------------- clocks (GCC)
#define GCC_BASE              0x00100000U
#define GCC_QUP_VOTE_REG      0x52008U       // bit6 wrap0 M-AHB, bit7 S-AHB, bit8 core, bit9 core2x, bit16 SE6 clock
#define GCC_WRAP0_M_AHB_HALT  0x17004U
#define GCC_WRAP0_S_AHB_HALT  0x17008U
#define GCC_WRAP0_S6_HALT     0x1772CU
#define GCC_WRAP0_S6_RCG      0x17730U       // CMD; +4 CFG, +8 M, +0xC N, +0x10 D

#define QUP_VOTE_BITS         (BIT6 | BIT7 | BIT8 | BIT9 | BIT16)

#define RCG_CMD_UPDATE        BIT0
#define RCG_CMD_ROOT_EN       BIT1
#define RCG_CFG_DUAL_EDGE     (2U << 12)     // M/N counter mode; source 0 = XO, divider 1

// 19.2 MHz * 48 / 125 = 7.3728 MHz = 115200 * 64 (16x oversampling: divider 4)
#define SE_CLK_M              48U
#define SE_CLK_N              125U
#define SE_CLK_HZ             7372800U

// ---------------------------------------------------------------- RPMh
// Command DB (read only) and the APPS RSC, DRV2 (HLOS).  Same as MunchTouchDxe.
#define CMD_DB_BASE           0x80860000U
#define CMD_DB_SIZE           0x00020000U
#define CMD_DB_DATA_OFFSET    0x90U
#define CMD_DB_MAX_SLV        8U
#define CMD_DB_ENTRY_SIZE     24U

#define RSC_DRV2_BASE         0x18220000U
#define RSC_TCS_OFFSET        0x0D00U
#define RSC_DRV_ID_REG        0x00U
#define RSC_TCS_STRIDE        672U
#define RSC_FIRST_ACTIVE_TCS  0U
#define RSC_ACTIVE_TCS_COUNT  2U

#define TCS_AMC_ENABLE        BIT16
#define TCS_AMC_TRIGGER       BIT24
#define CMD_MSGID_LEN         8U
#define CMD_MSGID_RESP_REQ    BIT8
#define CMD_MSGID_WRITE       BIT16
#define CMD_STATUS_COMPL      BIT16

// ---------------------------------------------------------------- pin controller
#define TLMM_NORTH            0x0F900000U
#define TLMM_PIN_STRIDE       0x1000U

#define BT_UART_CTS_GPIO      16U
#define BT_UART_RTS_GPIO      17U
#define BT_UART_TX_GPIO       18U
#define BT_UART_RX_GPIO       19U
#define BT_EN_GPIO            21U
#define BT_SW_CTRL_GPIO       124U

// ctl register: pull [1:0] (0 none, 1 down, 3 up), function [5:2], drive [8:6] (2 mA * (n+1)), oe bit 9.
// io register : bit0 input value, bit1 output value.
#define PIN_CTL(Func, Pull, Drive, Oe) \
  ((UINT32)(Pull) | ((UINT32)(Func) << 2) | ((UINT32)(Drive) << 6) | ((UINT32)(Oe) << 9))

#define PULL_NONE             0U
#define PULL_DOWN             1U
#define PULL_UP               3U
#define FUNC_GPIO             0U
#define FUNC_QUP6             1U

// ---------------------------------------------------------------- GENI serial engine 6
#define QUP0_WRAPPER_BASE     0x009C0000U
#define QUP_HW_VERSION        0x004U
#define QUP_SE_VERSION_2_5    0x20050000U    // from this version on the UART samples 16x, not 32x

#define SE6_BASE              0x00998000U

#define GENI_FORCE_DEFAULT    0x020U
#define GENI_OUTPUT_CTRL      0x024U
#define GENI_CGC_CTRL         0x028U
#define GENI_STATUS           0x040U
#define GENI_SER_M_CLK_CFG    0x048U
#define GENI_SER_S_CLK_CFG    0x04CU
#define GENI_IF_DISABLE_RO    0x064U
#define GENI_FW_REVISION_RO   0x068U
#define GENI_S_FW_REVISION_RO 0x06CU
#define GENI_MULTILOCK_MSA_RO 0x074U
#define GENI_CLK_SEL          0x07CU
#define GENI_DMA_MODE_EN      0x258U
#define GENI_BYTE_GRAN        0x254U
#define GENI_TX_PACKING_CFG0  0x260U
#define GENI_TX_PACKING_CFG1  0x264U
#define GENI_RX_PACKING_CFG0  0x284U
#define GENI_RX_PACKING_CFG1  0x288U
#define UART_LOOPBACK_CFG     0x22CU
#define UART_TX_TRANS_CFG     0x25CU
#define UART_TX_WORD_LEN      0x268U
#define UART_TX_STOP_BIT_LEN  0x26CU
#define UART_TX_TRANS_LEN     0x270U
#define UART_RX_TRANS_CFG     0x280U
#define UART_RX_WORD_LEN      0x28CU
#define UART_RX_STALE_CNT     0x294U
#define UART_TX_PARITY_CFG    0x2A4U
#define UART_RX_PARITY_CFG    0x2A8U
#define UART_MANUAL_RFR       0x2ACU
#define GENI_M_CMD0           0x600U
#define GENI_M_CMD_CTRL       0x604U
#define GENI_M_IRQ_STATUS     0x610U
#define GENI_M_IRQ_CLEAR      0x618U
#define GENI_S_CMD0           0x630U
#define GENI_S_CMD_CTRL       0x634U
#define GENI_S_IRQ_STATUS     0x640U
#define GENI_S_IRQ_CLEAR      0x648U
#define GENI_TX_FIFO          0x700U
#define GENI_RX_FIFO          0x780U
#define GENI_TX_FIFO_STATUS   0x800U
#define GENI_RX_FIFO_STATUS   0x804U
#define GENI_TX_WATERMARK     0x80CU
#define GENI_RX_WATERMARK     0x810U
#define GENI_RX_RFR_WATERMARK 0x814U
#define DMA_TX_IRQ_CLR        0xC44U
#define DMA_RX_IRQ_CLR        0xD44U
#define GENI_SE_GSI_EVENT_EN  0xE18U
#define GENI_SE_IRQ_EN        0xE1CU
#define GENI_HW_PARAM_0       0xE24U
#define GENI_HW_PARAM_1       0xE28U
#define DMA_GENERAL_CFG       0xE30U

#define GENI_STATUS_M_ACTIVE  BIT0
#define GENI_STATUS_S_ACTIVE  BIT12

#define M_CMD_DONE            BIT0
#define UART_PROTOCOL         2U
#define UART_START_TX         1U
#define UART_START_READ       1U
#define OPCODE_SHIFT          27

#define UART_CTS_MASK         BIT1           // tx_trans_cfg: 1 = ignore the CTS pin
#define UART_LOOPBACK_ALL     (BIT0 | BIT1)  // RX_TX_SORTED | CTS_RTS_SORTED

#define SE_READ(Off)          MmioRead32 (SE6_BASE + (Off))
#define SE_WRITE(Off, Val)    MmioWrite32 (SE6_BASE + (Off), (Val))

// ---------------------------------------------------------------- HCI / Qualcomm vendor
#define HCI_COMMAND_PKT       0x01U
#define HCI_EVENT_PKT         0x04U
#define HCI_EV_CMD_COMPLETE   0x0EU
#define HCI_OP_RESET          0x0C03U        // OGF 3, OCF 3
#define QCA_OP_EDL            0xFC00U        // vendor, "EDL patch command"
#define QCA_EDL_PATCH_VER_REQ 0x19U

// ---------------------------------------------------------------- state
STATIC UINT32   mOversample = 16;
STATIC UINT32   mHwVersion;

STATIC
VOID
BtPrint (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buffer[220];

  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);
  gST->ConOut->OutputString (gST->ConOut, Buffer);
}

STATIC
VOID
BtHex (
  IN CONST CHAR16  *Label,
  IN CONST UINT8   *Data,
  IN UINTN         Len
  )
{
  UINTN  I;
  CHAR16 Line[100];
  UINTN  Pos;

  Pos = UnicodeSPrint (Line, sizeof (Line), L"  %s (%u):", Label, (UINT32)Len);
  for (I = 0; (I < Len) && (I < 24); I++) {
    Pos += UnicodeSPrint (&Line[Pos], sizeof (Line) - Pos * sizeof (CHAR16), L" %02x", Data[I]);
  }
  if (Len > 24) {
    UnicodeSPrint (&Line[Pos], sizeof (Line) - Pos * sizeof (CHAR16), L" ...");
  }
  BtPrint (L"%s\r\n", Line);
}

// ---------------------------------------------------------------------------- GPIO

STATIC
VOID
PinSetup (
  IN UINT32  Gpio,
  IN UINT32  Ctl
  )
{
  MmioWrite32 (TLMM_NORTH + TLMM_PIN_STRIDE * Gpio, Ctl);
}

STATIC
VOID
PinOutput (
  IN UINT32   Gpio,
  IN BOOLEAN  High
  )
{
  MmioWrite32 (TLMM_NORTH + TLMM_PIN_STRIDE * Gpio + 4, High ? BIT1 : 0);
}

STATIC
UINT32
PinInput (
  IN UINT32  Gpio
  )
{
  return MmioRead32 (TLMM_NORTH + TLMM_PIN_STRIDE * Gpio + 4) & 1;
}

// ---------------------------------------------------------------------------- RPMh

typedef struct {
  UINT32  IrqEnable, IrqStatus, IrqClear;     // relative to the TCS base
  UINT32  CmdStride;
  UINT32  Control, CmdEnable, MsgId, Addr, Data, CmdStatus;   // relative to a TCS
} RSC_REGS;

STATIC CONST RSC_REGS  mRegs27 = {0x00, 0x04, 0x08, 20, 0x14, 0x1C, 0x30, 0x34, 0x38, 0x3C};
STATIC CONST RSC_REGS  mRegs30 = {0x00, 0x04, 0x08, 24, 0x24, 0x2C, 0x34, 0x38, 0x3C, 0x40};

STATIC CONST RSC_REGS  *mR;
STATIC UINT32          mTcs = 0xFFFFFFFFU;

STATIC
BOOLEAN
NameEq (
  IN CONST UINT8  *Id,
  IN CONST CHAR8  *Name
  )
{
  UINTN I;

  for (I = 0; I < 8; I++) {
    UINT8 C = (UINT8)Name[I];
    if (Id[I] != C) {
      return FALSE;
    }
    if (C == 0) {
      return TRUE;
    }
  }
  return TRUE;
}

/**
  Find a resource in the AOP command DB.  Returns its address (0 if missing).
**/
STATIC
UINT32
CmdDbFind (
  IN CONST CHAR8  *Name
  )
{
  UINT32 I;
  UINT32 J;

  if (MmioRead32 (CMD_DB_BASE + 4) != 0x0C0330DBU) {         // magic db 30 03 0c
    return 0;
  }

  for (I = 0; I < CMD_DB_MAX_SLV; I++) {
    UINT32 Hdr0  = MmioRead32 (CMD_DB_BASE + 8 + I * 16);    // slv_id | header_offset << 16
    UINT32 Hdr1  = MmioRead32 (CMD_DB_BASE + 8 + I * 16 + 4);// data_offset | cnt << 16
    UINT32 Ents;
    UINT32 Cnt;

    if ((Hdr0 & 0xFFFFU) == 0) {
      break;
    }
    Ents = CMD_DB_DATA_OFFSET + (Hdr0 >> 16);
    Cnt  = Hdr1 >> 16;
    if ((Cnt > 512) || (Ents + Cnt * CMD_DB_ENTRY_SIZE > CMD_DB_SIZE)) {
      continue;
    }
    for (J = 0; J < Cnt; J++) {
      UINT32 E = CMD_DB_BASE + Ents + J * CMD_DB_ENTRY_SIZE;
      UINT32 Id[2];
      Id[0] = MmioRead32 (E);
      Id[1] = MmioRead32 (E + 4);
      if (NameEq ((UINT8 *)Id, Name)) {
        return MmioRead32 (E + 16);
      }
    }
  }
  return 0;
}

STATIC
VOID
TcsWrite (
  IN UINT32  Tcs,
  IN UINT32  Reg,
  IN UINT32  Val
  )
{
  MmioWrite32 (RSC_DRV2_BASE + RSC_TCS_OFFSET + RSC_TCS_STRIDE * Tcs + Reg, Val);
}

STATIC
UINT32
TcsRead (
  IN UINT32  Tcs,
  IN UINT32  Reg
  )
{
  return MmioRead32 (RSC_DRV2_BASE + RSC_TCS_OFFSET + RSC_TCS_STRIDE * Tcs + Reg);
}

STATIC
VOID
TcsWriteSync (
  IN UINT32  Tcs,
  IN UINT32  Reg,
  IN UINT32  Val
  )
{
  UINT32 I;

  TcsWrite (Tcs, Reg, Val);
  for (I = 0; I < 1000; I++) {
    if (TcsRead (Tcs, Reg) == Val) {
      return;
    }
    MicroSecondDelay (1);
  }
}

/**
  Read-only look at the RSC and pick a free active TCS.
**/
STATIC
BOOLEAN
RscInit (
  VOID
  )
{
  UINT32  Id;
  UINT32  Major;
  UINT32  I;

  if (mTcs != 0xFFFFFFFFU) {
    return TRUE;
  }

  Id    = MmioRead32 (RSC_DRV2_BASE + RSC_DRV_ID_REG);
  Major = (Id >> 16) & 0xFFU;
  if ((Major == 0) || (Major > 3)) {
    BtPrint (L"  RSC id %08x unexpected, not touching it\r\n", Id);
    return FALSE;
  }
  mR = (Major >= 3) ? &mRegs30 : &mRegs27;

  for (I = RSC_FIRST_ACTIVE_TCS; I < RSC_FIRST_ACTIVE_TCS + RSC_ACTIVE_TCS_COUNT; I++) {
    UINT32 En   = TcsRead (I, mR->CmdEnable);
    UINT32 Ctl  = TcsRead (I, mR->Control);
    if ((mTcs == 0xFFFFFFFFU) && (En == 0) && ((Ctl & TCS_AMC_TRIGGER) == 0)) {
      mTcs = I;
    }
  }
  if (mTcs == 0xFFFFFFFFU) {
    BtPrint (L"  no free active TCS\r\n");
    return FALSE;
  }
  MmioWrite32 (RSC_DRV2_BASE + RSC_TCS_OFFSET + mR->IrqClear, BIT0 << mTcs);
  return TRUE;
}

/**
  Send one active-set write (commit + valid) through the chosen TCS and wait for the AOP to
  answer.  Same sequence as Linux rpmh_rsc_send_data() / tcs_tx_done().
**/
STATIC
EFI_STATUS
RpmhSend (
  IN UINT32  Addr,
  IN UINT32  Data
  )
{
  UINT32      I;
  UINT32      Ctl;
  EFI_STATUS  Status;
  UINT32      Bit;

  Bit = BIT0 << mTcs;

  TcsWriteSync (mTcs, mR->CmdEnable, 0);
  TcsWrite (mTcs, mR->MsgId, CMD_MSGID_LEN | CMD_MSGID_RESP_REQ | CMD_MSGID_WRITE);
  TcsWrite (mTcs, mR->Addr, Addr);
  TcsWrite (mTcs, mR->Data, Data);
  TcsWrite (mTcs, mR->CmdEnable, BIT0);

  Ctl = TcsRead (mTcs, mR->Control) & ~TCS_AMC_TRIGGER;
  TcsWriteSync (mTcs, mR->Control, Ctl);
  Ctl &= ~TCS_AMC_ENABLE;
  TcsWriteSync (mTcs, mR->Control, Ctl);
  TcsWriteSync (mTcs, mR->Control, TCS_AMC_ENABLE);
  TcsWrite (mTcs, mR->Control, TCS_AMC_ENABLE | TCS_AMC_TRIGGER);

  Status = EFI_TIMEOUT;
  for (I = 0; I < 100000; I++) {                              // up to 100 ms
    if (((MmioRead32 (RSC_DRV2_BASE + RSC_TCS_OFFSET + mR->IrqStatus) & Bit) != 0) ||
        ((TcsRead (mTcs, mR->CmdStatus) & CMD_STATUS_COMPL) != 0)) {
      Status = EFI_SUCCESS;
      break;
    }
    MicroSecondDelay (1);
  }

  Ctl = TcsRead (mTcs, mR->Control) & ~TCS_AMC_TRIGGER;
  TcsWriteSync (mTcs, mR->Control, Ctl);
  Ctl &= ~TCS_AMC_ENABLE;
  TcsWriteSync (mTcs, mR->Control, Ctl);
  TcsWrite (mTcs, mR->CmdEnable, 0);
  MmioWrite32 (RSC_DRV2_BASE + RSC_TCS_OFFSET + mR->IrqClear, Bit);
  return Status;
}

STATIC
EFI_STATUS
BcmVote (
  IN CONST CHAR8  *Name,
  IN UINT32       Vote
  )
{
  UINT32  Addr;

  Addr = CmdDbFind (Name);
  if (Addr == 0) {
    return EFI_NOT_FOUND;
  }
  return RpmhSend (Addr, BIT30 | BIT29 | ((Vote & 0x3FFFU) << 14) | (Vote & 0x3FFFU));
}

/**
  Switch an RPMh VRM regulator on (active set): voltage in mV at +0, enable at +4, as in the
  downstream rpmh-regulator driver.  Or off.
**/
STATIC
EFI_STATUS
VrmSet (
  IN CONST CHAR8  *Name,
  IN UINT32       Millivolt,
  IN BOOLEAN      On
  )
{
  UINT32      Addr;
  EFI_STATUS  Status;

  Addr = CmdDbFind (Name);
  if (Addr == 0) {
    BtPrint (L"  vrm %a: not in cmd-db\r\n", Name);
    return EFI_NOT_FOUND;
  }
  if (On) {
    Status = RpmhSend (Addr + 0, Millivolt);
    if (EFI_ERROR (Status)) {
      BtPrint (L"  vrm %a @%05x voltage: %r\r\n", Name, Addr, Status);
      return Status;
    }
  }
  Status = RpmhSend (Addr + 4, On ? 1 : 0);
  BtPrint (L"  vrm %a @%05x %u mV %s: %r\r\n", Name, Addr, Millivolt, On ? L"on" : L"off", Status);
  return Status;
}

typedef struct {
  CONST CHAR8  *Name;
  UINT32       Millivolt;
} BT_RAIL;

// Order of the Linux/Android power-up: I/O first, then the chip's own rails.
STATIC CONST BT_RAIL  mRails[] = {
  { "smpa4",  1800 },     // pm8150 s4   vdd-io     (Wi-Fi side, system I/O rail)
  { "smpa6",   950 },     // pm8150 s6   vdd-aon + vdd-dig
  { "smpa5",  1900 },     // pm8150 s5   vdd-rfa1
  { "smpc8",  1350 },     // pm8150a s8  vdd-rfa2
  { "ldoa16", 3024 },     // pm8150 l16  vdd-asd (3.0-3.3 V in the device tree)
};

// ---------------------------------------------------------------------------- QUP clocks

STATIC
BOOLEAN
QupClocksOn (
  OUT UINT32  *M,
  OUT UINT32  *S,
  OUT UINT32  *C
  )
{
  *M = MmioRead32 (GCC_BASE + GCC_WRAP0_M_AHB_HALT);
  *S = MmioRead32 (GCC_BASE + GCC_WRAP0_S_AHB_HALT);
  *C = MmioRead32 (GCC_BASE + GCC_WRAP0_S6_HALT);
  return (((*M | *S | *C) & BIT31) == 0);
}

STATIC
BOOLEAN
WaitQupClocks (
  OUT UINT32  *M,
  OUT UINT32  *S,
  OUT UINT32  *C,
  IN  UINT32  Ms
  )
{
  UINT32  Retry;

  for (Retry = 0; Retry < Ms * 100; Retry++) {
    if (QupClocksOn (M, S, C)) {
      return TRUE;
    }
    MicroSecondDelay (10);
  }
  return QupClocksOn (M, S, C);
}

STATIC
BOOLEAN
EnableQupClocks (
  VOID
  )
{
  UINT32  M;
  UINT32  S;
  UINT32  C;
  UINT32  Level;
  UINT32  Vote;

  MmioWrite32 (GCC_BASE + GCC_QUP_VOTE_REG,
               MmioRead32 (GCC_BASE + GCC_QUP_VOTE_REG) | QUP_VOTE_BITS);

  if (WaitQupClocks (&M, &S, &C, 20)) {
    goto On;
  }

  // The s_ahb branch has hardware clock gating (bit 1): with it on, the status bit reads
  // "off" whenever the branch is idle.  Turn the gating off to get a real status.
  if (((M | C) & BIT31) == 0 && ((S & BIT31) != 0) && ((S & BIT1) != 0)) {
    MmioAnd32 (GCC_BASE + GCC_WRAP0_S_AHB_HALT, ~(UINT32)BIT1);
    if (WaitQupClocks (&M, &S, &C, 5)) {
      goto On;
    }
  }

  // RPMh bus vote (QUP core / config NoC BCMs), as the downstream kernel does.
  if (!RscInit ()) {
    goto Fail;
  }
  for (Level = 0; Level < 3; Level++) {
    Vote = (Level == 0) ? 1 : ((Level == 1) ? 0x40 : 0x800);
    BcmVote ("QUP0", Vote);
    BcmVote ("CN0", Vote);
    if (WaitQupClocks (&M, &S, &C, 5)) {
      goto On;
    }
  }

Fail:
  BtPrint (L"  QUP clocks did NOT start (m %08x s %08x se6 %08x)\r\n", M, S, C);
  return FALSE;

On:
  BtPrint (L"  QUP clocks on (m %08x s %08x se6 %08x)\r\n", M, S, C);
  return TRUE;
}

/**
  Program the SE6 root clock generator to 7.3728 MHz from the 19.2 MHz XO (M/N counter).
**/
STATIC
BOOLEAN
SetupSeClock (
  VOID
  )
{
  UINT32  Cmd;
  UINT32  Retry;

  Cmd = GCC_BASE + GCC_WRAP0_S6_RCG;

  MmioWrite32 (Cmd + 0x08, SE_CLK_M);
  MmioWrite32 (Cmd + 0x0C, (~(SE_CLK_N - SE_CLK_M)) & 0xFFFFU);
  MmioWrite32 (Cmd + 0x10, (~SE_CLK_N) & 0xFFFFU);
  MmioWrite32 (Cmd + 0x04, RCG_CFG_DUAL_EDGE);
  MmioWrite32 (Cmd, MmioRead32 (Cmd) | RCG_CMD_ROOT_EN);
  MmioWrite32 (Cmd, MmioRead32 (Cmd) | RCG_CMD_UPDATE);

  for (Retry = 0; Retry < 1000; Retry++) {
    if ((MmioRead32 (Cmd) & RCG_CMD_UPDATE) == 0) {
      BtPrint (L"  se6 rcg: cmd %08x cfg %08x m %08x n %08x d %08x\r\n",
               MmioRead32 (Cmd), MmioRead32 (Cmd + 4), MmioRead32 (Cmd + 8),
               MmioRead32 (Cmd + 0xC), MmioRead32 (Cmd + 0x10));
      return TRUE;
    }
    MicroSecondDelay (1);
  }

  BtPrint (L"  se6 rcg update did not finish (cmd %08x)\r\n", MmioRead32 (Cmd));
  return FALSE;
}

// ---------------------------------------------------------------------------- GENI UART

STATIC
VOID
GeniPackingLsbFirst (
  VOID
  )
{
  // Port of geni_se_config_packing (bpw 8, 4 words per FIFO entry, LSB first, TX and RX).
  UINT32 Cfg[4];
  UINT32 I;

  for (I = 0; I < 4; I++) {
    Cfg[I] = ((I * 8) << 5) | (7U << 1);
  }
  Cfg[3] |= BIT0;     // stop bit on the last vector

  SE_WRITE (GENI_TX_PACKING_CFG0, Cfg[0] | (Cfg[1] << 10));
  SE_WRITE (GENI_TX_PACKING_CFG1, Cfg[2] | (Cfg[3] << 10));
  SE_WRITE (GENI_RX_PACKING_CFG0, Cfg[0] | (Cfg[1] << 10));
  SE_WRITE (GENI_RX_PACKING_CFG1, Cfg[2] | (Cfg[3] << 10));
  SE_WRITE (GENI_BYTE_GRAN, 0);
}

STATIC
VOID
UartSetFlowControl (
  IN BOOLEAN  UseCts
  )
{
  UINT32  Cfg;

  Cfg = SE_READ (UART_TX_TRANS_CFG);
  if (UseCts) {
    Cfg &= ~UART_CTS_MASK;
  } else {
    Cfg |= UART_CTS_MASK;
  }
  SE_WRITE (UART_TX_TRANS_CFG, Cfg);
}

/**
  Port of geni_se_init() + qcom_geni_serial_port_setup() + set_termios() for 8N1 at BT_BAUD,
  FIFO mode, polled.  Starts the secondary (receive) sequencer.
**/
STATIC
EFI_STATUS
UartInit (
  VOID
  )
{
  UINT32  Fw;
  UINT32  RxDepth;
  UINT32  Div;

  Fw = SE_READ (GENI_FW_REVISION_RO);
  BtPrint (L"  se6: fw %08x proto %u s-fw %08x hw0 %08x hw1 %08x ifdis %08x lock %08x\r\n",
           Fw, (Fw >> 8) & 0xFF, SE_READ (GENI_S_FW_REVISION_RO), SE_READ (GENI_HW_PARAM_0),
           SE_READ (GENI_HW_PARAM_1), SE_READ (GENI_IF_DISABLE_RO),
           SE_READ (GENI_MULTILOCK_MSA_RO));

  if (((Fw >> 8) & 0xFFU) != UART_PROTOCOL) {
    BtPrint (L"  SE6 firmware is not UART (proto %u): stopping\r\n", (Fw >> 8) & 0xFF);
    return EFI_UNSUPPORTED;
  }
  if ((SE_READ (GENI_IF_DISABLE_RO) & BIT0) != 0) {
    BtPrint (L"  SE6 FIFO interface disabled (GPI DMA only): stopping\r\n");
    return EFI_UNSUPPORTED;
  }

  mHwVersion = MmioRead32 (QUP0_WRAPPER_BASE + QUP_HW_VERSION);
  mOversample = (mHwVersion >= QUP_SE_VERSION_2_5) ? 16 : 32;
  Div = SE_CLK_HZ / (BT_BAUD * mOversample);
  BtPrint (L"  qup hw version %08x -> %ux sampling, clock divider %u\r\n", mHwVersion, mOversample, Div);

  // geni_se_init(): clear interrupts, clock gating, default IO, FIFO mode.
  SE_WRITE (GENI_SE_GSI_EVENT_EN, 0);
  SE_WRITE (GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
  SE_WRITE (GENI_S_IRQ_CLEAR, 0xFFFFFFFFU);
  SE_WRITE (DMA_TX_IRQ_CLR, 0xFFFFFFFFU);
  SE_WRITE (DMA_RX_IRQ_CLR, 0xFFFFFFFFU);

  SE_WRITE (GENI_CGC_CTRL, SE_READ (GENI_CGC_CTRL) | 0x7FU);
  SE_WRITE (DMA_GENERAL_CFG, SE_READ (DMA_GENERAL_CFG) | 0xFU);
  SE_WRITE (GENI_OUTPUT_CTRL, 0x7FU);
  SE_WRITE (GENI_FORCE_DEFAULT, BIT0);
  SE_WRITE (GENI_SE_IRQ_EN, SE_READ (GENI_SE_IRQ_EN) | 0xFU);
  SE_WRITE (GENI_DMA_MODE_EN, SE_READ (GENI_DMA_MODE_EN) & ~BIT0);

  RxDepth = (SE_READ (GENI_HW_PARAM_1) >> 16) & 0x3FU;
  if (RxDepth < 8) {
    RxDepth = 16;
  }
  SE_WRITE (GENI_RX_WATERMARK, 2);
  SE_WRITE (GENI_RX_RFR_WATERMARK, RxDepth - 2);

  GeniPackingLsbFirst ();

  // Cancel anything left running by the bootloader.
  if ((SE_READ (GENI_STATUS) & GENI_STATUS_M_ACTIVE) != 0) {
    SE_WRITE (GENI_M_CMD_CTRL, BIT2);
    MicroSecondDelay (2000);
  }
  if ((SE_READ (GENI_STATUS) & GENI_STATUS_S_ACTIVE) != 0) {
    SE_WRITE (GENI_S_CMD_CTRL, BIT2);
    MicroSecondDelay (2000);
  }
  SE_WRITE (GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
  SE_WRITE (GENI_S_IRQ_CLEAR, 0xFFFFFFFFU);

  // 8N1, hardware flow control, no loopback.
  SE_WRITE (UART_LOOPBACK_CFG, 0);
  SE_WRITE (UART_TX_TRANS_CFG, SE_READ (UART_TX_TRANS_CFG) & ~(UART_CTS_MASK | BIT0));
  SE_WRITE (UART_TX_PARITY_CFG, 0);
  SE_WRITE (UART_RX_TRANS_CFG, SE_READ (UART_RX_TRANS_CFG) & ~BIT4);
  SE_WRITE (UART_RX_PARITY_CFG, 0);
  SE_WRITE (UART_TX_WORD_LEN, 8);
  SE_WRITE (UART_RX_WORD_LEN, 8);
  SE_WRITE (UART_TX_STOP_BIT_LEN, 0);
  SE_WRITE (UART_RX_STALE_CNT, 10 * 16);
  SE_WRITE (UART_MANUAL_RFR, 0);

  // Baud rate: core clock (selected clock source 0) divided down.
  SE_WRITE (GENI_CLK_SEL, 0);
  SE_WRITE (GENI_SER_M_CLK_CFG, (Div << 4) | BIT0);
  SE_WRITE (GENI_SER_S_CLK_CFG, (Div << 4) | BIT0);

  // Receive sequencer: runs until cancelled.
  SE_WRITE (GENI_S_CMD0, UART_START_READ << OPCODE_SHIFT);

  BtPrint (L"  se6 initialised, status %08x clk cfg %08x\r\n",
           SE_READ (GENI_STATUS), SE_READ (GENI_SER_M_CLK_CFG));
  return EFI_SUCCESS;
}

/**
  Send Len bytes (Len <= 64) and wait for the transmit command to finish.  A stuck command
  (CTS held off, no clock) times out after 100 ms and is cancelled.
**/
STATIC
EFI_STATUS
UartSend (
  IN CONST UINT8  *Data,
  IN UINTN        Len
  )
{
  UINTN   I;
  UINT32  Word;
  UINT32  Retry;

  SE_WRITE (GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
  SE_WRITE (UART_TX_TRANS_LEN, (UINT32)Len);
  SE_WRITE (GENI_M_CMD0, UART_START_TX << OPCODE_SHIFT);

  for (I = 0; I < Len; I += 4) {
    Word = 0;
    if (I + 0 < Len) Word |= (UINT32)Data[I + 0];
    if (I + 1 < Len) Word |= (UINT32)Data[I + 1] << 8;
    if (I + 2 < Len) Word |= (UINT32)Data[I + 2] << 16;
    if (I + 3 < Len) Word |= (UINT32)Data[I + 3] << 24;
    SE_WRITE (GENI_TX_FIFO, Word);
  }

  for (Retry = 0; Retry < 100000; Retry++) {                  // up to 100 ms
    if ((SE_READ (GENI_M_IRQ_STATUS) & M_CMD_DONE) != 0) {
      SE_WRITE (GENI_M_IRQ_CLEAR, M_CMD_DONE);
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
  }

  SE_WRITE (GENI_M_CMD_CTRL, BIT2);                           // cancel
  MicroSecondDelay (2000);
  if ((SE_READ (GENI_STATUS) & GENI_STATUS_M_ACTIVE) != 0) {
    SE_WRITE (GENI_M_CMD_CTRL, BIT1);                         // abort
    MicroSecondDelay (2000);
  }
  SE_WRITE (GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
  return EFI_TIMEOUT;
}

/**
  Drain the receive FIFO into Buffer (up to Max bytes).  Returns the number of bytes taken.
**/
STATIC
UINTN
UartDrain (
  OUT UINT8  *Buffer,
  IN  UINTN  Max
  )
{
  UINT32  Status;
  UINT32  Words;
  UINT32  Total;
  UINT32  Last;
  UINT32  W;
  UINT32  B;
  UINT32  Word;
  UINTN   Got;

  Got    = 0;
  Status = SE_READ (GENI_RX_FIFO_STATUS);
  Words  = Status & 0x1FFFFFFU;
  if (Words == 0) {
    return 0;
  }

  Last  = (Status >> 28) & 7U;
  Total = 4 * (Words - 1);
  Total += (((Status & BIT31) != 0) && (Last != 0)) ? Last : 4;

  for (W = 0; W < Words; W++) {
    Word = SE_READ (GENI_RX_FIFO);
    for (B = 0; B < 4; B++) {
      if ((W * 4 + B < Total) && (Got < Max)) {
        Buffer[Got++] = (UINT8)(Word >> (8 * B));
      }
    }
  }
  return Got;
}

/**
  Collect received bytes until the line is quiet for IdleMs after the first byte, or until
  TimeoutMs has passed with nothing at all.
**/
STATIC
UINTN
UartReceive (
  OUT UINT8   *Buffer,
  IN  UINTN   Max,
  IN  UINT32  TimeoutMs,
  IN  UINT32  IdleMs
  )
{
  UINTN   Got;
  UINTN   N;
  UINT32  Tick;
  UINT32  Idle;

  Got  = 0;
  Idle = 0;
  for (Tick = 0; Tick < TimeoutMs * 10; Tick++) {            // 100 us per tick
    N = UartDrain (Buffer + Got, Max - Got);
    if (N != 0) {
      Got += N;
      Idle = 0;
    } else if ((Got != 0) && (++Idle > IdleMs * 10)) {
      break;
    }
    if (Got >= Max) {
      break;
    }
    MicroSecondDelay (100);
  }
  return Got;
}

STATIC
VOID
UartFlush (
  VOID
  )
{
  UINT8  Scratch[64];

  while (UartDrain (Scratch, sizeof (Scratch)) != 0) {
  }
}

/**
  Internal loopback: the serial engine feeds its own TX back to RX (CTS follows RTS).
  Nothing leaves the SoC.  Proves clock, GENI firmware and both FIFO paths.
**/
STATIC
BOOLEAN
UartSelfTest (
  VOID
  )
{
  STATIC CONST UINT8  Pattern[] = { 0xA5, 0x5A, 0x12, 0x34, 0xFF, 0x00, 0x81 };
  UINT8               Rx[16];
  UINTN               Got;
  EFI_STATUS          Status;
  BOOLEAN             Ok;

  SE_WRITE (UART_LOOPBACK_CFG, UART_LOOPBACK_ALL);
  UartFlush ();
  Status = UartSend (Pattern, sizeof (Pattern));
  Got    = UartReceive (Rx, sizeof (Rx), 100, 5);
  SE_WRITE (UART_LOOPBACK_CFG, 0);
  UartFlush ();

  Ok = !EFI_ERROR (Status) && (Got == sizeof (Pattern)) && (CompareMem (Rx, Pattern, Got) == 0);
  BtPrint (L"  loopback: tx %r, rx %u bytes -> %s\r\n", Status, (UINT32)Got, Ok ? L"OK" : L"FAILED");
  if (!Ok && (Got != 0)) {
    BtHex (L"rx", Rx, Got);
  }
  return Ok;
}

// ---------------------------------------------------------------------------- HCI

/**
  Send one HCI command packet and collect the answer.  Returns the number of bytes received.
**/
STATIC
UINTN
HciCommand (
  IN  UINT16       Opcode,
  IN  CONST UINT8  *Param,
  IN  UINT8        ParamLen,
  OUT UINT8        *Rx,
  IN  UINTN        RxMax
  )
{
  UINT8       Tx[16];
  UINTN       Got;
  EFI_STATUS  Status;

  Tx[0] = HCI_COMMAND_PKT;
  Tx[1] = (UINT8)(Opcode & 0xFF);
  Tx[2] = (UINT8)(Opcode >> 8);
  Tx[3] = ParamLen;
  if (ParamLen != 0) {
    CopyMem (&Tx[4], Param, ParamLen);
  }

  UartFlush ();
  Status = UartSend (Tx, 4 + (UINTN)ParamLen);
  BtHex (L"tx", Tx, 4 + (UINTN)ParamLen);
  if (EFI_ERROR (Status)) {
    BtPrint (L"  tx failed: %r (cts pin %u, status %08x m_irq %08x)\r\n", Status,
             PinInput (BT_UART_CTS_GPIO), SE_READ (GENI_STATUS), SE_READ (GENI_M_IRQ_STATUS));
    return 0;
  }

  Got = UartReceive (Rx, RxMax, 600, 20);
  if (Got == 0) {
    BtPrint (L"  rx: nothing (cts pin %u, rx pin %u)\r\n", PinInput (BT_UART_CTS_GPIO),
             PinInput (BT_UART_RX_GPIO));
  } else {
    BtHex (L"rx", Rx, Got);
  }
  return Got;
}

/**
  Look for "command complete" for Opcode in a received buffer.  Returns a pointer to the
  event's return parameters (status first) or NULL.
**/
STATIC
CONST UINT8 *
FindCommandComplete (
  IN CONST UINT8  *Rx,
  IN UINTN        Len,
  IN UINT16       Opcode,
  OUT UINTN       *ParamLen
  )
{
  UINTN  I;

  for (I = 0; I + 6 <= Len; I++) {
    if ((Rx[I] == HCI_EVENT_PKT) && (Rx[I + 1] == HCI_EV_CMD_COMPLETE) &&
        (Rx[I + 2] >= 4) && (I + 3 + Rx[I + 2] <= Len) &&
        (Rx[I + 4] == (UINT8)(Opcode & 0xFF)) && (Rx[I + 5] == (UINT8)(Opcode >> 8))) {
      *ParamLen = Rx[I + 2] - 3;
      return &Rx[I + 6];
    }
  }
  return NULL;
}

/**
  HCI Reset, then the Qualcomm patch-version request.  Returns TRUE if the chip answered
  the reset with a command complete.
**/
STATIC
BOOLEAN
ChipAlive (
  IN CONST CHAR16  *Mode
  )
{
  UINT8        Rx[64];
  UINTN        Got;
  UINTN        PLen;
  CONST UINT8  *P;
  UINT8        Req;
  BOOLEAN      Alive;

  BtPrint (L"HCI test, %s:\r\n", Mode);

  Got   = HciCommand (HCI_OP_RESET, NULL, 0, Rx, sizeof (Rx));
  P     = FindCommandComplete (Rx, Got, HCI_OP_RESET, &PLen);
  Alive = (P != NULL);
  if (Alive) {
    BtPrint (L"  HCI Reset: command complete, status %02x\r\n", (PLen != 0) ? P[0] : 0xFF);
  } else if (Got != 0) {
    BtPrint (L"  HCI Reset: got data but no command complete (wrong baud?)\r\n");
  }

  Req = QCA_EDL_PATCH_VER_REQ;
  Got = HciCommand (QCA_OP_EDL, &Req, 1, Rx, sizeof (Rx));
  P   = FindCommandComplete (Rx, Got, QCA_OP_EDL, &PLen);
  if (P != NULL) {
    Alive = TRUE;
    // Return parameters: status, 0x19, product id (4), patch version (2), rom version (2), soc id (4).
    if (PLen >= 14) {
      BtPrint (L"  QCA version: product %08x patch %04x rom %04x soc %08x\r\n",
               P[2] | (P[3] << 8) | (P[4] << 16) | ((UINT32)P[5] << 24),
               P[6] | (P[7] << 8), P[8] | (P[9] << 8),
               P[10] | (P[11] << 8) | (P[12] << 16) | ((UINT32)P[13] << 24));
    } else {
      BtPrint (L"  QCA version: short answer (%u bytes), see the dump above\r\n", (UINT32)PLen);
    }
  }
  return Alive;
}

// ---------------------------------------------------------------------------- main

STATIC
VOID
ChipPowerOff (
  VOID
  )
{
  UINTN  I;

  PinOutput (BT_EN_GPIO, FALSE);
  for (I = ARRAY_SIZE (mRails); I > 0; I--) {
    VrmSet (mRails[I - 1].Name, mRails[I - 1].Millivolt, FALSE);
  }
}

STATIC
VOID
EFIAPI
MunchBtReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN    I;
  BOOLEAN  Alive;

  gBS->CloseEvent (Event);

  BtPrint (L"\r\nMunchBt: QCA6390 chip-alive test\r\n");

  if (!EnableQupClocks ()) {
    goto Done;
  }
  if (!SetupSeClock ()) {
    goto Done;
  }
  if (EFI_ERROR (UartInit ())) {
    goto Done;
  }
  if (!UartSelfTest ()) {
    BtPrint (L"  serial engine does not loop back: stopping before the chip is powered\r\n");
    goto Done;
  }

  // Pins: UART on gpio 16..19 (function qup6, bias as in the device tree, 2 mA), BT_EN low.
  PinOutput (BT_EN_GPIO, FALSE);
  PinSetup (BT_EN_GPIO, PIN_CTL (FUNC_GPIO, PULL_DOWN, 0, 1));
  PinSetup (BT_SW_CTRL_GPIO, PIN_CTL (FUNC_GPIO, PULL_NONE, 0, 0));
  PinSetup (BT_UART_CTS_GPIO, PIN_CTL (FUNC_QUP6, PULL_NONE, 0, 0));
  PinSetup (BT_UART_RTS_GPIO, PIN_CTL (FUNC_QUP6, PULL_DOWN, 0, 0));
  PinSetup (BT_UART_TX_GPIO,  PIN_CTL (FUNC_QUP6, PULL_UP, 0, 0));
  PinSetup (BT_UART_RX_GPIO,  PIN_CTL (FUNC_QUP6, PULL_NONE, 0, 0));
  MicroSecondDelay (100000);                                  // BT_EN low long enough to reset the chip
  BtPrint (L"  pins set; sw_ctrl %u (BT_EN low)\r\n", PinInput (BT_SW_CTRL_GPIO));

  // Rails through RPMh.
  if (!RscInit ()) {
    BtPrint (L"  RSC not usable: cannot power the chip\r\n");
    goto Done;
  }
  for (I = 0; I < ARRAY_SIZE (mRails); I++) {
    if (EFI_ERROR (VrmSet (mRails[I].Name, mRails[I].Millivolt, TRUE))) {
      BtPrint (L"  rail %a failed: stopping, BT_EN stays low\r\n", mRails[I].Name);
      goto Done;
    }
    MicroSecondDelay (2000);
  }
  MicroSecondDelay (100000);

  // BT_EN high, then give the ROM time to start.
  PinOutput (BT_EN_GPIO, TRUE);
  MicroSecondDelay (150000);
  BtPrint (L"  BT_EN high; sw_ctrl %u cts %u rx %u\r\n", PinInput (BT_SW_CTRL_GPIO),
           PinInput (BT_UART_CTS_GPIO), PinInput (BT_UART_RX_GPIO));

  UartFlush ();
  UartSetFlowControl (TRUE);
  Alive = ChipAlive (L"RTS/CTS flow control");
  if (!Alive) {
    UartSetFlowControl (FALSE);
    Alive = ChipAlive (L"CTS ignored");
    UartSetFlowControl (TRUE);
  }

  if (Alive) {
    BtPrint (L"MunchBt: RESULT chip ALIVE (the QCA6390 answers HCI at %u baud)\r\n", BT_BAUD);
  } else {
    BtPrint (L"MunchBt: RESULT no answer from the chip (sw_ctrl %u)\r\n", PinInput (BT_SW_CTRL_GPIO));
  }

  if (!MUNCH_BT_LEAVE_ON) {
    ChipPowerOff ();
  }

Done:
  MicroSecondDelay (MUNCH_BT_HOLD_MS * 1000);
}

EFI_STATUS
EFIAPI
MunchBtDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT Event;

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                MunchBtReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &Event
                );
}
