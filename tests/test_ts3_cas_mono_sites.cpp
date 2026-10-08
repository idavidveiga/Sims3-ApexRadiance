// Synthetic native .text chunks. Does not access the game or run/hook Mono.
#include "ts3_cas_mono_sites.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>
using namespace ApexCasMono;

static std::vector<std::uint8_t> Materialize(const BridgePattern& p) {
    std::vector<std::uint8_t> v;
    for (std::size_t i=0; i<p.bytes.size();) {
        if (p.bytes[i] == ' ') { ++i; continue; }
        if (p.bytes[i] == '?' && p.bytes[i+1] == '?') {
            v.push_back(0x7Fu); i+=2; continue;
        }
        const int hi = Hex(p.bytes[i]), lo = Hex(p.bytes[i+1]);
        assert(hi >= 0 && lo >= 0);
        v.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        i+=2;
    }
    assert(v.size()==p.length);
    return v;
}
static std::vector<std::uint8_t> Fixture() {
    std::vector<std::uint8_t> v(1300, 0xCC);
    std::size_t positions[] = {14,97,501,947};
    for (std::size_t k=0;k<kSiteCount;++k) {
        const auto p=Materialize(kBridgePatterns[k]);
        std::copy(p.begin(),p.end(),v.begin()+positions[k]);
    }
    return v;
}
static BridgeSignatureEvidence Process(std::span<const std::uint8_t> v, std::size_t step) {
    BridgeSignatureEvidence scan;
    for (std::size_t pos=0;pos<v.size();pos+=step) {
        const std::size_t prior=std::min(pos,kMaxBridgePatternLength-1);
        const std::size_t begin=pos-prior;
        const std::size_t end=std::min(v.size(),pos+step);
        scan.Observe(v.subspan(begin,end-begin),
                     static_cast<std::uint32_t>(0x1000+begin),prior);
    }
    return scan;
}
static void TestIdentityAndStreaming() {
    const auto data=Fixture();
    for (auto step: {1u,7u,16u,37u,4096u}) {
        const auto s=Process(data,step);
        assert(s.AllUnique());
        std::uint32_t expected[]={0x1000+14,0x1000+97,0x1000+501,0x1000+947};
        for (std::size_t k=0;k<kSiteCount;++k) {
            const auto hit=s.Get(kBridgePatterns[k].site);
            assert(hit.count==1 && hit.firstRva==expected[k]);
        }
    }
}
static void TestNoFalseMatchesAndDupes() {
    auto data=Fixture();
    data[14] ^= 1;
    assert(!Process(data,37).AllUnique());
    auto second=Materialize(kBridgePatterns[1]);
    std::copy(second.begin(),second.end(),data.begin()+1147);
    const auto scan=Process(data,19);
    assert(scan.Get(BridgeSite::ScriptHostFindClass).count==0);
    assert(scan.Get(BridgeSite::MonoClassGetMethods).count==2);
    assert(!scan.AllUnique());
}
static void TestWildcardAndBounds() {
    auto source=Materialize(kBridgePatterns[1]);
    assert(MatchesPattern(source,0,kBridgePatterns[1]));
    source[8]=0x44; // wildcard Jcc offset
    assert(MatchesPattern(source,0,kBridgePatterns[1]));
    source[0]^=1;
    assert(!MatchesPattern(source,0,kBridgePatterns[1]));
    assert(!MatchesPattern(source,source.size(),kBridgePatterns[1]));
    BridgeSignatureEvidence s;
    s.Observe(std::span<const std::uint8_t>(source),0x1000,source.size()+1);
    assert(s.Get(BridgeSite::MonoClassGetMethods).count==0);
}
int main() {
    TestIdentityAndStreaming();
    TestNoFalseMatchesAndDupes();
    TestWildcardAndBounds();
    std::cout<<"PASS: 3 read-only TS3 Mono bridge-site scanner test groups\n";
}
