"""Portable structural tests for the Apex Hair/Hats parent-IL prototype.

Synthetic IL only, no EA bytes and no MonoPatcher. The real original DLL
variants are verified separately by inspect_approved_file at development time.
"""
import sys
import unittest
from pathlib import Path
import struct

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cas_hair_il_relocation import (
    RelocationError, disassemble, parse_finally, relocate_control_flow,
    relocate_parent_loop, inspect_approved_file
)


def toy() -> tuple[bytes, bytes]:
    # Two disjoint try/finally regions; three short branches/leave ops.
    il = bytes([
        0x2B, 0x06,       # br.s -> IL_0008
        0x00,             # nop
        0xDE, 0x03,       # leave.s -> IL_0008
        0x00, 0xDC,       # finally handler 1
        0x00,             # nop
        0x00,             # nop at IL_0008, second protected region
        0xDE, 0x03,       # leave.s -> IL_000E
        0x00, 0xDC,       # finally handler 2
        0x00,
        0x2A,             # ret
    ])
    clauses = struct.pack("<6I", 2, 0, 5, 5, 2, 0)
    clauses += struct.pack("<6I", 2, 8, 3, 11, 2, 0)
    eh = bytes([0x41, 52, 0, 0]) + clauses
    assert len(il) == 15 and len(eh) == 52
    return il, eh


class HairILRelocationTests(unittest.TestCase):
    def test_both_finally_regions_and_branches_relocated(self):
        il, eh = toy()
        insertion = bytes([0x16, 0x28]) + struct.pack("<I", 0x0A000023)
        updated, updated_eh, offsets = relocate_control_flow(il, eh, 9, insertion)
        self.assertEqual(len(updated), 30)
        self.assertEqual(len(disassemble(updated)), len(disassemble(il)) + 2)
        self.assertEqual(len(parse_finally(updated_eh, len(updated))), 2)
        first, second = parse_finally(updated_eh, len(updated))
        self.assertEqual(first[0], 2)
        self.assertEqual(second[0], 2)
        # All ORIGINAL jump targets still refer to ORIGINAL instructions.
        original = disassemble(il)
        translated = {i.pc: i for i in disassemble(updated)}
        for op in original:
            for old in op.destinations:
                self.assertIn(offsets[old], translated)
        self.assertEqual(updated[offsets[9] - 6:offsets[9]], insertion)
        self.assertEqual(translated[offsets[9]].op, 0xDD)

    def test_il_decoder_rejects_broken_branches(self):
        with self.assertRaises(RelocationError):
            disassemble(bytes([0x2B, 0x7F, 0x2A]))
        with self.assertRaises(RelocationError):
            disassemble(bytes([0x28, 0x00]))
        with self.assertRaises(RelocationError):
            disassemble(bytes([0xFE]))
        with self.assertRaises(RelocationError):
            disassemble(bytes([0x45, 0xFF, 0xFF, 0xFF, 0x7F]))

    def test_switch_target_cannot_land_inside_instruction(self):
        self.assertEqual(disassemble(
            bytes([0x45, 1, 0, 0, 0, 0, 0, 0, 0, 0x2A]))[0].destinations,
            (9,))
        with self.assertRaises(RelocationError):
            disassemble(bytes([0x45, 1, 0, 0, 0, 2, 0, 0, 0, 0x2A]))

    def test_exception_metadata_fails_closed(self):
        il, eh = toy()
        self.assertEqual(len(parse_finally(eh, len(il))), 2)
        with self.assertRaises(RelocationError):
            parse_finally(eh[:28], len(il))
        with self.assertRaises(RelocationError):
            parse_finally(eh[:4] + bytes(24) + eh[28:], len(il))
        with self.assertRaises(RelocationError):
            parse_finally(eh[:4] + struct.pack("<6I", 0,0,5,5,2,0)+eh[28:], len(il))

    def test_reject_wrong_insertion_and_non_original_parent(self):
        il, eh = toy()
        with self.assertRaises(RelocationError):
            relocate_control_flow(il, eh, 999, bytes([0x00]))
        with self.assertRaises(RelocationError):
            relocate_control_flow(il, eh, 9, bytes([0x2B, 0x00]))
        with self.assertRaises(RelocationError):
            relocate_parent_loop(il, eh, 0x0A000023, 0x0600191B)

    def test_unknown_input_never_gets_a_usable_patched_method(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            bad = Path(directory) / "UI.dll"
            bad.write_bytes(b"MZ" + bytes(1024))
            with self.assertRaises((ValueError, IndexError, struct.error)):
                inspect_approved_file(bad)


if __name__ == "__main__":
    unittest.main()
