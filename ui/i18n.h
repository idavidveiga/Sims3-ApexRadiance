#pragma once
// Menu language (docs/ui.md "Languages"): English, Portuguese (Brazil), Spanish and French.
//
// The English text is the key: code keeps writing English literals, and the text is translated where it is shown. The
// Violet widgets (ui/widgets.h) translate every label, description, tooltip, note, button, tab, chip and title they
// draw, so most call sites need nothing. Text that is not drawn by a widget (ImGui::Text, CalcTextSize for a layout) goes
// through Tr(); text built at run time goes through Trf() with an English format string, whose translation keeps the
// same {} placeholders (a translation that does not format falls back to the English one).
// Translations live in i18n/*.cpp, one table per part of the menu, registered at start-up (I18n::Table). A text with no
// entry, or with an empty translation, shows in English; the development build lists them (Developer > Language).
// Logs, file names, the Developer page and the S3SS / Arro names stay in English.
#include <format>
#include <string>
#include <string_view>

namespace I18n {

enum class Lang : int { English, Portuguese, Spanish, French, Count };

// The saved choice: -1 = automatic (Windows' display language), else a Lang
int Choice();
void SetChoice(int choice); // takes effect at once; the caller saves it (ApexConfig::UiSettings::language)
Lang Current();
// The Windows display language as a Lang (English when it is none of the others)
Lang SystemLanguage();
// "English", "Português", "Español", "Français" (each in its own language)
const char* NativeName(Lang lang);

// One translated text: the English key and its translations (nullptr or "" = not translated yet, English is shown)
struct Entry {
    const char* en;
    const char* pt;
    const char* es;
    const char* fr;
};
// A static object in each i18n/*.cpp registers its table (before DllMain's code runs)
struct Table {
    Table(const Entry* entries, size_t count);
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
// Entries whose translation has other {} placeholders than the English text (checked once; development build)
std::string PlaceholderProblems();
// The same English text in two tables with different translations, and entries missing a language (one per line)
std::string TableProblems();

} // namespace I18n
