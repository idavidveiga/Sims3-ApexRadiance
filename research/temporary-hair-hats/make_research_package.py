#!/usr/bin/env python3
"""Research-only packaging of an x86 .NET 2 MonoPatcher experiment into one TS3 S3SA.
Never include its result in Apex's normal release or ship MonoPatcher in Apex.
"""
from __future__ import annotations
import argparse
import struct
from pathlib import Path

RESOURCE_TYPE = 0x073FAA07
RESOURCE_GROUP = 0
RESOURCE_INSTANCE = 0xA9E340F8E1160201
GAME_VERSION = "1.0.0.18"


def make_s3sa(assembly: bytes) -> bytes:
    if not assembly.startswith(b"MZ") or b"BSJB" not in assembly:
        raise ValueError("Expected an actual .NET PE assembly")
    chunks = (len(assembly) + 511) // 512
    if chunks > 65535:
        raise ValueError("Assembly too large for research S3SA wrapper")
    pad = assembly.ljust(chunks * 512, b"\0")
    # All-zero metadata table is the simple pass-through S3SA encoding:
    # no data-dependent obfuscation; preserves every original assembly byte.
    return (b"\x02" + struct.pack("<I", len(GAME_VERSION)) +
            GAME_VERSION.encode("utf-16le") +
            struct.pack("<I", 0x2BC4F79F) + bytes(64) +
            struct.pack("<H", chunks) + bytes(chunks * 8) + pad)


def make_package(assembly: bytes) -> bytes:
    body = make_s3sa(assembly)
    resource_offset = 96
    index_offset = resource_offset + len(body)
    index = (struct.pack("<III", 3, RESOURCE_TYPE, RESOURCE_GROUP) +
             struct.pack("<IIIIII",
                         RESOURCE_INSTANCE >> 32,
                         RESOURCE_INSTANCE & 0xFFFFFFFF,
                         resource_offset, len(body) | 0x80000000,
                         len(body), 0x00010000))
    header = bytearray(96)
    header[:4] = b"DBPF"
    struct.pack_into("<I", header, 4, 2)
    struct.pack_into("<I", header, 0x24, 1)
    struct.pack_into("<I", header, 0x2C, len(index))
    struct.pack_into("<I", header, 0x3C, 3)
    struct.pack_into("<I", header, 0x40, index_offset)
    return bytes(header) + body + index


def test_roundtrip(payload: bytes, assembly: bytes) -> None:
    # Structural validation of all fields, not a TS3 runtime test.
    assert payload[:4] == b"DBPF"
    count = struct.unpack_from("<I", payload, 0x24)[0]
    index_pos = struct.unpack_from("<I", payload, 0x40)[0]
    assert count == 1
    flags, typ, group = struct.unpack_from("<III", payload, index_pos)
    assert flags == 3 and typ == RESOURCE_TYPE and group == RESOURCE_GROUP
    hi, lo, ofs, packed_size, usize, marker = struct.unpack_from(
        "<IIIIII", payload, index_pos + 12)
    assert ((hi << 32) | lo) == RESOURCE_INSTANCE
    assert marker == 0x00010000 and packed_size & 0x80000000
    body = payload[ofs:ofs+usize]
    assert len(body) == usize and len(body) == (packed_size & 0x7FFFFFFF)
    assert body[0] == 2
    n = struct.unpack_from("<I", body, 1)[0]
    pos = 5 + n * 2 + 4 + 64
    chunks = struct.unpack_from("<H", body, pos)[0]
    assert body[pos+2:pos+2+chunks*8] == bytes(chunks*8)
    recovered = body[pos+2+chunks*8:pos+2+chunks*8+len(assembly)]
    assert recovered == assembly


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("managed_dll", type=Path)
    ap.add_argument("out_package", type=Path)
    args = ap.parse_args()
    original = args.managed_dll.read_bytes()
    package = make_package(original)
    test_roundtrip(package, original)
    args.out_package.parent.mkdir(parents=True, exist_ok=True)
    args.out_package.write_bytes(package)
    print("PASS: standalone temporary research package validated by roundtrip")


if __name__ == "__main__":
    main()
