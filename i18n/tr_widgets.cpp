// Translations of the texts the Violet widgets write themselves (ui/widgets.cpp; see ui/i18n.h). Part of Apex Radiance.
#include "ui/i18n.h"
#include <iterator>

namespace {

// {English (exactly as in the code), Portuguese (Brazil), Spanish}
const I18n::Entry kEntries[] = {
    {"Advanced", "Avançado", "Avanzado"},
    {"Open this page", "Abrir esta página", "Abrir esta página"},
    {"Changed from the default", "Diferente do padrão", "Distinto del valor predeterminado"},
    {"Reset to default", "Voltar ao padrão", "Restablecer valor predeterminado"},
    {"Measured cost on your GPU per frame", "Custo medido na sua GPU por quadro", "Costo medido en tu GPU por fotograma"},
    {"{} turned on", "{} ligado", "{} activado"},
    {"{} turned off", "{} desligado", "{} desactivado"},
    {"{} reset", "{} voltou ao padrão", "{} restablecido"},
    {"{} changed", "{} alterado", "{} cambiado"},
    // Language names are shown in their own language
    {"English", "English", "English"},
    {"Português", "Português", "Português"},
    {"Español", "Español", "Español"},
    {"Automatic ({})", "Automático ({})", "Automático ({})"},
    {"Language", "Idioma", "Idioma"},
    {"The language of this menu", "O idioma deste menu", "El idioma de este menú"},
    {"Language, menu key, text size and saving", "Idioma, tecla do menu, tamanho do texto e salvamento", "Idioma, tecla del menú, tamaño del texto y guardado"},
};
const I18n::Table kTable(kEntries, std::size(kEntries));

} // namespace
