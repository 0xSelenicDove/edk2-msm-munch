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

## Boot menu (SimpleInit) — REVERTED (bootloops, see boot-test results below)
- `ButtonsDxe` (volume/power keys) depends on `PmicGpioProtocol`/`PmicPONProtocol`, both from `PmicDxe`. With `PmicDxe` disabled, keys cannot work either. Same root cause as USB.
- Re-enabled: `PmicDxe` (Apriori + dxe), `SimpleInitMain` + `KernelFdtDxe` in `sm8250.fdf`; Windows boot option is now appended after SimpleInit so the menu comes first.
- "Reboot to Android" needs the installer-zip build (`ENABLE_LINUX_UTILS=1`), which CI already produces.
- Untested: build not run locally (no toolchain), nothing flashed. If `PmicDxe` crashes again (the old `DebugLib.c +78` assert), keys and menu navigation will not work.

## Real munch hardware data (pulled from a live phone, 2026-11-17)
Source: `/sys/firmware/fdt` on a stock Android 14 / kernel 4.19.157 munch (`22021211RG`, hwc CN). `munch.dtb` now contains this real tree instead of the alioth copy (the model string is now `xiaomi munch`). The DSDT is still the alioth copy.

| Item | munch (live DT) | alioth (old copy) |
| :--- | :--- | :--- |
| Touch | FocalTech `focaltech,fts` on **SPI** `qupv3_se4_spi` (`spi@990000`), reset GPIO 33, IRQ GPIO 39, 10 points | focaltech on a different bus/pins |
| USB | `ssusb@a600000` (usb0) and `ssusb@a800000` (usb1), both `dr_mode = "drd"`, `qcom,dwc-usb3-msm`; PD PHY `qcom,usb-pdphy@1700` | same controllers, board wiring differs |
| Fuel gauge / charger | **external TI `bq27z561` gauge + `bq2597x` master/slave chargers** + `smb1390`; PM8150B `qcom,qpnp-smb5` and `fg-gen4` nodes also present | internal `fg-gen4`, `bq2597x-standalone` |
| RAM | 0x80000000 size 0x39900000, 0x200000000 size 0x180000000, 0xC0000000 size 0x140000000 (~12 GB) | 0x80000000 size 0x3bb00000, 0xC0000000 size 0x1c0000000 (~8 GB) |

### Implications
- **Touch:** Windows needs a SPI bus node (QUP SE4 at 0x990000) plus a GPIO controller, then the FocalTech device with those pins. It is SPI, not I2C, so I2C-based examples in the tree do not apply.
- **PmicDxe crash (hypothesis):** `PmicDxe` has fuel-gauge/battery-profile code (`PmicFgBattProfileTokenPtrArrCallBack`, "Reading Profile Match"). munch reads its battery through an external `bq27z561`, which is a plausible reason the PMIC battery path fails. Not verified.
- **Memory map:** `PlatformMemoryMapLib.c` is still alioth-shaped and ends at 8 GB. munch has ~12 GB laid out differently, which may relate to the earlier MEMORY_MANAGEMENT BSOD.

### Boot-test results (2026-11-17)
- The plain `boot-munch.img` (boot header v1, 2048-byte page) is rejected by the bootloader: `fastboot boot` gives "Load Error" for older builds, and flashing it falls back to fastboot. Stock boot is header v3 (4096-byte page). Use the installer ZIP, which patches the real boot image.
- With `PmicDxe` re-enabled the phone bootloops: on-screen "…back registration failed, Status = 0x8000000000000003" followed by `PmicDxe` `ASSERT DebugLib.c +78: Format != ((void *) 0)`.
- Stock `boot_a` is backed up (not in git); Android was restored.

## Result: USB host works (2026-11-17)
USB host now works in Windows (mouse verified). Summary of what mattered:
- Bootloader state (probed on screen): usb30_prim GDSC on, resets released, master/sleep/AXI/PHY-aux clocks on, `mock_utmi_clk` off, DWC3 in device mode (`CURMOD 0`), `phy_pipe_clk` off.
- Firmware `MunchUsbDxe`: enable `mock_utmi_clk` (GCC 0xF01C), QSCRATCH `GENERAL_CFG` UTMI-as-PIPE select, stop device controller, `GCTL.PRTCAPDIR = host`. xHCI then reads `HCIVERSION 0x0110`. GCC (0x100000) had to be added to the UEFI memory map.
- Windows: `USB0` as `PNP0D10` with inbox usbxhci. Tried and rejected: `QCOM25A6` + qcxhcifilter8250 (CLOCK_WATCHDOG), with or without the extra wake interrupts and the IORT; URS/Type-C path (needs PEP); real PEP0 (qcpep fails add).
- The earlier hangs were the controller still being in device mode, then the Qualcomm filter driver.

## USB mouse reliability (2026-11-18)
First builds had a flaky mouse: sometimes not detected after boot, needing replugs. Cause: `USB0` declared `_S0W = 3` (can wake from D3 while the system runs), so Windows idled the xHCI into D3, but the wake interrupts a sleeping controller needs (HS-PHY DP/DM wake, pwr_event) are not wired in `_CRS`. Fix: `_S0W = 0` (D0 only). Verified: the mouse stays connected and works after replugging.

## Shutdown BSOD (open)
Shutting down Windows ends in `INTERNAL_POWER_ERROR` (0xA0). One earlier log entry had parameters `0xBEEF, 0x8003, 0xC4000001` (PSCI CPU_SUSPEND), `0xFFFFFFFE` (INVALID_PARAMETERS), but disabling idle states C3, then C3+C4 (WFI-only) did not remove the BSOD, so the cause is not simply those idle states. `DisplayParameters = 1` is set in the registry; the four numbers appear at the top-left of the blue screen. The file system stays clean afterwards (the BSOD happens at the final power-off).

## Touch firmware bring-up (branch `feat/munch-touch`, not merged)

**Result (2026-11-18/19): proof of life reached.** `MunchTouchDxe` reads the FocalTech chip ID from firmware: `Touch chip ID: 56 52 (expect 56 52)`. The SPI bus, rails and framing (4-byte header, 3 dummy bytes, CRC16-kermit) are all confirmed. Windows still needs a FocalTech SPI touch driver (none in the image).

What was wrong, in the order it was found (each item came from filming the boot screen):

1. **`s_ahb` was never broken.** `gcc_qupv3_wrap_0_s_ahb_clk` (`0x17008`) has hardware clock gating (bit 1). With it on, the status bit 31 reads "off" while the branch is idle; Linux skips the halt check in that mode (`clk_branch_in_hwcg_mode`). Clearing bit 1 makes the status read on (`0x00000000`) and SE4 becomes accessible (`fw 00000101`, proto 1 = SPI, FIFO interface enabled). The RPMh bus-vote theory from the first investigation was wrong for this clock.
2. **The touch rails cannot be switched over SPMI.** The pm8150l LDOs are on **SPMI SID 5** (SID 4 has no PIDs `0x40-0x4F`; the APID map reports `70 51 106 24 63 53 125 0` APIDs per SID, arbiter `ver 50010000`, PIDs `0x540-0x54A` are owned by EE 4, not Apps). They are controlled by RPMh. Fix: an RPMh client in firmware.
3. **RPMh from firmware works.**
   * cmd-db at `0x80860000`: magic `db 30 03 0c` at +4, 8 resource headers from +8 (16 bytes each), data from `0x90`, entries 24 bytes (`id[8]`, `prio[2]`, `addr`, `len`, `offset`). `ldoc1` = `0x42D00`, `ldoc10` = `0x41200` (`ldoc10` +4 = enable).
   * APPS RSC DRV2 at `0x18220000`: id `0x00020000` (v2.x register layout), child cfg `0x800C8104` (8 TCS for DRV2), TCS base `0x18220000 + 0xD00`, stride 672 bytes. The bootloader leaves TCS0 busy (cmd_enable 1, control `0x01010000`), so TCS1 (control `0x00010000`, idle) is used.
   * A VRM vote is: voltage in mV at `addr+0`, enable `1` at `addr+4`, msgid `8 | RESP_REQ | WRITE`, trigger via CONTROL (AMC enable then trigger), completion = command status bit 16 (`0x00010101`). The RSC IRQ status bit is not reliable here; poll the command status. Votes: `ldoc1` 1800 mV, `ldoc10` 3200 mV. Verified by reading the LDO registers over SPMI (read-only observer): `vset 0708` / `0CF0`, enable `0x80`.
   * No BCM vote (QUP0/CN0) was needed for QUP; the fallback code for it is still in the driver but unused.
4. **SPI command opcode.** Full duplex is GENI opcode **3** (`TX_ONLY | RX_ONLY`). Opcode 7 is "TX then RX": it sends all bytes and only then clocks in the reply, which gave all-equal RX bytes (`0D 0D ...`) and a zero loopback. With opcode 3 the SE loopback returns `A5 3C 0F F0` and the chip answers.
5. SPI mode 0 works, 19.2 MHz / 4. Pins GPIO 8-11 function 1 (`qup4`) confirmed against the live DT and `pinctrl-sm8250.c`. Reset GPIO 33 is released 300 ms after the rails; the irq pin (GPIO 39) reads 1 afterwards.

Open points:
* The driver holds the boot screen 20 s and is a test build; it must not go into master as is (it prints on screen and stops the boot for 20 s). Next step for a real touch driver is on the Windows side (ACPI SPI node, FocalTech driver); the firmware only needs to power the rails, or Windows needs its own RPMh path (PEP) to do it.
* `L10` reads `vset 0CF0` (3.28 V) after the vote of 3200 mV (PMIC step rounding, not investigated).


## Windows touch driver: feasibility and plan (2026-11-19)

Facts that decide the design:

* **Test signing is on** in the BCD copy we have (`AllowPrereleaseSignatures` `16000049` = 1, `DisableIntegrityChecks` `16000048` = 1), so a test-signed driver loads.
* **PEP0 is the fake `PNP0D80` stub**, so Qualcomm's SPI/GPIO stack (`qcspi8250` `QCOM250F`, `qcgpio8250` `QCOM250D`) cannot get clocks, pins or regulators from it and is not a safe base. The inbox `hidspi_km` (`PNP0C51`) is no use either: the chip does not speak HID-over-SPI (FTS register protocol with CRC).
* **The firmware already prepares the hardware** and none of it is undone by Windows (no driver owns it): QUP clock votes (`GCC+0x52008`) with s_ahb hwcg off, GPIO 8-11 muxed to SE4, the two RPMh LDO votes, reset released, SE4 GENI initialised (SPI mode 0, 8 bit, 4.8 MHz). So a Windows driver only has to do register-level SPI transfers and decode touch frames.
* **Touch frame** (from the vendor `focaltech_core.c`): read register `0x01` (command byte `0xA0` = read + CRC, 2 length bytes, 3 dummy bytes), 63 data bytes: byte 2 low nibble = point count, then 6 bytes per point: `[3]` bits 7:6 event (0 down, 1 up, 2 contact), `[3]` low nibble and `[4]` = X high bits, `[5]` high nibble = id, low nibble and `[6]` = Y, `[7]`/`[8]` = precision and area. X = `((b3 & 0x0F) << 11) + (b4 << 3) + ((b7 & 0xE0) >> 5)`, Y likewise with `b5`, `b6`, `(b7 & 0x1C) >> 2`; ids >= 10 mean no point. Coordinate range 0..10800 x 0..24000 (DT `display-coords` `0x2a30` / `0x5dc0`), 10 points. The INT pin (GPIO 39, active low) signals a frame.
* One frame is 72 bytes with the framing, more than the SE FIFO (16 words, 64 bytes), so the driver must refill TX and drain RX during the transfer (or read fewer points).

Proposed design (not started):

1. **Firmware** (`MunchTouchDxe`): keep the bring-up, drop the on-screen text and the 20 s hold (quiet mode), keep the RPMh VRM votes. Add an ACPI device `MFTS0001` to the munch DSDT with two memory resources: SE4 (`0x990000`, `0x4000`) and the south TLMM page holding GPIO 39 (`0x0F527000`, `0x1000`, level in the `io` register at +4).
2. **Windows driver** (KMDF, `munchfts.sys`): map the two resources, poll GPIO 39 every ~8 ms (a GPIO interrupt would need the Qualcomm GPIO driver), do the GENI full-duplex transfer (opcode 3) with FIFO refill, CRC-check, decode, and report through VHF (Virtual HID Framework) as a 10-point HID touch digitizer. Build with the WDK on a GitHub Actions Windows runner (ARM64), test-sign, and install with `pnputil` (needs a way to run commands on the phone: the earlier Startup-folder script, or USB Ethernet + RDP now that USB host works).
3. Risks: Windows power management without a real PEP (the device stays in D0, fine), the rails/clock votes surviving Windows sleep (untested), and the first driver install needing a working input path (the USB mouse works).

## Windows touch works (2026-11-19)

Touch input works in Windows on the munch with `munchfts.sys` (branch `feat/munch-touch`, not merged). Pieces:

* **Firmware** `MunchTouchDxe` (quiet mode, `MUNCH_TOUCH_VERBOSE` = FALSE): clocks, pins, RPMh rails, reset, SE4 init; prints one line (`MunchTouch: chip ID 56 52`) and holds 1 s.
* **ACPI** `AcpiTables/munch/touch_fts.asl`: device `MFTS0001` with memory resources SE4 (`0x990000`, `0x4000`) and the south TLMM GPIO 39 page (`0xF527000`, `0x1000`). **`DSDT.aml` is a committed file**: CI does not recompile it, so after editing any `.asl` run the `build-dsdt.yml` workflow and commit the resulting `DSDT.aml`, otherwise the device is missing in Windows (this cost one test cycle).
* **Driver** `windows/munchfts/` (KMDF + VHF): maps the two resources, re-initialises GENI SPI (same sequence as the firmware, full-duplex opcode 3), polls the FTS touch register `0x01` from a system thread every 8 ms (1 ms timer resolution), decodes up to 10 contacts and submits HID reports through VHF. Log: `C:\Windows\Temp\munchfts.log`.
* **Lessons from bring-up**:
  * VHF in a KMDF driver needs `HKR,,"LowerFilters",0x00010000,"vhf"` in the INF `.HW` section; without it `VhfCreate` returns `STATUS_NOT_SUPPORTED` (`0xC00000BB`).
  * A WDF high-resolution timer callback did not give usable logging at PASSIVE_LEVEL; a system thread with `KeDelayExecutionThread` works.
  * Updating the driver while the device is running leaves it in `CM_PROB_NEED_RESTART`; the new version only loads after a reboot.
  * Windows 11 24H2 (build 26100) ARM64, test signing on. CI (`build-munchfts.yml`, windows-2022 + NuGet WDK 10.0.26100.6584) builds, test-signs and makes the catalog (`munchfts-test.cer` must be added to the Root and TrustedPublisher stores).
  * Install without a shell on the phone: from Android, `fsck.ntfs -d` then `mount.ntfs -o rw` on the Windows partition (`/dev/block/sda36`, dirty after every forced restart), copy the package to `C:\munchfts` plus a Startup script (`...\Users\<user>\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\munchfts-startup.cmd`) that re-launches `install.cmd` elevated (UAC prompt, click Yes with the mouse) unless `C:\munchfts\done.txt` exists. Remove `done.txt` to re-run it.
* Open points: the system thread polls forever at 125 Hz (no D-state or sleep handling, no interrupt), rails/clocks and Windows sleep are untested, the on-screen firmware line and test-cert install are test-grade, and none of this is merged to master.

### Calibration (2026-11-19)

* The chip reports positions in 1/8 pixel units: range about 0..8639 x 0..19199 for the 1080x2400 panel (first guess of 10800 x 24000 from the DT `display-coords` was wrong and made touches fall short away from the top-left). The HID descriptor now uses 8639 x 19199.
* Idle frames are all `0xFF` (point count `0x0F`); they are treated as "no contact" so a pending finger is lifted.
* The chip never reports the outermost strip: smallest values seen were X ~41 and Y ~195-228 (about 5 px and 25-28 px from the edge), largest X ~8290, Y ~19110. `MapAxis()` stretches only the outer zones (about 100 px left/right, 200 px top/bottom) so the corners can be reached; the middle of the screen is unchanged. Corner taps (recycle bin, top-left) are still harder than elsewhere.
* Writing the edge-filter register `0x9c` (vendor default 2) to 0 took effect (read back 0) but did not make the top corners easier, so it is left at the default (the driver only logs it).
* A new driver can be swapped without UAC or reinstall by overwriting `munchfts.sys` in every `Windows\System32\DriverStore\FileRepository\munchfts.inf_arm64_*` directory (integrity checks are disabled), then a normal reboot.
