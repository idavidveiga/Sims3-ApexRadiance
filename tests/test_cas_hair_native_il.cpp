// Pure C++ tests for Apex Hair/Hats CIL bytecode preparation. No EA binaries
// are included or needed; no runtime hooks can be installed by this test.
#include "cas_hair_native_il.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace ApexCasNativeIl;
namespace D = ApexCasNativeIl::Detail;

static std::vector<std::uint8_t> ToyIL() {
    // Two try/finally regions and three short branches.
    return {0x2b,0x06, 0x00, 0xde,0x03, 0x00,0xdc, 0x00,
            0x00, 0xde,0x03, 0x00,0xdc, 0x00, 0x2a};
}
static std::vector<std::uint8_t> ToyEH() {
    std::vector<std::uint8_t> eh{0x41,52,0,0};
    const auto clause=[&](std::uint32_t a,std::uint32_t b,
                          std::uint32_t c,std::uint32_t d){
        D::Push32(eh,2);D::Push32(eh,a);D::Push32(eh,b);
        D::Push32(eh,c);D::Push32(eh,d);D::Push32(eh,0);
    };
    clause(0,5,5,2);
    clause(8,3,11,2);
    assert(eh.size()==52);
    return eh;
}
static void TestSHA256() {
    const std::vector<std::uint8_t> empty, abc{'a','b','c'};
    assert(D::DigestEquals(empty,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    assert(D::DigestEquals(abc,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(!D::DigestEquals(abc,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}
static void TestFinallyAndBranchRelocation() {
    const auto il=ToyIL(),eh=ToyEH();
    std::vector<D::Op> before,after;
    assert(D::Decode(il,before) && before.size()==9);
    const std::uint8_t yield[]{0x16,0x28,0x23,0,0,0x0a};
    Rewritten patched;
    std::string error;
    assert(Relocate(il,eh,9,yield,patched,&error));
    assert(patched.validated && patched.il.size()==30 && patched.eh.size()==52);
    assert(D::Decode(patched.il,after) && after.size()==before.size()+2);
    std::array<D::Clause,2> clauses{};
    assert(D::Clauses(patched.eh,patched.il.size(),clauses));
    assert(clauses[0].start==0 && clauses[0].handler==11);
    assert(clauses[1].start==14 && clauses[1].handler==26);
    // Old br.s and leave.s are now long form and reach the old targets.
    assert(after[0].code==0x38 && after[0].targets==std::vector<std::size_t>{14});
    assert(after[2].code==0xdd && after[2].targets==std::vector<std::size_t>{14});
    const auto ret=std::find_if(after.begin(),after.end(),[](const D::Op& op){
        return op.code==0x2a;
    });
    assert(ret!=after.end() && ret->start==29);
    assert(!patched.sourceDigest.empty());
}
static void TestFailClosedOnMalformedILAndEH() {
    std::vector<D::Op> ops;
    const auto il=ToyIL(), eh=ToyEH();
    assert(!D::Decode(std::vector<std::uint8_t>{0x28,0x01},ops));
    assert(!D::Decode(std::vector<std::uint8_t>{0xfe},ops));
    assert(!D::Decode(std::vector<std::uint8_t>{0x2b,0x7f,0x2a},ops));
    std::array<D::Clause,2> clauses{};
    auto badEh=eh;badEh[4]=0;
    assert(!D::Clauses(badEh,il.size(),clauses));
    badEh=eh;badEh[1]=51;
    assert(!D::Clauses(badEh,il.size(),clauses));
    Rewritten out;
    const std::uint8_t yield[]{0x16,0x28,0x23,0,0,0x0a};
    assert(!Relocate(il,eh,1,yield,out));
    assert(!Relocate(il,eh,9,std::span<const std::uint8_t>(yield,2),out));
    assert(!Relocate(il,badEh,9,yield,out));
    // The real parent rewriter cannot be used against fabricated metadata,
    // an unverified UI.dll variant, or an unknown original body.
    assert(!BuildHairParent(il,eh,Variant::Unknown,out));
    assert(!BuildHairParent(il,eh,Variant::Ea169,out));
    assert(!BuildHairParent(il,eh,Variant::AlternateUI,out));
    assert(!out.validated && out.il.empty());
}
int main() {
    TestSHA256();
    TestFinallyAndBranchRelocation();
    TestFailClosedOnMalformedILAndEH();
    std::cout<<"PASS: SHA-256, native Hair/Hats IL relocation, two finally regions, fail-closed gates\n";
}
