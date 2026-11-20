//
// PCIe root complex 0 (pcie@1c00000): the Wi-Fi / Bluetooth chip (QCA6390, 17CB:1101).
//
// Everything hardware-related (rails, WLAN_EN, clocks, PHY, link training, PARF ECAM mode and
// blockers, iATU windows) is done by MunchPcieDxe before the OS starts and left powered.  So this
// node is purely a description: no _DEP, no PowerResource and nothing that needs the Qualcomm PEP
// or GPIO controller drivers (those are what kept the USB role-switch devices from starting).
//
// ECAM (MCFG, segment 0, buses 0-1) at 0x60000000:
//   bus 0 device 0 -> 0x60000000  root port (DBI)
//   bus 1 device 0 -> 0x60100000  endpoint (iATU config region)
// Everything else in the two 1 MB windows is blocked in hardware by the firmware.
//
// Memory window 0x60300000 - 0x63FFFFFF is the iATU MEM region (PCI address == CPU address).
// Legacy INTx: SPI 149..152 = GSIV 181..184.  MSI: GIC MSI frame in the MADT (0x17A10000).
//

Device (PCI0) {
  Name (_HID, EISAID ("PNP0A08"))
  Name (_CID, EISAID ("PNP0A03"))
  Name (_UID, 0)
  Name (_SEG, 0)
  Name (_BBN, 0)
  Name (_CCA, 1)                     // dma-coherent in the device tree

  Name (_PRT, Package () {
    Package () {0x0FFFF, 0, 0, 181},
    Package () {0x0FFFF, 1, 0, 182},
    Package () {0x0FFFF, 2, 0, 183},
    Package () {0x0FFFF, 3, 0, 184}
  })

  Method (_CRS, 0, NotSerialized) {
    Name (RBUF, ResourceTemplate () {
      WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
        0x0000,                      // granularity
        0x0000,                      // min
        0x0001,                      // max
        0x0000,                      // translation
        0x0002)                      // length
      DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
        0x00000000,                  // granularity
        0x60300000,                  // min
        0x63FFFFFF,                  // max
        0x00000000,                  // translation
        0x03D00000)                  // length
    })
    Return (RBUF)
  }

  // Keep the root complex in D0 while the system runs: nothing can power it back up.
  Name (_S0W, 0)

  Name (SUPP, 0)
  Name (CTRL, 0)

  Method (_OSC, 4, NotSerialized) {
    If (LEqual (Arg0, ToUUID ("33DB4D5B-1FF7-401C-9657-7441C03DD766"))) {
      CreateDWordField (Arg3, 0, CDW1)
      CreateDWordField (Arg3, 4, CDW2)
      CreateDWordField (Arg3, 8, CDW3)
      Store (CDW2, SUPP)
      Store (CDW3, CTRL)

      // Grant only native PME.  No native hot plug, no AER, and no "PCI Express Capability
      // Structure control" (bit 4): ASPM / clock PM stay as the firmware left them (off).
      And (CTRL, 0x04, CTRL)

      If (LNotEqual (Arg1, One)) {
        Or (CDW1, 0x08, CDW1)         // unknown revision
      }
      If (LNotEqual (CDW3, CTRL)) {
        Or (CDW1, 0x10, CDW1)         // some capabilities were masked
      }
      Store (CTRL, CDW3)
      Return (Arg3)
    } Else {
      Or (CDW1, 4, CDW1)              // unrecognised UUID
      Return (Arg3)
    }
  }

  Method (_DSM, 4, NotSerialized) {
    If (LEqual (Arg0, ToUUID ("E5C937D0-3553-4D7A-9117-EA4D19C3434D"))) {
      Switch (ToInteger (Arg2)) {
        Case (0) {
          Return (Buffer () {0x21, 0x03})   // function 0 (self), 5, 8, 9
        }
        Case (5) {
          Return (1)                        // ignore the boot configuration: the OS assigns BARs / windows
        }
        Case (8) {
          Return (1)
        }
        Case (9) {
          Return (Package () {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFF})
        }
        Default {
        }
      }
    }
    Return (Buffer () {0})
  }

  // Root port (bus 0, device 0, function 0).
  Device (RP1) {
    Name (_ADR, 0x00000000)
    Name (_S0W, 0)

    // ACPI companion of the QCA6390 Wi-Fi endpoint (bus 1, device 0, function 0).  Qualcomm's
    // Windows-on-ARM laptop firmware describes the Wi-Fi device under its root port; the driver
    // may expect an ACPI companion (experiment: nothing in the driver reads ACPI properties).
    // Deliberately no _DEP, no power resources and no _PRW: nothing here may need the PEP.
    Device (WLN0) {
      Name (_ADR, 0x00000000)
      Name (_S0W, 0)
    }
  }
}
