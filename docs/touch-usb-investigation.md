# Touch and USB bring-up investigation (munch)

## Findings

1. **`munch` is a copy of `alioth` (Poco F3).**
   - `Platform/Xiaomi/sm8250/FdtBlob_compat/munch.dtb` has the same md5 as `alioth.dtb`; its model string is `xiaomi alioth`.
   - `AcpiTables/munch/DSDT.aml` has the same md5 as `alioth/DSDT.aml`.
   - Touch IC, GPIO/I2C assignment and USB-C/PD wiring are board-specific, so none of that data can be trusted for munch.
2. **The DSDT is almost empty.** `dsdt_common.asl` includes only `ufs.asl` and `Pep_lpi.asl`. Commented out: `usb.asl`, `buses.asl` (QUP I2C/SPI), `qcgpio.asl` (TLMM), `spmi.asl`, `pmic_*`, `pep.asl`, `graphics.asl`, `cust_touch.asl`, `cust_arraybutton.asl`, `wcnss_bt.asl`. Windows only loads drivers for devices the DSDT declares, so **no USB controller and no touch device exist for Windows**. This is the root cause for both symptoms.
3. **UEFI side already has a USB host stack in the image** (`XhciPciEmulation`, `XhciDxe`, `UsbConfigDxe`, `UsbPwrCtrlDxe`, `UsbfnDwc3Dxe`, plus `UsbBusDxe`/`UsbKbDxe`/`UsbMouseDxe`). These binaries come from the `Platform/EFI_Binaries` submodule, which is not checked out in this working tree, so I could not inspect them. UEFI-level USB keyboard may or may not work; test it separately from Windows.
4. The memory map (`PlatformMemoryMapLib.c`) already maps `USB30_PRIM` (0x0A600000) and `USB30_SEC` (0x0A800000), plus QUPV3 GSI and TLMM regions, so those MMIO ranges are reachable.
5. No `ButtonsDxe` is wired into `munch.fdf.inc` (alioth has one), so volume-key behaviour is unverified.

## Recommendation: do USB first

USB is the better first target: it needs fewer board-specific facts and it unlocks a USB keyboard/mouse (and a USB Ethernet dongle), which makes the device usable and makes every later step easier to debug. Touch needs the exact touch IC, I2C/SPI bus, IRQ/reset GPIOs, regulators and firmware, plus a Windows driver that supports that IC.

### USB plan
1. Check out submodules (`git submodule update --init Platform/EFI_Binaries`) and confirm the SM8250 USB binaries exist.
2. Add `Include("usb.asl")` to `Dsdt.asl`. The only `usb.asl` in the tree is `Platform/Google/msm8998/AcpiTables/usb.asl` (a different SoC); it must be rewritten for SM8250: DWC3 `USB30_PRIM` at `0x0A600000` (size `0x11B000`), its SPI interrupts, and host-only mode. Take addresses and IRQs from the DT / downstream `kona.dtsi` for sm8250.
3. Add the supporting nodes it depends on (`qcgpio.asl`, `pep.asl`, and the PMIC/SPMI bits for VBUS and Type-C role) or tell Windows the controller is already powered by UEFI.
4. Build with `iasl`, regenerate `DSDT.aml` (`USE_IASL=true` in `configs/devices/munch.conf`), flash, and check Device Manager / `devmgmt` for an unknown or errored USB controller. Use a powered OTG hub and a keyboard.
5. If UEFI shows a USB keyboard in the boot menu but Windows does not, the problem is only ACPI; if neither works, check `UsbConfigDxe` / PMIC role switching (the closed `PmicDxe` is currently disabled in `Apriori.fdf.inc` and `dxe.fdf.inc`, which may leave the Type-C port without VBUS/role setup).

### Touch plan
1. Identify the touch IC and wiring on munch from a real source: the stock Android DTB (`/sys/firmware/devicetree` on the phone or from `boot`/`dtbo`) and `dmesg | grep -i touch`.
2. Port a matching `cust_touch.asl` and `buses.asl` (QUP I2C or SPI) from a similar device; SDM845 examples exist in-tree (`Platform/LG/sdm845/.../cust_touch.asl`) but are for other ICs.
3. Make sure a Windows driver exists for that IC (stock Goodix/FocalTech/Novatek drivers are not generally available for ARM64 Windows).

## Open decisions for you
- Do you have a dump of the stock munch DTB (or access to the phone to pull it)? That unblocks correct USB/touch/GPIO data and also fixes the copied `munch.dtb`.
- Is the serial/UART or a USB-less debugging method available? Without it, ACPI trial and error needs a full flash cycle each time.

## USB investigation results (binary analysis of `edk2-msm-binary` @ d097b8a)

Method: decoded the `.depex` files and `strings` of the SM8250 USB binaries. No device testing was done, so items marked *hypothesis* are unconfirmed.

### UEFI-side dependency chain
`XhciPciEmulation` → `UsbConfigDxe` → `UsbPwrCtrlDxe` → PMIC protocols from `PmicDxe`.

- `XhciPciEmulation` logs "Unable to open USB Config Protocol" and "Unable to configure USB in host mode", so it needs `UsbConfigDxe`.
- `UsbConfigDxe` logs "Failed to open Power Control USB protocol" and "Failed to call PMIC ChargerPort Status". Its depex includes GUID `b0760469-970c-487a-a4b5-28db7b45cef1`, which also appears in the `PmicDxe` binary (and `ChipInfo`).
- `PmicDxe` contains the SCHG charger code (`EFI_PmicSchgGetChargerPortType`, `pm_schg_driver.c`), i.e. the Type-C/charger-port state that USB config reads.
- **`PmicDxe` is commented out** in both `Platform/Qualcomm/sm8250/Apriori.fdf.inc` and `dxe.fdf.inc` (commit `cb9bf8f`). *Hypothesis:* with it removed, `UsbConfigDxe` never dispatches, so UEFI has no USB host at all. (I couldn't confirm which binary *installs* that GUID.)

### Why PmicDxe was disabled
Commits `5d54d77`, `b14d952` and `cb9bf8f` chased an `ASSERT DebugLib.c +78` crash by swapping the DebugLib to `BaseDebugLibNull` and then removing `PmicDxe`. *Hypothesis:* the assert string is baked into Qualcomm's closed binary (its own DebugLib is statically linked, with a Samsung build path in the strings), so changing the EDK2 DebugLib could not have affected it. The root cause of the crash is unknown.

### ACPI side (Windows)
Independent of the above: Windows needs a USB controller in the DSDT. Relevant MMIO is `USB30_PRIM` 0x0A600000 / `USB30_SEC` 0x0A800000, both already in the memory map.

### Next experiments, in order
1. Build a test image with `PmicDxe` re-enabled and `munch.dtb`/memory map unchanged. Check serial/framebuffer for the crash and whether a USB keyboard works in the UEFI menu. This separates the UEFI problem from the Windows problem.
2. Take the SM8250 USB node from the real kernel DT (needs the stock munch DTB) and write `usb.asl` for the primary controller, host mode only.
3. Add `usb.asl` to `munch/Dsdt.asl`, rebuild with `iasl`, test in Windows with a powered OTG hub.
4. Check whether the phone reports Type-C role/VBUS correctly. A bad role switch looks the same as missing ACPI.

### Environment note
`iasl`, `dtc` and a toolchain aren't installed on this Mac, so ACPI/DTB edits must be built in CI or a Linux box.

## Boot menu (SimpleInit) — uncommitted change set
- `ButtonsDxe` (volume/power keys) depends on `PmicGpioProtocol`/`PmicPONProtocol`, both from `PmicDxe`. With `PmicDxe` disabled, keys cannot work either. Same root cause as USB.
- Re-enabled: `PmicDxe` (Apriori + dxe), `SimpleInitMain` + `KernelFdtDxe` in `sm8250.fdf`; Windows boot option is now appended after SimpleInit so the menu comes first.
- "Reboot to Android" needs the installer-zip build (`ENABLE_LINUX_UTILS=1`), which CI already produces.
- Untested: build not run locally (no toolchain), nothing flashed. If `PmicDxe` crashes again (the old `DebugLib.c +78` assert), keys and menu navigation will not work.
