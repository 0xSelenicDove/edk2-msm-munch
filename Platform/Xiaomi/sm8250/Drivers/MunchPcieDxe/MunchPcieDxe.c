/** @file
  PCIe bring-up experiment for Xiaomi munch (SM8250): power the Wi-Fi/BT chip, train the
  root complex 0 link and read the endpoint's PCI ID.

  The Wi-Fi/Bluetooth chip is a Qualcomm QCA6390 behind PCIe root complex 0 (Gen3 x1).  Android
  brings the link up in its kernel; nothing in our firmware does.  This driver does the same
  steps once, before the OS starts, and prints the result on the early console:

    1. map the register windows (PARF/PHY, DBI/iATU/config window)
    2. RPMh: PHY rails (ldoa5/ldoa9), chip rails (smpa6/s4/s5, smpc8), bus bandwidth (BCM)
    3. GDSC + GCC clocks/resets for the controller, WLAN_EN high, PERST# held low
    4. QMP PHY init from the table in the downstream device tree, wait for PLL lock
    5. PARF/DBI root-complex setup (mainline pcie-qcom.c, ops_1_9_0 sequence)
    6. release PERST#, enable LTSSM, wait for link up
    7. outbound iATU window for bus 1 -> read vendor/device ID of 01:00.0 (expect 17CB:1101)

  Everything is left powered and the iATU windows stay programmed, so the same state can later
  be described to Windows with an MCFG table and a PCI0 node.  The log is shown on screen
  and written to \munch-pcie.log on the ESP after every step (a hang loses at most one step).

  Register layout, sequences and delays come from mainline Linux (pcie-qcom.c, dwc, gcc-sm8250.c,
  qmp-pcie) and the downstream device tree dumped from the phone.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextOut.h>

// Seconds the boot screen is held so the log can be read.
#define MUNCH_PCIE_HOLD_US   8000000U
#define MUNCH_PCIE_LOG_FILE  L"munch-pcie.log"
#define LOG_CAPACITY         (96 * 1024)

// ---------------------------------------------------------------- logging
STATIC CHAR8    *mLog;
STATIC UINTN    mLogLen;
STATIC BOOLEAN  mLogFileStarted;

STATIC
VOID
LogAppend (
  IN CONST CHAR16  *Text
  )
{
  if (mLog == NULL) {
    return;
  }
  while ((*Text != 0) && (mLogLen + 2 < LOG_CAPACITY)) {
    if (*Text != L'\r') {
      mLog[mLogLen++] = (CHAR8)*Text;
    }
    Text++;
  }
}

/** Log line: on screen and in the buffer. **/
STATIC
VOID
Log (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buffer[256];

  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);
  gST->ConOut->OutputString (gST->ConOut, Buffer);
  LogAppend (Buffer);
}

/** Log line: buffer (file) only, for bulk data. **/
STATIC
VOID
LogQuiet (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR16   Buffer[256];

  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);
  LogAppend (Buffer);
}

/** Write the whole log to the first volume that looks like the ESP.  Best effort. **/
STATIC
VOID
LogFlush (
  VOID
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            I;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *Dir;
  EFI_FILE_PROTOCOL                *File;
  UINTN                            Size;
  BOOLEAN                          Done;

  if ((mLog == NULL) || (mLogLen == 0)) {
    return;
  }
  Handles = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  Done = FALSE;
  for (I = 0; (I < Count) && !Done; I++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }
    if (!EFI_ERROR (Root->Open (Root, &Dir, L"EFI", EFI_FILE_MODE_READ, 0))) {
      Dir->Close (Dir);
      if (!mLogFileStarted) {
        // First flush of this boot: drop the previous run's file so no stale tail remains.
        if (!EFI_ERROR (Root->Open (Root, &File, MUNCH_PCIE_LOG_FILE, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
          File->Delete (File);
        }
        mLogFileStarted = TRUE;
      }
      if (!EFI_ERROR (Root->Open (Root, &File, MUNCH_PCIE_LOG_FILE,
                                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
        Size = mLogLen;
        File->SetPosition (File, 0);
        File->Write (File, &Size, mLog);
        File->Flush (File);
        File->Close (File);
        Done = TRUE;
      }
    }
    Root->Close (Root);
  }
  FreePool (Handles);
}

/** Mark a step: on screen, then flush the file so a later hang keeps what we have. **/
#define STEP(...)  do { Log (__VA_ARGS__); LogFlush (); } while (0)

// ---------------------------------------------------------------- clocks / resets (GCC)
#define GCC_BASE                0x00100000U
#define GCC_GPLL0_MODE          0x00000U
#define GCC_APCS_CLK_VOTE       0x52008U     // bit0 slv_axi 1 mstr_axi 2 cfg_ahb 3 aux 4 pipe 5 slv_q2a

#define GCC_PCIE0_BCR           0x6B000U
#define GCC_PCIE0_GDSCR         0x6B004U
#define GCC_PCIE0_SLV_Q2A_CBCR  0x6B010U
#define GCC_PCIE0_SLV_AXI_CBCR  0x6B014U
#define GCC_PCIE0_MSTR_AXI_CBCR 0x6B01CU
#define GCC_PCIE0_CFG_AHB_CBCR  0x6B024U
#define GCC_PCIE0_AUX_CBCR      0x6B028U
#define GCC_PCIE0_PIPE_CBCR     0x6B02CU
#define GCC_PCIE0_AUX_RCG       0x6B038U
#define GCC_PCIE0_PHY_BCR       0x6C01CU
#define GCC_PCIE0_NOCSR_BCR     0x6C020U
#define GCC_PCIE0_PHY_NOCSR_BCR 0x6C028U
#define GCC_PCIE_PHY_AUX_CBCR   0x6F004U
#define GCC_PCIE_REFGEN_RCG     0x6F014U
#define GCC_PCIE0_REFGEN_CBCR   0x6F02CU
#define GCC_PCIE_WIFI_CLKREF    0x8C004U
#define GCC_DDRSS_PCIE_SF_TBU   0x8D058U
#define GCC_AGGRE_NOC_PCIE_TBU  0x9000CU

#define CBCR_OFF                BIT31
#define CBCR_HWCG               BIT1
#define CBCR_EN                 BIT0

#define GCC_R(Off)              MmioRead32 (GCC_BASE + (Off))
#define GCC_W(Off, V)           MmioWrite32 (GCC_BASE + (Off), (V))

// ---------------------------------------------------------------- pin controller (all PCIe pins are in the north tile)
#define TLMM_NORTH              0x0F900000U
#define TLMM_PIN_STRIDE         0x1000U
#define PIN_WLAN_EN             20U
#define PIN_PERST               79U
#define PIN_CLKREQ              80U
#define PIN_WAKE                81U

// ctl register: pull [1:0] (0 none, 1 down, 3 up), function [5:2], drive [8:6] (2 mA * (n+1)), oe bit 9.
#define PIN_CTL(Func, Pull, Drive, Oe) \
  ((UINT32)(Pull) | ((UINT32)(Func) << 2) | ((UINT32)(Drive) << 6) | ((UINT32)(Oe) << 9))

// ---------------------------------------------------------------- RPMh (same as MunchTouchDxe)
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

typedef struct {
  UINT32  IrqEnable, IrqStatus, IrqClear;
  UINT32  CmdStride;
  UINT32  Control, CmdEnable, MsgId, Addr, Data, CmdStatus;
} RSC_REGS;

STATIC CONST RSC_REGS  mRegs27 = {0x00, 0x04, 0x08, 20, 0x14, 0x1C, 0x30, 0x34, 0x38, 0x3C};
STATIC CONST RSC_REGS  mRegs30 = {0x00, 0x04, 0x08, 24, 0x24, 0x2C, 0x34, 0x38, 0x3C, 0x40};
STATIC CONST RSC_REGS  *mR;
STATIC UINT32          mTcs = 0xFFFFFFFFU;

// ---------------------------------------------------------------- PCIe controller 0
#define PARF_BASE               0x01C00000U
#define PHY_BASE                0x01C06000U
#define PCIE_AXI_BASE           0x60000000U      // ECAM-style window: bus 0 = DBI, bus 1 = iATU config region
#define PCIE_AXI_SIZE           0x04000000U      // qcom,slv-addr-space-size
#define DBI_BASE                0x60000000U
#define ELBI_BASE               0x60000F20U
#define IATU_BASE               0x60001000U
#define CONF_BASE               0x60100000U      // bus 1, device 0, function 0 once the iATU region 0 is up
#define MEM_BASE                0x60300000U
#define MEM_LAST                0x63FFFFFFU

#define PARF_SYS_CTRL           0x000U
#define PARF_PHY_CTRL           0x040U
#define PARF_DBI_BASE_ADDR      0x168U
#define PARF_SLV_ADDR_SPACE     0x16CU            // older layout (kept)
#define PARF_SLV_ADDR_SPACE_V2  0x358U            // mainline PARF_SLV_ADDR_SPACE_SIZE for 2_7_0 / 1_9_0
// ECAM mode and "ECAM blockers": hardware windows that return an error response instead of
// forwarding config accesses to non-existent devices (SDM845 AML REB0/EEB0, mainline qcom_pci_config_ecam).
#define PARF_BLK_WR_BASE        0x360U            // root bus blocker: WR/RD base+hi, limit+hi
#define PARF_BLK_WR_LIMIT       0x368U
#define PARF_BLK_RD_BASE        0x370U
#define PARF_BLK_RD_LIMIT       0x378U
#define PARF_ECAM_BASE          0x380U
#define PARF_BLK2_WR_BASE       0x398U            // endpoint bus blocker
#define PARF_BLK2_WR_LIMIT      0x3A0U
#define PARF_BLK2_RD_BASE       0x3A8U
#define PARF_BLK2_RD_LIMIT      0x3B0U
#define SYS_CTRL_ECAM_BLK_RC    BIT26
#define SYS_CTRL_ECAM_BLK_EP    BIT30
#define MSI_FRAME_BASE          0x17A10000U       // GIC MSI frame (MADT), TYPER at +8
#define PARF_MHI_CLOCK_RESET    0x174U
#define PARF_AXI_WR_ADDR_HALT   0x1A8U
#define PARF_LTSSM              0x1B0U
#define PARF_DEVICE_TYPE        0x1000U

#define PHY_STATUS              0x814U           // qcom,phy-status-offset, bit 6 set = not ready
#define PHY_STATUS_BIT          BIT6

// DesignWare core (DBI) registers
#define DBI_COMMAND             0x004U
#define DBI_CLASS_REV           0x008U
#define DBI_BUSNUM              0x018U
#define DBI_MEMORY_BASE         0x020U
#define DBI_LNKCAP              0x07CU
#define DBI_LNKCTL_STA          0x080U           // status in the upper half: DLLLA = bit 29
#define DBI_PORT_LINK_CTRL      0x710U
#define DBI_PORT_DEBUG0         0x728U           // [5:0] LTSSM state
#define DBI_PORT_DEBUG1         0x72CU           // bit 4 link up
#define DBI_GEN2_CTRL           0x80CU
#define DBI_MISC_CTRL_1         0x8BCU           // bit 0 DBI read-only write enable

#define ELBI_SYS_STTS           0x008U           // bit 10 XMLH_LINK_UP
#define ELBI_CS2_ENABLE         0x0A4U           // 1: DBI writes go to the shadow (CS2) registers, e.g. BAR masks

// Unrolled iATU: outbound region N at IATU_BASE + N * 0x200
#define ATU_CTRL1               0x00U
#define ATU_CTRL2               0x04U
#define ATU_LOWER_BASE          0x08U
#define ATU_UPPER_BASE          0x0CU
#define ATU_LIMIT               0x10U
#define ATU_LOWER_TARGET        0x14U
#define ATU_UPPER_TARGET        0x18U
#define ATU_UPPER_LIMIT         0x20U
#define ATU_TYPE_MEM            0U
#define ATU_TYPE_CFG0           4U
#define ATU_REGION_EN           BIT31

typedef struct {
  UINT16  Offset;
  UINT8   Value;
  UINT8   DelayUs;
} PHY_REG;

// qcom,phy-sequence of pcie@1c00000 (offset from 0x01C06000, value, delay), phy-ver 0x44e.
STATIC CONST PHY_REG  mPhySeq[] = {
  {0x840, 0x03, 0}, {0x094, 0x08, 0}, {0x154, 0x34, 0}, {0x16c, 0x08, 0},
  {0x058, 0x0f, 0}, {0x0a4, 0x42, 0}, {0x110, 0x24, 0}, {0x11c, 0x03, 0},
  {0x118, 0xb4, 0}, {0x10c, 0x02, 0}, {0x1bc, 0x11, 0}, {0x0bc, 0x82, 0},
  {0x0d4, 0x03, 0}, {0x0d0, 0x55, 0}, {0x0cc, 0x55, 0}, {0x0b0, 0x1a, 0},
  {0x0ac, 0x0a, 0}, {0x0c4, 0x68, 0}, {0x0e0, 0x02, 0}, {0x0dc, 0xaa, 0},
  {0x0d8, 0xab, 0}, {0x0b8, 0x34, 0}, {0x0b4, 0x14, 0}, {0x158, 0x01, 0},
  {0x074, 0x06, 0}, {0x07c, 0x16, 0}, {0x084, 0x36, 0}, {0x078, 0x06, 0},
  {0x080, 0x16, 0}, {0x088, 0x36, 0}, {0x1b0, 0x1e, 0}, {0x1ac, 0xca, 0},
  {0x1b8, 0x18, 0}, {0x1b4, 0xa2, 0}, {0x050, 0x07, 0}, {0x010, 0x01, 0},
  {0x01c, 0x31, 0}, {0x020, 0x01, 0}, {0x024, 0xde, 0}, {0x028, 0x07, 0},
  {0x030, 0x4c, 0}, {0x034, 0x06, 0}, {0x29c, 0x12, 0}, {0x284, 0x35, 0},
  {0x23c, 0x11, 0}, {0x51c, 0x03, 0}, {0x518, 0x1c, 0}, {0x524, 0x1e, 0},
  {0x4e8, 0x00, 0}, {0x4ec, 0x0e, 0}, {0x4f0, 0x4a, 0}, {0x4f4, 0x0f, 0},
  {0x5b4, 0x04, 0}, {0x434, 0x7f, 0}, {0x444, 0x70, 0}, {0x510, 0x17, 0},
  {0x4d4, 0x04, 0}, {0x4d8, 0x07, 0}, {0x598, 0xd4, 0}, {0x59c, 0x54, 0},
  {0x5a0, 0xdb, 0}, {0x5a4, 0x3b, 0}, {0x5a8, 0x31, 0}, {0x584, 0x24, 0},
  {0x588, 0xe4, 0}, {0x58c, 0xec, 0}, {0x590, 0x3b, 0}, {0x594, 0x36, 0},
  {0x570, 0x3f, 0}, {0x574, 0x3f, 0}, {0x578, 0xff, 0}, {0x57c, 0x7f, 0},
  {0x580, 0x14, 0}, {0x4fc, 0x00, 0}, {0x4f8, 0xc0, 0}, {0x460, 0x30, 0},
  {0x464, 0x00, 0}, {0x5bc, 0x0c, 0}, {0x4dc, 0x1b, 0}, {0x408, 0x0c, 0},
  {0x414, 0x03, 0}, {0x5b8, 0x30, 0}, {0x9a4, 0x01, 0}, {0xc90, 0x00, 0},
  {0xc40, 0x01, 0}, {0xc48, 0x01, 0}, {0xc50, 0x00, 0}, {0xcb4, 0x33, 0},
  {0xcbc, 0x00, 0}, {0xce0, 0x58, 0}, {0xca4, 0x0f, 0}, {0x048, 0x90, 0},
  {0xc1c, 0xc1, 0}, {0x988, 0x77, 0}, {0x998, 0x0b, 0}, {0x8dc, 0x0d, 0},
  {0x9ec, 0x12, 0}, {0x800, 0x00, 0}, {0x844, 0x03, 0},
};

// ---------------------------------------------------------------- small helpers
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
VOID
PinSetup (
  IN UINT32  Gpio,
  IN UINT32  Ctl
  )
{
  MmioWrite32 (TLMM_NORTH + TLMM_PIN_STRIDE * Gpio, Ctl);
}

STATIC
UINT32
PinInput (
  IN UINT32  Gpio
  )
{
  return MmioRead32 (TLMM_NORTH + TLMM_PIN_STRIDE * Gpio + 4) & 1;
}

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

STATIC
BOOLEAN
CmdDbValid (
  VOID
  )
{
  return MmioRead32 (CMD_DB_BASE + 4) == 0x0C0330DBU;
}

/** Find a resource in the AOP command DB.  Returns its address (0 if missing). **/
STATIC
UINT32
CmdDbFind (
  IN CONST CHAR8  *Name
  )
{
  UINT32 I;
  UINT32 J;

  if (!CmdDbValid ()) {
    return 0;
  }
  for (I = 0; I < CMD_DB_MAX_SLV; I++) {
    UINT32 Hdr0 = MmioRead32 (CMD_DB_BASE + 8 + I * 16);
    UINT32 Hdr1 = MmioRead32 (CMD_DB_BASE + 8 + I * 16 + 4);
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

/** Write every command DB entry (name, address) to the log file: resource names are not documented. **/
STATIC
VOID
CmdDbDump (
  VOID
  )
{
  UINT32 I;
  UINT32 J;
  UINT32 Total;

  if (!CmdDbValid ()) {
    return;
  }
  Total = 0;
  for (I = 0; I < CMD_DB_MAX_SLV; I++) {
    UINT32 Hdr0 = MmioRead32 (CMD_DB_BASE + 8 + I * 16);
    UINT32 Hdr1 = MmioRead32 (CMD_DB_BASE + 8 + I * 16 + 4);
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
    LogQuiet (L"cmd-db slave %04x: %u entries\r\n", Hdr0 & 0xFFFFU, Cnt);
    for (J = 0; J < Cnt; J++) {
      UINT32 E = CMD_DB_BASE + Ents + J * CMD_DB_ENTRY_SIZE;
      CHAR8  N[9];
      UINT32 W;

      W = MmioRead32 (E);      CopyMem (&N[0], &W, 4);
      W = MmioRead32 (E + 4);  CopyMem (&N[4], &W, 4);
      N[8] = 0;
      LogQuiet (L"  %a %05x\r\n", N, MmioRead32 (E + 16));
      Total++;
    }
  }
  LogQuiet (L"cmd-db: %u entries total\r\n", Total);
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

STATIC
BOOLEAN
RscInit (
  VOID
  )
{
  UINT32  Id;
  UINT32  Major;
  UINT32  I;

  Id    = MmioRead32 (RSC_DRV2_BASE + RSC_DRV_ID_REG);
  Major = (Id >> 16) & 0xFFU;
  if ((Major == 0) || (Major > 3)) {
    Log (L"  RSC id %08x unexpected\r\n", Id);
    return FALSE;
  }
  mR = (Major >= 3) ? &mRegs30 : &mRegs27;

  for (I = RSC_FIRST_ACTIVE_TCS; I < RSC_FIRST_ACTIVE_TCS + RSC_ACTIVE_TCS_COUNT; I++) {
    UINT32 En  = TcsRead (I, mR->CmdEnable);
    UINT32 Ctl = TcsRead (I, mR->Control);
    if ((mTcs == 0xFFFFFFFFU) && (En == 0) && ((Ctl & TCS_AMC_TRIGGER) == 0)) {
      mTcs = I;
    }
  }
  if (mTcs == 0xFFFFFFFFU) {
    Log (L"  no free active TCS\r\n");
    return FALSE;
  }
  MmioWrite32 (RSC_DRV2_BASE + RSC_TCS_OFFSET + mR->IrqClear, BIT0 << mTcs);
  return TRUE;
}

/** One active-set RPMh write through the chosen TCS, waiting for the AOP to answer. **/
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
  for (I = 0; I < 100000; I++) {
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
  UINT32      Addr;
  EFI_STATUS  Status;

  Addr = CmdDbFind (Name);
  if (Addr == 0) {
    Log (L"  bcm %a: not in cmd-db\r\n", Name);
    return EFI_NOT_FOUND;
  }
  Status = RpmhSend (Addr, BIT30 | BIT29 | ((Vote & 0x3FFFU) << 14) | (Vote & 0x3FFFU));
  Log (L"  bcm %a @%05x vote %x: %r\r\n", Name, Addr, Vote, Status);
  return Status;
}

/** VRM regulator on (active set): voltage in mV at +0, enable at +4 (rpmh-regulator layout). **/
STATIC
EFI_STATUS
VrmEnable (
  IN CONST CHAR8  *Name,
  IN UINT32       Millivolt
  )
{
  UINT32      Addr;
  EFI_STATUS  Status;

  Addr = CmdDbFind (Name);
  if (Addr == 0) {
    Log (L"  vrm %a: not in cmd-db\r\n", Name);
    return EFI_NOT_FOUND;
  }
  Status = RpmhSend (Addr + 0, Millivolt);
  if (!EFI_ERROR (Status)) {
    Status = RpmhSend (Addr + 4, 1);
  }
  Log (L"  vrm %a @%05x %u mV: %r\r\n", Name, Addr, Millivolt, Status);
  return Status;
}

// ---------------------------------------------------------------- GCC helpers
STATIC
BOOLEAN
WaitBranch (
  IN CONST CHAR16  *Name,
  IN UINT32        Cbcr
  )
{
  UINT32 I;

  for (I = 0; I < 20000; I++) {            // 20 ms
    if ((GCC_R (Cbcr) & CBCR_OFF) == 0) {
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  Log (L"  clk %s stuck off (%08x)\r\n", Name, GCC_R (Cbcr));
  return FALSE;
}

/** Plain (unvoted) branch: set CLK_ENABLE and wait for it. **/
STATIC
BOOLEAN
BranchEnable (
  IN CONST CHAR16  *Name,
  IN UINT32        Cbcr
  )
{
  GCC_W (Cbcr, GCC_R (Cbcr) | CBCR_EN);
  return WaitBranch (Name, Cbcr);
}

STATIC
BOOLEAN
RcgUpdate (
  IN CONST CHAR16  *Name,
  IN UINT32        Cmd,
  IN UINT32        Cfg
  )
{
  UINT32 I;

  GCC_W (Cmd + 4, Cfg);
  GCC_W (Cmd, GCC_R (Cmd) | BIT0);          // UPDATE
  for (I = 0; I < 5000; I++) {
    if ((GCC_R (Cmd) & BIT0) == 0) {
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  Log (L"  rcg %s update stuck (%08x)\r\n", Name, GCC_R (Cmd));
  return FALSE;
}

STATIC
VOID
ResetBcr (
  IN UINT32   Bcr,
  IN BOOLEAN  Assert
  )
{
  GCC_W (Bcr, Assert ? (GCC_R (Bcr) | BIT0) : (GCC_R (Bcr) & ~(UINT32)BIT0));
  (VOID)GCC_R (Bcr);
}

STATIC
BOOLEAN
GdscEnable (
  VOID
  )
{
  UINT32 V;
  UINT32 I;

  V = GCC_R (GCC_PCIE0_GDSCR);
  Log (L"  gdsc before %08x\r\n", V);
  // Same wait values the Linux gdsc driver programs (en_rest 2, en_few 8, clk_dis 2).
  V = (V & ~((0xFU << 20) | (0xFU << 16) | (0xFU << 12))) | (2U << 20) | (8U << 16) | (2U << 12);
  V &= ~(UINT32)BIT0;                       // clear SW_COLLAPSE
  GCC_W (GCC_PCIE0_GDSCR, V);
  for (I = 0; I < 100000; I++) {            // 100 ms for PWR_ON
    if ((GCC_R (GCC_PCIE0_GDSCR) & BIT31) != 0) {
      Log (L"  gdsc on (%08x)\r\n", GCC_R (GCC_PCIE0_GDSCR));
      return TRUE;
    }
    MicroSecondDelay (1);
  }
  Log (L"  gdsc did NOT power on (%08x)\r\n", GCC_R (GCC_PCIE0_GDSCR));
  return FALSE;
}

/** All branch clocks of RC0 except the pipe clock (it comes from the PHY). **/
STATIC
BOOLEAN
ClocksEnable (
  VOID
  )
{
  UINT32   Gpll;
  BOOLEAN  Ok;

  Ok = TRUE;

  // aux: XO 19.2 MHz (source 0, no divider).
  Ok &= RcgUpdate (L"aux", GCC_PCIE0_AUX_RCG, 0);

  // PHY refgen: 100 MHz = GPLL0_OUT_MAIN (source 1) / 6 (HID divider 2*6-1), if GPLL0 is running.
  Gpll = GCC_R (GCC_GPLL0_MODE);
  if ((Gpll & BIT31) != 0) {
    Ok &= RcgUpdate (L"refgen", GCC_PCIE_REFGEN_RCG, (1U << 8) | 11U);
  } else {
    Log (L"  GPLL0 not locked (%08x): refgen stays on XO\r\n", Gpll);
  }

  // The two hardware-gated branches read "off" while idle: turn gating off to get a real status.
  GCC_W (GCC_PCIE0_SLV_AXI_CBCR, GCC_R (GCC_PCIE0_SLV_AXI_CBCR) & ~(UINT32)CBCR_HWCG);
  GCC_W (GCC_PCIE0_CFG_AHB_CBCR, GCC_R (GCC_PCIE0_CFG_AHB_CBCR) & ~(UINT32)CBCR_HWCG);

  // Voted branches: slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a (bit 4 = pipe stays off for now).
  GCC_W (GCC_APCS_CLK_VOTE, GCC_R (GCC_APCS_CLK_VOTE) | BIT0 | BIT1 | BIT2 | BIT3 | BIT5);
  Ok &= WaitBranch (L"slv_axi", GCC_PCIE0_SLV_AXI_CBCR);
  Ok &= WaitBranch (L"mstr_axi", GCC_PCIE0_MSTR_AXI_CBCR);
  Ok &= WaitBranch (L"cfg_ahb", GCC_PCIE0_CFG_AHB_CBCR);
  Ok &= WaitBranch (L"aux", GCC_PCIE0_AUX_CBCR);
  Ok &= WaitBranch (L"slv_q2a", GCC_PCIE0_SLV_Q2A_CBCR);

  // PHY side clocks and the reference clock to the Wi-Fi chip.
  Ok &= BranchEnable (L"phy_aux", GCC_PCIE_PHY_AUX_CBCR);
  Ok &= BranchEnable (L"refgen", GCC_PCIE0_REFGEN_CBCR);
  Ok &= BranchEnable (L"wifi_clkref", GCC_PCIE_WIFI_CLKREF);

  // SMMU TBU clocks (needed for DMA, not for config access): informational.
  BranchEnable (L"aggre_tbu", GCC_AGGRE_NOC_PCIE_TBU);
  BranchEnable (L"ddrss_tbu", GCC_DDRSS_PCIE_SF_TBU);

  Log (L"  clocks %a: vote %08x\r\n", Ok ? "ok" : "FAILED", GCC_R (GCC_APCS_CLK_VOTE));
  return Ok;
}

// ---------------------------------------------------------------- PCIe controller
#define PARF_R(Off)     MmioRead32 (PARF_BASE + (Off))
#define PARF_W(Off, V)  MmioWrite32 (PARF_BASE + (Off), (V))
#define DBI_R(Off)      MmioRead32 (DBI_BASE + (Off))
#define DBI_W(Off, V)   MmioWrite32 (DBI_BASE + (Off), (V))

STATIC
VOID
IatuOutbound (
  IN UINT32  Region,
  IN UINT32  Type,
  IN UINT32  Base,
  IN UINT32  Last,
  IN UINT32  Target
  )
{
  UINT32  R;

  R = IATU_BASE + Region * 0x200U;
  MmioWrite32 (R + ATU_LOWER_BASE, Base);
  MmioWrite32 (R + ATU_UPPER_BASE, 0);
  MmioWrite32 (R + ATU_LIMIT, Last);
  MmioWrite32 (R + ATU_UPPER_LIMIT, 0);
  MmioWrite32 (R + ATU_LOWER_TARGET, Target);
  MmioWrite32 (R + ATU_UPPER_TARGET, 0);
  MmioWrite32 (R + ATU_CTRL1, Type);
  MmioWrite32 (R + ATU_CTRL2, ATU_REGION_EN);
}

STATIC
EFI_STATUS
MapRegion (
  IN CONST CHAR16           *Name,
  IN EFI_PHYSICAL_ADDRESS   Base,
  IN UINT64                 Length
  )
{
  EFI_STATUS  Status;

  Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Base, Length, EFI_MEMORY_UC);
  LogQuiet (L"  map %s %08x+%x add: %r\r\n", Name, (UINT32)Base, (UINT32)Length, Status);
  // "already added / overlaps a descriptor" is fine: what matters is the attribute call.
  Status = gDS->SetMemorySpaceAttributes (Base, Length, EFI_MEMORY_UC);
  Log (L"  map %s %08x+%x: %r\r\n", Name, (UINT32)Base, (UINT32)Length, Status);
  return Status;
}

/** Root-complex register setup before PERST# is released (qcom_pcie_init_2_7_0 + post_init). **/
STATIC
VOID
ParfInit (
  VOID
  )
{
  UINT32  V;

  PARF_W (PARF_DEVICE_TYPE, 4);                           // root complex
  PARF_W (PARF_PHY_CTRL, PARF_R (PARF_PHY_CTRL) & ~(UINT32)BIT0);   // PHY_TEST_PWR_DOWN off
  PARF_W (PARF_DBI_BASE_ADDR, 0);
  PARF_W (PARF_SYS_CTRL, PARF_R (PARF_SYS_CTRL) & ~(UINT32)BIT7);   // MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN off
  PARF_W (PARF_MHI_CLOCK_RESET, PARF_R (PARF_MHI_CLOCK_RESET) | BIT4);
  PARF_W (PARF_AXI_WR_ADDR_HALT, PARF_R (PARF_AXI_WR_ADDR_HALT) | BIT31);
  PARF_W (PARF_SLV_ADDR_SPACE, PCIE_AXI_SIZE);
  PARF_W (PARF_SLV_ADDR_SPACE_V2, PCIE_AXI_SIZE);
  V = PARF_R (PARF_SYS_CTRL);
  Log (L"  parf sys_ctrl %08x type %08x\r\n", V, PARF_R (PARF_DEVICE_TYPE));
}

STATIC
VOID
PhyApply (
  VOID
  )
{
  UINTN  I;

  for (I = 0; I < ARRAY_SIZE (mPhySeq); I++) {
    MmioWrite32 (PHY_BASE + mPhySeq[I].Offset, mPhySeq[I].Value);
    if (mPhySeq[I].DelayUs != 0) {
      MicroSecondDelay (mPhySeq[I].DelayUs);
    }
  }
  (VOID)MmioRead32 (PHY_BASE + PHY_STATUS);     // make sure the writes are through
}

/** Wait for PHYSTATUS (bit 6 of 0x814) to clear.  Returns the wait in ms, or -1 on timeout. **/
STATIC
INT32
PhyWaitReady (
  IN UINT32  Ms
  )
{
  UINT32  I;

  for (I = 0; I < Ms; I++) {
    if ((MmioRead32 (PHY_BASE + PHY_STATUS) & PHY_STATUS_BIT) == 0) {
      return (INT32)I;
    }
    MicroSecondDelay (1000);
  }
  return -1;
}

/** Whole PHY register space to the log file (all of it is within the 4 KB window). **/
STATIC
VOID
PhyDump (
  VOID
  )
{
  UINT32  Off;

  for (Off = 0; Off < 0x1000; Off += 16) {
    LogQuiet (L"  phy %03x: %08x %08x %08x %08x\r\n", Off, MmioRead32 (PHY_BASE + Off),
              MmioRead32 (PHY_BASE + Off + 4), MmioRead32 (PHY_BASE + Off + 8), MmioRead32 (PHY_BASE + Off + 12));
  }
}

/**
  Start the QMP PHY.  Several variants are tried in one boot, because the exact sequence the
  downstream kernel uses is not known to be sufficient on its own:
    A  pipe clock first (mainline enables it before the PHY starts), then the table
    B  A after a PHY reset pulse
    C  B with the controller and both "nocsr" resets pulsed as well
**/
STATIC
BOOLEAN
PhyStart (
  VOID
  )
{
  INT32  Ms;
  UINT32 Attempt;

  GCC_W (GCC_APCS_CLK_VOTE, GCC_R (GCC_APCS_CLK_VOTE) | BIT4);      // pipe clock branch (source is the PHY)

  for (Attempt = 0; Attempt < 3; Attempt++) {
    if (Attempt >= 1) {
      ResetBcr (GCC_PCIE0_PHY_BCR, TRUE);
      if (Attempt == 2) {
        ResetBcr (GCC_PCIE0_BCR, TRUE);
        ResetBcr (GCC_PCIE0_NOCSR_BCR, TRUE);
        ResetBcr (GCC_PCIE0_PHY_NOCSR_BCR, TRUE);
      }
      MicroSecondDelay (1000);
      if (Attempt == 2) {
        ResetBcr (GCC_PCIE0_PHY_NOCSR_BCR, FALSE);
        ResetBcr (GCC_PCIE0_NOCSR_BCR, FALSE);
        ResetBcr (GCC_PCIE0_BCR, FALSE);
      }
      ResetBcr (GCC_PCIE0_PHY_BCR, FALSE);
      MicroSecondDelay (2500);
      ParfInit ();
    }
    PhyApply ();
    Ms = PhyWaitReady (100);
    Log (L"  phy %c: %a (status %08x pcs 800 %08x 840 %08x 844 %08x)\r\n", L'A' + Attempt,
         (Ms >= 0) ? "READY" : "not ready", MmioRead32 (PHY_BASE + PHY_STATUS),
         MmioRead32 (PHY_BASE + 0x800), MmioRead32 (PHY_BASE + 0x840), MmioRead32 (PHY_BASE + 0x844));
    if (Ms >= 0) {
      return TRUE;
    }
  }
  PhyDump ();
  return FALSE;
}

/** Root port configuration through the DBI (dw_pcie_setup_rc). **/
STATIC
VOID
RootPortInit (
  VOID
  )
{
  UINT32  V;

  DBI_W (DBI_MISC_CTRL_1, DBI_R (DBI_MISC_CTRL_1) | BIT0);          // allow writes to read-only fields

  // One lane.
  V = DBI_R (DBI_PORT_LINK_CTRL);
  DBI_W (DBI_PORT_LINK_CTRL, (V & ~(0x3FU << 16)) | (1U << 16));
  V = DBI_R (DBI_GEN2_CTRL);
  DBI_W (DBI_GEN2_CTRL, (V & ~(0x1FU << 8)) | (1U << 8));

  // PCI-PCI bridge class code 0x0604.
  V = DBI_R (DBI_CLASS_REV);
  DBI_W (DBI_CLASS_REV, (V & 0xFFFFU) | (0x0604U << 16));

  // Disable the root port's own BARs: with CS2 enabled the BAR registers are the size masks, and
  // 0 makes the BAR read as 0 when the OS sizes it.  (Without this Windows sees a BAR it has no
  // room for and the root port fails to start: CM_PROB_NORMAL_CONFLICT / 0xC0000018.)
  MmioWrite32 (ELBI_BASE + ELBI_CS2_ENABLE, 1);
  DBI_W (0x10, 0);
  DBI_W (0x14, 0);
  MmioWrite32 (ELBI_BASE + ELBI_CS2_ENABLE, 0);
  DBI_W (0x10, 0);
  DBI_W (0x14, 0);
  DBI_W (DBI_BUSNUM, 0x00FF0100U);                                  // primary 0, secondary 1, subordinate 255
  DBI_W (DBI_MEMORY_BASE, (((MEM_LAST >> 16) & 0xFFF0U) << 16) | ((MEM_BASE >> 16) & 0xFFF0U));
  DBI_W (DBI_COMMAND, (DBI_R (DBI_COMMAND) & ~0xFFFFU) | BIT1 | BIT2);   // memory space + bus master

  DBI_W (DBI_MISC_CTRL_1, DBI_R (DBI_MISC_CTRL_1) & ~(UINT32)BIT0);
}

/** Log what the OS will see in the root port's type-1 header, including what BAR sizing returns. **/
STATIC
VOID
RootPortDump (
  IN CONST CHAR16  *Title
  )
{
  UINT32  B0;
  UINT32  B1;

  Log (L"  %s:\r\n", Title);
  Log (L"    04 %08x 08 %08x 0c %08x 10 %08x 14 %08x 18 %08x\r\n", DBI_R (0x04), DBI_R (0x08), DBI_R (0x0C),
       DBI_R (0x10), DBI_R (0x14), DBI_R (0x18));
  Log (L"    1c %08x 20 %08x 24 %08x 28 %08x 2c %08x 30 %08x 3c %08x\r\n", DBI_R (0x1C), DBI_R (0x20),
       DBI_R (0x24), DBI_R (0x28), DBI_R (0x2C), DBI_R (0x30), DBI_R (0x3C));
  // BAR sizing exactly as an OS does it, then restore.
  DBI_W (0x10, 0xFFFFFFFFU);
  DBI_W (0x14, 0xFFFFFFFFU);
  B0 = DBI_R (0x10);
  B1 = DBI_R (0x14);
  DBI_W (0x10, 0);
  DBI_W (0x14, 0);
  Log (L"    bar sizing: bar0 %08x bar1 %08x\r\n", B0, B1);
}

STATIC
VOID
LinkStatus (
  OUT UINT32  *LtssmState,
  OUT BOOLEAN *DllLinkActive
  )
{
  *LtssmState    = DBI_R (DBI_PORT_DEBUG0) & 0x3FU;
  *DllLinkActive = (DBI_R (DBI_LNKCTL_STA) & BIT29) != 0;
}

STATIC
BOOLEAN
LinkTrain (
  VOID
  )
{
  UINT32   Ms;
  UINT32   State;
  UINT32   Last;
  UINT32   Changes;
  BOOLEAN  Up;

  PARF_W (PARF_LTSSM, PARF_R (PARF_LTSSM) | BIT8);                  // LTSSM enable

  Last    = 0xFFFFFFFFU;
  Changes = 0;
  Up      = FALSE;
  for (Ms = 0; Ms < 1000; Ms++) {
    LinkStatus (&State, &Up);
    if (State != Last) {
      LogQuiet (L"  t+%ums ltssm %02x\r\n", Ms, State);
      if (Changes < 12) {
        Log (L"  t+%ums ltssm %02x\r\n", Ms, State);
      }
      Changes++;
      Last = State;
    }
    if (Up) {
      break;
    }
    MicroSecondDelay (1000);
  }

  Log (L"  link %a: ltssm %02x lnksta %08x elbi %08x dbg1 %08x\r\n",
       Up ? "UP" : "DOWN", State, DBI_R (DBI_LNKCTL_STA), MmioRead32 (ELBI_BASE + ELBI_SYS_STTS),
       DBI_R (DBI_PORT_DEBUG1));
  return Up;
}

/** Hold PERST# low and prepare WLAN_EN.  BT_EN (GPIO 21) is deliberately left alone: MunchBtDxe owns it. **/
STATIC
VOID
PinsInit (
  VOID
  )
{
  PinOutput (PIN_PERST, FALSE);
  PinSetup (PIN_PERST, PIN_CTL (0, 1, 0, 1));         // gpio, pull-down, 2 mA, output
  PinSetup (PIN_CLKREQ, PIN_CTL (1, 3, 0, 0));        // pci_e0, pull-up, 2 mA
  PinSetup (PIN_WAKE, PIN_CTL (0, 3, 0, 0));          // gpio input, pull-up
  PinOutput (PIN_WLAN_EN, FALSE);
  PinSetup (PIN_WLAN_EN, PIN_CTL (0, 1, 7, 1));       // gpio, pull-down, 16 mA, output (low for now)
}

/**
  Describe the config space to Windows as a plain ECAM at 0x60000000, buses 0-1:
    bus 0 dev 0 = root port (DBI), bus 1 dev 0 = endpoint (iATU config region, 1 MB window).
  Everything else in the two 1 MB windows is blocked by the PARF blockers.  The iATU registers
  live at 0x60001000, inside the root-bus blocker, so the iATU MUST be programmed first.
**/
STATIC
VOID
EcamSetup (
  VOID
  )
{
  UINT32  V;

  IatuOutbound (0, ATU_TYPE_CFG0, CONF_BASE, CONF_BASE + 0xFFFFFU, 0x01000000U);
  IatuOutbound (1, ATU_TYPE_MEM, MEM_BASE, MEM_LAST, MEM_BASE);

  PARF_W (PARF_ECAM_BASE, PCIE_AXI_BASE);
  PARF_W (PARF_ECAM_BASE + 4, 0);

  PARF_W (PARF_BLK_WR_BASE, DBI_BASE + 0x1000U);      PARF_W (PARF_BLK_WR_BASE + 4, 0);
  PARF_W (PARF_BLK_WR_LIMIT, DBI_BASE + 0xFFFFFU);    PARF_W (PARF_BLK_WR_LIMIT + 4, 0);
  PARF_W (PARF_BLK_RD_BASE, DBI_BASE + 0x1000U);      PARF_W (PARF_BLK_RD_BASE + 4, 0);
  PARF_W (PARF_BLK_RD_LIMIT, DBI_BASE + 0xFFFFFU);    PARF_W (PARF_BLK_RD_LIMIT + 4, 0);

  PARF_W (PARF_BLK2_WR_BASE, CONF_BASE + 0x1000U);    PARF_W (PARF_BLK2_WR_BASE + 4, 0);
  PARF_W (PARF_BLK2_WR_LIMIT, CONF_BASE + 0xFFFFFU);  PARF_W (PARF_BLK2_WR_LIMIT + 4, 0);
  PARF_W (PARF_BLK2_RD_BASE, CONF_BASE + 0x1000U);    PARF_W (PARF_BLK2_RD_BASE + 4, 0);
  PARF_W (PARF_BLK2_RD_LIMIT, CONF_BASE + 0xFFFFFU);  PARF_W (PARF_BLK2_RD_LIMIT + 4, 0);

  V = PARF_R (PARF_SYS_CTRL);
  PARF_W (PARF_SYS_CTRL, V | SYS_CTRL_ECAM_BLK_RC | SYS_CTRL_ECAM_BLK_EP);

  Log (L"  ecam: sys_ctrl %08x ecam_base %08x\r\n", PARF_R (PARF_SYS_CTRL), PARF_R (PARF_ECAM_BASE));
  Log (L"  blk rc %08x..%08x  blk ep %08x..%08x\r\n", PARF_R (PARF_BLK_RD_BASE), PARF_R (PARF_BLK_RD_LIMIT),
       PARF_R (PARF_BLK2_RD_BASE), PARF_R (PARF_BLK2_RD_LIMIT));
}

/** Read one ECAM address after logging it, so a hang shows up as the last line of the log. **/
STATIC
UINT32
Probe (
  IN CONST CHAR16  *What,
  IN UINT32        Address
  )
{
  UINT32  V;

  Log (L"  probe %s @%08x ...\r\n", What, Address);
  LogFlush ();
  V = MmioRead32 (Address);
  Log (L"    = %08x\r\n", V);
  return V;
}

/**
  Prove that memory accesses reach the endpoint: size BAR0, place it at the start of the MEM window,
  enable memory decoding and read the first MHI registers.  A working path returns real values
  (MHI version 0x01000000 at +8); all-ones means the window/iATU setup is wrong.
**/
STATIC
VOID
BarProbe (
  VOID
  )
{
  UINT32  B0;
  UINT32  B1;
  UINT32  Cmd;
  UINT32  I;
  UINT32  V[8];

  MmioWrite32 (CONF_BASE + 0x10, 0xFFFFFFFFU);
  MmioWrite32 (CONF_BASE + 0x14, 0xFFFFFFFFU);
  B0 = MmioRead32 (CONF_BASE + 0x10);
  B1 = MmioRead32 (CONF_BASE + 0x14);
  Log (L"  bar0 sizing %08x %08x (size %x)\r\n", B0, B1, (UINT32)(~(B0 & ~0xFU) + 1));

  MmioWrite32 (CONF_BASE + 0x10, MEM_BASE + 0x100000U + (B0 & 0xFU));   // 0x60400000, 1 MB aligned
  MmioWrite32 (CONF_BASE + 0x14, 0);
  Cmd = MmioRead32 (CONF_BASE + 0x04);
  MmioWrite32 (CONF_BASE + 0x04, (Cmd & 0xFFFFU) | BIT1 | BIT2);       // memory space + bus master
  Log (L"  bar0 now %08x cmd %08x\r\n", MmioRead32 (CONF_BASE + 0x10), MmioRead32 (CONF_BASE + 0x04));

  Log (L"  probe bar0 @%08x ...\r\n", MEM_BASE + 0x100000U);
  LogFlush ();
  for (I = 0; I < 8; I++) {
    V[I] = MmioRead32 (MEM_BASE + 0x100000U + I * 4);
  }
  Log (L"    mhi regs: %08x %08x %08x %08x %08x %08x %08x %08x\r\n", V[0], V[1], V[2], V[3], V[4], V[5], V[6], V[7]);
  Log (L"    status +48 %08x  +4c %08x\r\n", MmioRead32 (MEM_BASE + 0x100000U + 0x48),
       MmioRead32 (MEM_BASE + 0x100000U + 0x4C));
}

STATIC
VOID
BringUp (
  VOID
  )
{
  UINT32      State;
  BOOLEAN     Up;
  UINT32      Id;

  Log (L"\r\nMunchPcie: QCA6390 on RC0\r\n");

  // 1. register windows
  STEP (L"1 map\r\n");
  MapRegion (L"parf/phy", PARF_BASE, 0x10000);
  MapRegion (L"axi", PCIE_AXI_BASE, PCIE_AXI_SIZE);

  // 2. RPMh
  STEP (L"2 rpmh\r\n");
  if (!RscInit ()) {
    Log (L"  RSC not usable: stop\r\n");
    return;
  }
  CmdDbDump ();
  PinsInit ();

  VrmEnable ("ldoa5", 880);                     // vreg-0p9: PHY core
  VrmEnable ("ldoa9", 1200);                    // vreg-1p8 node: 1.2 V PHY rail
  VrmEnable ("smpa6", 950);                     // vdd-wlan-aon / dig
  VrmEnable ("smpa4", 1800);                    // vdd-wlan-io
  VrmEnable ("smpa5", 1900);                    // vdd-wlan-rfa1
  VrmEnable ("smpc8", 1350);                    // vdd-wlan-rfa2
  MicroSecondDelay (5000);

  BcmVote ("CN0", 0x40);                        // config NoC (qhs_pcie0_cfg)
  BcmVote ("SN6", 0x200);                       // xs_pcie_0 slave
  BcmVote ("SN9", 0x200);                       // gem NoC -> PCIe
  BcmVote ("SN12", 0x200);                      // aggre NoC PCIe master path
  LogFlush ();

  // WLAN_EN: chip internal power sequencing starts here; >= 100 ms before PERST# is released.
  PinOutput (PIN_WLAN_EN, TRUE);
  Log (L"  WLAN_EN high, sw-ctrl %u\r\n", MmioRead32 (TLMM_NORTH + TLMM_PIN_STRIDE * 124 + 4) & 1);

  // 3. GDSC, clocks, resets
  STEP (L"3 gdsc+clocks\r\n");
  if (!GdscEnable ()) {
    LogFlush ();
    return;
  }
  if (!ClocksEnable ()) {
    Log (L"  stop before touching PARF\r\n");
    LogFlush ();
    return;
  }
  // Pulse the controller and PHY resets with the clocks running (as the Linux driver does).
  ResetBcr (GCC_PCIE0_BCR, TRUE);
  ResetBcr (GCC_PCIE0_PHY_BCR, TRUE);
  MicroSecondDelay (1000);
  ResetBcr (GCC_PCIE0_PHY_BCR, FALSE);
  ResetBcr (GCC_PCIE0_BCR, FALSE);
  MicroSecondDelay (2500);
  LogFlush ();

  // 4. PARF / PHY
  STEP (L"4 parf+phy\r\n");
  Log (L"  parf sys_ctrl before: %08x\r\n", PARF_R (PARF_SYS_CTRL));
  ParfInit ();
  if (!PhyStart ()) {
    LogFlush ();
    return;
  }
  MicroSecondDelay (1000);
  Log (L"  pipe cbcr %08x\r\n", GCC_R (GCC_PCIE0_PIPE_CBCR));

  // 5. PERST# release after the chip had its 100 ms
  STEP (L"5 perst\r\n");
  MicroSecondDelay (100000);
  PinOutput (PIN_PERST, TRUE);
  MicroSecondDelay (100000);
  Log (L"  perst high, clkreq %u wake %u\r\n", PinInput (PIN_CLKREQ), PinInput (PIN_WAKE));

  // 6. root port and link training
  STEP (L"6 link\r\n");
  Log (L"  dbi id %08x class %08x lnkcap %08x\r\n", DBI_R (0), DBI_R (DBI_CLASS_REV), DBI_R (DBI_LNKCAP));
  RootPortInit ();
  RootPortDump (L"rp header after init");
  Up = LinkTrain ();
  LinkStatus (&State, &Up);
  LogFlush ();
  if (!Up) {
    Log (L"  no link: endpoint not reachable\r\n");
    return;
  }
  Log (L"  link speed %u width x%u\r\n", (DBI_R (DBI_LNKCTL_STA) >> 16) & 0xFU,
       (DBI_R (DBI_LNKCTL_STA) >> 20) & 0x3FU);

  // 7. ECAM mode, iATU windows and blockers; then read the ID through ECAM
  STEP (L"7 ecam\r\n");
  Log (L"  msi frame typer %08x (SPI base [25:16], count [9:0])\r\n", MmioRead32 (MSI_FRAME_BASE + 8));
  EcamSetup ();
  MicroSecondDelay (1000);
  Id = Probe (L"rp 00:00.0", DBI_BASE);
  Id = Probe (L"ep 01:00.0", CONF_BASE);
  Log (L"  01:00.0 vendor/device %08x (expect 110117cb)\r\n", Id);
  Log (L"  cmd/status %08x class/rev %08x hdr %08x subsys %08x\r\n",
       MmioRead32 (CONF_BASE + 4), MmioRead32 (CONF_BASE + 8), MmioRead32 (CONF_BASE + 0xC),
       MmioRead32 (CONF_BASE + 0x2C));
  Log (L"  bar0 %08x bar1 %08x\r\n", MmioRead32 (CONF_BASE + 0x10), MmioRead32 (CONF_BASE + 0x14));
  // What Windows will touch while enumerating: must not hang.
  Probe (L"bus0 dev0 fn1", DBI_BASE + 0x1000U);
  Probe (L"bus0 dev1", DBI_BASE + 0x8000U);
  Probe (L"bus0 dev31", DBI_BASE + 0xF8000U);
  Probe (L"bus1 dev0 fn1", CONF_BASE + 0x1000U);
  Probe (L"bus1 dev1", CONF_BASE + 0x8000U);
  Probe (L"bus1 dev31", CONF_BASE + 0xF8000U);
  Probe (L"rp 00:00.0 again", DBI_BASE);
  BarProbe ();
  Log (L"MunchPcie: %a\r\n", (Id == 0x110117CBU) ? "QCA6390 FOUND" : "unexpected ID");
  LogFlush ();
}

STATIC
VOID
EFIAPI
MunchPcieReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);

  mLog = AllocateZeroPool (LOG_CAPACITY);
  BringUp ();
  LogFlush ();
  MicroSecondDelay (MUNCH_PCIE_HOLD_US);
}

EFI_STATUS
EFIAPI
MunchPcieDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT Event;

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                MunchPcieReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &Event
                );
}
