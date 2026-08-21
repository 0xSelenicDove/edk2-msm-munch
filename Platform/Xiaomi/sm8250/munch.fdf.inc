// ACPI Tables
FILE FREEFORM = 7E374E25-8E01-4FEE-87F2-390C23C606CD {   
  SECTION RAW = Platform/Xiaomi/sm8250/AcpiTables/munch/DSDT.aml
  SECTION RAW = Silicon/Qualcomm/sm8250/AcpiTables/Madt.aml
  SECTION RAW = Silicon/Qualcomm/sm8250/AcpiTables/Facp.aml
  SECTION RAW = Silicon/Qualcomm/sm8250/AcpiTables/Gtdt.aml
  SECTION RAW = Silicon/Qualcomm/sm8250/AcpiTables/bgrt.aml
  SECTION UI = "AcpiTables"
}

// Mainline device tree blob
FILE FREEFORM = 25462CDA-221F-47DF-AC1D-259CFAA4E326 {
  SECTION RAW = Platform/Xiaomi/sm8250/FdtBlob_compat/munch.dtb
}

// NTFS Filesystem Driver
FILE DRIVER = 5bd181db-0487-4f1a-ae73-820e165611b4 {
  SECTION PE32 = Platform/Xiaomi/sm8250/Drivers/ntfs_aa64.efi
  SECTION UI = "NtfsDxe"
}

// Diagnostic guarded PM8150B OTG boost for pre-attached DFP devices.
INF Platform/Xiaomi/sm8250/Drivers/MunchPmicVersionDxe/MunchPmicVersionDxe.inf
INF Platform/Xiaomi/sm8250/Drivers/MunchOtgDxe/MunchOtgDxe.inf
