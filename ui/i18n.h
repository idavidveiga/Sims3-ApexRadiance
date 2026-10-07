#pragma once
// Menu language (docs/ui.md "Languages"): every language The Sims 3 ships in. English, Portuguese (Brazil), Spanish and
// French have hand-written tables; the other seventeen are filled per language (i18n/lang_<code>.cpp).
//
// The English text is the key: code keeps writing English literals, and the text is translated where it is shown. The
// Violet widgets (ui/widgets.h) translate every label, description, tooltip, note, button, tab, chip and title they
// draw, so most call sites need nothing. Text that is not drawn by a widget (ImGui::Text, CalcTextSize for a layout) goes
// through Tr(); text built at run time goes through Trf() with an English format string, whose translation keeps the
// same {} placeholders (a translation that does not format falls back to the English one).
// Two kinds of tables, both registered at start-up by static objects:
// - i18n/tr_*.cpp (I18n::Table): {English, Portuguese, Spanish, French}, one table per part of the menu. These tables are
//   the list of every key: a text the menu shows must have an entry here (even before it is translated elsewhere).
// - i18n/lang_<code>.cpp (I18n::LangTable): {English, translation} pairs for one of the other languages. Adding a language
//   this way never touches the four-language tables.
// A text with no entry, or with an empty translation, shows in English; the development build lists them (Developer >
// Translations). Logs, file names, the Developer page and the S3SS / Arro names stay in English.
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace I18n {

// Saved as an int ([ui] language is written as Code()), so the first four keep their values; add new ones before Count
enum class Lang : int {
    English,
    Portuguese,
    Spanish,
    French,
    German,
    Italian,
    Dutch,
    Polish,
    Russian,
    Czech,
    Hungarian,
    Greek,
    Danish,
    Swedish,
    Norwegian,
    Finnish,
    Japanese,
    Korean,
    ChineseSimplified,
    ChineseTraditional,
    Thai,
    Count
};

// The writing system a language needs from the fonts (ui/violet_theme.cpp merges a Windows font for each non-Latin one;
// Segoe UI already covers Latin, Greek and Cyrillic)
enum class Script : int { Latin, Japanese, Korean, ChineseSimplified, ChineseTraditional, Thai };
Script ScriptOf(Lang lang);

// The saved choice: -1 = automatic (Windows' display language), else a Lang
int Choice();
void SetChoice(int choice); // takes effect at once; the caller saves it (ApexConfig::UiSettings::language)
Lang Current();
// The Windows display language as a Lang (English when it is none of the others)
Lang SystemLanguage();
// The language's own name in its own script: "English", "Português", "Deutsch", "日本語", "繁體中文" ...
const char* NativeName(Lang lang);
// The code used in [ui] language and in i18n/lang_<code>.cpp: "en", "pt", "es", "fr", "de", ..., "zh_hans", "zh_hant", "th"
const char* Code(Lang lang);
// The Lang for a code (-1 when unknown, which means automatic)
int FromCode(std::string_view code);

// One translated text: the English key and its translations (nullptr or "" = not translated yet, English is shown)
struct Entry {
    const char* en;
    const char* pt;
    const char* es;
    const char* fr;
};
// A static object in each i18n/tr_*.cpp registers its table (before DllMain's code runs)
struct Table {
    Table(const Entry* entries, size_t count);
};

// One text of a single-language table: the English key (byte-identical to the tr_*.cpp key) and its translation
// (nullptr or "" = not translated yet). {nullptr, nullptr} entries are skipped (an empty table keeps one as a sentinel).
struct Pair {
    const char* en;
    const char* text;
};
// A static object in each i18n/lang_<code>.cpp registers that language's pairs (any Lang after French)
struct LangTable {
    LangTable(Lang lang, const Pair* pairs, size_t count);
};

// The text in the current language (the English one when there is no translation). Null-safe.
const char* Tr(const char* english);
// The visible part of an ImGui label ("Text##id" -> "Text"), translated
std::string_view TrLabel(const char* label);
// The same for a string view (not null-terminated keys)
std::string_view Tr(std::string_view english);

// Formats a translated format string ("{} turned on"); falls back to the English format when the translation does not
// format with these arguments
template <typename... Args> std::string Trf(const char* englishFormat, const Args&... args) {
    const char* t = Tr(englishFormat);
    if (t != englishFormat) {
        try {
            return std::vformat(t, std::make_format_args(args...));
        } catch (const std::format_error&) {
        }
    }
    return std::vformat(englishFormat, std::make_format_args(args...));
}

// Development build: texts shown in the current language without a translation (since the language was set)
size_t MissingCount();
std::string MissingList(size_t max); // one per line
void ClearMissing();
// Entries whose translation has other {} placeholders than the English text, or drops the key's "##id" (every language)
std::string PlaceholderProblems();
// The same English text in two tables with different translations, four-language entries missing a language, and
// single-language entries whose key is in no tr_*.cpp table or appears twice with different texts (one per line)
std::string TableProblems();
// Translated keys per language, "de 0/1640" (one per line; informational, never a problem)
std::string Coverage();
// Every English key of the tr_*.cpp tables, once each, in registration order (tools/i18n_check compares it with
// i18n/keys.tsv)
std::vector<std::string_view> AllKeys();

} // namespace I18n
