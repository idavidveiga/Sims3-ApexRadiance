// Translations of the texts the Violet widgets write themselves (ui/widgets.cpp; see ui/i18n.h). Part of Apex Radiance.
#include "ui/i18n.h"
#include <iterator>

namespace {

// {English (exactly as in the code), Portuguese (Brazil), Spanish, French}
const I18n::Entry kEntries[] = {
    {"Advanced", "Avançado", "Avanzado", "Avancé"},
    {"Open this page", "Abrir esta página", "Abrir esta página", "Ouvrir cette page"},
    {"Changed from the default", "Diferente do padrão", "Distinto del valor predeterminado", "Modifié par rapport à la valeur par défaut"},
    {"Reset to default", "Voltar ao padrão", "Restablecer valor predeterminado", "Rétablir la valeur par défaut"},
    {"Measured cost on your GPU per frame", "Custo medido na sua GPU por quadro", "Costo medido en tu GPU por fotograma", "Coût mesuré sur votre GPU par image"},
    {"{} turned on", "{} ligado", "{} activado", "{} activé"},
    {"{} turned off", "{} desligado", "{} desactivado", "{} désactivé"},
    {"{} reset", "{} voltou ao padrão", "{} restablecido", "{} réinitialisé"},
    {"{} changed", "{} alterado", "{} cambiado", "{} modifié"},
    // Language names are shown in their own language
    {"English", "English", "English", "English"},
    {"Português", "Português", "Português", "Português"},
    {"Español", "Español", "Español", "Español"},
    {"Français", "Français", "Français", "Français"},
    {"Automatic ({})", "Automático ({})", "Automático ({})", "Automatique ({})"},
    {"Language", "Idioma", "Idioma", "Langue"},
    {"The language of this menu", "O idioma deste menu", "El idioma de este menú", "La langue de ce menu"},
    {"Language, menu key, text size and saving", "Idioma, tecla do menu, tamanho do texto e salvamento", "Idioma, tecla del menú, tamaño del texto y guardado",
     "Langue, touche du menu, taille du texte et sauvegarde"},
};
const I18n::Table kTable(kEntries, std::size(kEntries));

} // namespace
