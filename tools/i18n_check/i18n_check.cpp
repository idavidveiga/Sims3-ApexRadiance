// Offline check of the menu translation tables (console output only): texts in two tables with different
// translations, entries missing a language, translations whose {} placeholders differ from the English text.
// Build (x86 Native Tools prompt, from the repository root):
//   cl /nologo /std:c++20 /utf-8 /EHsc /I. /Iui /Iframework tools\i18n_check\i18n_check.cpp ui\i18n.cpp i18n\tr_*.cpp /Fe:%TEMP%\i18n_check.exe
#include "ui/i18n.h"
#include <cstdio>

int main() {
    const std::string tables = I18n::TableProblems();
    const std::string placeholders = I18n::PlaceholderProblems();
    std::printf("%s", tables.c_str());
    if (!placeholders.empty()) std::printf("placeholders differ:\n%s", placeholders.c_str());
    std::printf("%s\n", tables.empty() && placeholders.empty() ? "OK" : "PROBLEMS");
    return tables.empty() && placeholders.empty() ? 0 : 1;
}
