# 小米 POCO F4 / Redmi K40S（`munch`）的 EDK2 UEFI 固件

[English](README.md) | **简体中文**

为 **POCO F4 / Redmi K40S（代号 `munch`，骁龙 870 / SM8250-AC）** 提供 Tianocore EDK2 UEFI 固件和双系统启动，让 **Windows 11 ARM64 与 Android 双系统** 共存。基于 edk2-msm / Renegade Project 的工作。

> [!WARNING]
> 开发中的项目。刷机有风险，请先备份 `boot_a`。UEFI 只装进 **B 槽**，Android 保留在 **A 槽**，出问题随时可以切回去。

## 当前状态

| 功能 | 状态 | 说明 |
| :--- | :--- | :--- |
| UEFI 启动、开机菜单 | 🟢 可用 | 音量键 / 电源键由 `MunchButtonsDxe` 直接读取 PM8150；可在菜单里"切换到另一个槽"回到 Android |
| Windows 11 ARM 桌面 | 🟢 可用 | 能进入桌面，自动登录 |
| 屏幕 (GOP) / UFS 存储 | 🟢 可用 | 1080×2400 |
| USB-C 主机模式 | 🟢 可用 | 鼠标、键盘、USB 网卡可用（通过 Windows 自带 xHCI 驱动）。USB 设备模式 / 角色切换暂不可用 |
| 触摸屏 | 🟢 可用 | FocalTech FTS，固件 `MunchTouchDxe` + ACPI `MFTS0001` + Windows 驱动 `munchfts`；顶部边缘约 25 px 较难触到，暂无休眠恢复 |
| 日期和时间 | 🟢 可用 | `MunchClockDxe` 读取 PMIC RTC 计数器加上 ESP 上的偏移文件；`tools/windows/munch-clock-sync.ps1` 在 Windows 中保持偏移最新 |
| Wi-Fi | 🟡 研究中 | QCA6390（PCIe）。固件里已验证链路建立并读到芯片 ID，Windows 端（ACPI + 驱动）未完成 |
| 蓝牙 | 🟡 研究中 | 同一颗 QCA6390，走 UART；Windows 驱动依赖 PEP |
| Windows 电量 / 充电显示 | 🟠 计划中 | 电量计是外置 I2C 芯片，Windows 暂无驱动 |
| 蜂窝网络 | 🔴 困难 | 外置 SDX55 基带（PCIe/MHI），暂时请用 USB 共享网络或网线 |
| GPU / 音频 / 传感器 / 摄像头 | ⚪ 未开始 | 需要 ACPI 节点和驱动 |

## 安装

最简单的方式是 **[woa-installer](https://github.com/0xSelenicDove/woa-installer)**（Windows / macOS 图形界面，也有命令行），通过 USB 完成整个双系统流程。

**需要：** 已解锁 Bootloader、Magisk root（给 `adb shell` 授权超级用户）、开启 USB 调试、**Windows 11 ARM64 ISO**，以及 [Releases](https://github.com/0xSelenicDove/edk2-msm-munch/releases) 里的 `uefi-installer-munch.zip`（安装器也可以直接从本仓库下载最新版）。

1. 从 [woa-installer releases](https://github.com/0xSelenicDove/woa-installer/releases) 下载安装器。
2. 连接手机，运行向导，选择 ISO 和 UEFI 压缩包，按提示操作。如果 `esp` / `win` 分区不存在，它可以创建（会清除用户数据，需要你确认）。
3. 先通过只读的预检查，再安装。Android 保持在 A 槽，UEFI 装入 B 槽，手机重启进入 Windows。
4. 首次启动自动完成设置并登录（用户 `user`，空密码）。
5. 之后切换系统：开机画面出现后轻点音量上键进入 UEFI 菜单，选 **Boot Manager → Reboot to other slot**；或在电脑上运行 `tools/munch-switch-os.sh windows|android`。

> [!NOTE]
> 最后一次关机 / 重启时 Windows 会蓝屏 `INTERNAL_POWER_ERROR`（已知的固件问题，不影响数据）。长按电源键重启即可。
>
> 请使用 **安装器 ZIP**（boot header v3）；单独的 `boot-munch.img` 会被 Bootloader 拒绝。

如果手机卡住：同时按住 **电源键 + 音量下键** 10–15 秒进入 fastboot，然后 `fastboot set_active a` 回到 Android。

## 欢迎帮忙

这是一个单人维护、只有一台测试机的项目。如果你有 POCO F4 / Redmi K40S，即使不会写代码也能帮上忙：

* **试用并反馈**：提交一份[设备测试报告](https://github.com/0xSelenicDove/edk2-msm-munch/issues/new?template=device-test-report.yml)（机型、内存版本、国行/国际版、固件版本、哪些能用）。
* **认领任务**：[带 `help wanted` 标签的 Issue](https://github.com/0xSelenicDove/edk2-msm-munch/labels/help%20wanted)，例如电量计驱动、Wi-Fi / 蓝牙、触摸边缘校准、音频。
* 开发和测试流程见 [CONTRIBUTING.md](CONTRIBUTING.md)。固件由 GitHub Actions 构建，不需要本地 EDK2 工具链。

## 致谢

[Renegade Project](https://github.com/edk2-porting/)、[msumro](https://github.com/msumro)、[Gustave Monce / WOA-Project](https://github.com/WOA-Project)、[BigfootACA](https://github.com/BigfootACA)、[fxsheep](https://github.com/fxsheep)。

许可证：核心 EDK2 为 BSD 2-Clause；`GPLDrivers/` 下为 GPL v2。
