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

* **Dual-Boot Ready:** Integrates `BootShim.Dualboot` with native support for modern Header v3 (4096-byte page) Android GKI kernels.
* **Native File System Drivers:** Embedded `NtfsDxe` (`ntfs_aa64.efi`), `FatPkg` (`EnhancedFatDxe`), `ExfatDxe`, and `Ext2Dxe` dispatched at early boot for instant partition mounting.
* **Direct Windows ARM Loader:** Automated BDS detection for `\EFI\Microsoft\Boot\bootmgfw.efi` across GPT disk volumes.
* **Clean Silicon Dispatch:** Neutralized Qualcomm closed-source binary assertion traps (`BaseDebugLibNull`) and disabled hardware watchdog resets for maximum stability.
* **Native Resolution Display:** Pixel-perfect 1080 × 2400 display output with high-performance BLT rendering.

---

## 🎮 Physical Key Bindings

| Button | Key Event | Action in UEFI / Boot Manager |
| :--- | :--- | :--- |
| **Volume Up** | `SCAN_UP` | Enter **Boot Manager Menu / UEFI Settings** (or select item) |
| **Volume Down** | `SCAN_DOWN` | Navigate menu down |
| **Power Button** | `ENTER` | Confirm selection / Continue boot |
| **USB-C Keyboard (OTG)** | `ESC` / `Win` | Enter UEFI Setup Menu |

> [!NOTE]
> When the boot screen displays *"Press Windows key for UEFI settings"*, on the physical phone you simply press **Volume Up**.

---

## 🚀 Quick Start & Flashing Guide

### 1. Prerequisites
* Unlocked bootloader on your Xiaomi POCO F4 / Redmi K40S (`munch`).
* Storage partitioned into:
  * **ESP Partition (`sda35`):** FAT32 (~500 MB) — Contains **only** `/EFI/` bootloader files.
  * **Windows Partition (`sda36`):** NTFS (64 GB+) — Contains the full Windows 11 ARM64 OS.
* Stock HyperOS / Android 13/14 rooted boot image (`boot_a.img`).

---

### 2. Automated Installation via Dual-Boot ZIP (Recommended)
1. Download the latest `uefi-installer-munch.zip` from [Releases](https://github.com/0xSelenicDove/edk2-msm-munch/releases).
2. Flash in Magisk / KernelSU / Recovery, or push via ADB shell:
   ```bash
   mkdir -p /data/local/tmp/installer
   unzip -o /data/local/tmp/uefi-installer.zip -d /data/local/tmp/installer
   cd /data/local/tmp/installer
   chmod -R 755 /data/local/tmp/installer
   sh META-INF/com/google/android/update-binary dummy 1 /data/local/tmp/uefi-installer.zip
   ```
3. Reboot and **hold `Volume Up`** during power-on to launch Windows 11!

---

### 3. Optimal BCD Configuration
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

### 3. Memory Allocation on 12 GB RAM Models
* **Fix:** The EDK2 memory descriptor uses the standard `"RAM Partition"` naming with continuous physical address ranges extending up to `0x300000000` (12 GB).

---

## 📊 Windows 11 ARM Feature Status (POCO F4)

| Component | Status | Details |
| :--- | :--- | :--- |
| **UEFI Bootloader** | 🟢 **Working** | Tianocore EDK2 with ACPI & DTB support |
| **Display & GPU** | 🟢 **Working** | Adreno 650 with full DirectX & GPU acceleration |
| **Touchscreen** | 🟢 **Working** | Goodix / FocalTech Touch I2C Driver |
| **UFS Storage** | 🟢 **Working** | Qualcomm UFS 3.1 Controller |
| **Wi-Fi & Bluetooth** | 🟢 **Working** | WCN3990 / QCA6390 subsystem |
| **Audio** | 🟢 **Working** | Qualcomm WCD9385 Codec & Speakers |
| **USB-C OTG & Charging** | 🟢 **Working** | DWC3 Controller with Host & Peripheral support |
| **Battery & Charging** | 🟢 **Working** | SMB1396 / SPMI Fuel Gauge |
| **Cellular (LTE/5G)** | 🟡 **Work in progress** | Requires QMI/MBIM modem firmware configuration |

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
