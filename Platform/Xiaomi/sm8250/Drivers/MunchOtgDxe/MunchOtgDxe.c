#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define PMIC_ARB_CORE_BASE        0x0C440000U
#define PMIC_ARB_CHNLS_BASE       0x0C600000U
#define PMIC_ARB_OBSERVER_BASE    0x0E600000U
#define PMIC_ARB_CONFIG_BASE      0x0C40A000U

#define PMIC_ARB_VERSION          0x0000U
#define PMIC_ARB_FEATURES         0x0004U
#define PMIC_ARB_APID_MAP_BASE    0x0900U
#define PMIC_ARB_APID_OWNER_BASE  0x0700U
#define PMIC_ARB_MAX_APIDS        128U

#define PMIC_ARB_CMD              0x00U
#define PMIC_ARB_STATUS           0x08U
#define PMIC_ARB_WDATA0           0x10U
#define PMIC_ARB_RDATA0           0x18U

#define PMIC_ARB_OP_EXT_WRITEL    0U
#define PMIC_ARB_OP_EXT_READL     1U
#define PMIC_ARB_STATUS_DONE      BIT0
#define PMIC_ARB_STATUS_ERROR     (BIT1 | BIT2 | BIT3)

#define PM8150B_SID               2U
#define PM8150B_DCDC_PPID         0x211U
#define PM8150B_TYPEC_PPID        0x215U
#define TYPE_C_STATUS_4_REG       0x150EU
#define TYPEC_DFP_MODE            BIT7
#define TYPEC_VBUS_STATUS         BIT6
#define TYPEC_VBUS_ERROR          BIT5
#define TYPEC_DEBOUNCE_DONE       BIT4
#define TYPEC_CC_ATTACHED         BIT0

#define DCDC_CMD_OTG_REG          0x1140U
#define DCDC_OTG_CFG_REG          0x1153U
#define OTG_EN_BIT                BIT0
#define OTG_EN_SRC_CFG_BIT        BIT1

STATIC
INT32
FindApid (
  IN UINT16  Ppid,
  IN BOOLEAN RequireWriteOwner
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

    Map = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_APID_MAP_BASE +
                      (Index * sizeof (UINT32)));
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
WaitForTransaction (
  IN UINTN StatusAddress
  )
{
  UINT32 Retry;

  for (Retry = 0; Retry < 1000; Retry++) {
    UINT32 Status;

    Status = MmioRead32 (StatusAddress);
    if ((Status & PMIC_ARB_STATUS_DONE) != 0) {
      return ((Status & PMIC_ARB_STATUS_ERROR) == 0) ?
             EFI_SUCCESS : EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SpmiReadByte (
  IN  UINT16 Address,
  OUT UINT8  *Value
  )
{
  INT32  Apid;
  UINTN  Channel;
  UINT32 Command;
  EFI_STATUS Status;

  Apid = FindApid ((UINT16)((PM8150B_SID << 8) | (Address >> 8)), FALSE);
  if (Apid < 0) {
    return EFI_NOT_FOUND;
  }

  Channel = PMIC_ARB_OBSERVER_BASE + (0x80U * (UINT32)Apid);
  Command = (PMIC_ARB_OP_EXT_READL << 27) | ((Address & 0xFFU) << 4);
  MmioWrite32 (Channel + PMIC_ARB_CMD, Command);
  Status = WaitForTransaction (Channel + PMIC_ARB_STATUS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = (UINT8)MmioRead32 (Channel + PMIC_ARB_RDATA0);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SpmiWriteByte (
  IN UINT16 Address,
  IN UINT8  Value
  )
{
  INT32  Apid;
  UINTN  Channel;
  UINT32 Command;

  Apid = FindApid ((UINT16)((PM8150B_SID << 8) | (Address >> 8)), TRUE);
  if (Apid < 0) {
    return EFI_ACCESS_DENIED;
  }

  Channel = PMIC_ARB_CHNLS_BASE + (0x10000U * (UINT32)Apid);
  Command = (PMIC_ARB_OP_EXT_WRITEL << 27) | ((Address & 0xFFU) << 4);
  MmioWrite32 (Channel + PMIC_ARB_WDATA0, Value);
  MmioWrite32 (Channel + PMIC_ARB_CMD, Command);
  return WaitForTransaction (Channel + PMIC_ARB_STATUS);
}

STATIC
EFI_STATUS
SpmiMaskedWriteByte (
  IN UINT16 Address,
  IN UINT8  Mask,
  IN UINT8  Value
  )
{
  UINT8 Current;
  EFI_STATUS Status;

  Status = SpmiReadByte (Address, &Current);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Current = (UINT8)((Current & ~Mask) | (Value & Mask));
  return SpmiWriteByte (Address, Current);
}

EFI_STATUS
EFIAPI
MunchOtgDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  UINT32     Retry;
  UINT32     Version;
  UINT8      TypecStatus;
  UINT8      OtgConfig;
  UINT8      OtgCommand;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  OtgConfig  = 0;
  OtgCommand = 0;

  Version = MmioRead32 (PMIC_ARB_CORE_BASE + PMIC_ARB_VERSION);
  if ((Version & 0xF0000000U) != 0x50000000U) {
    DEBUG ((DEBUG_ERROR, "MunchOtgDxe: unsupported PMIC arbiter 0x%08x\n", Version));
    return EFI_UNSUPPORTED;
  }

  TypecStatus = 0;
  for (Retry = 0; Retry < 100; Retry++) {
    Status = SpmiReadByte (TYPE_C_STATUS_4_REG, &TypecStatus);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MunchOtgDxe: Type-C status read failed: %r\n", Status));
      return Status;
    }

    if ((TypecStatus & (TYPEC_VBUS_STATUS | TYPEC_VBUS_ERROR)) != 0) {
      DEBUG ((DEBUG_ERROR, "MunchOtgDxe: external/error VBUS present (0x%02x), refusing source mode\n",
              TypecStatus));
      return EFI_ACCESS_DENIED;
    }

    if ((TypecStatus & (TYPEC_CC_ATTACHED | TYPEC_DEBOUNCE_DONE | TYPEC_DFP_MODE)) ==
        (TYPEC_CC_ATTACHED | TYPEC_DEBOUNCE_DONE | TYPEC_DFP_MODE)) {
      break;
    }

    MicroSecondDelay (10000);
  }

  if ((TypecStatus & (TYPEC_CC_ATTACHED | TYPEC_DEBOUNCE_DONE | TYPEC_DFP_MODE)) !=
      (TYPEC_CC_ATTACHED | TYPEC_DEBOUNCE_DONE | TYPEC_DFP_MODE)) {
    DEBUG ((DEBUG_WARN, "MunchOtgDxe: CC role not initialized (0x%02x); controlled no-VBUS diagnostic continues\n",
            TypecStatus));
  }

  Status = SpmiMaskedWriteByte (DCDC_OTG_CFG_REG, OTG_EN_SRC_CFG_BIT, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MunchOtgDxe: failed to select software VBUS control: %r\n", Status));
    return Status;
  }

  Status = SpmiMaskedWriteByte (DCDC_CMD_OTG_REG, OTG_EN_BIT, OTG_EN_BIT);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MunchOtgDxe: failed to enable OTG boost: %r\n", Status));
    return Status;
  }

  MicroSecondDelay (20000);
  Status = SpmiReadByte (DCDC_OTG_CFG_REG, &OtgConfig);
  if (!EFI_ERROR (Status)) {
    Status = SpmiReadByte (DCDC_CMD_OTG_REG, &OtgCommand);
  }

  if (EFI_ERROR (Status) || ((OtgConfig & OTG_EN_SRC_CFG_BIT) != 0) ||
      ((OtgCommand & OTG_EN_BIT) == 0)) {
    DEBUG ((DEBUG_ERROR, "MunchOtgDxe: OTG verification failed cfg=0x%02x cmd=0x%02x: %r\n",
            OtgConfig, OtgCommand, Status));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "MunchOtgDxe: guarded PM8150B OTG boost enabled\n"));
  return EFI_SUCCESS;
}
