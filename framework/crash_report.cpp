// Crash report (see crash_report.h).
#include "crash_report.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "build_flavor.h"
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace CrashReport {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
MiniDumpWriteDumpFn g_writeDump = nullptr;
wchar_t g_textPath[MAX_PATH] = {};
wchar_t g_dumpPath[MAX_PATH] = {};
std::atomic<bool> g_installed{false};
std::atomic<LONG> g_written{0};
thread_local int t_depth = 0;

// The feature line: two fixed buffers, the reader takes the last complete one
char g_features[2][1024] = {};
std::atomic<int> g_featuresCurrent{0};
std::mutex g_featuresLock;

struct Out {
    HANDLE file;
    void Write(const char* s) {
        DWORD n = 0;
        WriteFile(file, s, static_cast<DWORD>(std::strlen(s)), &n, nullptr);
    }
};

// "ADDRESS Module+0xOFFSET" for an address inside a loaded image, "ADDRESS (private memory)" otherwise; false when it
// is not committed memory. The base comes from VirtualQuery (no loader lock), the name from GetModuleFileNameW.
bool Describe(uintptr_t addr, char* out, size_t size, bool* executable = nullptr) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (executable)
        *executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    if (mbi.Type != MEM_IMAGE || !mbi.AllocationBase) {
        std::snprintf(out, size, "%08X (%s memory)", static_cast<unsigned>(addr), mbi.Type == MEM_PRIVATE ? "private" : "mapped");
        return true;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    char name[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof name, nullptr, nullptr);
    std::snprintf(out, size, "%08X %s+0x%X", static_cast<unsigned>(addr), name[0] ? name : "?",
                  static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
    return true;
}

bool Readable(uintptr_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void WriteText(EXCEPTION_POINTERS* ep) {
    HANDLE f = CreateFileW(g_textPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    Out o{f};
    char line[1400], a[400], b[400];
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(line, sizeof line, "%s %s (%s) crash report, %04u-%02u-%02u %02u:%02u:%02u.%03u, thread %lu\r\n", APEX_PRODUCT_NAME, APEX_VERSION_STRING,
                  kPublicBuild ? "normal mode" : "developer mode", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  GetCurrentThreadId());
    o.Write(line);
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    const uintptr_t at = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
    if (!Describe(at, a, sizeof a)) std::snprintf(a, sizeof a, "%08X (not mapped)", static_cast<unsigned>(at));
    std::snprintf(line, sizeof line, "Exception %08lX at %s", er->ExceptionCode, a);
    o.Write(line);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        std::snprintf(line, sizeof line, ", %s %08X\r\n", er->ExceptionInformation[0] == 8 ? "executing" : (er->ExceptionInformation[0] ? "writing" : "reading"),
                      static_cast<unsigned>(er->ExceptionInformation[1]));
    else std::snprintf(line, sizeof line, "\r\n");
    o.Write(line);
    std::snprintf(line, sizeof line, "EAX %08lX EBX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX EBP %08lX ESP %08lX EIP %08lX\r\n", c->Eax, c->Ebx, c->Ecx, c->Edx,
                  c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);
    o.Write(line);
    // registers that point into code (a call through a register, a this pointer's vtable ...)
    const DWORD regs[] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp};
    const char* names[] = {"EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "EBP"};
    for (int i = 0; i < 7; i++) {
        bool exec = false;
        if (Describe(regs[i], b, sizeof b, &exec) && exec) {
            std::snprintf(line, sizeof line, "  %s -> %s\r\n", names[i], b);
            o.Write(line);
        }
    }
    const int cur = g_featuresCurrent.load(std::memory_order_acquire);
    std::snprintf(line, sizeof line, "Apex features on: %s\r\n", g_features[cur][0] ? g_features[cur] : "(not recorded yet)");
    o.Write(line);
    // The stack: every value that points into a module's code (return addresses, mostly), with its position
    o.Write("Stack values pointing into code (ESP+offset: address module+offset):\r\n");
    const uintptr_t sp = c->Esp;
    int shown = 0;
    for (uintptr_t off = 0; off < 0x4000 && shown < 96; off += 4) {
        const uintptr_t p = sp + off;
        if ((off == 0 || (p & 0xFFF) == 0) && !Readable(p, 4)) break;
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
        bool exec = false;
        if (v < 0x10000 || !Describe(v, b, sizeof b, &exec) || !exec) continue;
        std::snprintf(line, sizeof line, "  +%04X: %s\r\n", static_cast<unsigned>(off), b);
        o.Write(line);
        shown++;
    }
    // Raw words at ESP (the first 32), for the case where the return address itself is the garbage
    o.Write("Raw stack at ESP:\r\n");
    for (uintptr_t off = 0; off < 0x80; off += 0x20) {
        if (!Readable(sp + off, 0x20)) break;
        const DWORD* w = reinterpret_cast<const DWORD*>(sp + off);
        std::snprintf(line, sizeof line, "  +%02X: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\r\n", static_cast<unsigned>(off), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                      w[7]);
        o.Write(line);
    }
    o.Write(g_writeDump ? "Minidump: ApexRadiance_Crash.dmp\r\n" : "Minidump: not written (dbghelp.dll not available)\r\n");
    FlushFileBuffers(f);
    CloseHandle(f);
}

void WriteDump(EXCEPTION_POINTERS* ep) {
    if (!g_writeDump) return;
    HANDLE f = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo);
    g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
    CloseHandle(f);
}

void WriteReport(EXCEPTION_POINTERS* ep) {
    __try {
        WriteText(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        WriteDump(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    // A filter that took over and chains back to this one would loop: a second entry on the same thread passes
    if (t_depth > 0) return EXCEPTION_CONTINUE_SEARCH;
    struct Depth {
        Depth() { ++t_depth; }
        ~Depth() { --t_depth; }
    } depth;
    if (g_written.exchange(1) == 0 && ep && ep->ExceptionRecord && ep->ContextRecord) WriteReport(ep);
    return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Install() {
    if (g_installed.load()) return;
    const std::wstring dir = ApexPaths::ApexDirectory();
    if (dir.empty() || dir.size() + 32 >= MAX_PATH) return;
    wcscpy_s(g_textPath, (dir + L"ApexRadiance_Crash.txt").c_str());
    wcscpy_s(g_dumpPath, (dir + L"ApexRadiance_Crash.dmp").c_str());
    if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    g_previous = SetUnhandledExceptionFilter(Filter);
    g_installed.store(true);
}

void Refresh() {
    if (!g_installed.load()) return;
    const LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(Filter);
    if (cur != Filter) g_previous = cur; // someone set theirs since: it runs after the report
}

void SetFeatureLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_featuresLock);
    const int next = 1 - g_featuresCurrent.load(std::memory_order_relaxed);
    strncpy_s(g_features[next], line.c_str(), _TRUNCATE);
    g_featuresCurrent.store(next, std::memory_order_release);
}

} // namespace CrashReport
