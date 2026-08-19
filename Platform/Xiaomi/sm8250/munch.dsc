[Defines]
  VENDOR_NAME                    = Xiaomi
  PLATFORM_NAME                  = munch
  PLATFORM_GUID                  = 7f8e8a93-5492-4f6c-8a1a-4c4f8d229e01
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Qualcomm/sm8250/sm8250.fdf
  DEVICE_DXE_FV_COMPONENTS       = Platform/Xiaomi/sm8250/munch.fdf.inc

!include Platform/Qualcomm/sm8250/sm8250.dsc

[LibraryClasses.common]
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf

[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|5
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x00
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x00000000

  gQcomTokenSpaceGuid.PcdMipiFrameBufferAddress|0x9c000000
  gQcomTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gQcomTokenSpaceGuid.PcdMipiFrameBufferHeight|2400
  gQcomTokenSpaceGuid.PcdMipiFrameBufferDelay|0

  gRenegadePkgTokenSpaceGuid.PcdDeviceVendor|"Xiaomi"
  gRenegadePkgTokenSpaceGuid.PcdDeviceProduct|"POCO F4 / Redmi K40S"
  gRenegadePkgTokenSpaceGuid.PcdDeviceCodeName|"munch"
