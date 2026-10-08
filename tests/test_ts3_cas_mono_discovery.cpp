// No The Sims 3 binaries. All Mono metadata and runtime callbacks are fake.
// A passing test does not establish the game's EA 1.69 native ABI.
#include "ts3_cas_mono_discovery.h"
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
using namespace ApexCasMono;
struct Memory {
    std::unordered_map<std::uint32_t, std::uint8_t> data;
    void Word(std::uint32_t addr, std::uint32_t v) {
        for (int i=0; i<4; ++i) data[addr+i] = static_cast<std::uint8_t>(v >> (8*i));
    }
    void String(std::uint32_t addr, std::string_view s) {
        for (std::size_t i=0; i<s.size(); ++i)
            data[addr+static_cast<std::uint32_t>(i)] = static_cast<std::uint8_t>(s[i]);
        data[addr+static_cast<std::uint32_t>(s.size())] = 0;
    }
    bool Read(std::uint32_t ptr, void* out, std::size_t n) const {
        if (!ptr || (n && ptr > UINT32_MAX - (n-1))) return false;
        auto* dst = static_cast<std::uint8_t*>(out);
        for (std::size_t i=0; i<n; ++i) {
            const auto item = data.find(ptr+static_cast<std::uint32_t>(i));
            if (item == data.end()) return false;
            dst[i] = item->second;
        }
        return true;
    }
};
struct Fixture {
    Memory memory;
    std::vector<std::uint32_t> methods{0x1100, 0x1000};
    std::uint32_t classResult = 0x2000;
    int lookupCalls = 0, enumerateCalls = 0;
    bool freezeEnumerator = false;
    bool useOpaqueDecreasingCookie = false;
    Fixture() {
        const auto& m = methods;
        (void)m;
        memory.Word(0x1104, 0x0600191B);
        memory.Word(0x1000, 0x0006); // public instance
        memory.Word(0x1004, kHairPopulateToken);
        memory.Word(0x1008, 0x2000);
        memory.Word(0x100C, 0x4000);
        memory.Word(0x1014, 0); // method may not be transformed yet
        memory.Word(0x1018, 0x5000);
        memory.Word(0x2000, 0x3000);
        memory.Word(0x2034, 0x6000);
        memory.Word(0x2038, 0x7000);
        memory.Word(0x3014, 0x8000);
        memory.Word(0x4004, 0x56780001); // adjacent flags not param count
        memory.Word(0x400C, 0x9000);
        memory.Word(0x4010, 0xA000);
        memory.data[0x9006] = 0x01;
        memory.data[0xA006] = 0x02;
        memory.String(0x5000, "PopulateTypesGrid");
        memory.String(0x6000, "CASHair");
        memory.String(0x7000, "Sims3.UI.CAS");
        memory.String(0x8000, "C:\\Game\\Bin\\UI.dll");
    }
    Discovery Run(DiscoveryGate gate = {true,true,true,true}) {
        return DiscoverHairPopulate(gate,
            [&](const char* ns, const char* name) -> std::uint32_t {
                ++lookupCalls;
                assert(std::string_view(ns) == "Sims3.UI.CAS");
                assert(std::string_view(name) == "CASHair");
                return classResult;
            },
            [&](std::uint32_t owner, std::uint32_t* iter) -> std::uint32_t {
                ++enumerateCalls;
                assert(owner == classResult);
                // Mono's iterator is an opaque cookie, not a counter.
                if (useOpaqueDecreasingCookie) {
                    if (*iter == 0) { *iter = 0xDEADC0DEu; return methods[0]; }
                    if (*iter == 0xDEADC0DEu) { *iter = 5u; return methods[1]; }
                    return 0;
                }
                if (static_cast<std::size_t>(*iter) >= methods.size()) return 0;
                const auto value = methods[static_cast<std::size_t>(*iter)];
                if (!freezeEnumerator) ++*iter;
                return value;
            },
            [&](std::uint32_t addr, void* dst, std::size_t n) {
                return memory.Read(addr, dst, n);
            });
    }
};

void TestStrictDiscovery() {
    Fixture f;
    const auto r=f.Run();
    assert(r.status == DiscoveryStatus::Ready);
    assert(r.method == 0x1000 && r.runtimeMethod == 0);
    assert(r.methodsVisited == 2 && f.lookupCalls == 1 && f.enumerateCalls == 3);
}

void TestNonmonotonicMonoIteratorCookie() {
    Fixture f;
    f.useOpaqueDecreasingCookie = true;
    const auto r = f.Run();
    assert(r.status == DiscoveryStatus::Ready && r.method == 0x1000);
    assert(r.methodsVisited == 2);
}

void TestFailClosedGates() {
    Fixture f;
    for (DiscoveryGate g : {
        DiscoveryGate{false,true,true,true},
        DiscoveryGate{true,false,true,true},
        DiscoveryGate{true,true,false,true},
        DiscoveryGate{true,true,true,false}
    }) {
        const auto r=f.Run(g);
        assert(r.method == 0 && r.runtimeMethod == 0);
        assert(r.status != DiscoveryStatus::Ready);
    }
    assert(f.lookupCalls == 0 && f.enumerateCalls == 0);
}
void TestNotFoundAndUnknownClass() {
    Fixture f;
    f.methods={0x1100};
    assert(f.Run().status == DiscoveryStatus::NotFound);
    Fixture other;
    other.classResult=0;
    assert(other.Run().status == DiscoveryStatus::MissingClass);
    assert(other.enumerateCalls == 0);
}
void TestRejectWrongSignatureOrOwner() {
    Fixture f;
    f.memory.data[0xA006] = 0x08;
    assert(f.Run().status == DiscoveryStatus::IdentityMismatch);
    assert(f.enumerateCalls == 2);
    Fixture foreign;
    foreign.memory.Word(0x1008, 0x2200);
    assert(foreign.Run().status == DiscoveryStatus::ForeignClass);
}
void TestRejectDuplicates() {
    Fixture f;
    f.methods.push_back(0x1200);
    f.memory.Word(0x1204, kHairPopulateToken);
    const auto r=f.Run();
    assert(r.status == DiscoveryStatus::DuplicateTarget);
    assert(r.method == 0);
}
void TestStopUnprogressingAndUnreadableEnumerators() {
    Fixture f;
    f.freezeEnumerator=true;
    assert(f.Run().status == DiscoveryStatus::BadEnumeration);
    Fixture broken;
    broken.memory.data.erase(0x1104);
    assert(broken.Run().status == DiscoveryStatus::BadEnumeration);
}
void TestRejectUnboundedMetadata() {
    Fixture f;
    f.methods.assign(2048, 0x1100);
    f.methods.push_back(0x1000);
    const auto r=f.Run();
    assert(r.status == DiscoveryStatus::TooManyMethods);
    assert(r.method == 0 && r.methodsVisited == 2048);
}
}

int main() {
    TestStrictDiscovery();
    TestNonmonotonicMonoIteratorCookie();
    TestFailClosedGates();
    TestNotFoundAndUnknownClass();
    TestRejectWrongSignatureOrOwner();
    TestRejectDuplicates();
    TestStopUnprogressingAndUnreadableEnumerators();
    TestRejectUnboundedMetadata();
    std::cout << "PASS: 8 gated native CAS method-discovery test groups\n";
}
