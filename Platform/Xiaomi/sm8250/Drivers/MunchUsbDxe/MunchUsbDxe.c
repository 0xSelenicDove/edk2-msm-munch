/** @file
  USB host bring-up for Xiaomi munch (SM8250): primary DWC3 controller (usb30_prim).

  Qualcomm's UsbConfigDxe never dispatches on this platform (it waits for a protocol
  nothing provides), so nothing prepares the controller before the OS touches it.
  What the bootloader leaves behind (measured with this driver's probe):
    - usb30_prim GDSC on, resets released, master/sleep/AXI/PHY-aux clocks running
    - mock_utmi_clk off
    - the DWC3 core in device mode

  Before the OS starts, this driver:
    1. enables mock_utmi_clk (the controller's UTMI clock),
    2. selects UTMI as the PIPE clock (no USB 3.0 PHY is running; the controller is
       used high-speed only, same as dwc3_qcom_select_utmi_clk() in Linux),
    3. stops the device controller if it is running, and
    4. switches the core to host mode.

  The OS then drives the controller as a plain xHCI (ACPI PNP0D10).

  Register offsets: Linux drivers/clk/qcom/gcc-sm8250.c, drivers/usb/dwc3/core.h and
  dwc3-qcom.c.  Define MUNCH_USB_VERBOSE for a register dump on screen.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define GCC_BASE                       0x00100000U
#define GCC_USB30_PRIM_BCR             0x0F000U
#define GCC_USB30_PRIM_GDSCR           0x0F004U
#define GCC_USB30_PRIM_MOCK_UTMI_CBCR  0x0F01CU

#define USB_BASE                       0x0A600000U
#define DWC3_GCTL                      0xC110U
#define DWC3_GSTS                      0xC118U
#define DWC3_DCTL                      0xC704U
#define DWC3_DSTS                      0xC70CU
#define DWC3_GCTL_PRTCAPDIR_MASK       (BIT12 | BIT13)
#define DWC3_GCTL_PRTCAP_HOST          (1U << 12)
#define DWC3_DCTL_RUN_STOP             BIT31
#define DWC3_DSTS_DEVCTRLHLT           BIT22

// Qualcomm glue ("QSCRATCH") registers.
#define QSCRATCH_BASE                  (USB_BASE + 0xF8800U)
#define QSCRATCH_GENERAL_CFG           0x08U
#define PIPE_UTMI_CLK_SEL              BIT0
#define PIPE3_PHYSTATUS_SW             BIT3
#define PIPE_UTMI_CLK_DIS              BIT8

STATIC
BOOLEAN
WaitClockOn (
  IN UINT32  CbcrOffset
  )
{
  UINT32 Retry;

  for (Retry = 0; Retry < 1000; Retry++) {
    if ((MmioRead32 (GCC_BASE + CbcrOffset) & BIT31) == 0) {
      return TRUE;
    }
    MicroSecondDelay (10);
  }

  return FALSE;
}

// State of the controller as found (diagnostic, printed once).
STATIC UINT32 mPreGctl;
STATIC UINT32 mPreCmd;
STATIC UINT32 mPreSts;
STATIC UINT32 mPrePort;
STATIC UINT32 mPostPort;
STATIC BOOLEAN mWasReset;

STATIC
EFI_STATUS
EnableUsbHost (
  OUT UINT32  *XhciCaps
  )
{
  UINT32 Value;
  UINT32 Retry;
  UINT32 Ops;

  // The power domain must already be on (the bootloader leaves it on); never touch
  // the controller otherwise, an access to a powered-off block hangs the system.
  if ((MmioRead32 (GCC_BASE + GCC_USB30_PRIM_GDSCR) & BIT31) == 0) {
    return EFI_NOT_READY;
  }

  // 0. After a restart the controller still holds the previous OS's host-mode state
  //    (a fresh boot from the bootloader finds it in device mode instead).  Reset the
  //    whole block so the OS driver starts from a clean controller.
  mPreGctl = MmioRead32 (USB_BASE + DWC3_GCTL);
  if ((MmioRead32 (USB_BASE + DWC3_GSTS) & 3) == 1) {
    Ops       = USB_BASE + (MmioRead32 (USB_BASE) & 0xFFU);
    mPreCmd   = MmioRead32 (Ops + 0x00);
    mPreSts   = MmioRead32 (Ops + 0x04);
    mPrePort  = MmioRead32 (Ops + 0x400);
    mWasReset = TRUE;
    MmioOr32 (GCC_BASE + GCC_USB30_PRIM_BCR, BIT0);
    MicroSecondDelay (200);
    MmioAnd32 (GCC_BASE + GCC_USB30_PRIM_BCR, ~(UINT32)BIT0);
    MicroSecondDelay (2000);
  }

  // 1. UTMI clock.
  MmioOr32 (GCC_BASE + GCC_USB30_PRIM_MOCK_UTMI_CBCR, BIT0);
  if (!WaitClockOn (GCC_USB30_PRIM_MOCK_UTMI_CBCR)) {
    return EFI_TIMEOUT;
  }

  // 2. Use the UTMI clock as the PIPE clock.
  MmioOr32 (QSCRATCH_BASE + QSCRATCH_GENERAL_CFG, PIPE_UTMI_CLK_DIS);
  MicroSecondDelay (500);
  MmioOr32 (QSCRATCH_BASE + QSCRATCH_GENERAL_CFG, PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
  MicroSecondDelay (500);
  MmioAnd32 (QSCRATCH_BASE + QSCRATCH_GENERAL_CFG, ~(UINT32)PIPE_UTMI_CLK_DIS);

  // 3. Stop the device controller if the bootloader left it running.
  if ((MmioRead32 (USB_BASE + DWC3_DCTL) & DWC3_DCTL_RUN_STOP) != 0) {
    MmioAnd32 (USB_BASE + DWC3_DCTL, ~(UINT32)DWC3_DCTL_RUN_STOP);
    for (Retry = 0; Retry < 1000; Retry++) {
      if ((MmioRead32 (USB_BASE + DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) != 0) {
        break;
      }
      MicroSecondDelay (10);
    }
  }

  // 4. Host mode.
  Value = MmioRead32 (USB_BASE + DWC3_GCTL);
  Value = (Value & ~DWC3_GCTL_PRTCAPDIR_MASK) | DWC3_GCTL_PRTCAP_HOST;
  MmioWrite32 (USB_BASE + DWC3_GCTL, Value);
  MicroSecondDelay (1000);

  if ((MmioRead32 (USB_BASE + DWC3_GSTS) & 3) != 1) {
    return EFI_DEVICE_ERROR;
  }

  // xHCI CAPLENGTH (low byte) / HCIVERSION (high half): only valid in host mode.
  *XhciCaps = MmioRead32 (USB_BASE + 0x00);
  mPostPort = MmioRead32 (USB_BASE + (*XhciCaps & 0xFFU) + 0x400);
  return ((*XhciCaps >> 16) >= 0x0100) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
EFIAPI
MunchUsbReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS Status;
  UINT32     Caps;

  gBS->CloseEvent (Event);

  Caps   = 0;
  Status = EnableUsbHost (&Caps);
  Print (L"USB host: %r", Status);
  if (!EFI_ERROR (Status)) {
    Print (L" (xHCI %x.%02x)", (Caps >> 24) & 0xFF, (Caps >> 16) & 0xFF);
  }
  Print (L"\r\n");
  if (mWasReset) {
    Print (L"USB reset: gctl %08x cmd %08x sts %08x port %08x -> port %08x\r\n",
           mPreGctl, mPreCmd, mPreSts, mPrePort, mPostPort);
  }
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
