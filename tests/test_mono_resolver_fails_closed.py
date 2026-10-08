"""Protect the retired Mono ICall resolver on every game build in CI."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MonoResolverSafetyTests(unittest.TestCase):
    def test_historical_header_signature_and_steam_target_are_disabled(self):
        source = (ROOT / "features" / "fast_create_a_style.cpp").read_text(encoding="utf-8")
        definition = source.split("const GameAddress kLookupInternalCall{", 1)[1].split("};", 1)[0]
        self.assertIn("{}", definition)
        self.assertNotIn("0x00E82680", definition)
        self.assertIn("nullptr", definition)
        self.assertNotIn("81 EC 08 08 00 00", definition)
        self.assertNotIn("0x81", definition)
        self.assertIn("#ifndef APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS", source)

    def test_mono_export_lookup_is_read_only(self):
        source = (ROOT / "features" / "ts3_mono_runtime_probe.cpp").read_text(encoding="utf-8")
        function = source.split("std::string InspectMonoExports() {", 1)[1].split(
            "// Focused, non-invasive baseline", 1
        )[0]
        self.assertIn("GetModuleHandleW(nullptr)", function)
        self.assertIn("GetProcAddress(m.handle, name)", function)
        for symbol in ("mono_compile_method", "mono_jit_info_table_find",
                       "mono_method_get_token", "mono_method_desc_search_in_image"):
            self.assertIn(symbol, function)
        for forbidden in ("DetourBatch", "WriteBytes", "WriteCode", "LoadLibrary",
                          "mono_compile_method("):
            self.assertNotIn(forbidden, function)

    def test_native_anchor_inspector_does_not_install_hooks(self):
        source = (ROOT / "features" / "ts3_mono_runtime_probe.cpp").read_text(encoding="utf-8")
        function = source.split("std::string InspectMonoRuntimeAnchors() {", 1)[1].split(
            "std::string InspectLoadedExe() {", 1
        )[0]
        self.assertIn("GameAddr::Id::MonoTypeGetObject", function)
        self.assertIn("GameAddr::Id::MonoDomainFree", function)
        self.assertIn("MemPatch::ReadBytes", function)
        self.assertNotIn("MemPatch::Write", function)
        self.assertNotIn("DetourBatch::InstallHooks", function)
        self.assertNotIn("GameAddr::Resolve()", function)


if __name__ == "__main__":
    unittest.main()
