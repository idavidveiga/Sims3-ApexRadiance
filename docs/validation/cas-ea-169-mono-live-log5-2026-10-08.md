# EA App 1.69 — live Mono diagnostics, fifth log

Date: 2026-10-08. Source: player's `ApexRadiance_LOG_LIVE(5).txt`.
Source log is not committed, only a technical summary.

## Results

- `TS3.exe` version `1.69.47.024017`; Apex 2.10.1.
- During `GameAddr` initialization all 272 addresses were found.
- `MonoTypeGetObject` resolved to VA `0x00EA8A70`.
- `MonoDomainFree` resolved to VA `0x00E75390`.
- **The same session installed Apex ScriptMath EntryChain layer 8**
  on both functions; log reports normal next pointers to the game's
  original code. Therefore, after world load, both function entries
  start with an Apex JMP, and a naive literal-byte comparison with
  their unpatched prologues reports `0/2`.
- At 11:39:15 the probe claimed both prologues were changed. This
  cannot establish another mod's involvement: the changes are already
  accounted for by Apex's own hooks. It was a diagnostic bug.
- Export check: 1 loaded tested module, **0 matching Mono exports**.
  This supports an embedded, non-exported Mono runtime; does not
  establish that JIT symbols or `MonoMethod` metadata do not exist.
- Historical internal-call resolver candidate: unique match at
  executable RVA `0xA826A0`; four raw CALL references, identical
  to previous logs, no new identity or ABI proof.
- No `CASHair.PopulateTypesGrid(bool)` method pointer or interpreter
  adapter is resolved in this log. Hair/Hats optimization is NOT live.
- Module inventory includes `MonoPatcher.asi` and
  `Sims3Performance.asi`. Inventory proves presence, not that
  either installed a hook; remove temporary MonoPatcher when the
  research phase is over. Production Apex never depends on it.
- No `[ERROR]` lines reported in this log. Absence of errors does
  not establish game correctness or Hair/Hats acceleration.

## Fix on research branch

`EntryChain::OwnsEntry(Site)` (read-only) compares the live JMP with
the target recorded in the Apex hook chain. The Mono anchors inspector
now tests a known original prologue either directly on unhooked entries
or in an intact Apex-owned trampoline, only while a `ScriptMath`
layer is registered and the expected live JMP is still present.

If the entry was changed by another mod, the trampoline differs, or
the hook ownership is missing, the probe reports a conflict/integrity
warning rather than accepting any JMP. This does **not** authorize
attaching another hook to a Mono function.

`tests/test_mono_resolver_fails_closed.py` checks source-level use
of the ownership + saved-prologue guard in CI. This is an offline
regression test, not a runtime compatibility assertion.

**Decision:** previous four logs and the fifth log are sufficient;
do not ask the player to repeat any scan or to uninstall Apex's
own ScriptMath optimization to make the original-byte check pass.
