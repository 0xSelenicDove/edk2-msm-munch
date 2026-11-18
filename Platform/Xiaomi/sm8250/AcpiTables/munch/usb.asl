//
// USB primary controller (DWC3 @ 0x0A600000) for Xiaomi munch.
// Layout follows Qualcomm's dual-role "URS" model used by the SM8150/SM8250 Windows
// drivers (urssynopsys.inf matches ACPI\QCOM24B6 or compatible ID ACPI\PNP0CA1,
// qcursext.inf matches ACPI\QCOMFFE1). Host mode only for now.
//
// Values from the live munch device tree: usb0 = ssusb@a600000, xHCI/dwc3 irq SPI 133
// (GIC 165), pwr_event_irq SPI 130 (GIC 162).
//
// NOT YET DONE: clocks / GDSC / PHY power (needs PEP0 tables), PMIC VBUS and Type-C
// role switching. Until then Windows may enumerate the device but fail to start it.
//
Device (URS0)
{
    Name (_HID, "QCOM24B6")
    Name (_CID, Package (0x02) { "PNP0CA1", "QCOMFFE1" })
    Name (_UID, Zero)
    Name (_CCA, Zero)

    Name (_CRS, ResourceTemplate ()
    {
        Memory32Fixed (ReadWrite, 0x0A600000, 0x000FFFFF)
    })

    Device (USB0)
    {
        Name (_ADR, Zero)
        Name (_S0W, 0x03)

        Name (_CRS, ResourceTemplate ()
        {
            Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, ,, ) { 165 }
            Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, ,, ) { 162 }
        })

        Method (_STA, 0, NotSerialized)
        {
            Return (0x0F)
        }

        Device (RHUB)
        {
            Name (_ADR, Zero)
            Device (PRT1)
            {
                Name (_ADR, One)
                // Connectable, Type-C (USB 2.0 and 3.x with switch)
                Name (_UPC, Package (0x04) { One, 0x09, Zero, Zero })
            }
        }

        // USB controller device-specific method (as in Qualcomm reference DSDTs)
        Method (_DSM, 4, NotSerialized)
        {
            If (Arg0 == ToUUID ("ce2ee385-00e6-48cb-9f05-2edb927c4899"))
            {
                If (Arg2 == Zero)
                {
                    If (Arg1 == Zero) { Return (Buffer (One) { 0x1D }) }
                    Return (Buffer (One) { 0x01 })
                }
                If (Arg2 == 0x02) { Return (Zero) }
                If (Arg2 == 0x03) { Return (Zero) }
                If (Arg2 == 0x04) { Return (0x02) }
            }
            Return (Buffer (One) { 0x00 })
        }
    }
}
