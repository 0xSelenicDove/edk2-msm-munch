# Contributing

Thanks for looking. This port only has one tested phone, so reports and small fixes are both valuable.

## Easiest ways to help
* Run a build on your own POCO F4 / Redmi K40S and file a **device test report** (Issues > New issue).
* Pick an issue labelled `help wanted` or `good first issue`.
* Improve the docs: anything that confused you while installing is a good README fix.

## How the project is built and tested
* **No local toolchain needed.** The `build-munch.yml` GitHub Actions workflow builds the firmware; push a branch and run it (`gh workflow run build-munch.yml --ref <branch>`), then download the `uefi-installer-munch.zip` artifact. After a change to an `.asl` file, run *Build munch DSDT.aml* and commit its output (`DSDT.aml` is committed).
* **Test with the A/B slot setup.** Android stays in slot A, UEFI goes in slot B, so a bad build can always be recovered: hold Power + Volume Down into fastboot and `fastboot set_active a`. See the README install section and `tools/munch-switch-os.sh`.
* **Use the installer ZIP**, never the plain `boot-munch.img` (wrong boot header version).

## Where things live
* `Platform/Xiaomi/sm8250/Drivers/Munch*Dxe` are the small firmware drivers (USB, touch, buttons, OTG, clock).
* `Platform/Xiaomi/sm8250/AcpiTables` is the ACPI for Windows; `windows/munchfts` is the touch driver; `tools/windows` has Windows-side helper scripts.
* `docs/` has investigation notes (what was tried, what the hardware does).

## Pull requests
Keep them small and say what you tested on the device (build, what you saw). Please do not change the memory map without testing on hardware: bad maps freeze Windows at the spinner.
