/** @file
  Touchscreen bring-up for Xiaomi munch (SM8250), step 1: PROBE ONLY (read-only).

  The touch chip is a FocalTech on SPI (QUP wrapper 0, serial engine 4 at 0x990000,
  pins GPIO 119-122), reset GPIO 33, interrupt GPIO 39.  This probe prints what the
  bootloader left behind (clocks, pin configuration) so we know what must be set up.

  Only the clock controller and pin controller are read.  The SPI engine registers
  are read only if all of its clocks are already running.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define GCC_BASE              0x00100000U
#define GCC_QUP_VOTE_REG      0x52008U       // bit6 wrap0 M-AHB, bit7 S-AHB, bit14 SE4 clock
#define GCC_WRAP0_M_AHB_HALT  0x17004U
#define GCC_WRAP0_S_AHB_HALT  0x17008U
#define GCC_WRAP0_S4_HALT     0x174CCU
#define GCC_WRAP0_S4_RCG      0x174D0U

// TLMM tiles (sm8250.dtsi): west 0xF100000, south 0xF500000, north 0xF900000.
#define TLMM_WEST             0x0F100000U
#define TLMM_SOUTH            0x0F500000U
#define TLMM_NORTH            0x0F900000U
#define TLMM_PIN_STRIDE       0x1000U

#define SE4_BASE              0x00990000U

STATIC
VOID
ShowPin (
  IN UINT32        TileBase,
  IN UINT32        Gpio,
  IN CONST CHAR16  *Role
  )
{
  UINT32 Ctl;
  UINT32 Io;

  Ctl = MmioRead32 (TileBase + TLMM_PIN_STRIDE * Gpio);
  Io  = MmioRead32 (TileBase + TLMM_PIN_STRIDE * Gpio + 4);
  // ctl: pull [1:0] (0 none, 1 down, 3 up), function [5:2], drive [8:6], output enable bit 9.
  Print (L"  gpio%-3u %-9s ctl %08x func %u pull %u drive %u oe %u | io %08x (in %u out %u)\r\n",
         Gpio, Role, Ctl, (Ctl >> 2) & 0xF, Ctl & 3, (Ctl >> 6) & 7, (Ctl >> 9) & 1,
         Io, Io & 1, (Io >> 1) & 1);
}

STATIC
VOID
EFIAPI
MunchTouchReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT32  Vote;
  UINT32  MAhb;
  UINT32  SAhb;
  UINT32  S4;
  BOOLEAN ClocksOn;

  gBS->CloseEvent (Event);

  Vote = MmioRead32 (GCC_BASE + GCC_QUP_VOTE_REG);
  MAhb = MmioRead32 (GCC_BASE + GCC_WRAP0_M_AHB_HALT);
  SAhb = MmioRead32 (GCC_BASE + GCC_WRAP0_S_AHB_HALT);
  S4   = MmioRead32 (GCC_BASE + GCC_WRAP0_S4_HALT);

  Print (L"\r\nMunchTouch probe\r\n");
  Print (L"  QUP vote reg %08x (m_ahb %u s_ahb %u se4 %u)\r\n", Vote,
         (Vote >> 6) & 1, (Vote >> 7) & 1, (Vote >> 14) & 1);
  Print (L"  wrap0 m_ahb %08x %s  s_ahb %08x %s  se4 %08x %s\r\n",
         MAhb, ((MAhb & BIT31) != 0) ? L"OFF" : L"on",
         SAhb, ((SAhb & BIT31) != 0) ? L"OFF" : L"on",
         S4,   ((S4 & BIT31) != 0)   ? L"OFF" : L"on");
  Print (L"  se4 rcg cmd %08x cfg %08x\r\n",
         MmioRead32 (GCC_BASE + GCC_WRAP0_S4_RCG),
         MmioRead32 (GCC_BASE + GCC_WRAP0_S4_RCG + 4));

  ShowPin (TLMM_NORTH, 119, L"spi4");
  ShowPin (TLMM_NORTH, 120, L"spi4");
  ShowPin (TLMM_NORTH, 121, L"spi4");
  ShowPin (TLMM_NORTH, 122, L"spi4");
  ShowPin (TLMM_SOUTH, 33,  L"ts-reset");
  ShowPin (TLMM_SOUTH, 39,  L"ts-irq");
  ShowPin (TLMM_SOUTH, 38,  L"ts-rel");
  ShowPin (TLMM_WEST,  140, L"ts-act");

  ClocksOn = ((MAhb | SAhb | S4) & BIT31) == 0;
  if (ClocksOn) {
    // GENI serial engine: FW revision at +0x68, HW version at +0x60 (read only).
    Print (L"  se4 regs: hw %08x fwrev %08x status %08x\r\n",
           MmioRead32 (SE4_BASE + 0x60), MmioRead32 (SE4_BASE + 0x68),
           MmioRead32 (SE4_BASE + 0x40));
  } else {
    Print (L"  se4 clocks not all on: SPI engine registers NOT read (safe)\r\n");
  }

  MicroSecondDelay (6000000);
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
