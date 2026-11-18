/** @file
  Touchscreen bring-up for Xiaomi munch (SM8250), step 2: read the touch chip ID.

  The touch chip is a FocalTech FTS (FT3658 class) on SPI.  From the device tree:
    * SPI bus  : QUP wrapper 0, serial engine 4 (0x990000), pins GPIO 8..11 (function 1)
    * reset    : GPIO 33 (active low), interrupt: GPIO 39
    * supplies : pm8150l LDO1 (1.8 V I/O) and LDO10 (3.2 V core)
    * SPI mode 0, 8 bit, up to 6 MHz

  This driver does everything needed for a proof of life and prints the result on the
  early console before the OS starts:
    1. enable the QUP clocks (AHB master/slave and the SE4 core clock).  The s_ahb branch
       has hardware clock gating, so its status bit reads "off" while idle; hwcg is switched
       off to get a real status.  If that is not enough, an RPMh BCM vote (QUP0/CN0 through
       the APPS RSC DRV2, addresses from the AOP command DB) is sent as a fallback.
    2. mux the SPI pins, drive reset low, power the two LDOs, release reset
    3. initialise the GENI serial engine in FIFO mode as an SPI master
    4. read register 0xA3 / 0x9F (chip ID, expected 0x56 0x52) with the FTS framing

  Nothing is written to any other peripheral.  If a step fails, the driver prints why
  and returns without touching the next hardware block.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

// ---------------------------------------------------------------- clocks (GCC)
#define GCC_BASE              0x00100000U
#define GCC_QUP_VOTE_REG      0x52008U       // bit6 wrap0 M-AHB, bit7 S-AHB, bit14 SE4 clock
#define GCC_WRAP0_M_AHB_HALT  0x17004U
#define GCC_WRAP0_S_AHB_HALT  0x17008U
#define GCC_WRAP0_S4_HALT     0x174CCU
#define GCC_WRAP0_S4_RCG      0x174D0U

#define GCC_WRAP0_CORE_HALT   0x23000U
#define GCC_WRAP0_CORE2X_HALT 0x23008U

// bit6 m_ahb, bit7 s_ahb, bit8 core, bit9 core_2x, bit14 se4
#define QUP_VOTE_BITS         (BIT6 | BIT7 | BIT8 | BIT9 | BIT14)

// ---------------------------------------------------------------- RPMh (fallback only)
// Command DB (read only) and the APPS RSC, DRV2 (HLOS).  Layout from Linux cmd-db.c / rpmh-rsc.c.
#define CMD_DB_BASE           0x80860000U
#define CMD_DB_SIZE           0x00020000U
#define CMD_DB_DATA_OFFSET    0x90U              // after version, magic, 8 rsc headers, checksum, reserved
#define CMD_DB_MAX_SLV        8U
#define CMD_DB_ENTRY_SIZE     24U

#define RSC_DRV2_BASE         0x18220000U
#define RSC_TCS_OFFSET        0x0D00U            // qcom,tcs-offset
#define RSC_DRV_ID_REG        0x00U
#define RSC_DRV_CHILD_CFG     0x0CU
#define RSC_DRV_ID_NUM        2U                 // qcom,drv-id
#define RSC_TCS_STRIDE        672U
#define RSC_FIRST_ACTIVE_TCS  0U                 // tcs-config: 2 ACTIVE TCS come first
#define RSC_ACTIVE_TCS_COUNT  2U

#define TCS_AMC_ENABLE        BIT16
#define TCS_AMC_TRIGGER       BIT24
#define CMD_MSGID_LEN         8U
#define CMD_MSGID_RESP_REQ    BIT8
#define CMD_MSGID_WRITE       BIT16
#define CMD_STATUS_COMPL      BIT16

// ---------------------------------------------------------------- pin controller
#define TLMM_SOUTH            0x0F500000U
#define TLMM_NORTH            0x0F900000U
#define TLMM_PIN_STRIDE       0x1000U

#define TS_RESET_GPIO         33U            // south tile
#define TS_IRQ_GPIO           39U            // south tile
#define SPI_GPIO_FIRST        8U             // north tile, gpio 8..11 = SE4 MISO/MOSI/CLK/CS

// ctl register: pull [1:0] (0 none, 1 down, 3 up), function [5:2], drive [8:6] (2 mA * (n+1)), oe bit 9.
// io register : bit0 input value, bit1 output value.
#define PIN_CTL(Func, Pull, Drive, Oe) \
  ((UINT32)(Pull) | ((UINT32)(Func) << 2) | ((UINT32)(Drive) << 6) | ((UINT32)(Oe) << 9))

// ---------------------------------------------------------------- PMIC arbiter (SPMI)
#define PMIC_ARB_CORE_BASE        0x0C440000U
#define PMIC_ARB_CHNLS_BASE       0x0C600000U
#define PMIC_ARB_OBSERVER_BASE    0x0E600000U
#define PMIC_ARB_CONFIG_BASE      0x0C40A000U
#define PMIC_ARB_FEATURES         0x0004U
#define PMIC_ARB_APID_MAP_BASE    0x0900U
#define PMIC_ARB_APID_OWNER_BASE  0x0700U
#define PMIC_ARB_MAX_APIDS        512U
#define PMIC_ARB_CMD              0x00U
#define PMIC_ARB_STATUS           0x08U
#define PMIC_ARB_WDATA0           0x10U
#define PMIC_ARB_RDATA0           0x18U
#define PMIC_ARB_OP_EXT_WRITEL    0U
#define PMIC_ARB_OP_EXT_READL     1U
#define PMIC_ARB_STATUS_DONE      BIT0
#define PMIC_ARB_STATUS_ERROR     (BIT1 | BIT2 | BIT3)

#define PM8150L_SID               5U         // pm8150l LDOs: APIDs map to SID 5 (SID 4 has none); owned by the RPMh EE
#define LDO1_BASE                 0x4000U    // pm8150a_l1  (vcc_i2c, 1.8 V)
#define LDO10_BASE                0x4900U    // pm8150a_l10 (vdd, 3.2 V)
#define LDO_REG_TYPE              0x04U
#define LDO_REG_SUBTYPE           0x05U
#define LDO_REG_STATUS1           0x08U
#define LDO_REG_VSET_LB           0x40U
#define LDO_REG_VSET_UB           0x41U
#define LDO_REG_ENABLE            0x46U
#define LDO_TYPE                  0x10U
#define LDO_ENABLE_BIT            BIT7

// ---------------------------------------------------------------- GENI serial engine
#define SE4_BASE              0x00990000U

#define GENI_FORCE_DEFAULT    0x020U
#define GENI_OUTPUT_CTRL      0x024U
#define GENI_CGC_CTRL         0x028U
#define GENI_STATUS           0x040U
#define GENI_SER_M_CLK_CFG    0x048U
#define GENI_IF_DISABLE_RO    0x064U
#define GENI_FW_REVISION_RO   0x068U
#define GENI_MULTILOCK_MSA_RO 0x074U
#define GENI_CLK_SEL          0x07CU
#define GENI_DMA_MODE_EN      0x258U
#define GENI_BYTE_GRAN        0x254U
#define GENI_TX_PACKING_CFG0  0x260U
#define GENI_TX_PACKING_CFG1  0x264U
#define GENI_RX_PACKING_CFG0  0x284U
#define GENI_RX_PACKING_CFG1  0x288U
#define SPI_CPHA              0x224U
#define SPI_LOOPBACK          0x22CU
#define SPI_CPOL              0x230U
#define SPI_DEMUX_OUTPUT_INV  0x24CU
#define SPI_DEMUX_SEL         0x250U
#define SPI_TRANS_CFG         0x25CU
#define SPI_WORD_LEN          0x268U
#define SPI_TX_TRANS_LEN      0x26CU
#define SPI_RX_TRANS_LEN      0x270U
#define GENI_M_CMD0           0x600U
#define GENI_M_CMD_CTRL       0x604U
#define GENI_M_IRQ_STATUS     0x610U
#define GENI_M_IRQ_EN         0x614U
#define GENI_M_IRQ_CLEAR      0x618U
#define GENI_S_IRQ_CLEAR      0x648U
#define GENI_TX_FIFO          0x700U
#define GENI_RX_FIFO          0x780U
#define GENI_RX_FIFO_STATUS   0x804U
#define GENI_TX_WATERMARK     0x80CU
#define GENI_RX_WATERMARK     0x810U
#define GENI_RX_RFR_WATERMARK 0x814U
#define GENI_IOS              0x908U
#define DMA_TX_IRQ_CLR        0xC44U
#define DMA_RX_IRQ_CLR        0xD44U
#define GENI_SE_IRQ_EN        0xE1CU
#define GENI_HW_PARAM_0       0xE24U
#define DMA_GENERAL_CFG       0xE30U

#define M_CMD_DONE            BIT0
#define SPI_TX_RX_OPCODE      7U
#define SPI_PROTOCOL          1U

#define SPI_DIV               4U             // 19.2 MHz / 4 = 4.8 MHz (device tree allows 6 MHz)

#define SE_READ(Off)          MmioRead32 (SE4_BASE + (Off))
#define SE_WRITE(Off, Val)    MmioWrite32 (SE4_BASE + (Off), (Val))

// ---------------------------------------------------------------- FTS protocol
#define FTS_REG_CHIP_ID       0xA3U          // expected 0x56
#define FTS_REG_CHIP_ID2      0x9FU          // expected 0x52
#define FTS_READ_CMD          0xA0U          // 0x80 read | 0x20 CRC enabled
#define FTS_DUMMY             3U
#define FTS_HEADER            4U
#define FTS_XFER_LEN          (FTS_HEADER + FTS_DUMMY + 1 + 2)   // = 10 bytes for a 1-byte read

//
// ---------------------------------------------------------------------------- SPMI
//

STATIC
INT32
FindApid (
  IN UINT16   Ppid,
  IN BOOLEAN  RequireWriteOwner
  )
{
  UINT32 Count;
  UINT32 Index;
  INT32  FirstMatch;

  Count = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_FEATURES) & 0x7FFU;
  if ((Count == 0) || (Count > PMIC_ARB_MAX_APIDS)) {
    Count = PMIC_ARB_MAX_APIDS;
  }

  FirstMatch = -1;
  for (Index = 0; Index < Count; Index++) {
    UINT32 Map;
    UINT32 Owner;

    Map = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_APID_MAP_BASE + (Index * sizeof (UINT32)));
    if ((((Map >> 8) & 0xFFFU) != Ppid) || (Map == 0)) {
      continue;
    }
    if (FirstMatch < 0) {
      FirstMatch = (INT32)Index;
    }
    Owner = MmioRead32 (PMIC_ARB_CONFIG_BASE + PMIC_ARB_APID_OWNER_BASE +
                        (Index * sizeof (UINT32))) & 0x7U;
    if (Owner == 0) {
      return (INT32)Index;
    }
  }

  return RequireWriteOwner ? -1 : FirstMatch;
}

STATIC
EFI_STATUS
WaitForSpmi (
  IN UINTN  StatusAddress
  )
{
  UINT32 Retry;

  for (Retry = 0; Retry < 1000; Retry++) {
    UINT32 Status;

    Status = MmioRead32 (StatusAddress);
    if ((Status & PMIC_ARB_STATUS_DONE) != 0) {
      return ((Status & PMIC_ARB_STATUS_ERROR) == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SpmiRead (
  IN  UINT16  Address,
  OUT UINT8   *Value
  )
{
  INT32       Apid;
  UINTN       Channel;
  EFI_STATUS  Status;

  Apid = FindApid ((UINT16)((PM8150L_SID << 8) | (Address >> 8)), FALSE);
  if (Apid < 0) {
    return EFI_NOT_FOUND;
  }

  Channel = PMIC_ARB_OBSERVER_BASE + (0x80U * (UINT32)Apid);
  MmioWrite32 (Channel + PMIC_ARB_CMD, (PMIC_ARB_OP_EXT_READL << 27) | ((Address & 0xFFU) << 4));
  Status = WaitForSpmi (Channel + PMIC_ARB_STATUS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = (UINT8)MmioRead32 (Channel + PMIC_ARB_RDATA0);
  return EFI_SUCCESS;
}

/**
  Print one LDO's identity and state (read only; the LDOs are owned by RPMh, so the Apps EE
  cannot write them over SPMI).
**/
STATIC
VOID
ReadLdo (
  IN CONST CHAR16  *Name,
  IN UINT16        Base
  )
{
  UINT8       Type;
  UINT8       Sub;
  UINT8       Stat;
  UINT8       Lb;
  UINT8       Ub;
  UINT8       En;
  EFI_STATUS  Status;

  Type = Sub = Stat = Lb = Ub = En = 0xEE;
  Status  = SpmiRead (Base + LDO_REG_TYPE, &Type);
  Status |= SpmiRead (Base + LDO_REG_SUBTYPE, &Sub);
  Status |= SpmiRead (Base + LDO_REG_STATUS1, &Stat);
  Status |= SpmiRead (Base + LDO_REG_VSET_LB, &Lb);
  Status |= SpmiRead (Base + LDO_REG_VSET_UB, &Ub);
  Status |= SpmiRead (Base + LDO_REG_ENABLE, &En);

  Print (L"  %s @%04x: type %02x sub %02x st %02x vset %02x%02x en %02x %r\r\n",
         Name, Base, Type, Sub, Stat, Ub, Lb, En, Status);
}

//
// ---------------------------------------------------------------------------- GPIO
//

STATIC
VOID
PinSetup (
  IN UINT32  Tile,
  IN UINT32  Gpio,
  IN UINT32  Ctl
  )
{
  MmioWrite32 (Tile + TLMM_PIN_STRIDE * Gpio, Ctl);
}

STATIC
VOID
PinOutput (
  IN UINT32   Tile,
  IN UINT32   Gpio,
  IN BOOLEAN  High
  )
{
  MmioWrite32 (Tile + TLMM_PIN_STRIDE * Gpio + 4, High ? BIT1 : 0);
}

STATIC
UINT32
PinInput (
  IN UINT32  Tile,
  IN UINT32  Gpio
  )
{
  return MmioRead32 (Tile + TLMM_PIN_STRIDE * Gpio + 4) & 1;
}

//
// ---------------------------------------------------------------------------- GENI SPI
//

STATIC
UINT16
CrcKermit (
  IN CONST UINT8  *Data,
  IN UINTN        Len
  )
{
  UINT16 Crc;
  UINTN  I;
  UINTN  J;

  Crc = 0xFFFF;
  for (I = 0; I < Len; I++) {
    Crc ^= Data[I];
    for (J = 0; J < 8; J++) {
      Crc = ((Crc & 1) != 0) ? (UINT16)((Crc >> 1) ^ 0x8408) : (UINT16)(Crc >> 1);
    }
  }
  return Crc;
}

STATIC
VOID
GeniPackingEightBit (
  VOID
  )
{
  // Port of geni_se_config_packing (bpw 8, 4 words per FIFO entry, MSB first, TX and RX).
  UINT32 Cfg[4];
  UINT32 Idx;
  UINT32 I;

  Idx = 7;
  for (I = 0; I < 4; I++) {
    Cfg[I]  = (Idx << 5) | (1U << 4) | (7U << 1);
    Idx     = ((I + 1) * 8) + 7;
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
GeniInit (
  VOID
  )
{
  UINT32 Depth;

  // geni_se_init(): clear interrupts, force clock gating off, default IO.
  SE_WRITE (GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
  SE_WRITE (GENI_S_IRQ_CLEAR, 0xFFFFFFFFU);
  SE_WRITE (DMA_TX_IRQ_CLR, 0xFFFFFFFFU);
  SE_WRITE (DMA_RX_IRQ_CLR, 0xFFFFFFFFU);

  SE_WRITE (GENI_CGC_CTRL, SE_READ (GENI_CGC_CTRL) | 0x7FU);
  SE_WRITE (DMA_GENERAL_CFG, SE_READ (DMA_GENERAL_CFG) | 0xFU);
  SE_WRITE (GENI_OUTPUT_CTRL, 0x7FU);
  SE_WRITE (GENI_FORCE_DEFAULT, BIT0);
  SE_WRITE (GENI_SE_IRQ_EN, SE_READ (GENI_SE_IRQ_EN) | 0xFU);

  Depth = (SE_READ (GENI_HW_PARAM_0) >> 16) & 0x3FU;
  SE_WRITE (GENI_RX_WATERMARK, (Depth > 3) ? Depth - 3 : 1);
  SE_WRITE (GENI_RX_RFR_WATERMARK, (Depth > 2) ? Depth - 2 : 1);
  SE_WRITE (GENI_M_IRQ_EN, SE_READ (GENI_M_IRQ_EN) | 0x7EU | M_CMD_DONE);

  // FIFO mode (no DMA), SPI mode 0, CS0, no loopback.
  SE_WRITE (GENI_DMA_MODE_EN, SE_READ (GENI_DMA_MODE_EN) & ~BIT0);
  SE_WRITE (SPI_LOOPBACK, 0);
  SE_WRITE (SPI_DEMUX_SEL, 0);
  SE_WRITE (SPI_CPHA, 0);
  SE_WRITE (SPI_CPOL, 0);
  SE_WRITE (SPI_DEMUX_OUTPUT_INV, 0);
  SE_WRITE (SPI_TRANS_CFG, SE_READ (SPI_TRANS_CFG) & ~BIT1);   // no CS toggle between words

  GeniPackingEightBit ();
  SE_WRITE (SPI_WORD_LEN, (8 - 4) & 0x3FFU);

  // Core clock from XO (source 0), divided by SPI_DIV.
  SE_WRITE (GENI_CLK_SEL, 0);
  SE_WRITE (GENI_SER_M_CLK_CFG, (SPI_DIV << 4) | BIT0);
}

/**
  One full-duplex SPI transfer of Len bytes (Len <= 12).  Returns EFI_SUCCESS when the
  controller reports done and all bytes are back.
**/
STATIC
EFI_STATUS
GeniTransfer (
  IN  CONST UINT8  *Tx,
  OUT UINT8        *Rx,
  IN  UINTN        Len,
  OUT UINT32       *IrqStatus
  )
{
  UINT32  Word;
  UINTN   I;
  UINTN   Got;
  UINT32  Timeout;
  UINT32  Wc;

  SE_WRITE (GENI_M_IRQ_CLEAR, 0xFFFFFFFFU);
  SE_WRITE (SPI_TX_TRANS_LEN, (UINT32)Len);
  SE_WRITE (SPI_RX_TRANS_LEN, (UINT32)Len);
  SE_WRITE (GENI_M_CMD0, (SPI_TX_RX_OPCODE << 27));         // no FRAGMENTATION: CS drops at end

  for (I = 0; I < Len; I += 4) {
    Word = 0;
    if (I + 0 < Len) Word |= (UINT32)Tx[I + 0];
    if (I + 1 < Len) Word |= (UINT32)Tx[I + 1] << 8;
    if (I + 2 < Len) Word |= (UINT32)Tx[I + 2] << 16;
    if (I + 3 < Len) Word |= (UINT32)Tx[I + 3] << 24;
    SE_WRITE (GENI_TX_FIFO, Word);
  }

  for (Timeout = 0; Timeout < 50000; Timeout++) {            // up to 50 ms
    if ((SE_READ (GENI_M_IRQ_STATUS) & M_CMD_DONE) != 0) {
      break;
    }
    MicroSecondDelay (1);
  }
  *IrqStatus = SE_READ (GENI_M_IRQ_STATUS);

  if (Timeout >= 50000) {
    SE_WRITE (GENI_M_CMD_CTRL, BIT2);                        // cancel
    MicroSecondDelay (2000);
    SE_WRITE (GENI_M_CMD_CTRL, BIT1);                        // abort
    MicroSecondDelay (2000);
    return EFI_TIMEOUT;
  }

  Got = 0;
  Wc  = SE_READ (GENI_RX_FIFO_STATUS) & 0x1FFFFFFU;
  while ((Wc > 0) && (Got < Len)) {
    Word = SE_READ (GENI_RX_FIFO);
    for (I = 0; I < 4 && Got < Len; I++) {
      Rx[Got++] = (UINT8)(Word >> (8 * I));
    }
    Wc--;
  }

  return (Got == Len) ? EFI_SUCCESS : EFI_END_OF_MEDIA;
}

/**
  Read one FTS register with the FocalTech SPI framing and CRC.
**/
STATIC
EFI_STATUS
FtsReadReg (
  IN  UINT8   Reg,
  OUT UINT8   *Value,
  OUT UINT8   *Raw       // FTS_XFER_LEN bytes of the raw response
  )
{
  UINT8       Tx[FTS_XFER_LEN];
  UINT32      Irq;
  EFI_STATUS  Status;
  UINT16      CrcCalc;
  UINT16      CrcRead;

  ZeroMem (Tx, sizeof (Tx));
  ZeroMem (Raw, FTS_XFER_LEN);
  Tx[0] = Reg;
  Tx[1] = FTS_READ_CMD;
  Tx[2] = 0;
  Tx[3] = 1;                                                 // one data byte

  Status = GeniTransfer (Tx, Raw, FTS_XFER_LEN, &Irq);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = Raw[FTS_HEADER + FTS_DUMMY];
  if ((Raw[3] & 0xA0U) != 0) {
    return EFI_NOT_READY;                                    // chip reports busy/error status
  }

  CrcCalc = CrcKermit (&Raw[FTS_HEADER + FTS_DUMMY], 1);
  CrcRead = (UINT16)(Raw[FTS_HEADER + FTS_DUMMY + 2] << 8) | Raw[FTS_HEADER + FTS_DUMMY + 1];
  return (CrcCalc == CrcRead) ? EFI_SUCCESS : EFI_CRC_ERROR;
}

//
// ---------------------------------------------------------------------------- RPMh
//

typedef struct {
  UINT32  IrqEnable, IrqStatus, IrqClear;     // relative to the TCS base
  UINT32  CmdStride;
  UINT32  Control, CmdEnable, MsgId, Addr, Data, CmdStatus;   // relative to a TCS
} RSC_REGS;

STATIC CONST RSC_REGS  mRegs27 = {0x00, 0x04, 0x08, 20, 0x14, 0x1C, 0x30, 0x34, 0x38, 0x3C};
STATIC CONST RSC_REGS  mRegs30 = {0x00, 0x04, 0x08, 24, 0x24, 0x2C, 0x34, 0x38, 0x3C, 0x40};

STATIC CONST RSC_REGS  *mR;

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
  Find a resource in the AOP command DB.  Returns its address (0 if missing) and, if asked,
  the first auxiliary data word (the BCM bandwidth unit).
**/
STATIC
UINT32
CmdDbFind (
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Aux
  )
{
  UINT32 I;
  UINT32 J;

  if (Aux != NULL) {
    *Aux = 0;
  }
  if (MmioRead32 (CMD_DB_BASE + 4) != 0x0C0330DBU) {         // magic db 30 03 0c
    return 0;
  }

  for (I = 0; I < CMD_DB_MAX_SLV; I++) {
    UINT32 Hdr0  = MmioRead32 (CMD_DB_BASE + 8 + I * 16);    // slv_id | header_offset << 16
    UINT32 Hdr1  = MmioRead32 (CMD_DB_BASE + 8 + I * 16 + 4);// data_offset | cnt << 16
    UINT32 Ents;
    UINT32 DataO;
    UINT32 Cnt;

    if ((Hdr0 & 0xFFFFU) == 0) {
      break;
    }
    Ents  = CMD_DB_DATA_OFFSET + (Hdr0 >> 16);
    DataO = CMD_DB_DATA_OFFSET + (Hdr1 & 0xFFFFU);
    Cnt   = Hdr1 >> 16;
    if ((Cnt > 512) || (Ents + Cnt * CMD_DB_ENTRY_SIZE > CMD_DB_SIZE)) {
      continue;
    }
    for (J = 0; J < Cnt; J++) {
      UINT32 E = CMD_DB_BASE + Ents + J * CMD_DB_ENTRY_SIZE;
      UINT32 Id[2];
      Id[0] = MmioRead32 (E);
      Id[1] = MmioRead32 (E + 4);
      if (NameEq ((UINT8 *)Id, Name)) {
        UINT32 Len = MmioRead32 (E + 20) & 0xFFFFU;
        UINT32 Off = MmioRead32 (E + 20) >> 16;
        if ((Aux != NULL) && (Len >= 4) && (DataO + Off + 4 <= CMD_DB_SIZE)) {
          *Aux = MmioRead32 (CMD_DB_BASE + DataO + Off);
        }
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

STATIC UINT32  mTcs = 0xFFFFFFFFU;

/**
  Read-only look at the RSC and pick a free active TCS.  This is the first access to the RSC.
**/
STATIC
BOOLEAN
RscInit (
  VOID
  )
{
  UINT32  Id;
  UINT32  Cfg;
  UINT32  Major;
  UINT32  I;

  Id    = MmioRead32 (RSC_DRV2_BASE + RSC_DRV_ID_REG);
  Major = (Id >> 16) & 0xFFU;
  Print (L"  RSC drv2 id %08x\r\n", Id);
  if ((Major == 0) || (Major > 3)) {
    Print (L"  RSC id unexpected, not touching it\r\n");
    return FALSE;
  }
  mR  = (Major >= 3) ? &mRegs30 : &mRegs27;
  Cfg = MmioRead32 (RSC_DRV2_BASE + RSC_DRV_CHILD_CFG);
  Print (L"  RSC child cfg %08x (tcs for drv2: %u)\r\n", Cfg, (Cfg >> (6 * RSC_DRV_ID_NUM)) & 0x3FU);

  for (I = RSC_FIRST_ACTIVE_TCS; I < RSC_FIRST_ACTIVE_TCS + RSC_ACTIVE_TCS_COUNT; I++) {
    UINT32 En   = TcsRead (I, mR->CmdEnable);
    UINT32 Ctl  = TcsRead (I, mR->Control);
    Print (L"  tcs%u: cmd_enable %08x control %08x\r\n", I, En, Ctl);
    if ((mTcs == 0xFFFFFFFFU) && (En == 0) && ((Ctl & TCS_AMC_TRIGGER) == 0)) {   // bootloader leaves AMC enable set
      mTcs = I;
    }
  }
  if (mTcs == 0xFFFFFFFFU) {
    Print (L"  no free active TCS\r\n");
    return FALSE;
  }
  MmioWrite32 (RSC_DRV2_BASE + RSC_TCS_OFFSET + mR->IrqClear, BIT0 << mTcs);
  return TRUE;
}

/**
  Send one active-set BCM vote (commit + valid) through the chosen TCS and wait for the AOP
  to answer.  Same sequence as Linux rpmh_rsc_send_data() / tcs_tx_done().
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

  // __tcs_set_trigger(true)
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

  Print (L"    rpmh %05x <- %08x: %r (cmd status %08x)\r\n", Addr, Data, Status,
         TcsRead (mTcs, mR->CmdStatus));

  // __tcs_set_trigger(false), reclaim
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
  UINT32  Aux;
  UINT32  Addr;

  Addr = CmdDbFind (Name, &Aux);
  if (Addr == 0) {
    Print (L"    BCM %a not in cmd-db\r\n", Name);
    return EFI_NOT_FOUND;
  }
  return RpmhSend (Addr, BIT30 | BIT29 | ((Vote & 0x3FFFU) << 14) | (Vote & 0x3FFFU));
}

/**
  Switch an RPMh VRM regulator on (active set): voltage in mV at +0, enable at +4, as in the
  downstream rpmh-regulator driver.
**/
STATIC
EFI_STATUS
VrmEnable (
  IN CONST CHAR8  *Name,
  IN UINT32       Millivolt
  )
{
  UINT32      Addr;
  EFI_STATUS  Status;

  Addr = CmdDbFind (Name, NULL);
  Print (L"  vrm %a @%05x -> %u mV\r\n", Name, Addr, Millivolt);
  if (Addr == 0) {
    return EFI_NOT_FOUND;
  }
  Status = RpmhSend (Addr + 0, Millivolt);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return RpmhSend (Addr + 4, 1);
}

//
// ---------------------------------------------------------------------------- main
//

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
  *C = MmioRead32 (GCC_BASE + GCC_WRAP0_S4_HALT);
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
  UINT32  Aux1;
  UINT32  Aux2;

  MmioWrite32 (GCC_BASE + GCC_QUP_VOTE_REG,
               MmioRead32 (GCC_BASE + GCC_QUP_VOTE_REG) | QUP_VOTE_BITS);

  if (WaitQupClocks (&M, &S, &C, 20)) {
    goto On;
  }
  // Stage 1: the s_ahb branch has hardware clock gating (bit 1).  With it on, the status bit
  // reads "off" whenever the branch is idle (Linux skips the halt check in that mode).  Turn
  // the gating off so the branch really stays running and the status becomes meaningful.
  if (((M | C) & BIT31) == 0 && ((S & BIT31) != 0) && ((S & BIT1) != 0)) {
    Print (L"  s_ahb is hw-gated (%08x): disabling hwcg\r\n", S);
    MmioAnd32 (GCC_BASE + GCC_WRAP0_S_AHB_HALT, ~(UINT32)BIT1);
    if (WaitQupClocks (&M, &S, &C, 5)) {
      goto On;
    }
    Print (L"  hwcg off, s_ahb still %08x\r\n", S);
  }

  // Stage 2: RPMh bus vote (QUP core / config NoC BCMs), as the downstream kernel does.
  Aux1 = Aux2 = 0;
  Print (L"  cmd-db: QUP0 @%05x CN0 @%05x (hdr %08x %08x)\r\n",
         CmdDbFind ("QUP0", &Aux1), CmdDbFind ("CN0", &Aux2),
         MmioRead32 (CMD_DB_BASE), MmioRead32 (CMD_DB_BASE + 4));
  Print (L"  bcm unit QUP0 %08x CN0 %08x\r\n", Aux1, Aux2);

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
    Print (L"  vote %x: s_ahb %08x\r\n", Vote, S);
  }

Fail:
  Print (L"  QUP clocks did NOT start (m %08x s %08x se4 %08x)\r\n", M, S, C);
  return FALSE;

On:
  Print (L"  QUP clocks on (m %08x s %08x se4 %08x, rcg cfg %08x)\r\n", M, S, C,
         MmioRead32 (GCC_BASE + GCC_WRAP0_S4_RCG + 4));
  return TRUE;
}

STATIC
VOID
EFIAPI
MunchTouchReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32      Fw;
  UINT32      Attempt;
  UINT8       Raw[FTS_XFER_LEN];
  UINT8       Id1;
  UINT8       Id2;
  EFI_STATUS  S1;
  EFI_STATUS  S2;
  UINT32      I;

  gBS->CloseEvent (Event);

  Print (L"\r\nMunchTouch step 2\r\n");

  if (!EnableQupClocks ()) {
    MicroSecondDelay (20000000);
    return;
  }

  // Pins: SPI bus on gpio 8..11 (function 1, no pull, 6 mA); reset low, irq input pulled up.
  for (I = 0; I < 4; I++) {
    PinSetup (TLMM_NORTH, SPI_GPIO_FIRST + I, PIN_CTL (1, 0, 2, 0));
  }
  PinOutput (TLMM_SOUTH, TS_RESET_GPIO, FALSE);
  PinSetup (TLMM_SOUTH, TS_RESET_GPIO, PIN_CTL (0, 3, 3, 1));
  PinSetup (TLMM_SOUTH, TS_IRQ_GPIO, PIN_CTL (0, 3, 3, 0));
  MicroSecondDelay (5000);

  // Rails: 1.8 V I/O first, then 3.2 V core (same order as the Linux driver), through RPMh.
  if (!RscInit ()) {
    Print (L"  RSC not usable: cannot power the touch chip\r\n");
    MicroSecondDelay (15000000);
    return;
  }
  VrmEnable ("ldoc1", 1800);
  MicroSecondDelay (2000);
  VrmEnable ("ldoc10", 3200);
  MicroSecondDelay (10000);
  ReadLdo (L"L1 ", LDO1_BASE);
  ReadLdo (L"L10", LDO10_BASE);

  // Release reset and give the chip time to boot.
  PinOutput (TLMM_SOUTH, TS_RESET_GPIO, TRUE);
  MicroSecondDelay (300000);
  Print (L"  reset released, irq pin %u\r\n", PinInput (TLMM_SOUTH, TS_IRQ_GPIO));

  // Serial engine.  Registers are only touched now that the clocks are confirmed running.
  Fw = SE_READ (GENI_FW_REVISION_RO);
  Print (L"  se4: fw %08x proto %u hw0 %08x ifdis %08x lock %08x\r\n",
         Fw, (Fw >> 8) & 0xFF, SE_READ (GENI_HW_PARAM_0),
         SE_READ (GENI_IF_DISABLE_RO), SE_READ (GENI_MULTILOCK_MSA_RO));

  if (((Fw >> 8) & 0xFFU) != SPI_PROTOCOL) {
    Print (L"  SE4 firmware is not SPI (proto %u): stopping\r\n", (Fw >> 8) & 0xFF);
    MicroSecondDelay (8000000);
    return;
  }
  if ((SE_READ (GENI_IF_DISABLE_RO) & BIT0) != 0) {
    Print (L"  SE4 FIFO interface disabled (GPI DMA only): stopping\r\n");
    MicroSecondDelay (8000000);
    return;
  }

  GeniInit ();
  Print (L"  se4 initialised, clk cfg %08x status %08x\r\n",
         SE_READ (GENI_SER_M_CLK_CFG), SE_READ (GENI_STATUS));

  // Self test: SE loopback (MOSI -> MISO inside the engine) proves clocks and FIFOs work.
  {
    UINT8   LbTx[4] = {0xA5, 0x3C, 0x0F, 0xF0};
    UINT8   LbRx[4] = {0, 0, 0, 0};
    UINT32  LbIrq;

    SE_WRITE (SPI_LOOPBACK, 1);
    S1 = GeniTransfer (LbTx, LbRx, 4, &LbIrq);
    SE_WRITE (SPI_LOOPBACK, 0);
    Print (L"  loopback: %02x %02x %02x %02x (want a5 3c 0f f0) %r\r\n", LbRx[0], LbRx[1], LbRx[2], LbRx[3], S1);
  }

  // Is MISO driven by the chip?  Read the pin without and with a pull-up.
  Print (L"  miso level: no pull %u", PinInput (TLMM_NORTH, SPI_GPIO_FIRST));
  PinSetup (TLMM_NORTH, SPI_GPIO_FIRST, PIN_CTL (1, 3, 2, 0));
  MicroSecondDelay (1000);
  Print (L", pull-up %u (irq %u)\r\n", PinInput (TLMM_NORTH, SPI_GPIO_FIRST), PinInput (TLMM_SOUTH, TS_IRQ_GPIO));
  PinSetup (TLMM_NORTH, SPI_GPIO_FIRST, PIN_CTL (1, 0, 2, 0));

  Id1 = Id2 = 0;
  S1 = S2 = EFI_NOT_STARTED;
  for (Attempt = 0; Attempt < 4; Attempt++) {
    UINT8  Tx2[FTS_XFER_LEN];
    UINT8  Rx2[FTS_XFER_LEN];
    UINT32 Irq2;

    SE_WRITE (SPI_CPHA, Attempt & 1);
    SE_WRITE (SPI_CPOL, ((Attempt & 2) != 0) ? BIT2 : 0);

    S1 = FtsReadReg (FTS_REG_CHIP_ID, &Id1, Raw);
    Print (L"  mode %u A3: %02x %02x %02x %02x|%02x %02x %02x|%02x|%02x %02x %r\r\n", Attempt,
           Raw[0], Raw[1], Raw[2], Raw[3], Raw[4], Raw[5], Raw[6], Raw[7], Raw[8], Raw[9], S1);

    ZeroMem (Tx2, sizeof (Tx2));
    Tx2[0] = FTS_REG_CHIP_ID;
    Tx2[1] = 0x80;                                            // read, no CRC
    Tx2[3] = 1;
    GeniTransfer (Tx2, Rx2, FTS_XFER_LEN, &Irq2);
    Print (L"         no-crc: %02x %02x %02x %02x|%02x %02x %02x|%02x|%02x %02x\r\n",
           Rx2[0], Rx2[1], Rx2[2], Rx2[3], Rx2[4], Rx2[5], Rx2[6], Rx2[7], Rx2[8], Rx2[9]);

    if (!EFI_ERROR (S1)) {
      S2 = FtsReadReg (FTS_REG_CHIP_ID2, &Id2, Raw);
      break;
    }
    MicroSecondDelay (50000);
  }

  if (EFI_ERROR (S1)) {
    Print (L"  last attempt raw: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x  %r\r\n",
           Raw[0], Raw[1], Raw[2], Raw[3], Raw[4], Raw[5], Raw[6], Raw[7], Raw[8], Raw[9], S1);
    Print (L"  Touch chip ID: NOT READ (%r) m_irq %08x\r\n", S1, SE_READ (GENI_M_IRQ_STATUS));
  } else {
    Print (L"  Touch chip ID: %02x %02x (expect 56 52)  %r\r\n", Id1, Id2, S2);
  }

  MicroSecondDelay (20000000);
}

EFI_STATUS
EFIAPI
MunchTouchDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT Event;

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                MunchTouchReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &Event
                );
}
