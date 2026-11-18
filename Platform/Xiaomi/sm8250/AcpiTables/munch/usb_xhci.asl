// Snapdragon 865/870 primary DWC3 controller (usb30_prim) as a standard ACPI xHCI.
//
// The firmware (MunchUsbDxe) has already turned the controller on and switched it to
// host mode, so Windows' inbox usbxhci driver can drive it directly (ACPI\PNP0D10).
// Qualcomm's QCOM25A6 filter driver hangs the system here (CLOCK_WATCHDOG_TIMEOUT),
// and the URS/Type-C path needs a working PEP, which this platform does not have.
//
// Hardware: register window 0xA600000, xHCI interrupt GIC 165 (SPI 133).
Device (USB0)
{
    Name (_HID, "PNP0D10")
    Name (_UID, Zero)
    Name (_CCA, Zero)
    Name (_S0W, 0x03)

    Name (_CRS, ResourceTemplate ()
    {
        Memory32Fixed (ReadWrite, 0x0A600000, 0x00100000)

        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, ,, )
        {
            0x000000A5
        }
    })

    Device (RHUB)
    {
        Name (_ADR, Zero)

        Device (PRT1)
        {
            Name (_ADR, One)
            Name (_UPC, Package () { 0xFF, 0x09, Zero, Zero })
            Name (_PLD, Package ()
            {
                Buffer (0x10)
                {
                    0x82, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00,
                    0x31, 0x1C, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00
                }
            })
        }
    }
}
