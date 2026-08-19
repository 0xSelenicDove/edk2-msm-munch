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
* Unlocked bootloader on your Xiaomi POCO F4.
* Re-partitioned storage with an `esp` (FAT32, ~500MB) and `win` (NTFS, 64GB+) partition created via WOA Helper or parted.
* Windows 11 ARM64 deployed to the `win` partition.

### 2. Download Pre-built Release
Download the latest `uefi-installer-munch.zip` from the [Releases](https://github.com/0xSelenicDove/edk2-msm-munch/releases) page.

### 3. Assemble Dual-Boot Image (macOS / Linux / Windows)
```bash
# 1. Patch your stock Android kernel with BootShim
kernelpatcher stock_kernel.bin BootShim.Dualboot.bin 7340032

# 2. Append UEFI firmware payload
cat patched_kernel.bin munch_UEFI.fd > final_kernel.bin

# 3. Repack into boot.img (Header v3) and flash via Fastboot
fastboot flash boot_a dualboot.img
fastboot reboot
```

---

## 🛠️ Building from Source

### Automated CI / CD Build
Every commit pushed to the `master` branch automatically triggers the `.github/workflows/build-munch.yml` pipeline with fast Azure Ubuntu mirrors and IPv4 routing, generating flashable zips and `.fd` binaries under GitHub Releases.

### Local Linux Build (Ubuntu 22.04 / 24.04)
```bash
# Install toolchain
sudo apt-get install -y build-essential clang llvm gcc-aarch64-linux-gnu python3 python3-distutils uuid-dev nasm

# Clone repository with submodules
git clone --recursive https://github.com/0xSelenicDove/edk2-msm-munch.git
cd edk2-msm-munch

# Build UEFI payload for munch
./build.sh --device munch
```

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
