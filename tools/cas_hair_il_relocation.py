#!/usr/bin/env python3
"""Offline-only EA CAS Hair/Hats parent-method IL continuation experiment.

Reads a USER-SUPPLIED, SHA256-verified UI.dll variant. It neither changes the
assembly nor installs a Mono hook. No EA bytes are embedded or redistributed.

The single insertion calls Simulator.Sleep(0) *after* the entire part/preset
group and *before* the original enumerator MoveNext call (IL_0514).
The original first-entry and filtered-part branches target MoveNext AFTER the
insertion; they do not pause until at least one full group has completed.

This is structural validation ONLY. A valid IL/EH layout is NOT proof that
Sleep(0) is safe inside the game's MINT interpreter or that CAS is faster.
No .package, UI.dll patch or Apex .asi optimization is generated here.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct

from verify_hair_ui_contract import (
    ContractError, il_bytes_at, identify_variant, rva_offset, verify,
)

# All operands and opcodes in the two *verified* 1621-byte methods.
BYTE = set(range(0x0E, 0x14)) | {0x1F, 0xFE12}
WORD = set(range(0xFE09, 0xFE0F))
DWORD = {0x20, 0x22}
QWORD = {0x21, 0x23}
TOKEN = {0x27, 0x28, 0x29, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74,
         0x75, 0x79, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81,
         0x8C, 0x8D, 0x8F, 0xA3, 0xA4, 0xA5, 0xC2, 0xC6, 0xD0,
         0xFE06, 0xFE07, 0xFE15, 0xFE16, 0xFE1C}
SHORT = set(range(0x2B, 0x38)) | {0xDE}
LONG = set(range(0x38, 0x45)) | {0xDD}
NONE = (set(range(0x00, 0x0E)) | set(range(0x14, 0x1F)) |
        {0x25, 0x26, 0x2A, 0x76, 0x7A, 0x8E, 0xDC, 0xDF, 0xE0} |
        set(range(0x46, 0x6F)) | set(range(0x82, 0x8C)) |
        set(range(0x90, 0xA3)) | set(range(0xB3, 0xBB)) |
        set(range(0xD1, 0xDC)) |
        {0xFE00, 0xFE01, 0xFE02, 0xFE03, 0xFE04, 0xFE05,
         0xFE0F, 0xFE11, 0xFE13, 0xFE14, 0xFE17, 0xFE18,
         0xFE1A, 0xFE1D, 0xFE1E})
YIELD_PC = 0x514
EXPECTED_ORIGINAL_BYTES = 1621
EXPECTED_INSTRUCTIONS = 528


class RelocationError(ValueError):
    """Refuse any unrecognized method, control flow or exception table."""


@dataclass(frozen=True)
class Instruction:
    pc: int
    op: int
    raw: bytes
    destinations: tuple[int, ...] = ()


def disassemble(il: bytes) -> list[Instruction]:
    insns: list[Instruction] = []
    pc = 0
    while pc < len(il):
        start = pc
        op = il[pc]
        pc += 1
        if op == 0xFE:
            if pc >= len(il):
                raise RelocationError("truncated two-byte opcode")
            op = 0xFE00 | il[pc]
            pc += 1
        if op in BYTE or op in SHORT:
            size = 1
        elif op in WORD:
            size = 2
        elif op in DWORD or op in TOKEN or op in LONG:
            size = 4
        elif op in QWORD:
            size = 8
        elif op == 0x45:
            if pc + 4 > len(il):
                raise RelocationError("truncated switch")
            count = struct.unpack_from("<I", il, pc)[0]
            if count > (len(il) - pc - 4) // 4:
                raise RelocationError("out-of-bounds switch")
            size = 4 + count * 4
        elif op in NONE:
            size = 0
        else:
            raise RelocationError(f"unsupported opcode {op:#x} at {start:#x}")
        end = pc + size
        if end > len(il):
            raise RelocationError(f"truncated operand at {start:#x}")
        destinations: tuple[int, ...] = ()
        if op in SHORT:
            destinations = (end + struct.unpack_from("<b", il, pc)[0],)
        elif op in LONG:
            destinations = (end + struct.unpack_from("<i", il, pc)[0],)
        elif op == 0x45:
            n = struct.unpack_from("<I", il, pc)[0]
            destinations = tuple(end + delta for delta in struct.unpack_from(
                f"<{n}i", il, pc + 4))
        insns.append(Instruction(start, op, il[start:end], destinations))
        pc = end
    boundaries = {i.pc for i in insns} | {len(il)}
    if any(target not in boundaries for i in insns for target in i.destinations):
        raise RelocationError("branch target lands inside an IL operand")
    return insns


def parse_finally(eh: bytes, il_size: int) -> list[tuple[int, ...]]:
    if len(eh) != 52 or eh[:4] != bytes([0x41, 52, 0, 0]):
        raise RelocationError("expected exactly two fat finally clauses")
    clauses = [struct.unpack_from("<6I", eh, pos) for pos in (4, 28)]
    for flags, try_pc, try_len, handler_pc, handler_len, reserved in clauses:
        if (flags != 2 or reserved != 0 or
            try_pc + try_len > il_size or handler_pc + handler_len > il_size or
            try_pc + try_len > handler_pc):
            raise RelocationError("invalid EH range or a non-finally handler")
    return clauses


def method_and_eh(data: bytes, rva: int) -> tuple[bytes, bytes]:
    loc = rva_offset(data, rva)
    if loc + 12 > len(data):
        raise RelocationError("missing fat method header")
    flags = struct.unpack_from("<H", data, loc)[0]
    if flags & 3 != 3 or not flags & 0x08:
        raise RelocationError("expected fat method header with EH section")
    header_len = (flags >> 12) * 4
    if header_len != 12:
        raise RelocationError("unsupported method header")
    il_len = struct.unpack_from("<I", data, loc + 4)[0]
    start = loc + header_len
    eh_at = (start + il_len + 3) & ~3
    if eh_at + 4 > len(data):
        raise RelocationError("missing exception section")
    eh_len = int.from_bytes(data[eh_at + 1:eh_at + 4], "little")
    if eh_at + eh_len > len(data):
        raise RelocationError("truncated exception section")
    return data[start:start + il_len], data[eh_at:eh_at + eh_len]


def relocate_parent_loop(il: bytes, eh: bytes, sleep_token: int,
                         add_hair_token: int) -> tuple[bytes, bytes, dict[int, int]]:
    if len(il) != EXPECTED_ORIGINAL_BYTES:
        raise RelocationError("unexpected original Hair/Hats method size")
    ops = disassemble(il)
    if len(ops) != EXPECTED_INSTRUCTIONS:
        raise RelocationError("unexpected Hair/Hats opcode count")
    clauses = parse_finally(eh, len(il))
    op_at = {op.pc: op for op in ops}
    if (op_at[YIELD_PC].op != 0x11 or
        op_at[0x50F].op != 0x3F or
        op_at[0x50F].destinations != (0x42E,) or
        op_at[0x516].op != 0x6F or
        not any(c[1] < YIELD_PC < c[1] + c[2] for c in clauses)):
        raise RelocationError("complete-part loop boundary differs")
    calls = [op for op in ops if op.op in (0x28, 0x6F) and
             int.from_bytes(op.raw[-4:], "little") == add_hair_token]
    if len(calls) != 2 or {op.pc for op in calls} != {0x3CC, 0x481}:
        raise RelocationError("AddHairTypeGridItem call sites have changed")
    incoming = {i.pc for i in ops if YIELD_PC in i.destinations}
    if incoming != {0x2AC, 0x2F5, 0x421}:
        raise RelocationError("loop-entry/filtered-item branch layout changed")
    # A 0 call argument is an immediate, not a managed pointer.
    insertion = b"\x16\x28" + struct.pack("<I", sleep_token)
    positions: dict[int, int] = {}
    offset = 0
    for ins in ops:
        if ins.pc == YIELD_PC:
            offset += len(insertion)
        positions[ins.pc] = offset
        offset += 5 if ins.op in SHORT else len(ins.raw)
    positions[len(il)] = offset

    relocated = bytearray()
    for ins in ops:
        if ins.pc == YIELD_PC:
            relocated += insertion
        now = len(relocated)
        if positions[ins.pc] != now:
            raise RelocationError("IL relocation position mismatch")
        if ins.op in SHORT or ins.op in LONG:
            op = 0xDD if ins.op == 0xDE else (
                ins.op + 0x0D if ins.op in SHORT else ins.op)
            relocated += struct.pack("<Bi",
                op, positions[ins.destinations[0]] - (now + 5))
        elif ins.op == 0x45:
            count = len(ins.destinations)
            base = now + 5 + count * 4
            relocated += struct.pack("<BI", 0x45, count)
            for target in ins.destinations:
                relocated += struct.pack("<i", positions[target] - base)
        else:
            relocated += ins.raw
    if len(relocated) != positions[len(il)]:
        raise RelocationError("invalid relocated IL byte length")
    rebuilt_eh = bytearray(eh[:4])
    for flags, start, size, handler, hsize, reserved in clauses:
        boundaries = (start, start + size, handler, handler + hsize)
        if any(p not in positions for p in boundaries):
            raise RelocationError("EH boundary not an instruction start")
        a,b,c,d = (positions[p] for p in boundaries)
        rebuilt_eh += struct.pack("<6I", flags,a,b-a,c,d-c,reserved)
    relocated_ops = disassemble(bytes(relocated))
    if len(relocated_ops) != len(ops) + 2:
        raise RelocationError("unexpected patched opcode count")
    if len(parse_finally(bytes(rebuilt_eh), len(relocated))) != 2:
        raise RelocationError("incorrect relocated exception handlers")
    # Every original branch must still reach the same ORIGINAL instruction.
    # The newly inserted Sleep is reached only by fall-through after a
    # fully processed part, never by a branch into a different state.
    for before, after in zip(ops, (x for x in relocated_ops if x.pc !=
                                  positions.get(YIELD_PC, -1) - 6 and x.pc !=
                                  positions.get(YIELD_PC, -1) - 5)):
        if before.destinations and after.destinations != tuple(
                positions[target] for target in before.destinations):
            raise RelocationError("relocated control-flow target changed")
    return bytes(relocated), bytes(rebuilt_eh), positions


def inspect_approved_file(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    verify(data)  # ALL eight method digests and call sites, not just class name
    variant = identify_variant(data)
    parent, child = variant.methods[6:]
    il, eh = method_and_eh(data, parent.rva)
    if il != il_bytes_at(data, parent.rva):
        raise RelocationError("independent IL extractors disagree")
    if hashlib.sha256(il).hexdigest() != parent.digest:
        raise RelocationError("original parent-body SHA mismatch")
    changed, new_eh, offsets = relocate_parent_loop(
        il, eh, variant.sleep_memberref, child.token)
    return {
        "variant": variant.name, "original_il_bytes": len(il),
        "prototype_il_bytes": len(changed),
        "original_instructions": len(disassemble(il)),
        "prototype_instructions": len(disassemble(changed)),
        "original_sha256": parent.digest,
        "prototype_sha256": hashlib.sha256(changed).hexdigest(),
        "finally_clauses": len(parse_finally(new_eh,len(changed))),
        "loop_body_instruction": hex(offsets[YIELD_PC]),
        "sleep_memberref": hex(variant.sleep_memberref),
        "hook_installed": False,
        "runtime_gameplay_tested": False,
    }


def main() -> None:
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original_ui_dll", type=Path)
    args = parser.parse_args()
    try:
        report = inspect_approved_file(args.original_ui_dll)
    except (OSError, ContractError, RelocationError) as exc:
        parser.exit(2, f"FAIL (no changes made): {exc}\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    print("OFFLINE ONLY: no managed CAS hook or gameplay speedup installed.")


if __name__ == "__main__":
    main()
