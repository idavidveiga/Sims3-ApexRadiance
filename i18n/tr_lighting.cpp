// Translations of the menu texts (see ui/i18n.h): Night Lights (patches/night_terrain_relight_patch.cpp) and Every-Story
// Ground Light (patches/split_level_ground_light_patch.cpp) on the Lighting and Water & Snow pages. Part of Apex Radiance.
#include "ui/i18n.h"
#include "apex_version.h"
#include <iterator>

namespace {

// {English (exactly as in the code), Portuguese (Brazil), Spanish, French}
const I18n::Entry kEntries[] = {
    // ---- features (names and hover descriptions) ----
    {"Night Lights", "Luzes Noturnas", "Luces Nocturnas", "Lumières Nocturnes"},
    {"At night, street lamps and lot lamps light the ground, objects, fences, walls, roofs, ponds and "
     "snow around them with smooth, warm light and no hard edges at lot borders. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "À noite, postes e lâmpadas do lote iluminam o chão, objetos, cercas, paredes, telhados, lagos e neve em volta com luz "
     "suave e quente, sem bordas marcadas nas divisas dos lotes. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "De noche, las farolas y las lámparas del solar iluminan el suelo, objetos, cercas, paredes, techos, estanques y nieve a "
     "su alrededor con luz suave y cálida, sin bordes marcados en los límites de los solares. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "La nuit, les réverbères et les lampes des terrains éclairent le sol, les objets, les clôtures, les murs, les toits, les "
     "étangs et la neige autour d'une lumière douce et chaude, sans bord net aux limites des terrains. Fait partie "
     "d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Every-Story Ground Light", "Luz no Chão de Todos os Andares", "Luz en el Suelo desde Todos los Pisos", "Lumière au Sol de Tous les Étages"},
    {"Lamps on any floor of a lot light the ground and the yard around the house, with no hard edge at "
     "lot borders or between floors. Part of Night Lights in " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Lâmpadas de qualquer andar do lote iluminam o chão e o quintal em volta da casa, sem bordas marcadas nas divisas dos "
     "lotes nem entre andares. Parte das Luzes Noturnas do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Las lámparas de cualquier piso de un solar iluminan el suelo y el jardín alrededor de la casa, sin bordes marcados en "
     "los límites de los solares ni entre pisos. Parte de Luces Nocturnas en " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Les lampes de n'importe quel étage d'un terrain éclairent le sol et le jardin autour de la maison, sans bord net aux "
     "limites des terrains ni entre les étages. Fait partie des Lumières Nocturnes d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    // Every-Story Ground Light's own controls (RenderCustomUI)
    {"Already handled by Sims3SettingsSetter (its Split-Level Lighting Fix is on)",
     "Já feito pelo Sims3SettingsSetter (o Split-Level Lighting Fix dele está ligado)",
     "Ya lo hace Sims3SettingsSetter (su Split-Level Lighting Fix está activado)",
     "Déjà géré par Sims3SettingsSetter (son Split-Level Lighting Fix est activé)"},
    {"Lamps on every floor light the ground outside the lot, with no hard edge at the lot border",
     "Lâmpadas de todos os andares iluminam o chão fora do lote, sem borda marcada na divisa",
     "Las lámparas de todos los pisos iluminan el suelo fuera del solar, sin bordes marcados en el límite",
     "Les lampes de chaque étage éclairent le sol hors du terrain, sans bord net à la limite"},

    // ---- shared ----
    {"Reload save", "Recarregar o save", "Recargar la partida", "Recharger la partie"},
    {"This change shows after you load a save again", "Esta mudança aparece depois de carregar um save de novo",
     "Este cambio se ve al volver a cargar una partida", "Ce changement apparaît après avoir rechargé une partie"},
    {"Brightness", "Brilho", "Brillo", "Luminosité"},
    {"Turn it on", "Ligar", "Activar", "Activer"},
    {"Turn both on", "Ligar os dois", "Activar ambos", "Activer les deux"},

    // ---- Lighting > Lamps: lamp color and the reset button ----
    {"Lamp color", "Cor das lâmpadas", "Color de las lámparas", "Couleur des lampes"},
    {"Pink", "Rosa", "Rosa", "Rose"},
    {"Warm white", "Branco quente", "Blanco cálido", "Blanc chaud"},
    {"Stock lamps only; colors you chose in Build mode stay", "Só lâmpadas padrão; cores escolhidas no modo Construir ficam",
     "Solo lámparas de serie; los colores elegidos en modo Construir se mantienen",
     "Lampes d'origine seulement ; les couleurs choisies en mode Construction restent"},
    {"Reset Night Lights", "Restaurar Luzes Noturnas", "Restablecer Luces Nocturnas", "Réinitialiser Lumières Nocturnes"},
    {"Put every Night Lights setting back to default", "Volta todas as opções de Luzes Noturnas ao padrão",
     "Devuelve todos los ajustes de Luces Nocturnas a sus valores predeterminados", "Remet tous les réglages de Lumières Nocturnes par défaut"},
    {"Night Lights reset", "Luzes Noturnas restauradas", "Luces Nocturnas restablecidas", "Lumières Nocturnes réinitialisées"},

    // ---- Lighting > Ground ----
    {"Ground & Lots", "Chão e Lotes", "Suelo y Solares", "Sol et Terrains"},
    {"Lamp light on grass, streets and lots", "Luz das lâmpadas na grama, ruas e lotes", "Luz de lámparas en césped, calles y solares",
     "Lumière des lampes sur l'herbe, les rues et les terrains"},
    {"Street lamps light lots", "Postes iluminam os lotes", "Las farolas iluminan los solares", "Les réverbères éclairent les terrains"},
    {"Street lamp light flows onto lots with no hard edge", "A luz dos postes entra nos lotes sem borda marcada",
     "La luz de las farolas entra en los solares sin bordes marcados", "La lumière des réverbères s'étend sur les terrains sans bord net"},
    {"Lot lamps light the street", "Lâmpadas do lote iluminam a rua", "Las lámparas del solar iluminan la calle", "Les lampes du terrain éclairent la rue"},
    {"Outdoor lot lamps also light the grass and street nearby", "Lâmpadas externas do lote também iluminam a grama e a rua ao lado",
     "Las lámparas exteriores del solar también iluminan el césped y la calle cercanos",
     "Les lampes extérieures du terrain éclairent aussi l'herbe et la rue proches"},
    {"Light passes between floors", "A luz passa entre os andares", "La luz pasa entre pisos", "La lumière passe entre les étages"},
    {"Lamps light the floors above and below, with no hard edge", "Lâmpadas iluminam os andares de cima e de baixo, sem borda marcada",
     "Las lámparas iluminan los pisos de arriba y de abajo, sin bordes marcados",
     "Les lampes éclairent les étages au-dessus et en dessous, sans bord net"},
    {"Smooth ground light", "Luz suave no chão", "Luz suave en el suelo", "Lumière douce au sol"},
    {"Soft lamp light on the ground, without blocky steps or specks", "Luz suave das lâmpadas no chão, sem degraus quadriculados nem manchas",
     "Luz suave de lámparas en el suelo, sin escalones pixelados ni manchas", "Lumière douce des lampes au sol, sans paliers en blocs ni taches"},

    // ---- Lighting > Objects ----
    {"Objects", "Objetos", "Objetos", "Objets"},
    {"Fences, plants and outdoor furniture", "Cercas, plantas e móveis externos", "Cercas, plantas y muebles de exterior",
     "Clôtures, plantes et mobilier extérieur"},
    {"LAMP LIGHT", "LUZ DAS LÂMPADAS", "LUZ DE LÁMPARAS", "LUMIÈRE DES LAMPES"},
    {"Lamps light objects", "Lâmpadas iluminam objetos", "Las lámparas iluminan objetos", "Les lampes éclairent les objets"},
    {"Outdoor objects get lamp light, even in the shade of walls", "Objetos externos recebem luz das lâmpadas, mesmo à sombra de paredes",
     "Los objetos exteriores reciben luz de lámparas, incluso a la sombra de paredes",
     "Les objets extérieurs reçoivent la lumière des lampes, même à l'ombre des murs"},
    {"Raise it if objects look dark next to lamps", "Aumente se os objetos parecerem escuros perto das lâmpadas",
     "Súbelo si los objetos se ven oscuros junto a las lámparas", "Augmentez-la si les objets semblent sombres près des lampes"},
    {"Light stairs, railings, columns", "Iluminar escadas, grades, colunas", "Iluminar escaleras, barandas, columnas",
     "Éclairer escaliers, rampes, colonnes"},
    {"Pieces the game leaves unlit", "Peças que o jogo deixa sem luz", "Piezas que el juego deja sin luz", "Éléments que le jeu laisse sans lumière"},
    {"DOORS, COUNTERS AND FENCES", "PORTAS, BALCÕES E CERCAS", "PUERTAS, ENCIMERAS Y CERCAS", "PORTES, COMPTOIRS ET CLÔTURES"},
    {"Needs \"Street lamps light lots\" and \"Smooth ground light\" (Ground tab)",
     "Requer \"Postes iluminam os lotes\" e \"Luz suave no chão\" (aba Chão)",
     "Requiere \"Las farolas iluminan los solares\" y \"Luz suave en el suelo\" (pestaña Suelo)",
     "Nécessite « Les réverbères éclairent les terrains » et « Lumière douce au sol » (onglet Sol)"},
    {"Needs \"Street lamps light lots\" (Ground tab)", "Requer \"Postes iluminam os lotes\" (aba Chão)",
     "Requiere \"Las farolas iluminan los solares\" (pestaña Suelo)", "Nécessite « Les réverbères éclairent les terrains » (onglet Sol)"},
    {"Needs \"Smooth ground light\" (Ground tab)", "Requer \"Luz suave no chão\" (aba Chão)", "Requiere \"Luz suave en el suelo\" (pestaña Suelo)",
     "Nécessite « Lumière douce au sol » (onglet Sol)"},
    {"Ground light turned on", "Luz do chão ligada", "Luz del suelo activada", "Lumière au sol activée"},
    {"Street lamps light lots turned on", "\"Postes iluminam os lotes\" ligado", "\"Las farolas iluminan los solares\" activado",
     "« Les réverbères éclairent les terrains » activé"},
    {"Smooth ground light turned on", "Luz suave no chão ligada", "Luz suave en el suelo activada", "Lumière douce au sol activée"},
    {"Doors and windows stay lit", "Portas e janelas ficam iluminadas", "Puertas y ventanas siguen iluminadas", "Portes et fenêtres restent éclairées"},
    {"A front door is never darker than the wall around it", "Uma porta nunca fica mais escura que a parede em volta",
     "Una puerta nunca queda más oscura que la pared que la rodea", "Une porte n'est jamais plus sombre que le mur autour"},
    {"Seamless light on pieces", "Luz sem emendas nas peças", "Luz sin cortes en las piezas", "Lumière uniforme sur les éléments"},
    {"Counters and modular pieces outside show no color steps", "Balcões e peças modulares externas sem degraus de cor",
     "Encimeras y piezas modulares exteriores sin saltos de color", "Comptoirs et éléments modulaires extérieurs sans paliers de couleur"},
    {"Seamless light brightness", "Brilho da luz sem emendas", "Brillo de la luz sin cortes", "Luminosité de la lumière uniforme"},
    {"How bright that light is; 100% is the default", "O brilho dessa luz; 100% é o padrão", "Qué tan brillante es esa luz; 100% es el predeterminado",
     "Intensité de cette lumière ; 100% par défaut"},
    {"Fences and stairs catch light", "Cercas e escadas recebem luz", "Cercas y escaleras reciben luz", "Clôtures et escaliers captent la lumière"},
    {"Fences, posts, stairs and their snow match the lit ground", "Cercas, postes, escadas e a neve neles acompanham o chão iluminado",
     "Cercas, postes, escaleras y su nieve igualan el suelo iluminado", "Clôtures, poteaux, escaliers et leur neige suivent le sol éclairé"},
    {"Fence brightness", "Brilho das cercas", "Brillo de las cercas", "Luminosité des clôtures"},
    {"100% matches the ground around them", "100% iguala o chão em volta", "100% iguala el suelo alrededor", "100% correspond au sol autour"},

    // ---- Lighting > Buildings ----
    {"Buildings", "Edifícios", "Edificios", "Bâtiments"},
    {"Outside walls and roofs", "Paredes externas e telhados", "Paredes exteriores y techos", "Murs extérieurs et toits"},
    {"WALLS", "PAREDES", "PAREDES", "MURS"},
    {"How bright lit walls get; 100% is the game's dim look", "Brilho das paredes iluminadas; 100% é o visual fraco do jogo",
     "Brillo de las paredes iluminadas; 100% es el aspecto tenue del juego", "Luminosité des murs éclairés ; 100% = l'aspect terne du jeu"},
    {"ROOFS", "TELHADOS", "TECHOS", "TOITS"},
    {"Lamps light roofs", "Lâmpadas iluminam telhados", "Las lámparas iluminan los techos", "Les lampes éclairent les toits"},
    {"Roofs no longer stay black at night; softer roof shadows too", "Telhados não ficam mais pretos à noite; sombras mais suaves também",
     "Los techos ya no quedan negros de noche; sombras más suaves también", "Les toits ne restent plus noirs la nuit ; ombres plus douces aussi"},
    {"How bright lit roofs get; 60% is the default", "Brilho dos telhados iluminados; 60% é o padrão",
     "Brillo de los techos iluminados; 60% es el predeterminado", "Luminosité des toits éclairés ; 60% par défaut"},

    // ---- Water & Snow > Water ----
    {"Lamp Glow", "Brilho das Lâmpadas", "Resplandor de Lámparas", "Lueur des Lampes"},
    {"Lamp light on ponds at night", "Luz das lâmpadas nos lagos à noite", "Luz de lámparas en los estanques de noche", "Lumière des lampes sur les étangs la nuit"},
    {"Lamps glow on ponds", "Lâmpadas brilham nos lagos", "Las lámparas brillan en los estanques", "Les lampes luisent sur les étangs"},
    {"Ponds glow and sparkle near lamps at night", "Lagos brilham e cintilam perto das lâmpadas à noite",
     "Los estanques brillan y destellan cerca de las lámparas de noche", "Les étangs luisent et scintillent près des lampes la nuit"},
    {"Glow brightness", "Intensidade do brilho", "Intensidad del resplandor", "Intensité de la lueur"},
    {"How bright the glow and sparkles are; 100% is the default", "Força do brilho e dos reflexos; 100% é o padrão",
     "Intensidad del resplandor y los destellos; 100% es el predeterminado", "Intensité de la lueur et des reflets ; 100% par défaut"},

    // ---- Water & Snow > Snow ----
    {"Snow", "Neve", "Nieve", "Neige"},
    {"Sidewalks in winter", "Calçadas no inverno", "Aceras en invierno", "Trottoirs en hiver"},
    {"Walked-on sidewalks", "Calçadas pisadas", "Aceras transitadas", "Trottoirs piétinés"},
    {"How much sidewalk shows through the snow; 0% is the game's look", "Quanto da calçada aparece sob a neve; 0% é o visual do jogo",
     "Cuánta acera se ve bajo la nieve; 0% es el aspecto del juego", "Part du trottoir visible sous la neige ; 0% = l'aspect du jeu"},
    {"Needs \"Street lamps light lots\" (Lighting page, Ground tab)", "Requer \"Postes iluminam os lotes\" (página Iluminação, aba Chão)",
     "Requiere \"Las farolas iluminan los solares\" (página Iluminación, pestaña Suelo)",
     "Nécessite « Les réverbères éclairent les terrains » (page Éclairage, onglet Sol)"},
};
const I18n::Table kTable(kEntries, std::size(kEntries));

} // namespace
