// Filesystem Drivers
FILE DRIVER = 80FB68D4-7C52-4AFE-A91E-D3DDADB5C54F {
    SECTION PE32 = Platform/EFI_Binaries/Drivers/fsdrivers/ntfs_aa64.efi
    SECTION UI = "NtfsDxe"
}

FILE DRIVER = 2920E524-AD21-499E-9F4A-466BFDC3BFFB {
    SECTION PE32 = Platform/EFI_Binaries/Drivers/fsdrivers/exfat_aa64.efi
    SECTION UI = "ExfatDxe"
}

FILE DRIVER = 7DDA7772-B8F5-4859-9DBA-0D6F2DBA4AF1 {
    SECTION PE32 = Platform/EFI_Binaries/Drivers/fsdrivers/ext2_aa64.efi
    SECTION UI = "Ext2Dxe"
}

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
