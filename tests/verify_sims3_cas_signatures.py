#!/usr/bin/env python3
"""Check TS3 CASt/CAS managed method signatures against the real EA assemblies.

Uses the supplied, locally extracted UI.dll and SimIFace.dll. No third-party Python packages.
Do not commit or redistribute the proprietary game DLLs.
"""
import argparse
import struct
from pathlib import Path


def parse_cli(path):
    b = Path(path).read_bytes()
    u16 = lambda p: struct.unpack_from("<H", b, p)[0]
    u32 = lambda p: struct.unpack_from("<I", b, p)[0]
    u64 = lambda p: struct.unpack_from("<Q", b, p)[0]
    pe = u32(0x3C)
    opt = pe + 24
    section_base = opt + u16(pe + 20)
    sections = []
    for i in range(u16(pe + 6)):
        p = section_base + i * 40
        sections.append((u32(p + 12), max(u32(p + 8), u32(p + 16)), u32(p + 20)))

    def rva_offset(rva):
        for virtual_address, length, raw in sections:
            if virtual_address <= rva < virtual_address + length:
                return raw + rva - virtual_address
        raise ValueError(f"RVA not in PE sections: {rva:#x}")

    cli_dir = u32(opt + 96 + 14 * 8)
    md = rva_offset(u32(rva_offset(cli_dir) + 8))
    if b[md:md + 4] != b"BSJB":
        raise ValueError("not CLR metadata")
    p = (md + 16 + u32(md + 12) + 3) & ~3
    stream_count = u16(p + 2)
    p += 4
    streams = {}
    for _ in range(stream_count):
        offset = u32(p)
        end = b.index(0, p + 8)
        name = b[p + 8:end].decode("ascii")
        streams[name] = md + offset
        p = (end + 4) & ~3

    strings = streams["#Strings"]
    blobs = streams["#Blob"]
    p = streams.get("#~", streams.get("#-"))
    p += 6
    heaps = b[p]
    p += 2
    valid = u64(p)
    p += 16
    row_count = {}
    for table in range(64):
        if valid & (1 << table):
            row_count[table] = u32(p)
            p += 4

    s_size = 4 if heaps & 1 else 2
    b_size = 4 if heaps & 4 else 2
    g_size = 4 if heaps & 2 else 2
    ix_size = lambda t: 2 if row_count.get(t, 0) < 65536 else 4
    def coded_size(tables, bits):
        return 2 if max((row_count.get(t, 0) for t in tables), default=0) < (1 << (16 - bits)) else 4

    sizes = {
        0: 2 + s_size + g_size * 3,
        1: coded_size([0, 1, 26, 35], 2) + s_size * 2,
        2: 4 + s_size * 2 + coded_size([2, 1, 27], 2) + ix_size(4) + ix_size(6),
        3: ix_size(4),
        4: 2 + s_size + b_size,
        5: ix_size(6),
        6: 8 + s_size + b_size + ix_size(8),
        7: ix_size(8),
        8: 4 + s_size,
    }
    offsets = {}
    for t in range(9):
        if row_count.get(t):
            offsets[t] = p
            p += sizes[t] * row_count[t]

    read_index = lambda p, n: u32(p) if n == 4 else u16(p)
    def text_at(index):
        if not index:
            return ""
        pos = strings + index
        return b[pos:b.index(0, pos)].decode("utf-8", "replace")

    def blob_at(index):
        if not index:
            return b""
        pos = blobs + index
        head = b[pos]
        if head < 128:
            length, length_size = head, 1
        elif head < 192:
            length, length_size = ((head & 63) << 8) | b[pos + 1], 2
        else:
            length = ((head & 31) << 24) | (b[pos + 1] << 16) | (b[pos + 2] << 8) | b[pos + 3]
            length_size = 4
        return b[pos + length_size:pos + length_size + length]

    methods = []
    for i in range(row_count[6]):
        q = offsets[6] + i * sizes[6] + 8
        name_index = read_index(q, s_size)
        sig_index = read_index(q + s_size, b_size)
        methods.append((text_at(name_index), blob_at(sig_index).hex()))

    types = []
    for i in range(row_count[2]):
        q = offsets[2] + i * sizes[2] + 4
        name_index = read_index(q, s_size)
        namespace_index = read_index(q + s_size, s_size)
        fields_at = q + s_size * 2 + coded_size([2, 1, 27], 2)
        method_at = fields_at + ix_size(4)
        types.append((text_at(namespace_index), text_at(name_index),
                      read_index(fields_at, ix_size(4)), read_index(method_at, ix_size(6))))

    owned_methods = {}
    for i, (namespace, klass, _, start) in enumerate(types):
        end = types[i + 1][3] if i + 1 < len(types) else len(methods) + 1
        for name, sig in methods[start - 1:end - 1]:
            owned_methods[(namespace, klass, name)] = sig

    fields = []
    for i in range(row_count[4]):
        q = offsets[4] + i * sizes[4] + 2
        fields.append((text_at(read_index(q, s_size)), blob_at(read_index(q + s_size, b_size)).hex()))

    owned_fields = {}
    for i, (namespace, klass, field_start, _) in enumerate(types):
        field_end = types[i + 1][2] if i + 1 < len(types) else len(fields) + 1
        owned_fields[(namespace, klass)] = fields[field_start - 1:field_end - 1]

    return owned_methods, owned_fields


def main():
    arg = argparse.ArgumentParser()
    arg.add_argument("--ui", type=Path, required=True)
    arg.add_argument("--simiface", type=Path, required=True)
    args = arg.parse_args()

    ui_methods, _ = parse_cli(args.ui)
    api_methods, fields = parse_cli(args.simiface)
    requirements = [
        (ui_methods, ("Sims3.UI.CAS", "CASClothingCategory", "PopulateTypesGrid"), "200001"),
        (ui_methods, ("Sims3.UI.CAS", "CASClothingCategory", "PopulateGrid"), "200001"),
        (ui_methods, ("Sims3.UI.CAS", "CASHair", "PopulateTypesGrid"), "20010102"),
        (ui_methods, ("Sims3.UI", "UIManager", "GetCASThumbnailImage"), "00011295f8118099"),
        (api_methods, ("Sims3.SimIFace.CAS", "ICASUtils", "PartDataNumPresets"), "200109118508"),
        (api_methods, ("Sims3.SimIFace.CAS", "ICASUtils", "PartDataGetPresetId"), "20020911850809"),
        (api_methods, ("Sims3.SimIFace.CAS", "ICASUtils", "PartDataAddDesignPreset"), "2002091185080e"),
        (api_methods, ("Sims3.SimIFace.CAS", "ICASUtils", "PartDataRemoveDesignPreset"), "20020111850809"),
        (api_methods, ("Sims3.SimIFace", "IUIManager", "GetCASThumbnailImage"), "200109118580"),
        (api_methods, ("Sims3.SimIFace", "IWorld", "ObjectDesigner_GetPatternThumbnail"), "2003090b0b1d05"),
    ]
    failures = []
    for mapping, key, expected in requirements:
        actual = mapping.get(key)
        if actual != expected:
            failures.append(f"{'.'.join(key)}: expected {expected}, got {actual}")
    resource_fields = fields.get(("Sims3.SimIFace", "ResourceKey"), [])
    if resource_fields[:3] != [("TypeId", "0609"), ("GroupId", "0609"), ("InstanceId", "060b")]:
        failures.append(f"ResourceKey fields changed: {resource_fields[:3]}")
    if failures:
        for failure in failures:
            print("FAIL:", failure)
        raise SystemExit(1)
    print(f"PASS: {len(requirements)} managed method signatures and ResourceKey field order")
    print("NOTE: CLI signatures do NOT prove the native x86 Mono InternalCall calling convention.")
    print("NOTE: No CAS grid patch, in-game speedup, or thumbnail lifetime is validated here.")


if __name__ == "__main__":
    main()
