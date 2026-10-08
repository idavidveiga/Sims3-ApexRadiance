#!/usr/bin/env python3
"""Read-only original-CAS UI.dll contract audit for the user's EA App 1.69.

Requires the user's own extracted UI.dll as input; no EA DLL is bundled.
Checks exact original assembly and method IL digests, then verifies the
managed Hair/Hats calls. Refuses unknown builds. Does not patch the game.

Usage:
  python tools/verify_hair_ui_contract.py path/to/original/UI.dll
"""
from __future__ import annotations

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path

ORIGINAL_UI_SHA256 = "c78716f1eb0191f35b12eb8dfa4b47ef1bc1e22edcf88234eb633569074dec10"

@dataclass(frozen=True)
class Method:
    name: str
    token: int
    rva: int
    length: int
    digest: str
    expected_calls: int = 0
    callee: int = 0

# Exact RVA, token and original IL hash from the user's gameplay.package UI.dll.
# A full-assembly hash check is required before any of these addresses are used.
METHODS = (
    Method("CASHair.SetHairTypeCategory", 0x060018E4, 0x9EA74, 215,
           "66436c8539322513040d181f1fa4ae368ed4041fc7828e76c153e5c1a3684e8d", 1, 0x06001918),
    Method("CASHair.RefreshHairGrid", 0x060018EA, 0x9F2F1, 14,
           "dc5e72a83db25eac37a584255fb907eb7ec75149f1132a2e95f6c45c3d36be74", 1, 0x06001918),
    Method("CASHair.OnTrashButtonClick", 0x060018ED, 0x9F37C, 127,
           "ac18b60d9ab305efa6812ce86bdab76f6f92dc5b09a15693eaeb423b5b6b249e", 1, 0x06001918),
    Method("CASHair.OnSaveButtonClick", 0x060018F0, 0x9F448, 353,
           "904c375336297c0e17697433bd429f52e9c636e1ac0b8832279184b86ae540a6", 1, 0x06001918),
    Method("CASHair.OnUndo", 0x060018F9, 0x9F8E7, 14,
           "544664f8de09b35185d617611aa0ade51327d85e73f001bc50832fbfda663a61", 1, 0x06001918),
    Method("CASHair.OnRedo", 0x060018FA, 0x9F8F6, 14,
           "544664f8de09b35185d617611aa0ade51327d85e73f001bc50832fbfda663a61", 1, 0x06001918),
    Method("CASHair.PopulateTypesGrid", 0x06001918, 0xA0464, 1621,
           "e0866c4327c0eec9ae55485aee7601e6168aa651b0f201e168c440dadc10bd15", 2, 0x0600191B),
    Method("CASHair.AddHairTypeGridItem", 0x0600191B, 0xA0CB0, 292,
           "17a3648b46d839af579a406c81367e40cb1822bc0f04723858a7b3e92b32b365"),
)



# A second complete user-supplied UI.dll has the same CASHair loop shape
# (1,621 IL bytes, 528 instructions, two finally handlers) but DIFFERENT
# MethodDef/MemberRef tokens. Never mix the variants. Exact whole-file
# hashes are required before any original RVA/token is considered.
ALTERNATE_UI_SHA256 = "91a2ed9815ca2f42af19f7f680b8a6c206e65e0ce23494ed210fb74658b89e79"
ALTERNATE_METHODS = (
    Method("CASHair.SetHairTypeCategory", 0x06001216, 0x4C3D4, 215,
           "523fcf89999927405e13a4ca4cc8656003c026567744addf98d0c299737d20c0", 1, 0x0600124A),
    Method("CASHair.RefreshHairGrid", 0x0600121C, 0x4CC51, 14,
           "b35bf8e7c5a976788eeaad4b7535dd32f99b2d1af9545724ffd3f9d587c984fa", 1, 0x0600124A),
    Method("CASHair.OnTrashButtonClick", 0x0600121F, 0x4CCDC, 127,
           "e9782667ab56c80e1bec273bf65fb49fc1cafd7f9b65ad5a54fee378e534a5bb", 1, 0x0600124A),
    Method("CASHair.OnSaveButtonClick", 0x06001222, 0x4CDA8, 353,
           "346da0fc81044ad10b04b69c058785e5a26ae1be08cad7e963c283f05d39c8aa", 1, 0x0600124A),
    Method("CASHair.OnUndo", 0x0600122B, 0x4D247, 14,
           "c1c8f659beaf3ec61fffe49d42c893e26d67eb7d7fb75b76d404932e696b7412", 1, 0x0600124A),
    Method("CASHair.OnRedo", 0x0600122C, 0x4D256, 14,
           "c1c8f659beaf3ec61fffe49d42c893e26d67eb7d7fb75b76d404932e696b7412", 1, 0x0600124A),
    Method("CASHair.PopulateTypesGrid", 0x0600124A, 0x4DDC4, 1621,
           "020e281cc6f834ad0d58dfa26dfb8244245dd749224be19bb8504485a04c54c4", 2, 0x0600124D),
    Method("CASHair.AddHairTypeGridItem", 0x0600124D, 0x4E610, 292,
           "a6a823ba9853484b362200ad6be1d5c6e9994424b3edaf985026d2e7d883d964"),
)

@dataclass(frozen=True)
class UIVariant:
    name: str
    digest: str
    methods: tuple[Method, ...]
    sleep_memberref: int

UI_VARIANTS = (
    UIVariant("EA App 1.69 package", ORIGINAL_UI_SHA256, METHODS, 0x0A000023),
    UIVariant("user-supplied alternate UI.dll", ALTERNATE_UI_SHA256,
              ALTERNATE_METHODS, 0x0A00001D),
)

def identify_variant(data: bytes) -> UIVariant:
    digest = hashlib.sha256(data).hexdigest()
    for variant in UI_VARIANTS:
        if digest == variant.digest:
            return variant
    raise ContractError(
        "unknown or modified UI.dll; expected a known whole-file SHA256 "
        "and version-specific tokens, got " + digest +
        ". Do not use EA 1.69 or alternate RVAs on this image.")


class ContractError(ValueError):
    pass


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def rva_offset(data: bytes, rva: int) -> int:
    if len(data) < 256 or data[:2] != b"MZ":
        raise ContractError("not a Windows PE image")
    try:
        pe = _u32(data, 0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise ContractError("invalid PE header")
        coff = pe + 4
        nsections = _u16(data, coff + 2)
        optional = coff + 20
        option_size = _u16(data, coff + 16)
        if _u16(data, optional) != 0x10B:
            raise ContractError("expected a 32-bit PE file")
        table = optional + option_size
        for i in range(nsections):
            o = table + 40 * i
            size_virtual, virtual_address, size_raw, offset_raw = struct.unpack_from(
                "<IIII", data, o + 8)
            if virtual_address <= rva < virtual_address + max(size_virtual, size_raw):
                result = offset_raw + rva - virtual_address
                if result >= len(data):
                    raise ContractError("method RVA extends outside file")
                return result
    except (IndexError, struct.error) as ex:
        raise ContractError("truncated PE sections") from ex
    raise ContractError("method RVA is not mapped by a PE section")


def il_bytes_at(data: bytes, rva: int) -> bytes:
    try:
        offset = rva_offset(data, rva)
        first = data[offset]
        if first & 3 == 2:  # ECMA-335 tiny IL method
            size = first >> 2
            start = offset + 1
        elif first & 3 == 3:  # ECMA-335 fat IL method
            flags = _u16(data, offset)
            header_dwords = (flags >> 12) & 15
            if header_dwords < 3:
                raise ContractError("invalid fat method header")
            size = _u32(data, offset + 4)
            start = offset + header_dwords * 4
        else:
            raise ContractError("unrecognized IL method header")
        end = start + size
        if end > len(data):
            raise ContractError("truncated IL method")
        return data[start:end]
    except (IndexError, struct.error) as ex:
        raise ContractError("truncated IL header") from ex


def call_count(il: bytes, method_token: int) -> int:
    # Full-assembly and method SHA gates make a raw byte pattern exact for
    # these fixed originals; do not use it as a general IL disassembler.
    token = method_token.to_bytes(4, "little")
    return il.count(b"\x28" + token) + il.count(b"\x6F" + token)


def verify(data: bytes) -> list[str]:
    variant = identify_variant(data)
    results = [
        "PASS: recognized " + variant.name +
        "; Sleep(uint32) MemberRef=" + f"{variant.sleep_memberref:#010x}"
    ]
    for method in variant.methods:
        body = il_bytes_at(data, method.rva)
        actual = hashlib.sha256(body).hexdigest()
        if len(body) != method.length or actual != method.digest:
            raise ContractError(
                method.name + ": original IL mismatch; fail closed")
        if method.callee:
            calls = call_count(body, method.callee)
            if calls != method.expected_calls:
                raise ContractError(method.name + ": original call count mismatch")
            results.append(
                f"PASS: {method.name} token={method.token:#010x}, "
                f"IL={len(body)} bytes, {calls} call(s) to {method.callee:#010x}")
        else:
            results.append(
                f"PASS: {method.name} token={method.token:#010x}, "
                f"IL={len(body)} bytes")
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original_ui_dll", type=Path)
    args = parser.parse_args()
    try:
        reports = verify(args.original_ui_dll.read_bytes())
    except (OSError, ContractError) as ex:
        parser.exit(2, "FAIL: " + str(ex) + "\n")
    print("\n".join(reports))
    print("PASS: identified original Hair/Hats method contract verified. "
          "This does NOT validate a runtime hook or a speedup.")


if __name__ == "__main__":
    main()
