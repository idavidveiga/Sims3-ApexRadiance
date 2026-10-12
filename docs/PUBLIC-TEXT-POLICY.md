# Public text policy

Everything in this repository and on its GitHub pages is public: source and comments, documentation, commit messages,
branch names, pull request titles and bodies, workflow names and release notes. This page says what belongs there and
how it reads. `tools/public_text_check.py` enforces the mechanical rules locally (`.githooks`) and in CI
(`.github/workflows/public-text.yml`).

## What is public

| Public | Kept out of the repository |
|---|---|
| Source, build scripts, offline test harnesses | Machine-specific paths, backup inventories, local install notes |
| Feature, engine, validation and history pages | Chat logs, conversation quotes, personal preferences |
| Release notes for players | Raw captures, crash dumps, decompiled game code, shader bytecode |
| Contributor guides (`CONTRIBUTING.md`, `AGENTS.md`, repository skills that any contributor can use) | Assistant briefings tied to one machine or person (`CLAUDE.local.md`, `AGENTS.local.md`, ignored by Git) |

## How it reads

- Third person, present tense, product and engineering voice. State the behaviour and the design reason.
- No reported speech: not "the user asked", "the maintainer said", "approved", "requested", and no quotes from chats.
- No clock times; dates only in history, validation and release pages.
- No names of the tools or assistants used to write a change, and no links to private sessions.
- No personal data: real names beyond the public handle `loinyx`, e-mail addresses, home-folder paths.
- Third-party projects appear in credits, compatibility notes and migration code only, in neutral wording.
- Security products are named only in player-facing notes about false positives.

## Format and length

- Every public text uses its standard shape: feature, validation, history and engine pages follow
  [templates/](templates/); pull requests follow the template (Summary, Validation, Checklist); release notes follow
  [release-process.md](release-process.md); commit bodies say what changed and why in a few sentences.
- Lead with the result, then the reason, then the detail a maintainer needs. One fact in one place; link instead of
  repeating it.
- Short and specific: no filler, no restating the title, no lists of what was not done, no hedging beyond marking
  unverified facts. A page should read in five minutes; a PR description fits on one screen.

## Review before publishing

Every public change (code comments, docs, files, PRs, releases) is reviewed against this page before it is merged or
published, by a second reviewer when one is available. The reviewer checks the mechanical rules with
`tools/public_text_check.py` and reads the text once as an outside reader: is it accurate, standard in shape, concise
and free of private context? Findings are fixed before merging.

## Titles

| Item | Format | Example |
|---|---|---|
| Commit subject, PR title | `area: imperative summary`, at most 72 characters, no trailing period, no parentheses | `lighting: fix wall light on upper floors` |
| Release PR | `release: X.Y.Z` | `release: 2.13.0` |
| Branch | `feature/`, `fix/`, `docs/`, `chore/`, `release/` + slug | `feature/fx-effects` |
| GitHub release | `Apex Radiance X.Y.Z` (the headline goes in the notes) | `Apex Radiance 2.13.0` |

Areas: `lighting`, `color`, `fx`, `aa`, `ao`, `perf`, `ui`, `i18n`, `presets`, `build`, `ci`, `docs`, `tools`,
`release`, `deps`. Commit bodies explain what and why in full sentences; they carry no tool trailers.

## Checks

```sh
python tools/public_text_check.py --files <paths...>        # changed files
python tools/public_text_check.py --commit-msg <file>       # a commit message
python tools/public_text_check.py --title "<text>"          # a PR or release title
python tools/public_text_check.py --text-file <file>        # a PR body or release notes
```

Exceptions that are legitimate (this page, the check's own rules, third-party code) are listed in
`tools/public_text_allow.txt` as `path-glob<TAB>rule-id`.
