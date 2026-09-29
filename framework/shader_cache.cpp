// Background precompile of Apex's HLSL shaders (see shader_cache.h).
//
// Why: D3DCompile takes 10-100+ ms per variant (SMAA's blending-weight pass the longest). Called lazily on the render
// thread (first lamp, first roof, first water, first depth-blur or SMAA frame, first lot pass) it made one-time hitches;
// the 2026-09-28 engine study found d3dcompiler_47.dll in 8.5% of the samples of "Render frame" hitches
// (research\perf2\plan.md, items 7 and C5). D3DCompile needs no device and is called here from one worker thread only.
#include "shader_cache.h"
#include "apex_log.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <condition_variable>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>

#pragma comment(lib, "d3dcompiler.lib")

namespace ShaderCache {
namespace {

struct Job {
    Desc d;
    std::vector<DWORD> code; // the bytecode; never changes once state == Done
    std::string error;       // the compiler's message when it failed
    enum State : int { Queued, Compiling, Done } state = Queued;
    bool urgent = false; // a thread waits for it: compiled next
    double ms = 0.0;
};

struct Registry {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::unique_ptr<Job>> jobs; // Id = index; entries never move or go away
    HANDLE worker = nullptr;
    HANDLE workerLeft = nullptr; // manual-reset event: the worker has finished its loop (Shutdown waits for it)
    bool running = false;
    bool stop = false;
    bool started = false;
    int waits = 0;
    double waitMs = 0.0, workerMs = 0.0;
    std::string slowest;
    double slowestMs = 0.0;
};

// Never destroyed (no static destructor at process exit while a worker may still run). Built by the first Add, i.e. by the
// features' namespace-scope initialisers.
Registry& R() {
    static Registry* r = new Registry;
    return *r;
}

double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// The compiler's message without the trailing NUL / line breaks
std::string Message(ID3DBlob* errors) {
    if (!errors) return "unknown error";
    std::string s(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s.empty() ? "unknown error" : s;
}

// Outside the lock: a Compiling job is touched by the thread compiling it only.
void Compile(Job& j) {
    try {
        std::vector<D3D_SHADER_MACRO> macros;
        for (const auto& [name, value] : j.d.macros) macros.push_back({name.c_str(), value.c_str()});
        macros.push_back({nullptr, nullptr});
        ID3DBlob *code = nullptr, *errors = nullptr;
        const double t0 = NowMs();
        const HRESULT hr = D3DCompile(j.d.source.data(), j.d.source.size(), j.d.sourceName, macros.data(), nullptr, j.d.entry, j.d.target, j.d.flags, 0, &code, &errors);
        j.ms = NowMs() - t0;
        if (FAILED(hr) || !code || code->GetBufferSize() < 4) {
            j.error = Message(errors);
        } else {
            j.code.resize((code->GetBufferSize() + 3) / 4, 0);
            std::memcpy(j.code.data(), code->GetBufferPointer(), code->GetBufferSize());
        }
        if (errors) errors->Release();
        if (code) code->Release();
        std::string().swap(j.d.source); // the bytecode is kept for the session: the source is not needed again
    } catch (...) {
        j.code.clear();
        j.error = "out of memory";
    }
}

// The next job: an urgent one, else the lowest priority number, then the oldest. Lock held.
Job* PickLocked(Registry& r) {
    Job* best = nullptr;
    for (auto& j : r.jobs) {
        if (j->state != Job::Queued) continue;
        if (j->urgent) return j.get();
        if (!best || j->d.priority < best->d.priority) best = j.get();
    }
    return best;
}

DWORD WINAPI WorkerProc(LPVOID) {
    Registry& r = R();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // the game is loading meanwhile
    const double t0 = NowMs();
    int n = 0, failed = 0;
    for (;;) {
        Job* j = nullptr;
        {
            std::lock_guard<std::mutex> lk(r.m);
            if (r.stop) break;
            j = PickLocked(r);
            if (!j) break;
            j->state = Job::Compiling;
        }
        Compile(*j);
        {
            std::lock_guard<std::mutex> lk(r.m);
            j->state = Job::Done;
            if (j->ms > r.slowestMs) {
                r.slowestMs = j->ms;
                r.slowest = j->d.tag;
            }
        }
        r.cv.notify_all();
        n++;
        if (!j->error.empty()) {
            failed++;
            LOG_ERROR(std::format("[ShaderCache] {} did not compile: {}", j->d.tag, j->error));
        } else {
            LOG_DEBUG(std::format("[ShaderCache] {}: {:.1f} ms, {} bytes", j->d.tag, j->ms, j->code.size() * 4));
        }
    }
    const double ms = NowMs() - t0;
    std::string slowest;
    double slowestMs = 0.0;
    bool stopped = false;
    {
        std::lock_guard<std::mutex> lk(r.m);
        r.running = false;
        r.workerMs += ms;
        slowest = r.slowest;
        slowestMs = r.slowestMs;
        stopped = r.stop;
    }
    r.cv.notify_all();
    if (n)
        LOG_INFO(std::format("[ShaderCache] Precompiled {} Apex shader{} in {:.0f} ms on a background thread ({} failed; slowest: {}, {:.0f} ms){}", n, n == 1 ? "" : "s", ms, failed,
                             slowest, slowestMs, stopped ? " - stopped early" : ""));
    SetEvent(r.workerLeft);
    return 0;
}

// Lock held
void StartLocked(Registry& r) {
    if (r.running || r.stop) return;
    if (r.worker) {
        CloseHandle(r.worker);
        r.worker = nullptr;
    }
    if (!r.workerLeft) r.workerLeft = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (r.workerLeft) ResetEvent(r.workerLeft);
    r.running = true;
    r.worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (!r.worker) {
        r.running = false;
        LOG_ERROR("[ShaderCache] Could not start the precompile thread: each shader compiles when it is first needed");
    }
}

// The job of `id`, compiled. When the worker has not reached it, it becomes the worker's next job and this thread waits
// (logged). Without a worker (thread creation failed, or stopped) it is compiled here, as a last resort.
Job* Ready(Id id) {
    Registry& r = R();
    std::unique_lock<std::mutex> lk(r.m);
    if (id < 0 || id >= static_cast<int>(r.jobs.size())) return nullptr;
    Job* j = r.jobs[static_cast<size_t>(id)].get();
    if (j->state == Job::Done) return j;
    const double t0 = NowMs();
    j->urgent = true;
    if (!r.started) r.started = true;
    StartLocked(r);
    if (r.running && r.worker) SetThreadPriority(r.worker, THREAD_PRIORITY_NORMAL); // someone waits now
    bool here = false;
    while (j->state != Job::Done) {
        if (!r.running && j->state == Job::Queued) { // no worker left to do it
            j->state = Job::Compiling;
            lk.unlock();
            Compile(*j);
            lk.lock();
            j->state = Job::Done;
            here = true;
            break;
        }
        r.cv.wait(lk);
    }
    const double waited = NowMs() - t0;
    r.waits++;
    r.waitMs += waited;
    lk.unlock();
    r.cv.notify_all();
    LOG_WARNING(std::format("[ShaderCache] {}: {} ({:.1f} ms) on thread {} - the background precompile had not reached it", j->d.tag,
                            here ? "compiled on the requesting thread" : "waited for the precompile", waited, GetCurrentThreadId()));
    if (here && !j->error.empty()) LOG_ERROR(std::format("[ShaderCache] {} did not compile: {}", j->d.tag, j->error));
    return j;
}

} // namespace

Id Add(Desc desc) {
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.m);
    auto job = std::make_unique<Job>();
    job->d = std::move(desc);
    r.jobs.push_back(std::move(job));
    if (r.started && !r.running && !r.stop) StartLocked(r); // added after the precompile finished: compile it too
    return static_cast<Id>(r.jobs.size() - 1);
}

void Start() {
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.m);
    if (r.started) return;
    r.started = true;
    LOG_INFO(std::format("[ShaderCache] Precompiling {} Apex shaders on a background thread", r.jobs.size()));
    StartLocked(r);
}

namespace {
// The bytecode the device actually holds for a shader equals `code` (another mod's CreatePixelShader hook can hand back a
// different shader: a shader replacer keyed on the bytecode's hash or on the creation order)
bool SameFunction(IDirect3DPixelShader9* ps, const std::vector<DWORD>& code, UINT* heldBytes) {
    UINT size = 0;
    *heldBytes = 0;
    if (FAILED(ps->GetFunction(nullptr, &size)) || size == 0) return true; // cannot tell: trust it
    *heldBytes = size;
    if (size != code.size() * sizeof(DWORD)) return false;
    std::vector<DWORD> held(size / sizeof(DWORD));
    return SUCCEEDED(ps->GetFunction(held.data(), &size)) && std::memcmp(held.data(), code.data(), size) == 0;
}
} // namespace

Result CreatePixelShader(IDirect3DDevice9* dev, Id id, IDirect3DPixelShader9** out, std::string* compileError) {
    *out = nullptr;
    const Job* j = Ready(id);
    if (!j || j->code.empty()) {
        if (compileError) *compileError = j ? j->error : std::string("unknown shader id");
        return Result::CompileFailed;
    }
    if (!dev || FAILED(dev->CreatePixelShader(j->code.data(), out)) || !*out) {
        *out = nullptr;
        return Result::CreateFailed;
    }
    UINT held = 0;
    if (!SameFunction(*out, j->code, &held)) {
        // Replaced on the way: try once more with the same program plus a comment token after the version token (the
        // device ignores comments; a replacer matching the bytecode no longer recognises it)
        LOG_WARNING(std::format("[ShaderCache] {}: the device returned another pixel shader than Apex's ({} bytes instead of {}): another mod replaced it; "
                                "retrying with a marked copy",
                                j->d.tag, held, j->code.size() * sizeof(DWORD)));
        std::vector<DWORD> marked;
        marked.reserve(j->code.size() + 2);
        marked.push_back(j->code[0]);             // ps_3_0 version token
        marked.push_back(0x0000FFFE | (1u << 16)); // comment token, 1 DWORD long
        marked.push_back(0x58455041);              // "APEX"
        marked.insert(marked.end(), j->code.begin() + 1, j->code.end());
        IDirect3DPixelShader9* again = nullptr;
        if (SUCCEEDED(dev->CreatePixelShader(marked.data(), &again)) && again) {
            UINT heldAgain = 0;
            if (SameFunction(again, marked, &heldAgain)) {
                (*out)->Release();
                *out = again;
                LOG_INFO(std::format("[ShaderCache] {}: the marked copy is Apex's own shader", j->d.tag));
            } else {
                again->Release();
                LOG_WARNING(std::format("[ShaderCache] {}: replaced again ({} bytes); the effect may look wrong until that mod is removed", j->d.tag, heldAgain));
            }
        }
    }
    return Result::Ok;
}

std::string StatusText() {
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.m);
    const size_t total = r.jobs.size();
    size_t done = 0, failed = 0;
    for (const auto& j : r.jobs) {
        if (j->state != Job::Done) continue;
        done++;
        if (j->code.empty()) failed++;
    }
    std::string s;
    if (!r.started) s = std::format("{} Apex shaders registered, precompile not started", total);
    else if (done < total) s = std::format("precompiling Apex shaders: {} of {} done", done, total);
    else s = std::format("{} Apex shaders precompiled in {:.0f} ms on a background thread (slowest {} ms: {})", total, r.workerMs, static_cast<int>(r.slowestMs), r.slowest);
    if (failed) s += std::format(", {} failed (see ApexRadiance_LOG.txt)", failed);
    s += std::format("; render-thread waits: {}", r.waits);
    if (r.waits) s += std::format(" ({:.1f} ms)", r.waitMs);
    return s;
}

void Shutdown() {
    Registry& r = R();
    HANDLE left = nullptr;
    bool running = false;
    {
        std::lock_guard<std::mutex> lk(r.m);
        r.stop = true;
        running = r.running;
        left = r.workerLeft;
    }
    // Under the loader lock (FreeLibrary): wait until the worker is out of its loop (it stops between two compiles), not
    // for the thread to end, which needs the loader lock.
    if (running && left) WaitForSingleObject(left, 3000);
}

} // namespace ShaderCache
