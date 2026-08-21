// Snapdragon 865/870 primary USB controller through Windows USB Role Switch.
//
// QCOM2597 selects the Microsoft function stack.  PNP0CA1 causes UrsCx to
// enumerate the logical host child as URS\QCOM2597&HOST, which is the ID
// consumed by QcXhciFilter8250.inf.  The controller itself is deliberately
// not exposed as a direct ACPI\QCOM25A6 device.
Device (URS0)
{
    Name (_HID, "QCOM2597")
    Name (_CID, "PNP0CA1")
    Name (_UID, Zero)
    Name (_CCA, Zero)

    Name (_CRS, ResourceTemplate ()
    {
        Memory32Fixed (ReadWrite, 0x0A600000, 0x00100000)
    })

    Device (USB0)
    {
        Name (_ADR, Zero)
        Name (_UPC, Package (0x04)
        {
            One,        // Connectable
            0x09,       // USB Type-C with USB2/SS switch
            Zero,
            Zero
        })

        Name (_CRS, ResourceTemplate ()
        {
            // DWC3 core and power-event interrupts.
            Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, ,, )
            {
                0x000000A5
            }
            Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, ,, )
            {
                0x000000A2
            }

            // DP/DM HS-PHY and SS-PHY wake interrupts routed through PDC.
            Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, ,, )
            {
                0x00000211
            }
            Interrupt (ResourceConsumer, Edge, ActiveHigh, SharedAndWake, ,, )
            {
                0x0000020F
            }
            Interrupt (ResourceConsumer, Edge, ActiveHigh, SharedAndWake, ,, )
            {
                0x0000020E
            }
        })

        // Microsoft USB host-controller device-specific interface.
        Method (_DSM, 0x04, NotSerialized)
        {
            Switch (ToBuffer (Arg0))
            {
                Case (ToUUID ("CE2EE385-00E6-48CB-9F05-2EDB927C4899"))
                {
                    Switch (ToInteger (Arg2))
                    {
                        Case (Zero)
                        {
                            If ((ToInteger (Arg1) == Zero))
                            {
                                // Functions 0, 2, 3 and 4.
                                Return (Buffer () { 0x1D })
                            }
                            Return (Buffer () { 0x01 })
                        }
                        Case (0x02) { Return (Zero) } // Regular USB
                        Case (0x03) { Return (Zero) } // No endpoint offload yet
                        Case (0x04) { Return (0x02) } // Interrupter number
                    }
                }
            }
            Return (Buffer () { 0x00 })
        }

        // QcXhciFilter queries this method for optional PHY overrides. The
        // boot chain already initialized the munch HS PHY, so none are needed.
        Method (PHYC, 0x00, NotSerialized)
        {
            Return (Package () {})
        }
    }
}
