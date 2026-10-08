"""Unit tests for the read-only TS3 Hair/Hats IL contract verifier.

Synthetic Windows PE data only. Original EA binaries never enter CI.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from verify_hair_ui_contract import ContractError, call_count, il_bytes_at, verify


def fake_pe() -> bytearray:
    data = bytearray(2048)
    data[0:2] = b"MZ"
    data[0x3C:0x40] = (0x80).to_bytes(4, "little")
    data[0x80:0x84] = b"PE\0\0"
    data[0x86:0x88] = (1).to_bytes(2, "little")
    data[0x94:0x96] = (224).to_bytes(2, "little")
    data[0x98:0x9A] = (0x10B).to_bytes(2, "little")
    section = 0x80 + 24 + 224
    data[section:section + 5] = b".text"
    data[section + 8:section + 12] = (512).to_bytes(4, "little")
    data[section + 12:section + 16] = (0x1000).to_bytes(4, "little")
    data[section + 16:section + 20] = (512).to_bytes(4, "little")
    data[section + 20:section + 24] = (512).to_bytes(4, "little")
    return data


class HairILContractTests(unittest.TestCase):
    def test_tiny_method(self):
        pe = fake_pe()
        pe[0x210:0x214] = bytes([0x0E, 0x00, 0x17, 0x2A])
        self.assertEqual(il_bytes_at(bytes(pe), 0x1010), b"\x00\x17\x2A")

    def test_fat_method(self):
        pe = fake_pe()
        pe[0x220:0x22C] = (
            b"\x13\x30" + b"\x06\x00" +
            (4).to_bytes(4, "little") + bytes(4)
        )
        pe[0x22C:0x230] = b"\x17\x00\x16\x2A"
        self.assertEqual(il_bytes_at(bytes(pe), 0x1020), b"\x17\x00\x16\x2A")

    def test_unknown_and_truncated_method_fail_closed(self):
        with self.assertRaises(ContractError):
            il_bytes_at(b"invalid", 0x1010)
        pe = fake_pe()
        pe[0x210] = 0x03
        with self.assertRaises(ContractError):
            il_bytes_at(bytes(pe), 0x1010)

    def test_original_ui_digest_is_mandatory(self):
        with self.assertRaisesRegex(ContractError, "unknown or modified UI.dll"):
            verify(bytes(fake_pe()))

    def test_exact_call_counts(self):
        token = 0x06001918
        il = b"\x00\x28" + token.to_bytes(4, "little") + b"\x6F" + token.to_bytes(4, "little")
        self.assertEqual(call_count(il, token), 2)
        self.assertEqual(call_count(il, 0x0600191B), 0)

    def test_withdrawn_research_has_no_active_replacement(self):
        root = Path(__file__).resolve().parents[1]
        cs = (root / "research/temporary-hair-hats/TemporaryCasHairExperiment.cs").read_text()
        # Strip explanatory comments for an actual code-level check.
        source = "\n".join(line.split("//", 1)[0] for line in cs.splitlines())
        for forbidden in ("[ReplaceMethod", "MonoPatcher.PatchAll",
                          "MonoPatcher.ReplaceIL", "Simulator.Sleep("):
            self.assertNotIn(forbidden, source)
        proj = (root / "ApexRadiance.vcxproj").read_text()
        self.assertNotIn("TemporaryCasHairExperiment", proj)
        self.assertNotIn("MonoPatcher\\", proj)


if __name__ == "__main__":
    unittest.main()
