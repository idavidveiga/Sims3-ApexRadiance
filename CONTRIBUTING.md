# Working on Apex Radiance

Read [docs/README.md](docs/README.md), then the relevant feature, engine, validation and history pages. Existing rejected approaches require new evidence before another attempt. Keep a backup of the installed mod and its configuration before testing a build in game. Do not commit proprietary executables, shader bytecode, captures, credentials or build output. Automated contributors also follow [AGENTS.md](AGENTS.md).

## Reproducible Windows build

Install Visual Studio 2022 C++ Build Tools with MSVC v143 and a Windows SDK, Git and Node. From the repository root in PowerShell:

```powershell
./tools/development/setup.ps1
./tools/development/build.ps1 -Flavor Public
./tools/development/build.ps1 -Flavor Developer
./tools/development/check.ps1 -Suite Smoke
```

Setup uses the exact vcpkg revision in `vcpkg.json` and `x86-windows-static`. It downloads dependencies but does not install the mod into the game. An existing vcpkg checkout at another revision is refused rather than reset. Builds retain static CRT, C++20 and x86. Public artifacts omit developer tools; developer artifacts remain local. The build helper records the validated machine-local recipe.

`-Suite Full` runs the same three CPU harnesses at their full default coverage. These cover DXT, RefPack and CAS triangle sorting; they do not represent the full project's tests. Read each feature's validation page and run its relevant harnesses before changing it. D3D9 HAL tests and actual gameplay remain separate from the hosted CI job.

The GitHub build workflow checks pull requests and pushes to `main`. Downloaded CI artifacts are test builds, not automatically published releases. Existing Nexus workflows still run when a release is published.

## Branches, commits and pull requests

- Branches: `feature/<slug>`, `fix/<slug>`, `docs/<slug>`, `chore/<slug>`, `release/<x.y.z>`.
- Commit subjects and PR titles: `area: imperative summary`, at most 72 characters, no trailing period, no
  parentheses. Areas: `lighting`, `color`, `fx`, `aa`, `ao`, `perf`, `ui`, `i18n`, `presets`, `build`, `ci`, `docs`,
  `tools`, `release`. Details go in the body.
- One focused pull request per change, opened as a draft until its checks pass. Record the tested commit and what
  remains unverified in its Validation section.
- Public text follows [docs/PUBLIC-TEXT-POLICY.md](docs/PUBLIC-TEXT-POLICY.md). Enable the local hooks once with
  `git config core.hooksPath .githooks`; CI runs the same check.
- Never force-push over someone else's work. Merging and releasing are maintainer actions
  ([docs/release-process.md](docs/release-process.md)).

## Reverse engineering

Keep the local corpus under an ignored output directory or another private research folder. Identify executables and shader archives by SHA256 and build/version. Prefer the existing engine documentation and recover old research before recreating it. Work from the local Steam executable and read-only shader archive; preserve the project's decision not to dump the decrypted EA executable.

Use a reproducible scene and diagnostic captures before proposing a patch. Confirm calling conventions, register/state ownership and expected original bytes. Resolve non-Steam locations by validated signatures. Keep unknown facts explicitly unverified. Run the affected offline harnesses, then compare native D3D9 and DXVK in game when the change touches rendering.

Install the official Ghidra release and its required JDK 21. Set `JAVA_HOME` to that JDK if Ghidra cannot discover it. The following recipes keep generated game data under the ignored `outputs/research` directory:

```powershell
./tools/development/research.ps1 -GameBin 'C:\path\to\The Sims 3\Game\Bin'
./tools/development/research.ps1 -GameBin 'C:\path\to\The Sims 3\Game\Bin' -GhidraRoot 'C:\path\to\ghidra' -Analyze
./tools/development/research.ps1 -GameBin 'C:\path\to\The Sims 3\Game\Bin' -GhidraRoot 'C:\path\to\ghidra' -Export
```

Analyze imports the verified Steam 1.67.2 executable without modifying it; repeating an import refuses to overwrite the existing project. Headless analysis uses a configurable Java heap (`-MaxMemoryGB`, 8 GB by default). The upstream 2 GB heap and an 8 GB profile exhausted memory in Ghidra analyzers. The bounded profile disables the x86 constant reference, global stack variable and decompiler switch analyzers; the 8 GB import then completed successfully, indexed 38,803 functions and exported all 15 selected engine functions read-only. The recipe rejects logs reporting out-of-memory errors or failed imports before export. This first pass omits some inferred cross-references, stack variables and switch recovery, so verify these details when investigating each engine function. Export reopens the completed project read-only, writes a function index and decompiles 15 documented engine entry points. Generated names, types and control flow require manual verification; this is a replacement research baseline, not recovery of previous analyst annotations.
