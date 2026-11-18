/** @file
  USB controller bring-up for Xiaomi munch (SM8250), step 2: probe, enable the UTMI clock, switch the core to host mode.

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

// DWC3 core, usb30_prim (see Linux drivers/usb/dwc3/core.h)
#define USB_BASE                     0x0A600000U
#define DWC3_GCTL                    0xC110U
#define DWC3_GSTS                    0xC118U
#define DWC3_GSNPSID                 0xC120U
#define DWC3_GUSB2PHYCFG             0xC200U
#define DWC3_GUSB3PIPECTL            0xC2C0U
#define DWC3_DCFG                    0xC700U
#define DWC3_DCTL                    0xC704U
#define DWC3_DSTS                    0xC70CU
#define DWC3_GCTL_PRTCAPDIR_MASK     (BIT12 | BIT13)
#define DWC3_GCTL_PRTCAP_HOST        (1U << 12)
#define DWC3_DCTL_RUN_STOP           BIT31
#define DWC3_DSTS_DEVCTRLHLT         BIT22

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
UINT32
UsbRead (
  IN UINT32  Offset
  )
{
  return MmioRead32 (USB_BASE + Offset);
}

STATIC
VOID
ShowDwc3 (
  IN CONST CHAR16  *Title
  )
{
  Print (L"%s\r\n", Title);
  Print (L"  GSNPSID %08x  GCTL %08x  GSTS %08x (CURMOD %u)\r\n",
         UsbRead (DWC3_GSNPSID), UsbRead (DWC3_GCTL), UsbRead (DWC3_GSTS),
         UsbRead (DWC3_GSTS) & 3);
  Print (L"  GUSB2PHYCFG %08x  GUSB3PIPECTL %08x\r\n",
         UsbRead (DWC3_GUSB2PHYCFG), UsbRead (DWC3_GUSB3PIPECTL));
  Print (L"  DCFG %08x  DCTL %08x  DSTS %08x\r\n",
         UsbRead (DWC3_DCFG), UsbRead (DWC3_DCTL), UsbRead (DWC3_DSTS));
}

STATIC
BOOLEAN
WaitClockOn (
  IN UINT32  CbcrOffset
  )
{
  UINT32 Retry;

  for (Retry = 0; Retry < 1000; Retry++) {
    if ((GccRead (CbcrOffset) & BIT31) == 0) {
      return TRUE;
    }
    MicroSecondDelay (10);
  }

  return FALSE;
}

STATIC
VOID
SwitchToHost (
  VOID
  )
{
  UINT32 Value;
  UINT32 Retry;

  ShowDwc3 (L"Before:");

  // 1. mock_utmi clock: the RCG is already set to XO (cfg 0x100000); the
  //    root turns on when the branch is enabled.
  Print (L"Enabling mock_utmi_clk... ");
  MmioWrite32 (GCC_BASE + GCC_USB30_PRIM_MOCK_UTMI_CBCR,
               GccRead (GCC_USB30_PRIM_MOCK_UTMI_CBCR) | BIT0);
  Print (L"%s (cbcr %08x)\r\n", WaitClockOn (GCC_USB30_PRIM_MOCK_UTMI_CBCR) ? L"on" : L"TIMEOUT",
         GccRead (GCC_USB30_PRIM_MOCK_UTMI_CBCR));

  // 2. If the bootloader left the device controller running, stop it.
  if ((UsbRead (DWC3_DCTL) & DWC3_DCTL_RUN_STOP) != 0) {
    Print (L"Stopping device controller... ");
    MmioWrite32 (USB_BASE + DWC3_DCTL, UsbRead (DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
    for (Retry = 0; Retry < 1000; Retry++) {
      if ((UsbRead (DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) != 0) {
        break;
      }
      MicroSecondDelay (10);
    }
    Print (L"%s (dsts %08x)\r\n", (Retry < 1000) ? L"halted" : L"TIMEOUT", UsbRead (DWC3_DSTS));
  }

  // 3. Select host mode.
  Print (L"Selecting host mode... ");
  Value = UsbRead (DWC3_GCTL);
  Value = (Value & ~DWC3_GCTL_PRTCAPDIR_MASK) | DWC3_GCTL_PRTCAP_HOST;
  MmioWrite32 (USB_BASE + DWC3_GCTL, Value);
  MicroSecondDelay (1000);
  Print (L"gctl %08x gsts %08x\r\n", UsbRead (DWC3_GCTL), UsbRead (DWC3_GSTS));

  ShowDwc3 (L"After:");

  // 4. xHCI capability registers are only meaningful in host mode.
  Print (L"xHCI CAPLENGTH/HCIVERSION: ");
  Value = MmioRead32 (USB_BASE + 0x00);
  Print (L"%08x\r\n", Value);
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

  SwitchToHost ();

  // Give time to read or photograph the screen.
  MicroSecondDelay (8000000);
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
