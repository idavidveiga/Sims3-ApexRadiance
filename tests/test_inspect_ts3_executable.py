# Built-in PE triage tests. No executable is shipped with the repository.
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from inspect_ts3_executable import KNOWN, STEAM_RESOLVER, analyze, entropy


class StaticInspectorTests(unittest.TestCase):
    def test_build_fingerprints(self):
        self.assertEqual(KNOWN[0x6707155C], "EA App 1.69.47.024017")
        self.assertEqual(len(STEAM_RESOLVER), 13)

    def test_entropy_extremes(self):
        self.assertEqual(entropy(b""), 0.0)
        self.assertEqual(entropy(b"\x00" * 1000), 0.0)
        self.assertAlmostEqual(entropy(bytes(range(256))), 8.0, places=4)

    def test_rejects_non_pe(self):
        with TemporaryDirectory() as d:
            invalid = Path(d) / "not_a_game.exe"
            invalid.write_bytes(b"not a PE")
            with self.assertRaises(ValueError):
                analyze(invalid)

    def test_rejects_bad_pe_pointer(self):
        with TemporaryDirectory() as d:
            path = Path(d) / "out_of_bounds.exe"
            data = bytearray(0x200)
            data[:2] = b"MZ"
            data[0x3C:0x40] = (0xFFFFFF00).to_bytes(4, "little")
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                analyze(path)

    def test_rejects_missing_optional_header(self):
        with TemporaryDirectory() as d:
            path = Path(d) / "no_optional.exe"
            data = bytearray(0x200)
            data[:2] = b"MZ"
            data[0x3C:0x40] = (0x80).to_bytes(4, "little")
            data[0x80:0x84] = bytes((80, 69, 0, 0))  # exact PE signature
            data[0x84:0x86] = (0x14C).to_bytes(2, "little")
            data[0x86:0x88] = (1).to_bytes(2, "little")
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                analyze(path)

    def test_unchanged_input_file(self):
        with TemporaryDirectory() as d:
            path = Path(d) / "invalid.exe"
            payload = b"MZ" + b"X" * 0x200
            path.write_bytes(payload)
            with self.assertRaises(ValueError):
                analyze(path)
            self.assertEqual(path.read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
