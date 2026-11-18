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
#define GCC_QUSB2PHY_PRIM_BCR          0x12000U
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

// Synopsys femto HS PHY (usb2_phy0 in the device tree).
#define HSPHY_BASE                     0x088E3000U
#define PHY_UTMI_CTRL0                 0x3CU
#define PHY_UTMI_CTRL5                 0x50U
#define PHY_CTRL_COMMON0               0x54U
#define PHY_CTRL_COMMON1               0x58U
#define PHY_CTRL_COMMON2               0x5CU
#define PHY_CTRL1                      0x60U
#define PHY_CTRL2                      0x64U
#define PHY_OVERRIDE_X0                0x6CU
#define PHY_OVERRIDE_X1                0x70U
#define PHY_OVERRIDE_X2                0x74U
#define PHY_CFG0                       0x94U
#define PHY_REFCLK_CTRL                0xA0U

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

STATIC UINT8 mPhyBefore[9];

STATIC
VOID
PhyMask (
  IN UINT32  Offset,
  IN UINT32  Mask,
  IN UINT32  Value
  )
{
  UINT32 V;

  V = MmioRead32 (HSPHY_BASE + Offset);
  V = (V & ~Mask) | (Value & Mask);
  MmioWrite32 (HSPHY_BASE + Offset, V);
  MmioRead32 (HSPHY_BASE + Offset);
}

/**
  Initialise the high-speed PHY the way Linux (phy-qcom-snps-femto-v2) does, with the
  tuning values from the device tree.  The bootloader only does this for fastboot, so
  after a normal boot the PHY is unconfigured and the OS gets "port reset failed".
**/
STATIC
VOID
InitHsPhy (
  VOID
  )
{
  // State as found (diagnostic).
  mPhyBefore[0] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_CTRL_COMMON0);
  mPhyBefore[1] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_UTMI_CTRL5);
  mPhyBefore[2] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_CFG0);
  mPhyBefore[3] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_OVERRIDE_X0);
  mPhyBefore[4] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_OVERRIDE_X1);
  mPhyBefore[5] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_OVERRIDE_X2);
  mPhyBefore[6] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_REFCLK_CTRL);
  mPhyBefore[7] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_CTRL2);
  mPhyBefore[8] = (UINT8)MmioRead32 (HSPHY_BASE + PHY_UTMI_CTRL0);

  // PHY reset pulse (GCC_QUSB2PHY_PRIM_BCR).
  MmioOr32 (GCC_BASE + GCC_QUSB2PHY_PRIM_BCR, BIT0);
  MicroSecondDelay (150);
  MmioAnd32 (GCC_BASE + GCC_QUSB2PHY_PRIM_BCR, ~(UINT32)BIT0);
  MicroSecondDelay (150);

  PhyMask (PHY_CFG0, BIT1, BIT1);                    // UTMI_PHY_CMN_CTRL_OVERRIDE_EN
  PhyMask (PHY_UTMI_CTRL5, BIT1, BIT1);              // POR
  PhyMask (PHY_CTRL_COMMON0, 0x70, 0);               // FSEL: 19.2 MHz reference
  PhyMask (PHY_CTRL_COMMON1, BIT5, BIT5);            // PLLBTUNE
  PhyMask (PHY_REFCLK_CTRL, 0x3, 0x2);               // REFCLK_SEL default
  PhyMask (PHY_CTRL_COMMON1, BIT4, BIT4);            // VBUSVLDEXTSEL0
  PhyMask (PHY_CTRL1, BIT0, BIT0);                   // VBUSVLDEXT0

  // Tuning from the device tree (qcom,param-override-seq).
  MmioWrite32 (HSPHY_BASE + PHY_OVERRIDE_X0, 0xE7);
  MmioWrite32 (HSPHY_BASE + PHY_OVERRIDE_X1, 0xC9);
  MmioWrite32 (HSPHY_BASE + PHY_OVERRIDE_X2, 0x21);

  PhyMask (PHY_CTRL_COMMON2, BIT0, BIT0);            // VREGBYPASS
  PhyMask (PHY_CTRL2, BIT3 | BIT2, BIT3 | BIT2);     // SUSPEND_N_SEL | SUSPEND_N
  PhyMask (PHY_UTMI_CTRL0, BIT0, BIT0);              // SLEEPM
  PhyMask (PHY_CTRL_COMMON0, BIT2, 0);               // SIDDQ off (power up)
  PhyMask (PHY_UTMI_CTRL5, BIT1, 0);                 // release POR
  PhyMask (PHY_CTRL2, BIT3, 0);                      // SUSPEND_N_SEL off
  PhyMask (PHY_CFG0, BIT1, 0);                       // release the override
}

STATIC
EFI_STATUS
EnableUsbHost (
  OUT UINT32  *XhciCaps
  )
{
  UINT32 Value;
  UINT32 Retry;

  // The power domain must already be on (the bootloader leaves it on); never touch
  // the controller otherwise, an access to a powered-off block hangs the system.
  if ((MmioRead32 (GCC_BASE + GCC_USB30_PRIM_GDSCR) & BIT31) == 0) {
    return EFI_NOT_READY;
  }

  // 0. High-speed PHY.
  InitHsPhy ();
  MicroSecondDelay (1000);

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
  Print (L"USB PHY was: %02x %02x %02x | %02x %02x %02x | %02x %02x %02x\r\n",
         mPhyBefore[0], mPhyBefore[1], mPhyBefore[2], mPhyBefore[3], mPhyBefore[4],
         mPhyBefore[5], mPhyBefore[6], mPhyBefore[7], mPhyBefore[8]);
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
