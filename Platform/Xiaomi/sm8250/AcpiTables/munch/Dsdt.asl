//
// NOTE: The 3rd parameter (i.e. ComplianceRevision) must be >=2 for 64-bit integer support.
//
DefinitionBlock("DSDT.AML", "DSDT", 0x02, "QCOMM ", "SDM850 ", 3)
{
    Scope(\_SB_) {

		// Include("addSub.asl")
        Include("dsdt_common.asl")
		// Include("cust_dsdt.asl") 

        // Force the primary controller into a host-only Windows path.  VBUS is
        // supplied by MunchOtgDxe, so the incomplete PMIC Type-C/URS stack is
        // intentionally not exposed to Windows.
        Include("usb_host_test.asl")

		//
        // Buttons
		//
        // Include("cust_arraybutton.asl")

        //
        // Bluetooth
        //
        // Include("wcnss_bt.asl")
   }
}
