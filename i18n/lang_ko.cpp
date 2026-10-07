// Korean menu texts (see ui/i18n.h; how to translate: i18n/TRANSLATING.md). Part of Apex Radiance.
// One {English key, translation} pair per text. The key is a byte-identical copy of an English text of the
// i18n/tr_*.cpp tables (the full list, with the Portuguese, Spanish and French texts as hints, is i18n/keys.tsv).
// A text without a pair here, or with an empty translation, shows in English.
#include "ui/i18n.h"
#include <iterator>

namespace {

// {English (exactly as in i18n/keys.tsv), Korean}
const I18n::Pair kPairs[] = {
    {nullptr, nullptr}, // keeps the array valid while it has no translations (skipped by the lookup)
};
const I18n::LangTable kTable(I18n::Lang::Korean, kPairs, std::size(kPairs));

} // namespace
