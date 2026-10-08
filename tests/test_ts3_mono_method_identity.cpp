// Synthetic x86 MonoMethod fixtures; NO original game binaries or live pointers.
// These tests only validate fail-closed method identity, not hookability.
#include "ts3_mono_method_identity.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <string_view>

using ApexCasMono::Identity;
using ApexCasMono::InspectHairPopulate;

struct FakeMemory {
    std::unordered_map<std::uint32_t, std::uint8_t> bytes;

    void Word(std::uint32_t address, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bytes[address + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
    void String(std::uint32_t address, std::string_view value) {
        for (std::size_t i = 0; i < value.size(); ++i)
            bytes[address + static_cast<std::uint32_t>(i)] =
                static_cast<std::uint8_t>(value[i]);
        bytes[address + static_cast<std::uint32_t>(value.size())] = 0;
        // The following page may be unreadable: validator must not demand
        // a full 256-byte span once the string has terminated.
    }
    bool Read(std::uint32_t address, void* output, std::size_t count) const {
        auto* out = static_cast<std::uint8_t*>(output);
        for (std::size_t i = 0; i < count; ++i) {
            auto it = bytes.find(address + static_cast<std::uint32_t>(i));
            if (it == bytes.end()) return false;
            out[i] = it->second;
        }
        return true;
    }
};

static FakeMemory Fixture() {
    FakeMemory m;
    constexpr std::uint32_t method = 0x1000, klass = 0x2000,
                            image = 0x3000, signature = 0x4000;
    m.Word(method + 0x00, 0x0006); // public instance method, not static
    m.Word(method + 0x04, ApexCasMono::kHairPopulateToken);
    m.Word(method + 0x08, klass);
    m.Word(method + 0x0c, signature);
    m.Word(method + 0x14, 0xDEADBEEFu); // output only, never called
    m.Word(method + 0x18, 0x5000);
    m.Word(klass + 0x00, image);
    m.Word(klass + 0x34, 0x6000);
    m.Word(klass + 0x38, 0x7000);
    m.Word(image + 0x14, 0x8000);
    m.Word(signature + 0x04, 0xABCD0001u); // param_count is uint16; adjacent flags are nonzero
    m.Word(signature + 0x0C, 0x9000); // return MonoType*
    m.Word(signature + 0x10, 0xA000); // parameter MonoType*
    m.bytes[0x9006] = 0x01; // ELEMENT_TYPE_VOID
    m.bytes[0xA006] = 0x02; // ELEMENT_TYPE_BOOLEAN
    m.String(0x5000, "PopulateTypesGrid");
    m.String(0x6000, "CASHair");
    m.String(0x7000, "Sims3.UI.CAS");
    m.String(0x8000, "C:\\The Sims 3\\Game\\Bin\\UI.DLL");
    return m;
}

static Identity Status(FakeMemory& m, std::uint32_t candidate = 0x1000) {
    return InspectHairPopulate(candidate, [&](std::uint32_t addr,
                                     void* out, std::size_t len) {
        return m.Read(addr, out, len);
    }).status;
}

int main() {
    auto m = Fixture();
    const auto baseline = InspectHairPopulate(0x1000,
        [&](std::uint32_t a, void* b, std::size_t n) { return m.Read(a, b, n); });
    assert(baseline.Matches() && baseline.token == 0x06001918);
    assert(baseline.runtimeMethod == 0xDEADBEEFu);
    assert(Status(m, 0) == Identity::NullCandidate);
    assert(Status(m, 0xFFFFFFFEu) == Identity::InvalidMethod);

    m = Fixture(); m.Word(0x1004, 0x0600191B);
    assert(Status(m) == Identity::WrongToken);
    m = Fixture(); m.String(0x5000, "AddHairTypeGridItem");
    assert(Status(m) == Identity::InvalidMethod);
    m = Fixture(); m.String(0x6000, "CASClothingCategory");
    assert(Status(m) == Identity::WrongClass);
    m = Fixture(); m.String(0x7000, "Sims3.Gameplay");
    assert(Status(m) == Identity::WrongNamespace);
    m = Fixture(); m.String(0x8000, "C:\\mods\\WRONG.DLL");
    assert(Status(m) == Identity::WrongAssembly);
    m = Fixture(); m.Word(0x4004, 2);
    assert(Status(m) == Identity::WrongParameterCount);
    m = Fixture(); m.Word(0x1000, 0x0016); // ECMA Static flag
    assert(Status(m) == Identity::WrongStaticMethod);
    m = Fixture(); m.bytes[0x9006] = 0x02;
    assert(Status(m) == Identity::WrongReturnType);
    m = Fixture(); m.bytes[0xA006] = 0x08;
    assert(Status(m) == Identity::WrongParameterType);
    m = Fixture(); m.bytes.erase(0x9006);
    assert(Status(m) == Identity::WrongSignature);
    m = Fixture(); m.Word(0x4010, 0);
    assert(Status(m) == Identity::WrongSignature);
    m = Fixture(); m.Word(0x100c, 0);
    assert(Status(m) == Identity::InvalidMethod);
    m = Fixture(); m.bytes.erase(0x1008);
    assert(Status(m) == Identity::InvalidMethod);
    m = Fixture(); m.bytes.erase(0x6001);
    assert(Status(m) == Identity::WrongClass);

    std::cout << "PASS: 16 read-only x86 CAS method identity scenarios\n";
}
