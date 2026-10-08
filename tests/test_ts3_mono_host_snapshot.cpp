// Synthetic read-only x86 process memory: no EA game files, hooks or calls.
#include "ts3_mono_host_snapshot.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <unordered_map>
using namespace ApexCasMono;

namespace {
constexpr std::uint32_t kBase = 0x00400000u;
constexpr std::uint32_t kSize = 0x01200000u;
constexpr std::uint32_t kInitHeap = 0x00D7F2B0u;
constexpr std::uint32_t kHostGlobal = 0x011EE514u;
constexpr std::uint32_t kHost = 0x30100000u;
constexpr std::uint32_t kDomain = 0x30200000u;

struct Memory {
    std::unordered_map<std::uint32_t, std::uint8_t> data;
    void Word(std::uint32_t addr, std::uint32_t word) {
        for (unsigned i = 0; i < 4; ++i)
            data[addr + i] = static_cast<std::uint8_t>(word >> (8u * i));
    }
    bool Read(std::uint32_t addr, void* out, std::size_t length) const {
        if (!addr || (length && addr > UINT32_MAX - (length - 1))) return false;
        auto* bytes = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < length; ++i) {
            const auto it = data.find(addr + static_cast<std::uint32_t>(i));
            if (it == data.end()) return false;
            bytes[i] = it->second;
        }
        return true;
    }
    Memory() {
        const auto& p = kBridgePatterns[
            static_cast<std::size_t>(BridgeSite::ScriptHostInitHeap)];
        std::size_t index = 0;
        for (std::size_t pos = 0; pos < p.bytes.size();) {
            if (p.bytes[pos] == ' ') { ++pos; continue; }
            const int hi = Hex(p.bytes[pos]), lo = Hex(p.bytes[pos + 1]);
            data[kInitHeap + static_cast<std::uint32_t>(index++)] =
                hi < 0 ? 0u : static_cast<std::uint8_t>((hi << 4) | lo);
            pos += 2;
        }
        assert(index == p.length);
        Word(kInitHeap + 2, kHostGlobal);
        Word(kHostGlobal, kHost);
        Word(kHost + kMonoHostDomainOffset, kDomain);
    }
    HostSnapshot Inspect(std::uint32_t base = kBase,
                         std::uint32_t size = kSize,
                         std::uint32_t entry = kInitHeap) const {
        return InspectMonoHost(base, size, entry, [&](std::uint32_t addr,
            void* out, std::size_t length) { return Read(addr, out, length); });
    }
};

void TestReadyAndNoSideEffects() {
    const Memory m;
    const auto r = m.Inspect();
    assert(r.Ready());
    assert(r.globalRva == kHostGlobal - kBase);
    assert(r.host == kHost && r.domain == kDomain);
    assert(std::string_view(HostStatusName(r.status)).find("structurally available") !=
           std::string_view::npos);
}
void TestRejectUnknownAndWrongCode() {
    Memory m;
    assert(m.Inspect(kBase,kSize,0).status == HostStatus::NotFound);
    assert(m.Inspect(0,kSize).status == HostStatus::InvalidExeBounds);
    assert(m.Inspect(kBase,1).status == HostStatus::InvalidExeBounds);
    assert(m.Inspect(0xFFFFFFF0u,kSize).status == HostStatus::InvalidExeBounds);
    m.data[kInitHeap] = 0x90;
    assert(m.Inspect().status == HostStatus::InvalidInitHeap);
    m.data.erase(kInitHeap+7);
    assert(m.Inspect().status == HostStatus::InvalidInitHeap);
}
void TestGlobalStrictlyWithinModule() {
    Memory m;
    m.Word(kInitHeap+2, kBase-4);
    assert(m.Inspect().status == HostStatus::GlobalOutsideImage);
    m.Word(kInitHeap+2, kBase+kSize-2);
    assert(m.Inspect().status == HostStatus::GlobalOutsideImage);
    m.Word(kInitHeap+2, kHostGlobal+1);
    assert(m.Inspect().status == HostStatus::GlobalOutsideImage);
    m.Word(kInitHeap+2,kHostGlobal);
    m.data.erase(kHostGlobal+2);
    assert(m.Inspect().status == HostStatus::UnreadableGlobal);
}
void TestNotReadyAndUnreadableHost() {
    Memory m;
    m.Word(kHostGlobal,0);
    assert(m.Inspect().status == HostStatus::NotReady);
    m.Word(kHostGlobal,0xFFFFFFFCu);
    assert(m.Inspect().status == HostStatus::UnreadableHost);
    m.Word(kHostGlobal,kHost+1);
    assert(m.Inspect().status == HostStatus::UnreadableHost);
    m.Word(kHostGlobal,kHost);
    m.data.erase(kHost + kMonoHostDomainOffset+1);
    assert(m.Inspect().status == HostStatus::UnreadableHost);
}
void TestRequireNonNullAlignedDomain() {
    Memory m;
    m.Word(kHost+kMonoHostDomainOffset,0);
    assert(m.Inspect().status == HostStatus::MissingDomain);
    m.Word(kHost+kMonoHostDomainOffset,kDomain+1);
    assert(m.Inspect().status == HostStatus::MissingDomain);
}
}
int main() {
    TestReadyAndNoSideEffects();
    TestRejectUnknownAndWrongCode();
    TestGlobalStrictlyWithinModule();
    TestNotReadyAndUnreadableHost();
    TestRequireNonNullAlignedDomain();
    std::cout << "PASS: 5 synthetic read-only Mono ScriptHost gate groups\n";
}
