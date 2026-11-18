#!/usr/bin/env python3
"""Build a K40S USB-only IORT from measured SM8250 hardware data."""

from __future__ import annotations

import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Silicon/Qualcomm/sm8150/AcpiTables/BuiltIn/IORT.aml"
OUTPUT = ROOT / "Silicon/Qualcomm/sm8250/AcpiTables/IortUsb.aml"

# Captured from the munch runtime DT in TWRP.  The first two DT interrupts are
# the secure/non-secure global SMMU interrupts (GSIV 96/97).  IORT describes
# the non-secure one.  The final nine DT interrupts are the PMU interrupts.
CONTEXT_GSIVS = (
    *range(129, 151),
    *range(213, 225),
    *range(347, 378),
    *range(427, 442),
    444,
    450,
    451,
    453,
    455,
    456,
    457,
)
PMU_GSIVS = (*range(722, 730), 739)


def u16(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<H", blob, offset)[0]


def u32(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<I", blob, offset)[0]


def main() -> None:
    source = SOURCE.read_bytes()
    if source[:4] != b"IORT":
        raise SystemExit("reference file is not an IORT table")

    reference_named_component: bytes | None = None
    offset = u32(source, 40)
    for _ in range(u32(source, 36)):
        length = u16(source, offset + 1)
        node = source[offset : offset + length]
        if node[0] == 1 and b"\\_SB.USB0" in node:
            reference_named_component = node
        offset += length

    if reference_named_component is None:
        raise SystemExit("could not locate the reference USB0 named component")

    # IORT SMMUv1/v2 node.  Offsets are relative to the start of this node.
    global_interrupt_offset = 60
    context_interrupt_offset = 76
    pmu_interrupt_offset = context_interrupt_offset + len(CONTEXT_GSIVS) * 8
    smmu_length = pmu_interrupt_offset + len(PMU_GSIVS) * 8
    smmu = bytearray()
    smmu += struct.pack("<BHBIII", 3, smmu_length, 0, 0, 0, 0)
    smmu += struct.pack(
        "<QQIIIIIII",
        0x15000000,             # apps SMMU base
        0x00100000,             # runtime-DT register span
        3,                      # Arm CoreLink MMU-500
        0,
        global_interrupt_offset,
        len(CONTEXT_GSIVS),
        context_interrupt_offset,
        len(PMU_GSIVS),
        pmu_interrupt_offset,
    )
    smmu += struct.pack("<IIII", 97, 0, 0, 0)
    smmu += b"".join(struct.pack("<II", gsiv, 1) for gsiv in CONTEXT_GSIVS)
    smmu += b"".join(struct.pack("<II", gsiv, 1) for gsiv in PMU_GSIVS)
    if len(smmu) != smmu_length:
        raise SystemExit("internal SMMU-node length mismatch")

    # Preserve Qualcomm's named-component properties and device-name encoding,
    # but route munch USB SID 0 to the newly generated SMMU node at table 0x30.
    usb = bytearray(reference_named_component)
    mapping_offset = u32(usb, 12)
    if u32(usb, 8) != 1 or mapping_offset + 20 != len(usb):
        raise SystemExit("unexpected reference USB0 ID-mapping layout")
    struct.pack_into("<I", usb, mapping_offset + 8, 0)       # OutputBase/SID
    struct.pack_into("<I", usb, mapping_offset + 12, 48)    # SMMU table offset

    table = bytearray(source[:48] + smmu + usb)
    struct.pack_into("<I", table, 4, len(table))
    struct.pack_into("<I", table, 36, 2)
    struct.pack_into("<I", table, 40, 48)
    table[9] = 0
    table[9] = (-sum(table)) & 0xFF
    OUTPUT.write_bytes(table)
    print(f"wrote {OUTPUT} ({len(table)} bytes, checksum=0x{sum(table) & 0xff:02x})")


if __name__ == "__main__":
    main()
