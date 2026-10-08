"""Keep production Mono hooks disabled while testing isolated EA169 pilot gates."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MonoResolverSafetyTests(unittest.TestCase):
    def test_production_rejects_icall_hook_and_pilot_requires_exact_ea_169(self):
        source = (ROOT / "features" / "fast_create_a_style.cpp").read_text(encoding="utf-8")
        cache = (ROOT / "features" / "fast_cas_catalog.cpp").read_text(encoding="utf-8")
        # In ALL ordinary Win32 builds there is no native resolver hook:
        # only a dedicated pilot with an explicitly named compiler macro
        # can pass the Start gate. Faster Create-a-Style remains disabled.
        self.assertIn("#ifndef APEX_CAS_PRESET_CACHE_PILOT", cache)
        self.assertIn("#ifndef APEX_CAS_PRESET_CACHE_PILOT", source)
        self.assertIn("#ifndef APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS", source)
        self.assertIn("VerifiedExperimentalResolver(error)", source)
        guard = source.split("std::optional<uintptr_t> VerifiedExperimentalResolver(", 1)[1].split(
            "} // namespace", 1)[0]
        self.assertIn("g_gameVersion != GameVersion::EA", guard)
        self.assertIn("g_exeTimestamp != 0x6707155Cu", guard)
        self.assertIn("0xA826A0u", guard)
        for site in ("0x98A28Cu", "0xA6454Cu", "0xA84DDFu", "0xA9931Eu"):
            self.assertIn(site, guard)
        self.assertIn("GetModuleHandleW(L\"MonoPatcher.asi\")", guard)
        self.assertIn("GetModuleHandleW(L\"Sims3MonoModder.asi\")", guard)
        self.assertIn("MemPatch::ValidateBytes", guard)
        self.assertIn("MemPatch::ReadBytes", guard)
        self.assertIn("call[0]!=0xE8", guard)
        self.assertIn("std::nullopt", guard)

    def test_known_runtime_anchors_accept_only_apex_owned_hook_trampolines(self):
        source = (ROOT / "features" / "ts3_mono_runtime_probe.cpp").read_text(encoding="utf-8")
        block = source.split("std::string InspectMonoRuntimeAnchors() {", 1)[1].split(
            "std::string InspectLoadedExe() {", 1
        )[0]
        self.assertIn("EntryChain::Installed(", block)
        self.assertIn("EntryChain::Layer::ScriptMath", block)
        self.assertIn("EntryChain::OwnsEntry(a.site)", block)
        self.assertIn("EntryChain::Original(a.site)", block)
        self.assertIn("savedOriginal", block)
        self.assertIn("scriptMathLayer && ownEntry && savedOriginal", block)
        self.assertIn("entry ownership lost (possible conflict)", block)
        for forbidden in ("MemPatch::Write", "DetourBatch::InstallHooks"):
            self.assertNotIn(forbidden, block)

        header = (ROOT / "framework" / "entry_chain.h").read_text(encoding="utf-8")
        chain = (ROOT / "framework" / "entry_chain.cpp").read_text(encoding="utf-8")
        self.assertIn("bool OwnsEntry(Site site);", header)
        own = chain.split("bool OwnsEntry(Site site) {", 1)[1].split(
            "bool Installed(Site site, Layer layer)", 1
        )[0]
        self.assertIn("Outermost(state)", own)
        self.assertIn("MakeJmp(expected, state.fn, target)", own)
        self.assertIn("MemPatch::ReadBytes", own)
        self.assertNotIn("MemPatch::Write", own)

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
