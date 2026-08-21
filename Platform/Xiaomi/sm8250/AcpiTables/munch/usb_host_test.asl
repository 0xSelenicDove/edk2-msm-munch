// Snapdragon 865/870 primary DWC3/xHCI controller in host-only mode.
//
// QcXhciFilter8250 binds directly to QCOM25A6.  The USB0 namespace path is
// also named in IortUsb.aml, which maps controller DMA SID 0x140 through the
// apps SMMU.
Device (USB0)
{
    Name (_HID, "QCOM25A6")
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
        Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, ,, )
        {
            0x000000A2
        }
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
