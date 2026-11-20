# PCIe / Wi-Fi bring-up experiment (MunchPcieDxe)

Goal of step 1: prove that our firmware can power the QCA6390 Wi-Fi/BT chip, train PCIe root
complex 0 and read the endpoint's ID (expect `17CB:1101`), with no Windows involved.
Background and register sources: `.dl/notes/wifi.md` (local, git-excluded).

**Status: written and syntax-checked only. Not built with EDK2, not run on the phone.**

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
