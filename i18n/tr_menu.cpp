// Translations of the menu texts (see ui/i18n.h). Part of Apex Radiance.
#include "ui/i18n.h"
#include "apex_version.h"
#include <iterator>

namespace {

// {English (exactly as in the code), Portuguese (Brazil), Spanish}
const I18n::Entry kEntries[] = {
    {nullptr, nullptr, nullptr}, // placeholder
};
const I18n::Table kTable(kEntries, std::size(kEntries));

} // namespace
