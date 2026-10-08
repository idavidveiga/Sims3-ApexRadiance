#!/usr/bin/env python3
"""Read-only TS3 PE32 triage. Does not execute or alter the inspected executable.

Reports whether it is responsible to locate the native Mono JIT using static
bytes. No extra Python packages and no external patching frameworks are needed.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from math import log2
from pathlib import Path
import struct

KNOWN = {0x6707155C: "EA App 1.69.47.024017",
         0x52DEC247: "Steam 1.67.2.024037",
         0x52D872DA: "Retail 1.67.2.024002",
         0x568D4BAC: "EA 1.69.43.024017"}
STEAM_RESOLVER = bytes.fromhex("81 EC 08 08 00 00 53 55 8B AC 24 14 08")


def entropy(data):
    if not data:
        return 0.0
    bins = Counter(data)
    length = len(data)
    return -sum((n / length) * log2(n / length) for n in bins.values())


def analyze(path):
    data = Path(path).read_bytes()
    if len(data) < 0x200 or data[:2] != b"MZ":
        raise ValueError("Not a DOS/PE executable")
    u16 = lambda p: struct.unpack_from("<H", data, p)[0]
    u32 = lambda p: struct.unpack_from("<I", data, p)[0]
    pe = u32(0x3C)
    if pe + 24 >= len(data) or data[pe:pe+4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    machine, count = u16(pe+4), u16(pe+6)
    stamp, opt_size = u32(pe+8), u16(pe+20)
    opt = pe+24
    if u16(opt) != 0x10B or machine != 0x14C:
        raise ValueError("Expected a 32-bit x86 PE (PE32, I386)")
    entry_rva = u32(opt+16)
    image_base = u32(opt+28)
    sections = []
    header_end = opt + opt_size
    for i in range(count):
        pos = header_end + i*40
        if pos+40 > len(data):
            raise ValueError("Truncated section table")
        name = data[pos:pos+8].split(b"\0")[0].decode("ascii", "replace")
        virtual_size, rva, size, raw = (u32(pos+8), u32(pos+12),
                                       u32(pos+16), u32(pos+20))
        if raw+size > len(data):
            raise ValueError(f"Truncated {name} section")
        contents = data[raw:raw+size]
        sections.append({"name": name, "rva": rva, "size": size,
                         "virtual_size": virtual_size, "raw_offset": raw,
                         "entropy": round(entropy(contents), 4)})
    owner = next((s["name"] for s in sections
                  if s["rva"] <= entry_rva <
                  s["rva"] + max(s["virtual_size"], s["size"])), None)
    text_section = next((s for s in sections if s["name"] == ".text"), None)
    if not text_section:
        raise ValueError("Missing .text section")
    executable_code = data[text_section["raw_offset"]:
                           text_section["raw_offset"]+text_section["size"]]
    protected = owner == ".ooa" and text_section["entropy"] >= 7.95
    return {
        "file_name": Path(path).name,
        "file_size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "machine": "I386", "version": KNOWN.get(stamp, "unknown"),
        "pe_timestamp_hex": f"0x{stamp:08X}",
        "pe_timestamp_utc": datetime.fromtimestamp(stamp, timezone.utc).isoformat(),
        "image_base_hex": f"0x{image_base:08X}",
        "entrypoint_rva_hex": f"0x{entry_rva:08X}",
        "entrypoint_section": owner,
        "section_entropies": {s["name"]: s["entropy"] for s in sections},
        "steam_resolver_signature_hits": executable_code.count(STEAM_RESOLVER),
        "protected_static_image": protected,
        "verified_native_mono_entrypoint": False,
        "advice": (
            "Static executable cannot validate the native Mono ABI/JIT "
            "entrypoints; collect read-only runtime diagnostics in Apex "
            "after normal game initialization."
            if protected else
            "Inspect the live Mono runtime layout and calling convention "
            "before enabling hooks.")
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--json", action="store_true",
                        help="Print machine-readable analysis")
    args = parser.parse_args()
    report = analyze(args.executable)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"Game: {report['version']} ({report['machine']})")
        print(f"SHA256: {report['sha256']}")
        print(f"Entry point: {report['entrypoint_rva_hex']} "
              f"in {report['entrypoint_section']}")
        print(f".text entropy: {report['section_entropies']['.text']:.4f} bits/byte")
        print(f"Steam Mono resolver signatures on disk: "
              f"{report['steam_resolver_signature_hits']}")
        print(f"Protected static code image: {report['protected_static_image']}")
        print(f"Native Mono hook validated: "
              f"{report['verified_native_mono_entrypoint']}")
        print(report["advice"])


if __name__ == "__main__":
    main()
