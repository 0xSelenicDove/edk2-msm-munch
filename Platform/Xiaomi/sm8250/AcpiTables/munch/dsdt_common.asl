// EMUL is the only BSP placeholder referenced by an enabled device (UFS0).
Name(EMUL, 0xffffffff)  		// Holds the Device emulation type

//Audio Drivers
// Include("audio.asl")
		
        //
        // Storage - UFS/SD 
        //
        Include("ufs.asl")
        // Include("sdc.asl") // No SD support on polaris
        
        //
        // ASL Bridge Device
        //
		// Include("abd.asl")
			
//
// PMIC driver 
//
// Include("pmic_core.asl")

//
// PMICTCC driver
//
// Include("pmic_batt.asl")
			
        // Include("pep.asl")
        // Include("bam.asl")
        // Include("buses.asl")

        // MPROC Drivers (PIL Driver and Subsystem Drivers)
        // Include("win_mproc.asl")
        // Include("syscache.asl")
        // Include("HoyaSmmu.asl")
        // Include("graphics.asl")

        // Include("SCM.asl");

		//
		// SPMI driver 
        //
        // Include("spmi.asl")

        //
        // TLMM controller.
        //
        // Include("qcgpio.asl")

        // Include("pcie.asl")

        // Include("cbsp_mproc.asl")

        // Include("adsprpc.asl")
      
        //
        // RemoteFS
        // 
        // Include("rfs.asl")

        //
        // Qualcomm IPA
        // Include("ipa.asl")
		
        // Include("gsi.asl")
		
        // Include("qcdb.asl")    

        // copied from sm7325, need to check   
		Include("Pep_lpi.asl")

// QUPV3 GPI device node and resources
//
// Include("qgpi.asl")
