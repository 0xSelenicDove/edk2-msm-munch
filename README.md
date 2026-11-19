# EDK2 UEFI Firmware for Xiaomi POCO F4 / Redmi K40S (`munch`)

<div align="center">

![GitHub Workflow Status](https://img.shields.io/github/actions/workflow/status/0xSelenicDove/edk2-msm-munch/build-munch.yml?branch=master&label=Build%20UEFI)
![GitHub Release](https://img.shields.io/github/v/release/0xSelenicDove/edk2-msm-munch?include_prereleases&label=Latest%20Release)
![License](https://img.shields.io/badge/License-BSD--2--Clause-blue.svg)

**A high-performance Tianocore EDK2 UEFI environment and dual-boot bootloader for the Xiaomi POCO F4 / Redmi K40S (Codename: `munch`), powered by the Qualcomm Snapdragon 870 (SM8250-AC).**

</div>

---

## 📱 Supported Devices

| Parameter | Specification |
| :--- | :--- |
| **Device Model** | Xiaomi POCO F4 / Redmi K40S |
| **Codename** | `munch` / `munch_in` |
| **SoC** | Qualcomm Snapdragon 870 5G (`SM8250-AC`) |
| **CPU Architecture** | ARM64 (`aarch64` / Cortex-A77 + Cortex-A55) |
| **RAM** | 6GB / 8GB / 12GB LPDDR5 |
| **Display Panel** | 1080 × 2400 AMOLED (60Hz Framebuffer GOP) |
| **Android Firmware** | Xiaomi HyperOS 1.0 / MIUI 13 & 14 (Android 13 / 14 GKI Kernel) |
| **Target OS** | Microsoft Windows 11 ARM64 (Build 22621+), Linux, Android Dual-Boot |

---

## ✨ Features & Enhancements

* **Boots Windows 11 ARM64 to the desktop** with the A/B slot setup below.
* **Boot menu with working buttons:** a small driver (`MunchButtonsDxe`) reads Volume Up/Down and Power straight from the PM8150, because Qualcomm's `PmicDxe`/`ButtonsDxe` do not work on munch.
* **Android ⇄ Windows choice:** Windows starts by default; the UEFI Boot Manager has a *Reboot to other slot* entry that switches to Android.
* **Native File System Drivers:** `NtfsDxe`, `FatPkg` (`EnhancedFatDxe`), `ExfatDxe`, `Ext2Dxe`.
* **Direct Windows ARM Loader:** BDS detects `\EFI\Microsoft\Boot\bootmgfw.efi` on the mounted filesystems.
* **USB host works:** `MunchOtgDxe` powers the port (VBUS) and `MunchUsbDxe` brings the USB controller up in host mode before Windows starts, so a **USB mouse/keyboard works in Windows** through Windows' standard xHCI driver.
* **Touchscreen works:** `MunchTouchDxe` powers the FocalTech controller (RPMh rails, reset, SPI engine) and a small Windows driver, `munchfts` (`windows/munchfts`), reports touches to Windows as a 10-point HID touchscreen. See [Touchscreen](#touchscreen-munchfts).
* **Fast status text:** 20 ms per line on the early framebuffer console.

---

## 🎮 Boot Menu & Buttons

| Button | Action |
| :--- | :--- |
| **Volume Up** | Tap during the splash ("Press any side button for Boot Options", ~5 s) to open the UEFI front page |
| **Volume Down** | Move the highlight down |
| **Power** | Select |

On the front page choose **Boot Manager**. Useful entries:

* **Windows Boot Manager** – start Windows now.
* **Reboot to other slot** – switch to Android (slot A) and reboot.

If you do nothing, Windows starts after the splash.

> [!NOTE]
> Holding Volume Up during power-on is taken by the phone's own bootloader (recovery). Tap it *after* the splash text appears.

---

## 🚀 Install (A/B slot dual boot)

The recommended layout keeps stock Android untouched in **slot A** and installs UEFI only in **slot B**:

1. Phone has an unlocked bootloader, Magisk/root, and the Windows 11 ARM64 install on `sda36` (NTFS) + ESP on `sda35`.
2. **Back up** `boot_a` first (`dd if=/dev/block/bootdevice/by-name/boot_a of=/sdcard/boot_a_stock.img`).
3. Download `uefi-installer-munch.zip` from [Releases](https://github.com/0xSelenicDove/edk2-msm-munch/releases) and push it to the phone.
4. As root, copy `boot_a` into `boot_b`, then run the installer against slot B:
   ```bash
   dd if=/dev/block/bootdevice/by-name/boot_a of=/dev/block/bootdevice/by-name/boot_b bs=1M
   mkdir -p /data/local/tmp/installer && unzip -o /data/local/tmp/uefi-installer.zip -d /data/local/tmp/installer
   cd /data/local/tmp/installer && chmod -R 755 .
   sed -i 's/^slot=.*/slot=_b/' META-INF/com/google/android/update-binary
   sh META-INF/com/google/android/update-binary dummy 1 /data/local/tmp/uefi-installer.zip
   ```
5. Boot slot B: `fastboot set_active b && fastboot reboot`.
6. Switch OS later with the menu (*Reboot to other slot*), or from a PC with `tools/munch-switch-os.sh windows|android`.

> [!IMPORTANT]
> Use the **installer ZIP**. The plain `boot-munch.img` is boot header v1 and the bootloader rejects it; stock `boot` is header v3.

---

### Optimal BCD Configuration
For fast, clean, and error-free boot:
```cmd
bcdedit /store BCD /set {bootmgr} device boot
bcdedit /store BCD /set {default} device locate=\Windows\system32\winload.efi
bcdedit /store BCD /set {default} osdevice locate=\Windows
bcdedit /store BCD /set {default} path \Windows\system32\winload.efi
bcdedit /store BCD /set {default} systemroot \Windows
bcdedit /store BCD /set {default} testsigning Yes
bcdedit /store BCD /set {default} nointegritychecks Yes
bcdedit /store BCD /set {default} hypervisorlaunchtype Off
bcdedit /store BCD /set {default} vsmlaunchtype Off
bcdedit /store BCD /set {default} quietboot Yes
bcdedit /store BCD /set {default} bootstatuspolicy IgnoreAllFailures
```

---

## 🛠️ Troubleshooting & Knowledge Base

### 1. Error `0xc000000f` Right After Windows Logo
* **Cause:** When duplicate `\Windows\` directories exist on both `sda35` (FAT32 ESP) and `sda36` (NTFS), BCD's `locate` matches Partition 35 first, running out of space and missing subsystem files.
* **Fix:** Keep `sda35` clean with **only `/EFI/`**. Let the embedded `NtfsDxe` driver mount `sda36` directly.

### 2. Error `0xc0e90002` (Code Integrity Failed)
* **Fix:** Ensure `testsigning Yes` and `nointegritychecks Yes` are set in BCD, and offline driver packages in `DriverStore` have valid catalog associations.

### 3. `MEMORY_MANAGEMENT` BSOD / Windows freezes at the spinner
* Use the build from `master` as-is. The memory map here keeps the layout that reaches the desktop.
* Do **not** limit RAM or change the memory map without testing: a 4 GB-limited map and a hand-merged USB/menu build both froze at the spinner during bring-up.

---

## 📊 Current Status (Windows 11 ARM on POCO F4)

> [!WARNING]
> **Work in progress.** Windows boots to the desktop, a **USB mouse/keyboard works**, the **touchscreen works** (needs the `munchfts` driver, see below), and you can switch between Android and Windows at startup.

| Component | Status | Details |
| :--- | :--- | :--- |
| **UEFI firmware / boot** | 🟢 **Working** | Boots, mounts ESP + NTFS, starts Windows |
| **Windows desktop** | 🟢 **Working** | Reaches the desktop (display only) |
| **Boot menu + buttons** | 🟢 **Working** | Volume Up/Down + Power via `MunchButtonsDxe`; Android ⇄ Windows via A/B slots |
| **Display (GOP framebuffer)** | 🟢 **Working** | 1080 × 2400 framebuffer at `0x9c000000` |
| **UFS Storage** | 🟢 **Working** | `ufs.asl` |
| **USB-C host (mouse/keyboard)** | 🟢 **Working** | Firmware brings the controller up in host mode; Windows uses the inbox xHCI driver (`PNP0D10`). The phone's port is USB 2.0 hardware |
| **USB-C power (VBUS/OTG)** | 🟢 **Working** | `MunchOtgDxe` enables the PM8150B OTG boost |
| **USB-C device mode / role switching** | 🔴 **Not working** | Needs the Type-C manager and a real PEP (see below) |
| **Touchscreen** | 🟢 **Working** | FocalTech FTS on SPI (QUP SE4). Firmware `MunchTouchDxe` + ACPI `MFTS0001` + Windows driver `munchfts` (VHF touchscreen). The outermost ~25 px at the top edge are hard to reach; no sleep/resume support yet |
| **GPU / Wi-Fi / Bluetooth / Audio / Battery / Cellular** | ⚪ **Not started** | Need ACPI nodes and drivers that are not present yet |

### How USB host was made to work
* The bootloader leaves the USB controller powered, with most clocks running, but in **device mode** and with `mock_utmi_clk` off. Qualcomm's own USB bring-up driver (`UsbConfigDxe`) never runs on this platform because it waits for a protocol nothing provides.
* The USB 2.0 PHY is only configured by the bootloader in fastboot mode, so `MunchUsbDxe` also runs the Linux Synopsys-femto PHY init (with munch's tuning values from the device tree) on every boot; without it the mouse worked only right after a flash and died after any restart (Windows logged *port reset failed*).
* `MunchUsbDxe` (firmware) enables the UTMI clock, selects UTMI as the PIPE clock (the port is USB 2.0 only, so the USB 3.0 PHY is not needed; same trick as Linux `dwc3_qcom_select_utmi_clk`), stops the device controller and switches the core to host mode. It prints one status line before boot.
* The DSDT exposes `USB0` as a plain ACPI xHCI (`PNP0D10`). Qualcomm's `qcxhcifilter` driver (`QCOM25A6`) hangs Windows with `CLOCK_WATCHDOG_TIMEOUT`, so it is not used.
* Windows' own USB-C stack is not used either: the Type-C port manager and URS role switch depend on Qualcomm's PEP (power engine), and the real `QCOM2519` PEP fails to start (`CM_PROB_FAILED_ADD`) without full SM8250 clock/power tables. USB therefore only works as a host. (The phone has a USB 2.0 port, so nothing is lost on speed.)

<a id="touchscreen-munchfts"></a>
### Touchscreen (`munchfts`)
There is no working PEP on this platform, so Qualcomm's SPI/GPIO drivers cannot be used. Instead:

* **Firmware (`MunchTouchDxe`)** runs before Windows starts: it enables the QUP clocks (the `s_ahb` branch has hardware clock gating, which only *looks* like it is off), muxes the SPI pins (GPIO 8-11), powers the two touch rails through an **RPMh** request (pm8150l `ldoc1` 1.8 V and `ldoc10` 3.2 V; they are owned by RPMh and cannot be switched over SPMI; the vote goes through the APPS RSC DRV2 using addresses from the AOP command DB), releases reset (GPIO 33) and initialises the GENI serial engine (SPI mode 0, full-duplex opcode 3). It prints one line, `MunchTouch: chip ID 56 52`, and waits 1 s. Set `MUNCH_TOUCH_VERBOSE` in `MunchTouchDxe.c` for the full bring-up log.
* **ACPI** (`touch_fts.asl`): device `MFTS0001` exposing the SE4 registers and the GPIO 39 page. **`DSDT.aml` is a committed file**: after editing any `.asl`, run the *Build munch DSDT.aml* workflow and commit its output.
* **Windows driver (`windows/munchfts`)**: a KMDF driver that talks to SE4 directly, polls the FTS touch frame every 8 ms and reports up to 10 contacts through the Virtual HID Framework. Raw coordinates are 1/8 pixel (0..8639 x 0..19199). The CI workflow *Build munchfts* builds it for ARM64 (WDK from NuGet), test-signs it and produces `munchfts.sys`, `munchfts.inf`, `munchfts.cat` and `munchfts-test.cer`.
* **Installing the driver** (needs test signing, already set by the BCD steps above): add `munchfts-test.cer` to the *Trusted Root* and *Trusted Publishers* stores, then `pnputil /add-driver munchfts.inf /install` from an elevated prompt (reboot afterwards; the INF adds `vhf` as a lower filter). Driver log: `C:\Windows\Temp\munchfts.log`.
* Known gaps: polling instead of an interrupt, no sleep/resume handling, the outermost strip of the top edge is not reported by the chip, and the test certificate is self-signed.

The remaining reason for the fake PEP is unchanged: Qualcomm's closed `PmicDxe` (taken from a different product) asserts and bootloops on munch, so it stays disabled; keys are read directly by `MunchButtonsDxe` instead.

See [docs/touch-usb-investigation.md](docs/touch-usb-investigation.md) for the investigation notes.

---

## 🤝 Acknowledgements & Credits

* [Renegade Project](https://github.com/edk2-porting/) & [Renegade Wiki](https://wiki.renegade-project.cn/)
* [msumro](https://github.com/msumro) for original POCO F4 device tree research
* [Gustave Monce](https://github.com/WOA-Project) and the DuoWOA / SurfaceDuoPkg Project
* [BigfootACA](https://github.com/BigfootACA) for SimpleInit
* [fxsheep](https://github.com/fxsheep) for early Snapdragon UEFI ports

---

## 📄 License
* Core EDK2 codebase: [BSD 2-Clause License](LICENSE)
* GPL drivers in `GPLDrivers/`: GNU General Public License v2
