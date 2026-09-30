// Translations of the menu texts (see ui/i18n.h). Part of Apex Radiance.
// Performance features' descriptions (patches/performance_patches.cpp, shown on hover), the "Not available" note
// (framework/patch_base.cpp, ApexPatch::UnavailableReason) and the Sims3SettingsSetter detection line
// (framework/s3ss_detect.cpp, S3SSDetect::Summary).
#include "ui/i18n.h"
#include "apex_version.h"
#include <iterator>

namespace {

// {English (exactly as in the code), Portuguese (Brazil), Spanish, French}
const I18n::Entry kEntries[] = {
    // ---- Performance feature descriptions (hover) ----
    {"Remembers which of the game's packages holds each file the game asks for, so it does not search every package again. Fewer small "
     "stutters when objects, textures and lots load. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Memoriza qual pacote do jogo contém cada arquivo que o jogo pede, para não procurar em todos os pacotes de novo. Menos travadas "
     "pequenas quando objetos, texturas e lotes carregam. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Recuerda qué paquete del juego contiene cada archivo que el juego pide, para no volver a buscar en todos los paquetes. Menos tirones "
     "pequeños al cargar objetos, texturas y solares. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Retient quel paquet du jeu contient chaque fichier que le jeu demande, pour ne pas fouiller à nouveau tous les paquets. Moins de "
     "petites saccades au chargement des objets, textures et terrains. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Lets Faster Game File Lookups also remember files that no package has, so the game does not search every package for them again "
     "and again. Needs Faster Game File Lookups. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Faz as buscas rápidas de arquivos também memorizarem os arquivos que nenhum pacote tem, para o jogo não procurá-los em todos os "
     "pacotes repetidas vezes. Requer as buscas rápidas de arquivos. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Hace que las búsquedas rápidas de archivos también recuerden los archivos que ningún paquete tiene, para que el juego no los busque "
     "en todos los paquetes una y otra vez. Requiere las búsquedas rápidas de archivos. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Permet aux recherches rapides de fichiers de retenir aussi les fichiers qu'aucun paquet ne contient, pour que le jeu ne les cherche "
     "plus sans cesse dans tous les paquets. Nécessite les recherches rapides de fichiers. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Remembers which files of a kind each of the game's packages holds, so Create a Sim and Sim loading do not read the list of every "
     "package again. Fewer small stutters when Sims change outfits or load. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Memoriza quais arquivos de cada tipo cada pacote do jogo contém, para o Criar um Sim e o carregamento de Sims não lerem de novo a "
     "lista de todos os pacotes. Menos travadas pequenas quando Sims trocam de roupa ou carregam. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Recuerda qué archivos de cada tipo contiene cada paquete del juego, para que Crear un Sim y la carga de Sims no vuelvan a leer la "
     "lista de todos los paquetes. Menos tirones pequeños cuando los Sims cambian de ropa o cargan. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Retient quels fichiers de chaque type contient chaque paquet du jeu, pour que Créer un Sim et le chargement des Sims ne relisent pas "
     "la liste de tous les paquets. Moins de petites saccades quand les Sims changent de tenue ou se chargent. Fait partie d'" APEX_PRODUCT_NAME
     ". Crédits : @loinyx"},
    {"While the camera moves, the soft ambient shading of the outdoor walls of newly loaded lots waits until the camera stops (or "
     "two seconds), and never more than one wall pass runs per frame, so panning over a neighborhood that is loading stutters "
     "less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Enquanto a câmera se move, o sombreamento suave das paredes externas dos lotes recém-carregados espera a câmera parar (ou dois "
     "segundos), e nunca roda mais de uma passada de paredes por quadro, então mover a câmera sobre um bairro carregando trava menos. "
     "Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Mientras la cámara se mueve, el sombreado suave de las paredes exteriores de los solares recién cargados espera a que la cámara se "
     "detenga (o dos segundos), y nunca se hace más de una pasada de paredes por fotograma, así que recorrer un barrio que se está cargando "
     "da menos tirones. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Pendant que la caméra bouge, l'ombrage doux des murs extérieurs des terrains tout juste chargés attend que la caméra s'arrête (ou deux "
     "secondes), et jamais plus d'une passe de murs par image : survoler un quartier en cours de chargement saccade moins. Fait partie d'"
     APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"While the camera moves, lots relight in smaller steps each frame instead of taking up to 15 ms at once, so panning over busy "
     "neighborhoods stutters less. Lights finish as soon as the camera stops. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Enquanto a câmera se move, os lotes são reiluminados em passos menores a cada quadro em vez de levar até 15 ms de uma vez, então "
     "mover a câmera sobre bairros cheios trava menos. As luzes terminam assim que a câmera para. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Mientras la cámara se mueve, los solares se reiluminan en pasos más pequeños cada fotograma en lugar de tomar hasta 15 ms de golpe, "
     "así que recorrer barrios concurridos da menos tirones. Las luces terminan en cuanto la cámara se detiene. Parte de " APEX_PRODUCT_NAME
     ". Créditos: @loinyx",
     "Pendant que la caméra bouge, les terrains se rééclairent par petites étapes à chaque image au lieu de prendre jusqu'à 15 ms d'un coup : "
     "survoler des quartiers animés saccade moins. L'éclairage se termine dès que la caméra s'arrête. Fait partie d'" APEX_PRODUCT_NAME
     ". Crédits : @loinyx"},
    {"Compresses the textures the game builds while you play (terrain, Sims, lot views, thumbnails) several times faster, with "
     "exactly the same result, so those moments stutter less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Comprime as texturas que o jogo cria enquanto você joga (terreno, Sims, vistas de lotes, miniaturas) várias vezes mais rápido, com "
     "exatamente o mesmo resultado, então esses momentos travam menos. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Comprime las texturas que el juego crea mientras juegas (terreno, Sims, vistas de solares, miniaturas) varias veces más rápido, con "
     "exactamente el mismo resultado, así que esos momentos dan menos tirones. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Compresse les textures que le jeu crée pendant que vous jouez (sol, Sims, vues des terrains, miniatures) plusieurs fois plus vite, "
     "avec exactement le même résultat : ces moments saccadent moins. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Compresses what the game stores in its caches and saves (Sims, objects, terrain) with a much faster compressor in the game's own "
     "format, so those moments stutter less. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Comprime o que o jogo guarda nos caches e nos saves (Sims, objetos, terreno) com um compressor muito mais rápido no próprio formato "
     "do jogo, então esses momentos travam menos. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Comprime lo que el juego guarda en sus cachés y partidas (Sims, objetos, terreno) con un compresor mucho más rápido en el propio "
     "formato del juego, así que esos momentos dan menos tirones. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Compresse ce que le jeu stocke dans ses caches et ses sauvegardes (Sims, objets, sol) avec un compresseur bien plus rapide, au format "
     "du jeu : ces moments saccadent moins. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"While the camera moves, objects that just loaded or moved are placed in the scene a few hundred per frame instead of all "
     "at once, so panning over a lot that streams in stutters less. An object may appear a frame or two later; everything is "
     "placed at once as soon as the camera stops. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Enquanto a câmera se move, objetos que acabaram de carregar ou se mover entram na cena algumas centenas por quadro em vez de todos de "
     "uma vez, então mover a câmera sobre um lote carregando trava menos. Um objeto pode aparecer um ou dois quadros depois; tudo entra de "
     "uma vez assim que a câmera para. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Mientras la cámara se mueve, los objetos que acaban de cargarse o moverse entran en la escena unos cientos por fotograma en lugar de "
     "todos a la vez, así que recorrer un solar que se está cargando da menos tirones. Un objeto puede aparecer uno o dos fotogramas "
     "después; todo entra de golpe en cuanto la cámara se detiene. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Pendant que la caméra bouge, les objets qui viennent de se charger ou de bouger entrent dans la scène quelques centaines par image au "
     "lieu de tous d'un coup : survoler un terrain en cours de chargement saccade moins. Un objet peut apparaître une ou deux images plus "
     "tard ; tout entre d'un coup dès que la caméra s'arrête. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},
    {"Remembers where the game found each lot when it looks one up by its ID, instead of searching the whole world every time. "
     "Fewer stutters when lot lights update and less work for the game's scripts. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
     "Memoriza onde o jogo encontrou cada lote ao procurá-lo pelo ID, em vez de vasculhar o mundo inteiro toda vez. Menos travadas quando "
     "as luzes dos lotes atualizam e menos trabalho para os scripts do jogo. Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Recuerda dónde encontró el juego cada solar al buscarlo por su ID, en lugar de recorrer todo el mundo cada vez. Menos tirones cuando "
     "se actualizan las luces de los solares y menos trabajo para los scripts del juego. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Retient où le jeu a trouvé chaque terrain quand il le cherche par son ID, au lieu de parcourir tout le monde à chaque fois. Moins de "
     "saccades quand l'éclairage des terrains se met à jour et moins de travail pour les scripts du jeu. Fait partie d'" APEX_PRODUCT_NAME
     ". Crédits : @loinyx"},
    {"Rooms light up much sooner when you enter a lot, change floors or switch lamps: the lot you are on and the floor you look at "
     "go first, rooms reach their final look in fewer steps, and several small rooms are lit per frame. Part of " APEX_PRODUCT_NAME ". "
     "Credits: @loinyx",
     "Os cômodos acendem bem mais rápido quando você entra num lote, troca de andar ou mexe nas luzes: o lote onde você está e o andar "
     "que você olha vêm primeiro, os cômodos chegam ao visual final em menos etapas e vários cômodos pequenos são iluminados por quadro. "
     "Parte do " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Las habitaciones se iluminan mucho antes al entrar en un solar, cambiar de piso o tocar las lámparas: el solar donde estás y el piso "
     "que miras van primero, las habitaciones llegan a su aspecto final en menos pasos y se iluminan varias habitaciones pequeñas por "
     "fotograma. Parte de " APEX_PRODUCT_NAME ". Créditos: @loinyx",
     "Les pièces s'éclairent bien plus vite quand vous entrez sur un terrain, changez d'étage ou touchez aux lampes : le terrain où vous "
     "êtes et l'étage que vous regardez passent en premier, les pièces atteignent leur aspect final en moins d'étapes et plusieurs petites "
     "pièces sont éclairées par image. Fait partie d'" APEX_PRODUCT_NAME ". Crédits : @loinyx"},

    // ---- "Not available" note of a feature (ApexPatch::UnavailableReason) ----
    {"Not available on {}", "Não disponível em {}", "No disponible en {}", "Non disponible sur {}"},
    {"Not available on {} (game code not scanned yet)", "Não disponível em {} (código do jogo ainda não analisado)",
     "No disponible en {} (código del juego aún no analizado)", "Non disponible sur {} (code du jeu pas encore analysé)"},
    {"Not available on {}: missing {}", "Não disponível em {}: falta {}", "No disponible en {}: falta {}", "Non disponible sur {} : il manque {}"},
    {"an unknown game version", "uma versão desconhecida do jogo", "una versión desconocida del juego", "une version inconnue du jeu"},

    // ---- Sims3SettingsSetter detection line (Settings > Compatibility > Details) ----
    {"not scanned yet", "ainda não verificado", "aún no verificado", "pas encore vérifié"},
    {"official Sims3SettingsSetter loaded ({})", "Sims3SettingsSetter oficial carregado ({})", "Sims3SettingsSetter oficial cargado ({})",
     "Sims3SettingsSetter officiel chargé ({})"},
    {"official Sims3SettingsSetter not loaded", "Sims3SettingsSetter oficial não carregado", "Sims3SettingsSetter oficial no cargado",
     "Sims3SettingsSetter officiel non chargé"},
    {"; OLD COMBINED BUILD loaded ({}): " APEX_PRODUCT_NAME "'s features stay off",
     "; VERSÃO COMBINADA ANTIGA carregada ({}): os recursos do " APEX_PRODUCT_NAME " ficam desligados",
     "; VERSIÓN COMBINADA ANTIGUA cargada ({}): las funciones de " APEX_PRODUCT_NAME " quedan desactivadas",
     "; ANCIENNE VERSION COMBINÉE chargée ({}) : les fonctions d'" APEX_PRODUCT_NAME " restent désactivées"},
    {"; an older {} is also installed (idle): delete it from Game\\Bin", "; um {} mais antigo também está instalado (inativo): apague-o de Game\\Bin",
     "; también hay un {} más antiguo instalado (inactivo): bórralo de Game\\Bin", "; un ancien {} est aussi installé (inactif) : supprimez-le de Game\\Bin"},
};
const I18n::Table kTable(kEntries, std::size(kEntries));

} // namespace
