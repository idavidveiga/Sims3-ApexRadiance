// Offline check of the menu translation tables (console output only). Fails (exit 1, last line PROBLEMS) on: texts in
// two tables with different translations, four-language entries missing a language, single-language (lang_<code>.cpp)
// keys given two different texts, translations whose {} placeholders or "##id" tail differ from the English text.
// Informational lines (never a failure): how many keys each single-language table translates, keys of those tables
// that no tr_*.cpp table has any more (stale), and whether i18n/keys.tsv still lists exactly the current keys.
// Build and run (x86 Native Tools prompt, from the repository root; the release script does the same):
//   cl /nologo /std:c++20 /utf-8 /EHsc /I. /Iui /Iframework tools\i18n_check\i18n_check.cpp ui\i18n.cpp i18n\tr_*.cpp i18n\lang_*.cpp user32.lib /Fe:%TEMP%\i18n_check.exe
#include "ui/i18n.h"
#include <cstdio>
#include <fstream>
#include <set>
#include <string>

namespace {

// The key column of i18n/keys.tsv, unescaped (tools/i18n_keys.pl writes \t, \n, \r and \\ as escapes)
bool ReadTsvKeys(const char* path, std::set<std::string>& keys) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (header) {
            header = false;
            continue;
        }
        const std::string field = line.substr(0, line.find('\t'));
        std::string key;
        for (size_t i = 0; i < field.size(); i++) {
            if (field[i] == '\\' && i + 1 < field.size()) {
                const char c = field[++i];
                key += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
            } else {
                key += field[i];
            }
        }
        keys.insert(key);
    }
    return true;
}

} // namespace

int main() {
    const std::string tables = I18n::TableProblems();
    const std::string placeholders = I18n::PlaceholderProblems();
    std::printf("%s", tables.c_str());
    if (!placeholders.empty()) std::printf("placeholders differ:\n%s", placeholders.c_str());

    std::printf("coverage of the single-language tables:\n%s", I18n::Coverage().c_str());
    std::set<std::string> current, tsv;
    for (std::string_view k : I18n::AllKeys()) current.emplace(k);
    if (!ReadTsvKeys("i18n\\keys.tsv", tsv)) std::printf("note: i18n\\keys.tsv not found (run perl tools/i18n_keys.pl)\n");
    else if (tsv != current)
        std::printf("note: i18n\\keys.tsv is out of date (%zu keys listed, %zu in the tables): run perl tools/i18n_keys.pl\n", tsv.size(), current.size());
    else std::printf("i18n\\keys.tsv is up to date (%zu keys)\n", current.size());

    std::printf("%s\n", tables.empty() && placeholders.empty() ? "OK" : "PROBLEMS");
    return tables.empty() && placeholders.empty() ? 0 : 1;
}
