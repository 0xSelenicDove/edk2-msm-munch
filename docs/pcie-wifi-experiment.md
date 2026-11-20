# PCIe / Wi-Fi bring-up experiment (MunchPcieDxe)

Goal of step 1: prove that our firmware can power the QCA6390 Wi-Fi/BT chip, train PCIe root
complex 0 and read the endpoint's ID (expect `17CB:1101`), with no Windows involved.
Background and register sources: `.dl/notes/wifi.md` (local, git-excluded).

**Status: step 1 done on the phone (2026-11-19, CI `bc56775`): the link trains and the endpoint answers 17CB:1101.**

## What the driver does (`Platform/Xiaomi/sm8250/Drivers/MunchPcieDxe`)

Runs at ReadyToBoot (like the touch driver), prints on the boot screen, writes the same log to
`\munch-pcie.log` on the ESP after every step, then holds the screen 20 s.

1. Maps `0x01C00000+0x10000` (PARF/PHY) and `0x60000000+64 MB` (DBI, iATU, config/MEM windows)
   through the GCD (the munch memory map is not touched).
2. RPMh: `ldoa5` 0.88 V, `ldoa9` 1.2 V (PHY), `smpa6` 0.95 V, `smpa4` 1.8 V, `smpa5` 1.9 V,
   `smpc8` 1.35 V (chip), BCM votes `CN0`, `SN6`, `SN9`, `SN12` (config NoC, PCIe slave and
   master paths). Dumps every cmd-db name to the log file.
3. `WLAN_EN` (GPIO 20) high, PERST# (GPIO 79) held low, CLKREQ# (80) to `pci_e0`.
4. GDSC `pcie_0_gdsc`, GCC clocks (aux, refgen 100 MHz, slv/mstr axi, cfg ahb, slv q2a, phy aux,
   `wifi_clkref_en`, TBUs), BCR pulses. **Clock status is checked before the first PARF access.**
5. PARF init, QMP PHY table from the downstream DT (99 writes), wait for PLL lock, pipe clock.
6. 100 ms, release PERST#, 100 ms, DBI root-port setup, LTSSM enable, poll DLLLA for 1 s.
7. iATU region 0 (CFG0 -> `0x60100000`, target 01:00.0) and region 1 (MEM `0x60300000`..`0x63FFFFFF`),
   read config space of 01:00.0.

`BT_EN` (GPIO 21) is not touched; `MunchBtDxe` (other session) owns it. Both drivers vote the same
rails with the same voltages. If you build both, expect interleaved output: test them one at a time
the first time.

## Reading the result

| Last line on screen / in the log | Meaning |
|---|---|
| `MunchPcie: QCA6390 FOUND` | Done. Next: MCFG + `PCI0` in the DSDT, then the Windows driver. |
| `01:00.0 vendor/device ffffffff` | Link is up but config reads fail: check the iATU region 0 programming (CFG0 vs CFG1, target). |
| `link DOWN` + ltssm state | Link training failed. State `00` = detect: no endpoint seen (chip not powered / WLAN_EN / PERST / refclk). Stuck in polling/config states: PHY table or lane setup. |
| `phy NOT ready` | PLL did not lock: refgen/aux clocks or the PHY rails. |
| `clk ... stuck off` / `gdsc did NOT power on` | Power/bus vote problem: look at the BCM lines (`not in cmd-db` means the name is wrong; the log file lists all names). |
| screen freezes after step N | A bus hang on the first access of that step. The log file has everything up to the start of step N. |

## Things the first run will settle

* the `bcm` names/levels that are actually required (cmd-db dump in the log file);
* whether the delays (100 ms after WLAN_EN and after PERST#) are enough;
* the PCI subsystem ID of munch's module (goes into the Windows INF);
* whether SMMU/MSI/IORT need work (not exercised here).

## Not done yet

ACPI (MCFG, `PCI0` node without `_DEP`, MADT MSI frame check), IO window, ASPM policy, Windows
driver install, and moving the code out of the test-style "hold the screen 20 s" behaviour.

## Result (run 2, commit bc56775, saved log `.dl/pcie-ci2/run2.log`)

* Run 1 (commit 4cf7f66) stopped at "phy NOT ready (status 0x69)": rails, BCM votes, GDSC, clocks and
  PARF access were all fine, but the QMP PLL never locked.
* **Fix: enable the pipe clock branch (GCC vote bit 4) before applying the PHY table**, as mainline does.
  Variant A of the start ladder then reports READY at once (status 0xA6, bit 6 clear). B and C were not needed.
* Link: LTSSM 00 -> 0x11 (L0) within 1 ms of enabling it, `link UP`, **Gen2 x1** (LNKCAP says Gen3 capable;
  the speed was not pushed up). DLLLA set, `lnksta B0120008`.
* Endpoint 01:00.0: vendor/device **17CB:1101**, **subsystem 17CB:0108** (this is in the arm64
  `qcwlan6800sc8180.inf` hardware ID list, so no INF edit should be needed), class 02:80:00 (network,
  other), rev 01, BAR0 = 64-bit memory (unassigned, Windows/ACPI will place it in `0x60300000..0x63FFFFFF`).
* cmd-db has 135 entries; the full list is in the log file (BCM names CN0/SN6/SN9/SN12 and the VRM names
  used all exist). `qphy.lvl`, `cx.lvl` etc. exist as ARC resources but were not needed.
* Windows booted normally afterwards (the driver leaves the chip powered and the link up).

## Next

1. Remove the ladder (keep variant A), drop the 20 s hold and the verbose output (touch-driver style).
2. ACPI: MCFG (`0x60000000`, seg 0, bus 0-1) + `PCI0` node (`PNP0A08`, no `_DEP`, no power resources,
   `_OSC` denying ASPM), IO window, check the MADT MSI frame range vs the SPIs RC0 uses.
3. Windows: install `qcwlan6800sc8180` (Windows Update / WOA-Project Qualcomm-Reference-Drivers 8180_CLS);
   check whether it starts without the PEP; look at DMA/SMMU (IORT) if it does not.

## Windows side (2026-11-19, CI 406018e and 55b8ec1)

* **ACPI/enumeration works.** MCFG + `PCI0` (no `_DEP`) make Windows start `ACPI\PNP0A08`; with the root
  port BARs disabled via ELBI CS2 (`ELBI+0xA4`, BAR regs = size masks, write 0) the root port
  (17CB:010B) starts and Windows enumerates `PCI\VEN_17CB&DEV_1101&SUBSYS_010817CB&REV_01`.
  Before the CS2 fix the root port failed with CM_PROB_NORMAL_CONFLICT (0xC0000018).
* **ECAM safety:** PARF ECAM base 0x380 + two blocker windows (0x360-0x378 and 0x398-0x3B0, `SYS_CTRL`
  bits 26 and 30) make every address Windows probes while enumerating return FFFFFFFF; no hangs. The
  unrolled iATU registers (0x60001000) sit inside the root-bus blocker, so program the iATU first.
* **MMIO works.** Firmware probe: BAR0 is 1 MB 64-bit; placed at 0x60400000 the MHI registers read real
  values (MHIREGLEN 0x100, MHICFG 0x00800080, CHDBOFF 0x300; MHIVER slot reads DEADBEEF = chip not
  booted yet). So the iATU MEM window and the link are fine.
* **Driver `qcwlan6800sc8180` (WOA-Project Qualcomm-Reference-Drivers 8180_CLS, proprietary, not in repo):**
  * 1.0.1860 (200.0.80.0, 2022): binds, loads, `StartDevice` fails (Problem 10, 0xC0000001); the driver
    logs NDIS event 5006 "The version number is incorrect for this driver".
  * 1.0.2140 (200.0.112.0) and 1.0.2160 (200.0.117.0): bugcheck 0x7E / 0x50, access violation inside
    `qcwlan.sys` at RVA 0x1b6580 (first instruction of the BAR window-select helper called from
    `read32`, i.e. a read of the driver's global context pointer), reached from `+0x1b64e8` and
    `+0x1b35a4` (a 256 KB table fill in a function at `+0x1b3370`). Looks like a register read from a
    worker thread after the device start already failed (global context gone), not a bad BAR.
  * Same early failure in all builds is likely: something in start-up rejects the chip/platform
    (version check) before the crash. The SoC HW version (BAR+0x224) is read by `+0x1ad970`
    and stored at ctx+0xca10/0xca14; the code that rejects it was not found.
* **Reading crash dumps without Windows:** with CrashDumpEnabled=3 the small dump stays at offset 0
  of `pagefile.sys` until the next Windows boot (`PAGEDU64`). `.dl/wlan-drv/parse_dump.py` parses it
  (triage header offsets are absolute; driver list stride 0x90, DllBase at +0x38; name pool 8-byte
  aligned). Disassemble the driver with Apple `objdump -d` (coff-arm64).
* **Chip identity (firmware probe, 65963b7):** `SOC_HW_VERSION` (BAR+0x224) = `0x400A0200`, i.e.
  major 2 / minor 0 = QCA6390 v2.0, the version `wlanfw20.mbn` targets. `PCIE_SOC_GLOBAL_RESET`
  (0x3008) = 4, BAR window select (0x310C) = 0x40000000 (window enabled, window 0). Endpoint config
  space is normal: PM cap @40, MSI cap @50 (32 vectors, 32-bit address, per-vector mask), PCIe cap @70
  (Gen2 x1, ASPM L0s+L1 supported, ASPM control off), extended caps AER (0x100), 0x19 (0x148), 0x17
  (0x158). Subsystem 17CB:0108, class 02:80, rev 01. The same 5006 ("version number is incorrect")
  event is logged by 1.0.2140 right before its crash, so all builds reject something at start-up.
* **Firmware leftovers can stall the Windows boot.** The BAR probe leaves BAR0 assigned and memory +
  bus-master enabled on the endpoint; Windows then hung at the spinner forever (and once before on a
  first boot). The build without the probe (406018e) booted every time. The probe is now behind
  `MUNCH_PCIE_BAR_PROBE` (off) and restores BAR0/command when enabled. Keep the endpoint exactly as
  found (decode off, BAR 0) when handing over to Windows.
* **Windows disables a driver after it blames it for a crash**: `Services\qcwlan\Start` was already 4.
  A crash during install can also leave a truncated `drivers\qcwlan6800sc81.sys` (2 MB) that NTFS-3G
  cannot remove (I/O error).
* **Open:** why start-up rejects the chip; candidates are the chip version (read BAR+0x224 and the
  window-based registers from firmware and compare with ath11k's QCA6390 table), missing platform
  data the Qualcomm laptops provide (ACPI companion device / `_DSD` under RP1, PEP-provided
  resources), or WDI/NDIS expectations of this Windows build.

## Wi-Fi driver, round 3 (companion node, driver-store damage)

* **ACPI companion works but changes nothing.** `RP1.WLN0` (`_ADR 0`, no `_DEP`) attaches
  (`DEVPKEY_Device_BiosDeviceName = \_SB.PCI0.RP1.WLN0`, location path includes the ACPI node). The
  driver has no ACPI property reads (strings: registry keys only), and 1.0.2000 still crashes with the
  companion present.
* **Crash is common to every build from 1.0.2000 (July 2023) to 1.0.2160**: access violation at
  `qcwlan.sys` RVA 0x1b5018 (2000) / 0x1b6580 (2140, 2160), always the load of `[global + 0xa7c4]` in the
  chip register read helper (BAR window select at BAR+0x310C, ath11k-style). The global context
  pointer is **set inside `MiniportInitializeEx`** after the adapter allocation (RVA 0x2c594 in 2160)
  and **cleared in the halt/teardown path** (0x2a8f0). So a later initialization step fails, teardown
  runs, and a worker thread still reads registers. NDIS logs event 5006 ("version number is incorrect",
  `NDIS_STATUS_BAD_VERSION`) for the failed initialize. The failing step was not identified (the
  status is not built in the driver; it comes back from a lower layer).
* **Do not run `fsck.ntfs -d` / `ntfsfix -d` after every blue screen.** It clears the NTFS `$LogFile`,
  discarding Windows' last uncommitted metadata. After several crashed installs this left: a driver
  package registered in the DRIVERS hive with an empty `DriverStore\FileRepository` folder
  (`pnputil /add-driver` then fails with "The system cannot find the file specified"), a package on disk
  without a registry entry, files that Windows had copied rolled back, `Get-WindowsDriver` reporting
  "configuration registry database is corrupt", and a dirty DRIVERS hive (sequence numbers 63/62).
  Repair that worked: set the hive's secondary sequence number equal to the primary and fix the header
  checksum (2 bytes), keeping the original. Prefer read-only mounts (`mount.ntfs -o ro`) for reading
  logs/dumps; only mount read-write when files must be written.
* **State left in Windows (slot B):** driver packages `qcwlan6800sc8180.inf_arm64_3de23d1344b59ad5`
  (2140 files) and `..._66e78231ad6de634` (empty) in the driver store, `C:\munch-wlan*`
  folders, CrashControl = small dump / no auto-reboot, SYSTEM and DRIVERS hives dirty after the last
  crash (Windows replays its logs at the next boot).
