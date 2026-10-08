#pragma once
// Apex-owned, READ-ONLY validation of a supplied x86 MonoMethod pointer.
// This is NOT a Mono API, hook, interpreter entry or method resolver.
// A caller may supply a candidate only after independently obtaining it
// through a verified native runtime path. No speculative pointer scans.
//
// Layout facts are compared against the user's original UI.dll metadata
// (MethodDef 0x06001918) and historical The Sims 3 Mono x86 research.
// Unknown layouts fail closed. No pointers/objects survive this call.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ApexCasMono {

enum class Identity : std::uint8_t {
    Match, NullCandidate, InvalidMethod, WrongToken, WrongClass,
    WrongNamespace, WrongAssembly, WrongSignature, WrongParameterCount,
    WrongReturnType, WrongParameterType, WrongStaticMethod
};

struct Snapshot {
    Identity status = Identity::NullCandidate;
    std::uint32_t token = 0;
    std::uint32_t runtimeMethod = 0; // observation only; never callable
    std::uint32_t image = 0;
    bool Matches() const noexcept { return status == Identity::Match; }
};

inline constexpr std::uint32_t kHairPopulateToken = 0x06001918;
inline constexpr std::string_view kHairClass = "CASHair";
inline constexpr std::string_view kHairNamespace = "Sims3.UI.CAS";
inline constexpr std::string_view kHairMethod = "PopulateTypesGrid";
inline constexpr std::string_view kHairAssembly = "UI.dll";

inline bool EqualAscii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i], b = right[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

inline std::string_view BaseName(std::string_view path) noexcept {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

// Accessor contract: read(address, buffer, bytes) returns false if the ENTIRE
// span cannot be safely read. In production use MemPatch::ReadBytes; NEVER
// dereference a candidate pointer or invoke a Mono function here.
template<class Read>
Snapshot InspectHairPopulate(std::uint32_t candidate, Read&& read) {
    Snapshot out;
    if (!candidate) return out;
    auto u32 = [&](std::uint32_t address, std::uint32_t& result) {
        if (!address || address > UINT32_MAX - 4)
            return false;
        return read(address, &result, sizeof(result));
    };
    auto u16 = [&](std::uint32_t address, std::uint16_t& result) {
        return address && address <= UINT32_MAX - 2 &&
               read(address, &result, sizeof(result));
    };
    auto u8 = [&](std::uint32_t address, std::uint8_t& result) {
        return address && read(address, &result, sizeof(result));
    };
    auto getField = [&](std::uint32_t base, std::uint32_t offset,
                        std::uint32_t& result) {
        return base && base <= UINT32_MAX - offset &&
               u32(base + offset, result);
    };
    // Read only through the terminating NUL. A short string near a
    // committed-page boundary is valid even when its next 255 bytes are not.
    auto text = [&](std::uint32_t pointer, std::array<char, 256>& target,
                    std::string_view& value) {
        if (!pointer || pointer > UINT32_MAX - target.size()) return false;
        for (std::size_t i = 0; i < target.size(); ++i) {
            std::uint8_t ch = 0;
            if (!u8(pointer + static_cast<std::uint32_t>(i), ch)) return false;
            target[i] = static_cast<char>(ch);
            if (ch == 0) {
                if (i == 0) return false;
                value = std::string_view(target.data(), i);
                return true;
            }
        }
        return false;
    };

    std::uint32_t klass = 0, methodName = 0, signature = 0, methodFlags = 0;
    if (!getField(candidate, 0x00, methodFlags) ||
        !getField(candidate, 0x04, out.token) ||
        !getField(candidate, 0x08, klass) ||
        !getField(candidate, 0x0C, signature) ||
        !getField(candidate, 0x14, out.runtimeMethod) ||
        !getField(candidate, 0x18, methodName) || !klass || !signature) {
        out.status = Identity::InvalidMethod;
        return out;
    }
    if (out.token != kHairPopulateToken) {
        out.status = Identity::WrongToken;
        return out;
    }
    // CASHair.PopulateTypesGrid is an instance method. Reject any
    // candidate with ECMA MethodAttributes.Static (0x0010) set.
    if ((methodFlags & 0x0010u) != 0) {
        out.status = Identity::WrongStaticMethod;
        return out;
    }
    std::array<char, 256> buffer{};
    std::string_view name;
    if (!text(methodName, buffer, name) || name != kHairMethod) {
        out.status = Identity::InvalidMethod;
        return out;
    }
    std::uint32_t className = 0, nameSpace = 0, imageName = 0;
    if (!getField(klass, 0x00, out.image) ||
        !getField(klass, 0x34, className) ||
        !getField(klass, 0x38, nameSpace) || !out.image) {
        out.status = Identity::WrongClass;
        return out;
    }
    if (!text(className, buffer, name) || name != kHairClass) {
        out.status = Identity::WrongClass;
        return out;
    }
    if (!text(nameSpace, buffer, name) || name != kHairNamespace) {
        out.status = Identity::WrongNamespace;
        return out;
    }
    if (!getField(out.image, 0x14, imageName) ||
        !text(imageName, buffer, name) ||
        !EqualAscii(BaseName(name), kHairAssembly)) {
        out.status = Identity::WrongAssembly;
        return out;
    }
    // Mono 1.2.x MonoMethodSignature+0x04 stores uint16 param_count,
    // NOT uint32: the adjacent two bytes contain independent flags.
    // A 32-bit read here rejects valid methods with populated flags.
    std::uint16_t params = 0;
    if (signature > UINT32_MAX - 0x12 || !u16(signature + 0x04, params)) {
        out.status = Identity::WrongSignature;
        return out;
    }
    if (params != 1) {
        out.status = Identity::WrongParameterCount;
        return out;
    }
    std::uint32_t returnType = 0, paramType = 0;
    if (!getField(signature, 0x0C, returnType) ||
        !getField(signature, 0x10, paramType) ||
        !returnType || !paramType ||
        returnType > UINT32_MAX - 0x06 ||
        paramType > UINT32_MAX - 0x06) {
        out.status = Identity::WrongSignature;
        return out;
    }
    std::uint8_t retCode = 0, paramCode = 0;
    if (!u8(returnType + 0x06, retCode) ||
        !u8(paramType + 0x06, paramCode)) {
        out.status = Identity::WrongSignature;
        return out;
    }
    if (retCode != 0x01u) {
        out.status = Identity::WrongReturnType;
        return out;
    }
    if (paramCode != 0x02u) {
        out.status = Identity::WrongParameterType;
        return out;
    }
    out.status = Identity::Match;
    return out;
}
} // namespace ApexCasMono
