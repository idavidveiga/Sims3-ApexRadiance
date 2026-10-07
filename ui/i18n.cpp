// Menu language (see i18n.h).
#include "i18n.h"
#include "build_flavor.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace I18n {
namespace {

constexpr int kLangCount = static_cast<int>(Lang::Count);

struct Registered {
    const Entry* entries;
    size_t count;
};
struct RegisteredPairs {
    const Pair* pairs;
    size_t count;
};
// Function-local: the tables register from other files' static initialisers
std::vector<Registered>& Tables() {
    static std::vector<Registered> tables;
    return tables;
}
// One list per language (only the languages after French get entries)
std::array<std::vector<RegisteredPairs>, kLangCount>& LangTables() {
    static std::array<std::vector<RegisteredPairs>, kLangCount> tables;
    return tables;
}

std::atomic<int> g_choice{-1};
std::atomic<int> g_lang{static_cast<int>(Lang::English)};

std::once_flag g_buildOnce;
std::unordered_map<std::string_view, const Entry*> g_map;
// The single-language maps are built the first time their language is shown (a player uses one language, so the
// other sixteen never cost memory)
std::array<std::once_flag, kLangCount> g_langOnce;
std::array<std::unordered_map<std::string_view, const char*>, kLangCount> g_langMap;

std::mutex g_missingLock;
std::set<std::string, std::less<>> g_missing;

// Same order as Lang. code is what [ui] language saves and the suffix of i18n/lang_<code>.cpp.
struct LangInfo {
    const char* code;
    const char* native;
    Script script;
};
constexpr LangInfo kLangs[] = {
    {"en", "English", Script::Latin},
    {"pt", "Português", Script::Latin},
    {"es", "Español", Script::Latin},
    {"fr", "Français", Script::Latin},
    {"de", "Deutsch", Script::Latin},
    {"it", "Italiano", Script::Latin},
    {"nl", "Nederlands", Script::Latin},
    {"pl", "Polski", Script::Latin},
    {"ru", "Русский", Script::Latin}, // Segoe UI has Cyrillic
    {"cs", "Čeština", Script::Latin},
    {"hu", "Magyar", Script::Latin},
    {"el", "Ελληνικά", Script::Latin}, // Segoe UI has Greek
    {"da", "Dansk", Script::Latin},
    {"sv", "Svenska", Script::Latin},
    {"no", "Norsk", Script::Latin},
    {"fi", "Suomi", Script::Latin},
    {"ja", "日本語", Script::Japanese},
    {"ko", "한국어", Script::Korean},
    {"zh_hans", "简体中文", Script::ChineseSimplified},
    {"zh_hant", "繁體中文", Script::ChineseTraditional},
    {"th", "ไทย", Script::Thai},
};
static_assert(std::size(kLangs) == static_cast<size_t>(Lang::Count), "one LangInfo per Lang");

void Build() {
    std::call_once(g_buildOnce, [] {
        size_t n = 0;
        for (const Registered& t : Tables()) n += t.count;
        g_map.reserve(n);
        for (const Registered& t : Tables())
            for (size_t i = 0; i < t.count; i++)
                if (t.entries[i].en) g_map.emplace(t.entries[i].en, &t.entries[i]); // the first table's entry wins
    });
}

void BuildLang(Lang lang) {
    const int l = static_cast<int>(lang);
    std::call_once(g_langOnce[l], [l] {
        auto& map = g_langMap[l];
        for (const RegisteredPairs& t : LangTables()[l])
            for (size_t i = 0; i < t.count; i++)
                if (t.pairs[i].en && t.pairs[i].text && *t.pairs[i].text) map.emplace(t.pairs[i].en, t.pairs[i].text);
    });
}

const char* Pick(const Entry* e, Lang lang) {
    const char* s = lang == Lang::Portuguese ? e->pt : lang == Lang::Spanish ? e->es : lang == Lang::French ? e->fr : e->en;
    return s && *s ? s : nullptr;
}

// English, Portuguese, Spanish and French are columns of the tr_*.cpp tables; the others use LangTable pairs
bool HasOwnColumn(Lang lang) { return static_cast<int>(lang) <= static_cast<int>(Lang::French); }

// The text lives in this module's image (a string literal of the code), not on the heap or the stack (a text built at
// run time, or one already translated): only literals are keys that can be missing from the tables
bool IsLiteral(const void* p) {
    static uintptr_t begin = 0, end = 0;
    if (!begin) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&IsLiteral), &m))
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(m) + dos->e_lfanew);
        end = reinterpret_cast<uintptr_t>(m) + nt->OptionalHeader.SizeOfImage;
        begin = reinterpret_cast<uintptr_t>(m);
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= begin && a < end;
}

void NoteMissing(std::string_view key) {
    if (kPublicBuild) return;
    if (key.empty() || !IsLiteral(key.data())) return;
    // Values ("12 ms", "~0.4 ms", "80%"), numbers and single symbols are not texts to translate
    if ((key[0] >= '0' && key[0] <= '9') || key[0] == '~' || key[0] == '+' || key[0] == '-') return;
    bool letters = false;
    for (char c : key)
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            letters = true;
            break;
        }
    if (!letters) return;
    std::lock_guard<std::mutex> lock(g_missingLock);
    if (g_missing.size() < 4000 && g_missing.find(key) == g_missing.end()) g_missing.emplace(key);
}

Lang FromChoice(int choice) {
    if (choice >= 0 && choice < kLangCount) return static_cast<Lang>(choice);
    return SystemLanguage();
}

// The placeholders of a format string, in order ("{}", "{:.1f}", ...), without the escaped "{{" / "}}"
std::vector<std::string> Placeholders(const char* s) {
    std::vector<std::string> out;
    for (const char* p = s; p && *p; ++p) {
        if (p[0] == '{' && p[1] == '{') {
            ++p;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            ++p;
            continue;
        }
        if (*p != '{') continue;
        const char* e = std::strchr(p, '}');
        if (!e) break;
        out.emplace_back(p, e + 1);
        p = e;
    }
    return out;
}

// The "##id" tail of a text ("" when none): a translation must end with the same tail, or the ImGui id would change
// with the language
std::string_view IdTail(const char* s) {
    const char* hash = s ? std::strstr(s, "##") : nullptr;
    return hash ? std::string_view(hash) : std::string_view();
}

// A translation that would break formatting or the id (an empty one is only untranslated, not a problem)
bool BadTranslation(const char* en, const char* tr) {
    if (!tr || !*tr) return false;
    return Placeholders(tr) != Placeholders(en) || IdTail(tr) != IdTail(en);
}

} // namespace

Table::Table(const Entry* entries, size_t count) { Tables().push_back({entries, count}); }

LangTable::LangTable(Lang lang, const Pair* pairs, size_t count) {
    // The first four languages have their own column in the tr_*.cpp tables; a pair table for them would be ignored
    const int l = static_cast<int>(lang);
    if (HasOwnColumn(lang) || l >= kLangCount) return;
    LangTables()[l].push_back({pairs, count});
}

Script ScriptOf(Lang lang) {
    const int l = static_cast<int>(lang);
    return l >= 0 && l < kLangCount ? kLangs[l].script : Script::Latin;
}

int Choice() { return g_choice.load(std::memory_order_relaxed); }

void SetChoice(int choice) {
    if (choice < -1 || choice >= kLangCount) choice = -1;
    g_choice.store(choice, std::memory_order_relaxed);
    const Lang lang = FromChoice(choice);
    if (static_cast<int>(lang) != g_lang.exchange(static_cast<int>(lang), std::memory_order_relaxed)) ClearMissing();
}

Lang Current() { return static_cast<Lang>(g_lang.load(std::memory_order_relaxed)); }

Lang SystemLanguage() {
    const LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
    case LANG_PORTUGUESE: return Lang::Portuguese;
    case LANG_SPANISH: return Lang::Spanish;
    case LANG_FRENCH: return Lang::French;
    case LANG_GERMAN: return Lang::German;
    case LANG_ITALIAN: return Lang::Italian;
    case LANG_DUTCH: return Lang::Dutch;
    case LANG_POLISH: return Lang::Polish;
    case LANG_RUSSIAN: return Lang::Russian;
    case LANG_CZECH: return Lang::Czech;
    case LANG_HUNGARIAN: return Lang::Hungarian;
    case LANG_GREEK: return Lang::Greek;
    case LANG_DANISH: return Lang::Danish;
    case LANG_SWEDISH: return Lang::Swedish;
    case LANG_NORWEGIAN: return Lang::Norwegian; // Bokmal and Nynorsk share the primary id
    case LANG_FINNISH: return Lang::Finnish;
    case LANG_JAPANESE: return Lang::Japanese;
    case LANG_KOREAN: return Lang::Korean;
    case LANG_THAI: return Lang::Thai;
    case LANG_CHINESE:
        // Taiwan, Hong Kong, Macao and the neutral zh-Hant (sublanguage 0x1F) write Traditional characters; mainland
        // China, Singapore and the neutral zh-Hans write Simplified ones
        switch (SUBLANGID(id)) {
        case SUBLANG_CHINESE_TRADITIONAL:
        case SUBLANG_CHINESE_HONGKONG:
        case SUBLANG_CHINESE_MACAU:
        case 0x1F: return Lang::ChineseTraditional;
        default: return Lang::ChineseSimplified;
        }
    default: return Lang::English;
    }
}

const char* NativeName(Lang lang) {
    const int l = static_cast<int>(lang);
    return l >= 0 && l < kLangCount ? kLangs[l].native : kLangs[0].native;
}

const char* Code(Lang lang) {
    const int l = static_cast<int>(lang);
    return l >= 0 && l < kLangCount ? kLangs[l].code : kLangs[0].code;
}

int FromCode(std::string_view code) {
    for (int l = 0; l < kLangCount; l++)
        if (code == kLangs[l].code) return l;
    return -1;
}

std::string_view Tr(std::string_view english) {
    const Lang lang = Current();
    if (lang == Lang::English || english.empty()) return english;
    if (HasOwnColumn(lang)) {
        Build();
        const auto it = g_map.find(english);
        if (it != g_map.end())
            if (const char* s = Pick(it->second, lang)) return s;
    } else {
        BuildLang(lang);
        const auto& map = g_langMap[static_cast<int>(lang)];
        const auto it = map.find(english);
        if (it != map.end()) return it->second;
    }
    NoteMissing(english);
    return english;
}

const char* Tr(const char* english) {
    if (!english || !*english || Current() == Lang::English) return english;
    const std::string_view t = Tr(std::string_view(english));
    return t.data() == english ? english : t.data(); // table strings are null-terminated
}

std::string_view TrLabel(const char* label) {
    if (!label) return {};
    const char* hash = std::strstr(label, "##");
    return Tr(std::string_view(label, hash ? static_cast<size_t>(hash - label) : std::strlen(label)));
}

size_t MissingCount() {
    std::lock_guard<std::mutex> lock(g_missingLock);
    return g_missing.size();
}

std::string MissingList(size_t max) {
    std::lock_guard<std::mutex> lock(g_missingLock);
    std::string s;
    size_t n = 0;
    for (const std::string& m : g_missing) {
        if (n++ >= max) break;
        s += m;
        s += '\n';
    }
    return s;
}

void ClearMissing() {
    std::lock_guard<std::mutex> lock(g_missingLock);
    g_missing.clear();
}

std::string PlaceholderProblems() {
    std::string s;
    for (const Registered& t : Tables())
        for (size_t i = 0; i < t.count; i++) {
            const Entry& e = t.entries[i];
            if (!e.en) continue;
            for (const char* tr : {e.pt, e.es, e.fr})
                if (BadTranslation(e.en, tr)) s += std::string(e.en) + " -> " + tr + '\n';
        }
    for (int l = 0; l < kLangCount; l++)
        for (const RegisteredPairs& t : LangTables()[l])
            for (size_t i = 0; i < t.count; i++) {
                const Pair& p = t.pairs[i];
                if (p.en && BadTranslation(p.en, p.text)) s += std::string(kLangs[l].code) + ": " + p.en + " -> " + p.text + '\n';
            }
    return s;
}

std::string TableProblems() {
    auto same = [](const char* a, const char* b) { return std::strcmp(a ? a : "", b ? b : "") == 0; };
    std::string s;
    std::unordered_map<std::string_view, const Entry*> seen;
    for (const Registered& t : Tables())
        for (size_t i = 0; i < t.count; i++) {
            const Entry& e = t.entries[i];
            if (!e.en) continue;
            if (!e.pt || !*e.pt || !e.es || !*e.es || !e.fr || !*e.fr) s += std::string("missing a language: ") + e.en + '\n';
            const auto [it, added] = seen.emplace(e.en, &e);
            if (!added && !(same(it->second->pt, e.pt) && same(it->second->es, e.es) && same(it->second->fr, e.fr)))
                s += std::string("two different translations: ") + e.en + '\n';
        }
    // Single-language tables: the same key twice with two texts (which one shows would depend on the link order)
    for (int l = 0; l < kLangCount; l++) {
        std::unordered_map<std::string_view, const char*> pairs;
        for (const RegisteredPairs& t : LangTables()[l])
            for (size_t i = 0; i < t.count; i++) {
                const Pair& p = t.pairs[i];
                if (!p.en || !p.text || !*p.text) continue;
                const auto [it, added] = pairs.emplace(p.en, p.text);
                if (!added && !same(it->second, p.text)) s += std::string(kLangs[l].code) + ": two different translations: " + p.en + '\n';
            }
    }
    return s;
}

std::vector<std::string_view> AllKeys() {
    std::vector<std::string_view> out;
    std::set<std::string_view> seen;
    for (const Registered& t : Tables())
        for (size_t i = 0; i < t.count; i++)
            if (t.entries[i].en && seen.insert(t.entries[i].en).second) out.emplace_back(t.entries[i].en);
    return out;
}

std::string Coverage() {
    const std::vector<std::string_view> all = AllKeys();
    const std::set<std::string_view> keys(all.begin(), all.end());
    std::string s;
    for (int l = static_cast<int>(Lang::French) + 1; l < kLangCount; l++) {
        std::set<std::string_view> translated;
        std::string stale;
        size_t staleCount = 0;
        for (const RegisteredPairs& t : LangTables()[l])
            for (size_t i = 0; i < t.count; i++) {
                const Pair& p = t.pairs[i];
                if (!p.en || !p.text || !*p.text) continue;
                // A key no tr_*.cpp table has any more: the English text changed or was removed, so this translation
                // never shows; translate the new key and delete this pair
                if (!keys.count(p.en)) {
                    if (staleCount++ < 20) stale += std::string("  stale: ") + p.en + '\n';
                    continue;
                }
                translated.insert(p.en);
            }
        s += std::format("{} {}/{}", kLangs[l].code, translated.size(), keys.size());
        if (staleCount) s += std::format(" ({} stale keys)", staleCount);
        s += '\n';
        s += stale;
    }
    return s;
}

} // namespace I18n
