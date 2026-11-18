// Platform-extension plugin devices that Qualcomm's power-engine driver (qcpep) looks for.
// qcpep logs "QcppxInterfaces device interface not found" when the plugin device is absent.
// Reduced versions of the SM8150 reference devices (HIDs from the SM8250 INFs):
//   QCOM25A2 -> qcppx8250.inf  (ACPI PEPV3 platform extension plugin)
//   QCOM25AA -> qcsocpartition.inf
Device (QPPX)
{
    Name (_HID, "QCOM25A2")
    Name (_CID, "QCOMFFE2")
    Name (_UID, Zero)
    Name (_CCA, Zero)

    Method (_STA, 0, NotSerialized)
    {
        Return (0x0F)
    }

    Method (_CRS, 0, NotSerialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x01FEC004, 0x00000004)
        })
        Return (RBUF)
    }
}

Device (SOCP)
{
    Name (_HID, "QCOM25AA")
    Name (_UID, Zero)
    Method (_STA, 0, NotSerialized)
    {
        Return (0x0F)
    }
}
