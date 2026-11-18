// Platform variables consumed by the PEP0 tables.
// Values from the live munch device tree: qcom,msm-id = <0x164 0x20001> (SM8250-AC, v2.1).
Name (SOID, 0x00000164)   // SoC ID
Name (SVMJ, 0x0002)       // Chip major version
Name (STOR, One)          // Boot storage: 1 = UFS
Name (PUS3, One)          // Primary UFS is 3.x
Name (SUS3, Zero)         // No secondary UFS
Name (PLST, Zero)         // Platform subtype
Name (PSUB, "MTP08250")   // Platform subsystem ID
Method (BSID, 0, NotSerialized) { Return (Zero) }   // Board ID
