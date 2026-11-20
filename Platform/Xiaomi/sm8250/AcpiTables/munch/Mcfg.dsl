/*
 * MCFG for munch (SM8250): PCIe root complex 0 only.
 * ECAM at 0x60000000, segment 0, buses 0-1 (bus 0 = root port in the DBI, bus 1 = endpoint through the iATU).
 * Compile: iasl Mcfg.dsl   (the .aml is committed, like DSDT.aml)
 */

[000h 0000   4]                    Signature : "MCFG"    [Memory Mapped Configuration Table]
[004h 0004   4]                 Table Length : 0000003C
[008h 0008   1]                     Revision : 01
[009h 0009   1]                     Checksum : 00
[00Ah 0010   6]                       Oem ID : "QCOM  "
[010h 0016   8]                 Oem Table ID : "QCOMEDK2"
[018h 0024   4]                 Oem Revision : 00007280
[01Ch 0028   4]              Asl Compiler ID : "QCOM"
[020h 0032   4]        Asl Compiler Revision : 00000001

[024h 0036   8]                     Reserved : 0000000000000000

[02Ch 0044   8]                 Base Address : 0000000060000000
[034h 0052   2]         Segment Group Number : 0000
[036h 0054   1]             Start Bus Number : 00
[037h 0055   1]               End Bus Number : 01
[038h 0056   4]                     Reserved : 00000000
