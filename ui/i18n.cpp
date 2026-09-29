// Menu language (see i18n.h).
#include "i18n.h"
#include "build_flavor.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace I18n {
namespace {

struct Registered {
    const Entry* entries;
    size_t count;
};
// Function-local: the tables register from other files' static initialisers
std::vector<Registered>& Tables() {
    static std::vector<Registered> tables;
    return tables;
}

std::atomic<int> g_choice{-1};
std::atomic<int> g_lang{static_cast<int>(Lang::English)};

std::once_flag g_buildOnce;
std::unordered_map<std::string_view, const Entry*> g_map;

std::mutex g_missingLock;
std::set<std::string, std::less<>> g_missing;

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

const char* Pick(const Entry* e, Lang lang) {
    const char* s = lang == Lang::Portuguese ? e->pt : lang == Lang::Spanish ? e->es : lang == Lang::French ? e->fr : e->en;
    return s && *s ? s : nullptr;
}

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
    if constexpr (kPublicBuild) return;
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
    if (choice >= 0 && choice < static_cast<int>(Lang::Count)) return static_cast<Lang>(choice);
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

} // namespace

Table::Table(const Entry* entries, size_t count) { Tables().push_back({entries, count}); }

int Choice() { return g_choice.load(std::memory_order_relaxed); }

void SetChoice(int choice) {
    if (choice < -1 || choice >= static_cast<int>(Lang::Count)) choice = -1;
    g_choice.store(choice, std::memory_order_relaxed);
    const Lang lang = FromChoice(choice);
    if (static_cast<int>(lang) != g_lang.exchange(static_cast<int>(lang), std::memory_order_relaxed)) ClearMissing();
}

Lang Current() { return static_cast<Lang>(g_lang.load(std::memory_order_relaxed)); }

Lang SystemLanguage() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_PORTUGUESE: return Lang::Portuguese;
    case LANG_SPANISH: return Lang::Spanish;
    case LANG_FRENCH: return Lang::French;
    default: return Lang::English;
    }
}

const char* NativeName(Lang lang) {
    switch (lang) {
    case Lang::Portuguese: return "Português";
    case Lang::Spanish: return "Español";
    case Lang::French: return "Français";
    default: return "English";
    }
}

std::string_view Tr(std::string_view english) {
    const Lang lang = Current();
    if (lang == Lang::English || english.empty()) return english;
    Build();
    const auto it = g_map.find(english);
    if (it != g_map.end())
        if (const char* s = Pick(it->second, lang)) return s;
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
            const std::vector<std::string> en = Placeholders(e.en);
            for (const char* tr : {e.pt, e.es, e.fr})
                if (tr && *tr && Placeholders(tr) != en) {
                    s += e.en;
                    s += " -> ";
                    s += tr;
                    s += '\n';
                }
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
    return s;
}

} // namespace I18n
