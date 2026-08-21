// Minimal SM8250/PM8150B power and Type-C topology derived from Qualcomm's
// Kona reference ACPI.  The signed 8250 drivers bind to these HIDs and let
// Windows advertise a source role and enable the PM8150B OTG boost regulator.

Name (CCST, Zero)
Name (HSFL, Zero)

Device (SPMI)
{
    Name (_HID, "QCOM250C")
    Name (_CID, "PNP0CA2")
    Name (_UID, One)
    Name (_CCA, Zero)
    Name (_CRS, ResourceTemplate ()
    {
        Memory32Fixed (ReadWrite, 0x0C400000, 0x02800000)
    })

    Method (CONF, 0, NotSerialized)
    {
        Return (Buffer (0x1A)
        {
            0x00, 0x01, 0x01, 0x01, 0xFF, 0x00, 0x02, 0x00,
            0x0A, 0x07, 0x04, 0x07, 0x01, 0xFF, 0x10, 0x01,
            0x00, 0x01, 0x0C, 0x40, 0x00, 0x00, 0x02, 0x80,
            0x00, 0x00
        })
    }
}

Device (PMIC)
{
    Name (_DEP, Package () { SPMI })
    Name (_HID, "QCOM252E")
    Name (_CID, "PNP0CA3")

    Method (PMCF, 0, NotSerialized)
    {
        Return (Package ()
        {
            0x06,
            Package () { Zero, One },
            Package () { 0x02, 0x03 },
            Package () { 0x04, 0x05 },
            Package () { 0x06, 0x07 },
            Package () { 0x08, 0x09 },
            Package () { 0x0A, 0x0B }
        })
    }
}

// PMIC GPIO controller.  PTCC interrupts are expressed as virtual PMIC GPIO
// pins, so this device must enumerate before the Type-C controller.
Device (PM01)
{
    Name (_HID, "QCOM2530")
    Name (_UID, One)
    Name (_DEP, Package () { PMIC })
    Name (_CRS, ResourceTemplate ()
    {
        Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, ,, )
        {
            0x00000201
        }
    })

    Method (_DSM, 4, NotSerialized)
    {
        If (ToBuffer (Arg0) == ToUUID ("4F248F40-D5E2-499F-834C-27758EA1CD3F"))
        {
            If (ToInteger (Arg2) == Zero)
            {
                Return (Buffer () { 0x03 })
            }
            If (ToInteger (Arg2) == One)
            {
                Return (Package () { Zero, One })
            }
        }
        Return (Buffer () { 0x00 })
    }
}

Device (PTCC)
{
    Name (_HID, "QCOM2582")
    Name (_DEP, Package () { PMIC, PM01 })
    Name (_CRS, ResourceTemplate ()
    {
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x00F8 }
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x010E }
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x010D }
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x010C }
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x010B }
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x010A }
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x0109 }
        GpioInt (Edge, ActiveHigh, SharedAndWake, PullNone, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x0108 }
        GpioInt (Edge, ActiveHigh, Exclusive, PullUp, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x00DA }
        GpioInt (Edge, ActiveBoth, Exclusive, PullUp, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x00DF }
        GpioInt (Edge, ActiveHigh, Exclusive, PullUp, 0,
            "\\_SB.PM01", 0, ResourceConsumer, ,) { 0x00EB }
    })
}

// Windows USB Type-C port manager.  CON0 and URS0.USB0 deliberately carry
// identical _PLD data so UcmCx associates the physical connector with xHCI.
//
// Do not advertise QCOMFFE4 here. Windows binds that compatible ID to its
// boot-only GenPass driver instead of Qualcomm's functional QCOM257D driver.
// UCP1 also creates a fresh PnP instance after removing that old binding.
Device (UCP1)
{
    Name (_HID, "QCOM257D")
    Name (_UID, One)
    Name (_DEP, Package () { PTCC })

    Device (CON0)
    {
        Name (_ADR, Zero)
        Name (_PLD, Package ()
        {
            Buffer ()
            {
                0x82, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00,
                0x69, 0x0C, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00,
                0xFF, 0xFF, 0xFF, 0xFF
            }
        })
        Name (_UPC, Package () { One, 0x09, Zero, Zero })
        Name (_DSD, Package ()
        {
            ToUUID ("6B856E62-40F4-4688-BD46-5E888A2260DE"),
            Package ()
            {
                Package () { One, 0x04 },
                Package () { 0x02, 0x03 },
                Package () { 0x03, Zero },
                Package () { 0x04, One },
                Package () { 0x05, 0x03 },
                Package () { 0x06, Package () { 0x00019096 } },
                Package () { 0x07, Package () { 0x0001912C, 0x0002D0C8, 0x0003C096 } },
                Package () { 0x08, Package () { 0xFF01, 0x3C86 } },
                Package () { 0x09, One },
                Package () { 0x0A, One }
            }
        })
    }

    Method (_DSM, 4, NotSerialized)
    {
        If (ToBuffer (Arg0) == ToUUID ("18DE299F-9476-4FC9-B43B-8AEB713ED751"))
        {
            If (ToInteger (Arg2) == Zero)
            {
                Return (Buffer () { 0x01 })
            }
            If (ToInteger (Arg2) == One)
            {
                If (ToInteger (Arg3) == Zero)
                {
                    Return (Package () { 0x36019050 })
                }
                If (ToInteger (Arg3) == One)
                {
                    Return (Package () { 0x3601912C })
                }
                Return (Package () { Zero })
            }
        }
        Return (Buffer () { 0x00 })
    }

    Method (CCOT, 2, NotSerialized)
    {
        CCST = Arg0
        HSFL = Arg1
    }

    Method (CCVL, 0, NotSerialized)
    {
        Return (CCST)
    }
}
