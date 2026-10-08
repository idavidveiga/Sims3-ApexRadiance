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
    digest = hashlib.sha256(data).hexdigest()
    if digest != ORIGINAL_UI_SHA256:
        raise ContractError(
            "unknown or modified UI.dll; expected SHA256 " + ORIGINAL_UI_SHA256 +
            ", got " + digest + ". Do not use these EA 1.69 RVAs.")
    results = []
    for method in METHODS:
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
    print("PASS: EA 1.69 original Hair/Hats method contract verified. "
          "This does NOT validate a runtime hook or a speedup.")


if __name__ == "__main__":
    main()
