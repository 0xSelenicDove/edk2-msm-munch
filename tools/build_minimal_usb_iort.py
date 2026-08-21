#!/usr/bin/env python3
"""Build the SM8250 USB-only IORT from the matching SM8150 table."""

from __future__ import annotations

import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Silicon/Qualcomm/sm8150/AcpiTables/BuiltIn/IORT.aml"
OUTPUT = ROOT / "Silicon/Qualcomm/sm8250/AcpiTables/IortUsb.aml"


def u16(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<H", blob, offset)[0]


def u32(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<I", blob, offset)[0]


def main() -> None:
    source = SOURCE.read_bytes()
    if source[:4] != b"IORT":
        raise SystemExit("reference file is not an IORT table")

    nodes: list[bytes] = []
    offset = u32(source, 40)
    for _ in range(u32(source, 36)):
        length = u16(source, offset + 1)
        node = source[offset : offset + length]
        if node[0] == 3 and struct.unpack_from("<Q", node, 16)[0] == 0x15000000:
            nodes.append(node)
        elif node[0] == 1 and b"\\_SB.USB0" in node:
            nodes.append(node)
        offset += length

    if len(nodes) != 2 or nodes[0][0] != 3 or b"\\_SB.USB0" not in nodes[1]:
        raise SystemExit("could not locate the expected SMMU and USB0 nodes")

    table = bytearray(source[:48] + b"".join(nodes))
    struct.pack_into("<I", table, 4, len(table))
    struct.pack_into("<I", table, 36, len(nodes))
    struct.pack_into("<I", table, 40, 48)
    table[9] = 0
    table[9] = (-sum(table)) & 0xFF
    OUTPUT.write_bytes(table)
    print(f"wrote {OUTPUT} ({len(table)} bytes, checksum=0x{sum(table) & 0xff:02x})")


if __name__ == "__main__":
    main()
