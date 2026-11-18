/** @file
  USB controller bring-up for Xiaomi munch (SM8250), step 1: PROBE ONLY.

  Qualcomm's UsbConfigDxe never dispatches on this platform, so nothing
  prepares the primary DWC3 controller (usb30_prim) before Windows touches it.
  This version only READS the clock-controller state and prints it, so we can
  see what the bootloader left running before writing any enable code.

  Register offsets are from the Linux gcc-sm8250 driver (GCC base 0x100000).
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define GCC_BASE                     0x00100000U

#define GCC_USB30_PRIM_BCR           0x0F000U
#define GCC_USB30_PRIM_GDSCR         0x0F004U
#define GCC_USB30_PRIM_MASTER_CBCR   0x0F010U
#define GCC_USB30_PRIM_SLEEP_CBCR    0x0F018U
#define GCC_USB30_PRIM_MOCK_UTMI_CBCR 0x0F01CU
#define GCC_USB30_PRIM_MASTER_RCG    0x0F020U
#define GCC_USB30_PRIM_MOCK_UTMI_RCG 0x0F038U
#define GCC_USB30_PRIM_MOCK_UTMI_DIV 0x0F050U
#define GCC_USB3_PRIM_PHY_AUX_CBCR   0x0F054U
#define GCC_USB3_PRIM_PHY_COM_AUX_CBCR 0x0F058U
#define GCC_USB3_PRIM_PHY_PIPE_CBCR  0x0F05CU
#define GCC_USB3_PRIM_PHY_AUX_RCG    0x0F064U
#define GCC_CFG_NOC_USB3_PRIM_AXI_CBCR 0x0F07CU
#define GCC_AGGRE_USB3_PRIM_AXI_CBCR 0x0F080U
#define GCC_QUSB2PHY_PRIM_BCR        0x12000U
#define GCC_USB3_PHY_PRIM_BCR        0x50000U

STATIC
UINT32
GccRead (
  IN UINT32  Offset
  )
{
  return MmioRead32 (GCC_BASE + Offset);
}

STATIC
VOID
ShowClock (
  IN CONST CHAR16  *Name,
  IN UINT32        Offset
  )
{
  UINT32 Value;

  Value = GccRead (Offset);
  // CBCR: bit 31 CLK_OFF (1 = off), bit 0 CLK_ENABLE.
  Print (L"  %-22s %08x  %s\r\n", Name, Value,
         ((Value & BIT31) != 0) ? L"OFF" : L"on");
}

STATIC
VOID
ShowRcg (
  IN CONST CHAR16  *Name,
  IN UINT32        Offset
  )
{
  // RCG: CMD bit 31 ROOT_OFF, CFG at +4 holds source/divider.
  Print (L"  %-22s cmd=%08x cfg=%08x\r\n", Name,
         GccRead (Offset), GccRead (Offset + 4));
}

STATIC
VOID
EFIAPI
MunchUsbReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32 Gdsc;

  gBS->CloseEvent (Event);

  Gdsc = GccRead (GCC_USB30_PRIM_GDSCR);
  Print (L"\r\nMunchUsb probe (GCC @ 0x100000)\r\n");
  // GDSCR: bit 31 PWR_ON status, bit 0 SW_COLLAPSE (1 = collapsed/off).
  Print (L"  usb30_prim_gdsc        %08x  %s\r\n", Gdsc,
         ((Gdsc & BIT31) != 0) ? L"POWER ON" : L"power off");
  Print (L"  usb30_prim_bcr         %08x  (bit0 = reset asserted)\r\n",
         GccRead (GCC_USB30_PRIM_BCR));
  Print (L"  qusb2phy_prim_bcr      %08x\r\n", GccRead (GCC_QUSB2PHY_PRIM_BCR));
  Print (L"  usb3_phy_prim_bcr      %08x\r\n", GccRead (GCC_USB3_PHY_PRIM_BCR));

  ShowClock (L"master_clk",       GCC_USB30_PRIM_MASTER_CBCR);
  ShowClock (L"sleep_clk",        GCC_USB30_PRIM_SLEEP_CBCR);
  ShowClock (L"mock_utmi_clk",    GCC_USB30_PRIM_MOCK_UTMI_CBCR);
  ShowClock (L"cfg_noc_axi_clk",  GCC_CFG_NOC_USB3_PRIM_AXI_CBCR);
  ShowClock (L"aggre_axi_clk",    GCC_AGGRE_USB3_PRIM_AXI_CBCR);
  ShowClock (L"phy_aux_clk",      GCC_USB3_PRIM_PHY_AUX_CBCR);
  ShowClock (L"phy_com_aux_clk",  GCC_USB3_PRIM_PHY_COM_AUX_CBCR);
  ShowClock (L"phy_pipe_clk",     GCC_USB3_PRIM_PHY_PIPE_CBCR);
  ShowRcg   (L"master_clk_src",   GCC_USB30_PRIM_MASTER_RCG);
  ShowRcg   (L"mock_utmi_src",    GCC_USB30_PRIM_MOCK_UTMI_RCG);
  ShowRcg   (L"phy_aux_src",      GCC_USB3_PRIM_PHY_AUX_RCG);
  Print (L"  mock_utmi_postdiv      %08x\r\n", GccRead (GCC_USB30_PRIM_MOCK_UTMI_DIV));

  // Give time to read or photograph the screen.
  MicroSecondDelay (6000000);
}

EFI_STATUS
EFIAPI
MunchUsbDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT Event;

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                MunchUsbReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &Event
                );
}
