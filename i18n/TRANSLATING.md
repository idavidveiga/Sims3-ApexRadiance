# Translating the menu

How the menu languages work: `ui/i18n.h` (read it). English text is the key. The menu has the 21 languages of The Sims 3;
Settings > Menu > Language picks one (automatic = Windows' display language). Two kinds of tables:

- `i18n/tr_*.cpp`: {English, Portuguese (Brazil), Spanish, French} per entry. These tables are the list of every key:
  a new text always gets an entry here, with its three translations (sections below).
- `i18n/lang_<code>.cpp`: {English, translation} pairs for one other language (`de it nl pl ru cs hu el da sv no fi ja
  ko zh_hans zh_hant th`). A text without a pair shows in English. Format and rules: "Other languages" below.

`i18n/keys.tsv` lists every key once, for translators: run `perl tools/i18n_keys.pl` after changing a `tr_*.cpp` table
(`tools/i18n_check` says when it is out of date).

## Other languages (lang_<code>.cpp)

### Source: i18n/keys.tsv

UTF-8, tab-separated, one header line, then one line per key in the order the menu registers them (texts of one screen
stay together). Columns: `key` (the English text), `table` (`file:array` where it is defined, comma-separated when
two tables share it), `pt`, `es`, `fr` (the existing translations: use them for the meaning and the length, not as the
source), `placeholders` (the key's `{}` placeholders, in order, space-separated). Inside a field, `\\`, `\t`, `\n` and
`\r` stand for a backslash, a tab, a line break and a carriage return.

### File format

```cpp
// German menu texts (see ui/i18n.h; how to translate: i18n/TRANSLATING.md). Part of Apex Radiance.
// ...
#include "ui/i18n.h"
#include <iterator>

namespace {

// {English (exactly as in i18n/keys.tsv), German}
const I18n::Pair kPairs[] = {
    {"Night Lights", "Nachtbeleuchtung"},
    {"Press {} to save one screenshot with Apex's effects", "Drücke {}, um einen Screenshot mit den Effekten von Apex zu speichern"},
    {"An older {} is also installed. Delete it from Game\\Bin.", "Eine ältere {} ist ebenfalls installiert. Lösche sie aus Game\\Bin."},
    {nullptr, nullptr}, // keeps the array valid while it has no translations (skipped by the lookup); may stay
};
const I18n::LangTable kTable(I18n::Lang::German, kPairs, std::size(kPairs));

} // namespace
```

Only the lines inside `kPairs` change. Each pair is `{"<key>", "<translation>"},` on one line, in the order of
`keys.tsv` (easier to review). Keys and translations are C++ string literals:

- The key must be byte-identical to the `key` column: write the text itself, with `\"` for a quote, `\\` for a
  backslash and `\n` for a line break (keys.tsv already writes a backslash as `\\` and a line break as `\n`, so those
  can be copied as they are; only `"` needs escaping). A key that differs by one
  character (a curly apostrophe, a missing period, `...` instead of `…`) never matches: the text stays English, and
  `tools/i18n_check` lists it as stale.
- Write every non-ASCII character as itself (UTF-8: `é`, `ß`, `ł`, `ё`, `ά`, `日本`, `ไทย`). Never use `\x..` escapes:
  C++ reads every hex digit after `\x` as part of the escape, so `"\xE2\x80\xA6abc"` breaks on the `a`.
- An empty translation (`""`) or a missing pair means "not translated yet" (English is shown). Do not copy the English
  text as a translation unless the text really is the same in the language (a product name, "OK", "SMAA").
- One pair per key. The same key twice with two different translations fails the check.
- The `{nullptr, nullptr}` sentinel may stay or go once there are pairs.

### Rules

- Keep every `{}` placeholder of the key, exactly as written (`{}`, `{:.1f}`, `{:d}`), the same number of them and in
  the same order (`std::format` fills them in order). Move them where the grammar needs them. `{{` and `}}` are a
  literal brace: keep them as they are. A translation whose placeholders differ is rejected by `tools/i18n_check` and,
  in the game, falls back to English.
- Keep a `##id` tail if a key has one (`"Text##Id"` -> `"Texte##Id"`): the part after `##` is an ImGui id.
- Keep as they are: product and people names (Apex Radiance, The Sims 3 as the game calls itself in your language,
  Sims3SettingsSetter, S3SS, DXVK, SMAA, FXAA, Dear ImGui, Detours, toml++, Lucide, sims3fiend, Arro, idavidveiga,
  @loinyx), key names (Ctrl+Shift+F11, F6, Esc), units (ms, %, °, MB, FPS where the language keeps it), file names and
  paths, TOML keys.
- "Credits: @loinyx" at the end of a description: translate "Credits" (glossary) and keep "@loinyx".
- Upper-case group labels ("WALLS") stay upper case in languages with case (Greek: no accents in capitals); CJK and Thai
  have no case.
- Length: the menu is narrow. Stay close to the English length (CJK is naturally shorter). Long texts wrap; tabs,
  segments, buttons, chips and the sidebar cut a text that does not fit with "…" (full text on hover), so short labels
  matter most there (look at the `table` column: `tr_widgets.cpp` and short keys in `tr_menu.cpp` are those labels).
- Use the game's own words for game things, as The Sims 3 shows them in that language: Sims, lot, Build mode, Buy mode,
  map view, Options > Graphics, save / load, household. A player should recognise them from the game.
- Style: friendly and plain, like the English. Address the player as the language's games usually do: German "du",
  Dutch "je", Italian "tu", Polish "ty", Czech "ty", Hungarian "te", Danish / Swedish / Norwegian "du", Finnish
  "sinä" (or impersonal), Russian "вы" (lower case), Greek "εσύ"; Japanese polite form (です / ます) without honorific
  excess; Korean polite form (-요 / -습니다 for statements in notes); Chinese neutral, "你" (not "您"); Thai polite and
  neutral, without ครับ / ค่ะ. Norwegian is Bokmål. Simplified Chinese uses mainland terms; Traditional Chinese uses
  Taiwan terms (not a character conversion of the Simplified text).
- Build a glossary for the language first (the terms of the glossary table below, plus Sim, lot, Build mode, Night
  Lights, Reflections, Edge Smoothing, Depth Blur, Picture, Color, Performance, Overview, Settings, Profiles, Advanced,
  Reset, Undo, on / off) and use it everywhere. Feature names are Title Case in English; use the language's own title
  convention (German nouns, French-style sentence case for most Romance languages, no change in CJK / Thai).
- Keys whose `table` is `tr_developer.inc` only appear in developer mode: translate them last.
- Check: `tools/i18n_check` (its header has the command line). It must end with `OK`; its coverage line shows
  `<code> <translated>/<keys>` and lists stale keys.

## Adding texts (developers)

The rest of this page is for code changes: a new English text needs an entry in a `tr_*.cpp` table with Portuguese,
Spanish and French. The other languages follow in later translation passes (`lang_<code>.cpp`).

### What translates by itself

The Violet widgets (`ui/widgets.h`) translate everything they draw, so a call site with English literals needs only table
entries: row labels (the part before `##`), row descriptions (the `tooltip` / `description` parameters), `Tooltip`,
`PageTitle`, `MutedText`, `SectionLabel`, `GroupLabel` (upper-case keys: "WALLS"), `IconNote`, `Chip`, `Pill`, buttons
(`TextButton`, `IconTextButton`), `SidebarItem`, `SidebarGroup`, `TabBar` / `Segmented` / `SegmentedRow` labels and tooltips,
`BeginAdvanced`, slider label / description / `leftLabel` / `rightLabel` / `valueText`, `CardHeader` title / subtitle /
tooltip, `OverviewRow` name / phrase / tooltip / rightText, `SetNextRowBadge` text and tooltip, the change reports.

### What needs code

- Text drawn with raw ImGui (`ImGui::Text*`, `TextUnformatted`, `TextWrapped`, `TextColored`, `Button`, `Checkbox`,
  `Selectable`, `BeginTabItem`, `Combo` items, `CalcTextSize` used to lay out a text that is then drawn translated) in the
  PUBLIC build: wrap the text with `I18n::Tr("...")`. An ImGui widget whose label is also its ID (`Button`,
  `Checkbox`, `Selectable`, `BeginTabItem`, `TreeNode` ...) needs a fixed ID: `(std::string(I18n::Tr("Text")) +
  "###Id").c_str()` (`###` makes the ID only the part after it; `##` hashes the whole label, so the ID would change
  with the language). `I18n::TrLabel` returns a view that is NOT null-terminated: use `.data()` + `.size()`.
- User data shown as a row label (a profile name): `ApexUi::SetNextRowUntranslated()` before the row.
- Change reports: `ApexUi::ReportChange("English text")` with the English literal (the toast shows it translated, the
  log gets the English one).
- Text built at run time and shown in the public menu: `I18n::Trf("English {} format", args...)` instead of
  `std::format`. The translation must keep the same `{}` placeholders in the same order (Developer > Language lists any
  that do not). Never change a text that also goes to the log: logs stay English (split display and log text if needed).
- Text passed to a widget through a `std::string` built at run time: the widget looks the whole string up, so build it
  from translated parts (Trf) instead.

### Keys

A key must be byte-identical to the English text as it reaches the widget. For a literal made of pieces
(`"Part of " APEX_PRODUCT_NAME ". Credits: @loinyx"`) write the same pieces in the table (`apex_version.h` is included).
Escapes (`"Starting\xE2\x80\xA6"`) may be written as the same escape or as the UTF-8 character (the sources compile as
UTF-8). The same English text in two tables: the first registered one wins, so translate it the same way. After
adding, changing or removing entries, run `perl tools/i18n_keys.pl` so `i18n/keys.tsv` (the translators' list) follows;
a changed English text also makes the other languages' old translation stale (it shows English until retranslated).

### Not translated

Logs (`LOG_*`), TOML keys, file names, ImGui IDs after `##`, the Developer page and everything in `RenderDeveloperUI` or
under `if constexpr (!kPublicBuild)` (development build only), product and people names (Apex Radiance,
Sims3SettingsSetter, S3SS, DXVK, SMAA, FXAA, Dear ImGui, Detours, toml++, Lucide, sims3fiend, Arro, @loinyx), key names
(Ctrl+Shift+F11), units (ms, %, °). "Credits: @loinyx" at the end of a description becomes "Créditos: @loinyx" (pt, es) and "Crédits : @loinyx" (fr).

## Style (Portuguese, Spanish, French)

Portuguese: Brazilian, natural, "você", short. Spanish: neutral Latin American, "tú", short. French: natural, "vous", short. Keep the length close to the
English (the menu is narrow). Group labels stay upper case.

Glossary (keep these the same everywhere):

| English | Português | Español | Français |
|---|---|---|---|
| Night Lights | Luzes Noturnas | Luces Nocturnas | Lumières Nocturnes |
| Every-Story Ground Light | Luz no Chão de Todos os Andares | Luz en el Suelo desde Todos los Pisos | Lumière au Sol de Tous les Étages |
| Water & Snow | Água e Neve | Agua y Nieve | Eau et Neige |
| Lighting | Iluminação | Iluminación | Éclairage |
| Color | Cor | Color | Couleur |
| Picture | Imagem | Imagen | Image |
| Depth Blur | Desfoque de Profundidade | Desenfoque de Profundidad | Flou de Profondeur |
| Edge Smoothing / Anti-aliasing | Suavização de Bordas | Suavizado de Bordes | Lissage des Bords |
| Borderless window | Janela sem bordas | Ventana sin bordes | Fenêtre sans bordure |
| Display | Tela | Pantalla | Affichage |
| Performance | Desempenho | Rendimiento | Performances |
| Overview | Visão geral | Resumen | Vue d'ensemble |
| Settings | Configurações | Ajustes | Paramètres |
| Profiles | Perfis | Perfiles | Profils |
| Advanced | Avançado | Avanzado | Avancé |
| Experimental | Experimental | Experimental | Expérimental |
| lamp / lamps | lâmpada / lâmpadas | lámpara / lámparas | lampe / lampes |
| lot / lots | lote / lotes | solar / solares | terrain / terrains |
| Sim / Sims | Sim / Sims | Sim / Sims | Sim / Sims |
| Build mode | modo Construir | modo Construir | mode Construction |
| map view | vista do mapa | vista de mapa | vue de la carte |
| stutter / hitch | travada | tirón | saccade |
| frame (rendering) | quadro | fotograma | image |
| Turn on / Turn off | Ligar / Desligar | Activar / Desactivar | Activer / Désactiver |
| on / off (state) | ligado / desligado | activado / desactivado | activé / désactivé |
| Reset | Restaurar | Restablecer | Réinitialiser |
| Reload save | Recarregar o save | Recargar la partida | Recharger la partie |
| Credits | Créditos | Créditos | Crédits |
