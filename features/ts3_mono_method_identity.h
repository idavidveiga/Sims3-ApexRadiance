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
    WrongNamespace, WrongAssembly, WrongSignature, WrongParameterCount
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
    auto getField = [&](std::uint32_t base, std::uint32_t offset,
                        std::uint32_t& result) {
        return base && base <= UINT32_MAX - offset &&
               u32(base + offset, result);
    };
    auto text = [&](std::uint32_t pointer, std::array<char, 256>& target,
                    std::string_view& value) {
        if (!pointer || pointer > UINT32_MAX - target.size() ||
            !read(pointer, target.data(), target.size())) return false;
        std::size_t len = 0;
        while (len < target.size() && target[len]) ++len;
        if (len == target.size()) return false;
        value = std::string_view(target.data(), len);
        return true;
    };

    std::uint32_t klass = 0, methodName = 0, signature = 0;
    if (!getField(candidate, 0x04, out.token) ||
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
    std::uint32_t params = 0;
    if (!getField(signature, 0x04, params)) {
        out.status = Identity::WrongSignature;
        return out;
    }
    if (params != 1) {
        out.status = Identity::WrongParameterCount;
        return out;
    }
    out.status = Identity::Match;
    return out;
}
} // namespace ApexCasMono
