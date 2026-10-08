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


if __name__ == "__main__":
    unittest.main()
