// FocalTech FTS touch controller on SPI (QUP wrapper 0, serial engine 4).
//
// There is no PEP on this platform, so Qualcomm's SPI/GPIO stack cannot be used.  The
// firmware (MunchTouchDxe) has already enabled the QUP clocks, muxed GPIO 8-11 to the
// engine, powered the two touch rails through RPMh and released the chip reset.  The
// Windows driver (munchfts.sys) talks to the serial engine registers directly and polls the
// touch interrupt line (GPIO 39, active low) in the south TLMM tile.
//
//   0x00990000  QUP0 SE4 (GENI) registers
//   0x0F527000  TLMM south, GPIO 39 control/io registers (io register at +4)
Device (TCH0)
{
    Name (_HID, "MFTS0001")
    Name (_UID, Zero)
    Name (_CCA, Zero)
    // Keep the device in D0: the clocks and rails are not managed by Windows.
    Name (_S0W, Zero)

    Name (_CRS, ResourceTemplate ()
    {
        Memory32Fixed (ReadWrite, 0x00990000, 0x00004000)
        Memory32Fixed (ReadWrite, 0x0F527000, 0x00001000)
    })

    Method (_STA, 0, NotSerialized)
    {
        Return (0x0F)
    }
}
