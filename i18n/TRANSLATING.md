# Translating the menu (Portuguese, Spanish, French)

How the menu languages work: `ui/i18n.h` (read it). English text is the key; tables in `i18n/tr_*.cpp` give the Portuguese
(Brazil), Spanish and French text (entries are {English, Portuguese, Spanish, French}). Settings > Menu > Language picks the language (automatic = Windows' display language).

## What translates by itself

The Violet widgets (`ui/widgets.h`) translate everything they draw, so a call site with English literals needs only table
entries: row labels (the part before `##`), row descriptions (the `tooltip` / `description` parameters), `Tooltip`,
`PageTitle`, `MutedText`, `SectionLabel`, `GroupLabel` (upper-case keys: "WALLS"), `IconNote`, `Chip`, `Pill`, buttons
(`TextButton`, `IconTextButton`), `SidebarItem`, `SidebarGroup`, `TabBar` / `Segmented` / `SegmentedRow` labels and tooltips,
`BeginAdvanced`, slider label / description / `leftLabel` / `rightLabel` / `valueText`, `CardHeader` title / subtitle /
tooltip, `OverviewRow` name / phrase / tooltip / rightText, `SetNextRowBadge` text and tooltip, the change reports.

## What needs code

- Text drawn with raw ImGui (`ImGui::Text*`, `TextUnformatted`, `TextWrapped`, `TextColored`, `Button`, `Checkbox`,
  `Selectable`, `BeginTabItem`, `Combo` items, `CalcTextSize` used to lay out a text that is then drawn translated) in the
  PUBLIC build: wrap the text with `I18n::Tr("...")` (or `I18n::TrLabel` for "Text##id" labels; keep the `##id` part
  English so ImGui IDs do not change with the language).
- Text built at run time and shown in the public menu: `I18n::Trf("English {} format", args...)` instead of
  `std::format`. The translation must keep the same `{}` placeholders in the same order (Developer > Language lists any
  that do not). Never change a text that also goes to the log: logs stay English (split display and log text if needed).
- Text passed to a widget through a `std::string` built at run time: the widget looks the whole string up, so build it
  from translated parts (Trf) instead.

## Keys

A key must be byte-identical to the English text as it reaches the widget. For a literal made of pieces
(`"Part of " APEX_PRODUCT_NAME ". Credits: @loinyx"`) write the same pieces in the table (`apex_version.h` is included).
Escapes (`"Starting\xE2\x80\xA6"`) may be written as the same escape or as the UTF-8 character (the sources compile as
UTF-8). The same English text in two tables: the first registered one wins, so translate it the same way.

## Not translated

Logs (`LOG_*`), TOML keys, file names, ImGui IDs after `##`, the Developer page and everything in `RenderDeveloperUI` or
under `if constexpr (!kPublicBuild)` (development build only), product and people names (Apex Radiance,
Sims3SettingsSetter, S3SS, DXVK, SMAA, FXAA, Dear ImGui, Detours, toml++, Lucide, sims3fiend, Arro, @loinyx), key names
(Ctrl+Shift+F11), units (ms, %, °). "Credits: @loinyx" at the end of a description becomes "Créditos: @loinyx" (pt, es) and "Crédits : @loinyx" (fr).

## Style

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
