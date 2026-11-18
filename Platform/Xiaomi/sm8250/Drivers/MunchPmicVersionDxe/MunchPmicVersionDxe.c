/** @file
  Read-only PMIC version protocol shim for Xiaomi Munch.

  The bundled SM8250 UsbConfigDxe consumes Qualcomm's PMIC version protocol,
  but the matching PmicDxe binary is from a different product and also owns
  charger and fuel-gauge policy.  This driver supplies only the immutable
  Kona PMIC topology needed by USB initialization.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define EFI_QCOM_PMIC_VERSION_PROTOCOL_REVISION  0x0000000000010004ULL

#define EFI_PMIC_IS_UNKNOWN  0x00U
#define EFI_PMIC_IS_PM8150   0x1EU
#define EFI_PMIC_IS_PM8150L  0x1FU
#define EFI_PMIC_IS_PM8150B  0x20U

typedef struct {
  UINT32 PmicModel;
  UINT32 PmicAllLayerRevision;
  UINT32 PmicMetalRevision;
  UINT32 SlaveCount;
} EFI_PM_DEVICE_INFO_TYPE;

typedef struct {
  UINT32 PmicModel;
  UINT32 PmicAllLayerRevision;
  UINT32 PmicMetalRevision;
  UINT32 PmicVariantRevision;
} EFI_PM_DEVICE_INFO_EXT_TYPE;

typedef
EFI_STATUS
(EFIAPI *EFI_PM_GET_PMIC_INFO)(
  IN  UINT32                   PmicDeviceIndex,
  OUT EFI_PM_DEVICE_INFO_TYPE  *PmicDeviceInfo
  );

typedef
EFI_STATUS
(EFIAPI *EFI_PM_GET_PRIMARY_PMIC_INDEX)(
  OUT UINT32  *PmicDeviceIndex
  );

typedef
EFI_STATUS
(EFIAPI *EFI_PM_GET_PMIC_INFO_EXT)(
  IN  UINT32                       PmicDeviceIndex,
  OUT EFI_PM_DEVICE_INFO_EXT_TYPE  *PmicDeviceInfo
  );

typedef struct {
  UINT64                         Revision;
  EFI_PM_GET_PMIC_INFO           GetPmicInfo;
  EFI_PM_GET_PRIMARY_PMIC_INDEX  GetPrimaryPmicIndex;
  EFI_PM_GET_PMIC_INFO_EXT       GetPmicInfoExt;
} EFI_QCOM_PMIC_VERSION_PROTOCOL;

STATIC EFI_GUID mQcomPmicVersionProtocolGuid = {
  0x4684800a, 0x2755, 0x4edc,
  { 0xb4, 0x43, 0x7f, 0x8c, 0xeb, 0x32, 0x39, 0xd3 }
};

STATIC
UINT32
GetMunchPmicModel (
  IN UINT32  PmicDeviceIndex
  )
{
  STATIC CONST UINT32 mMunchPmicModels[] = {
    EFI_PMIC_IS_PM8150,
    EFI_PMIC_IS_PM8150L,
    EFI_PMIC_IS_PM8150B
  };

  if (PmicDeviceIndex >= ARRAY_SIZE (mMunchPmicModels)) {
    return EFI_PMIC_IS_UNKNOWN;
  }

  return mMunchPmicModels[PmicDeviceIndex];
}

STATIC
EFI_STATUS
EFIAPI
MunchGetPmicInfo (
  IN  UINT32                   PmicDeviceIndex,
  OUT EFI_PM_DEVICE_INFO_TYPE  *PmicDeviceInfo
  )
{
  UINT32 Model;

  if (PmicDeviceInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Model = GetMunchPmicModel (PmicDeviceIndex);
  if (Model == EFI_PMIC_IS_UNKNOWN) {
    return EFI_NOT_FOUND;
  }

  PmicDeviceInfo->PmicModel            = Model;
  PmicDeviceInfo->PmicAllLayerRevision = 1;
  PmicDeviceInfo->PmicMetalRevision    = 0;
  PmicDeviceInfo->SlaveCount           = 2;

  DEBUG ((DEBUG_INFO, "MunchPmicVersionDxe: PMIC[%u] model=0x%x\n",
          PmicDeviceIndex, Model));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MunchGetPrimaryPmicIndex (
  OUT UINT32  *PmicDeviceIndex
  )
{
  if (PmicDeviceIndex == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *PmicDeviceIndex = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MunchGetPmicInfoExt (
  IN  UINT32                       PmicDeviceIndex,
  OUT EFI_PM_DEVICE_INFO_EXT_TYPE  *PmicDeviceInfo
  )
{
  UINT32 Model;

  if (PmicDeviceInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Model = GetMunchPmicModel (PmicDeviceIndex);
  if (Model == EFI_PMIC_IS_UNKNOWN) {
    return EFI_NOT_FOUND;
  }

  PmicDeviceInfo->PmicModel            = Model;
  PmicDeviceInfo->PmicAllLayerRevision = 1;
  PmicDeviceInfo->PmicMetalRevision    = 0;
  PmicDeviceInfo->PmicVariantRevision  = 0;
  return EFI_SUCCESS;
}

STATIC EFI_QCOM_PMIC_VERSION_PROTOCOL mMunchPmicVersionProtocol = {
  EFI_QCOM_PMIC_VERSION_PROTOCOL_REVISION,
  MunchGetPmicInfo,
  MunchGetPrimaryPmicIndex,
  MunchGetPmicInfoExt
};

EFI_STATUS
EFIAPI
MunchPmicVersionDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE Handle;
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &mQcomPmicVersionProtocolGuid,
                  &mMunchPmicVersionProtocol,
                  NULL
                  );
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
          "MunchPmicVersionDxe: install read-only PMIC version protocol: %r\n",
          Status));
  return Status;
}
