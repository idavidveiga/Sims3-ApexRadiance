// Synthetic-only x86 rel32 candidate reference scanner tests.
// No EA executable, game memory, injection or patching is involved.
#include "ts3_mono_xref.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

using Ts3MonoXref::Reference;
using Ts3MonoXref::DecodeDirectCall;
using Ts3MonoXref::VisitDirectCallsToCandidates;

static void EmitCall(std::vector<std::uint8_t>& data,
                     std::uint32_t caller, std::uint32_t target) {
    const std::uint32_t displacement = target - (caller + 5u);
    data.push_back(0xE8);
    for (int i = 0; i < 4; ++i)
        data.push_back(static_cast<std::uint8_t>(
            displacement >> (i * 8)));
}

static void TestForwardAndBackwardDisplacements() {
    std::vector<std::uint8_t> forward;
    EmitCall(forward, 0x1000, 0x1A00);
    std::uint32_t result = 0;
    assert(DecodeDirectCall(forward.data(), forward.size(), 0x1000, result));
    assert(result == 0x1A00);

    std::vector<std::uint8_t> backward;
    EmitCall(backward, 0x5200, 0x2000);
    assert(DecodeDirectCall(backward.data(), backward.size(), 0x5200, result));
    assert(result == 0x2000);
}

static void TestInvalidAndShortSequences() {
    std::uint32_t to = 42;
    std::array<std::uint8_t, 4> shortCall{0xE8, 0, 0, 0};
    assert(!DecodeDirectCall(nullptr, 5, 0, to));
    assert(!DecodeDirectCall(shortCall.data(), shortCall.size(), 0, to));
    std::array<std::uint8_t, 5> notCall{0xE9, 0, 0, 0, 0};
    assert(!DecodeDirectCall(notCall.data(), notCall.size(), 0, to));
    assert(to == 42);
}

static void TestExactCandidateFilterAndOffsets() {
    std::vector<std::uint8_t> code = {0x90, 0x90};
    EmitCall(code, 0x1002, 0x4000);
    code.push_back(0x90);
    EmitCall(code, 0x1008, 0x4500);
    std::array<std::uint32_t, 2> targets{0x4000, 0x5000};
    std::vector<Reference> matches;
    const auto count = VisitDirectCallsToCandidates(
        std::span<const std::uint8_t>(code.data(), code.size()),
        0x1000, std::span<const std::uint32_t>(targets),
        [&](Reference item) { matches.push_back(item); });
    assert(count == 1 && matches.size() == 1);
    assert(matches[0].callerRva == 0x1002);
    assert(matches[0].candidateRva == 0x4000);
}

static void TestChunkBoundaryWithFourByteTail() {
    std::vector<std::uint8_t> code(8, 0x90);
    std::vector<std::uint8_t> call;
    EmitCall(call, 0x2008, 0x7000);
    code.insert(code.end(), call.begin(), call.end());
    code.insert(code.end(), 4, 0x90);
    // First chunk ends two bytes into CALL rel32, leaving an incomplete
    // instruction. The second buffer prepends the final four bytes of the
    // previous contiguous, readable chunk.
    const auto chunkEnd = std::size_t{10};
    std::array<std::uint32_t, 1> targets{0x7000};
    int count = 0;
    std::vector<std::uint8_t> first(code.begin(), code.begin() + chunkEnd);
    count += static_cast<int>(VisitDirectCallsToCandidates(
        first, 0x2000, targets, [&](Reference) { assert(false); }));
    std::vector<std::uint8_t> second(code.begin() + chunkEnd - 4, code.end());
    count += static_cast<int>(VisitDirectCallsToCandidates(
        second, 0x2000 + static_cast<std::uint32_t>(chunkEnd - 4),
        targets, [&](Reference ref) {
            assert(ref.callerRva == 0x2008 && ref.candidateRva == 0x7000);
        }));
    assert(count == 1);
}

static void TestAdjacentAndWraparound() {
    std::vector<std::uint8_t> code;
    EmitCall(code, 0xFFFF'FFFDu, 0x0000'0012u);
    // x86 rel32 wraps at 32 bits; the logic is arithmetic, not disassembly.
    std::uint32_t target = 0;
    assert(DecodeDirectCall(code.data(), code.size(), 0xFFFF'FFFDu, target));
    assert(target == 0x12u);
    std::array<std::uint32_t, 1> targets{0x12u};
    int count = 0;
    VisitDirectCallsToCandidates(code, 0xFFFF'FFFDu, targets,
        [&](Reference ref) {
            ++count;
            assert(ref.candidateRva == 0x12u);
        });
    assert(count == 1);
}

static void TestNoTargetsAndNoBytes() {
    std::array<std::uint8_t, 5> code{0xE8, 0, 0, 0, 0};
    std::array<std::uint32_t, 0> noTargets{};
    std::array<std::uint32_t, 1> target{5};
    const auto reject = [](Reference) { assert(false); };
    assert(VisitDirectCallsToCandidates(code, 0, noTargets, reject) == 0);
    assert(VisitDirectCallsToCandidates(
        std::span<const std::uint8_t>(code.data(), 4), 0, target, reject) == 0);
}

int main() {
    TestForwardAndBackwardDisplacements();
    TestInvalidAndShortSequences();
    TestExactCandidateFilterAndOffsets();
    TestChunkBoundaryWithFourByteTail();
    TestAdjacentAndWraparound();
    TestNoTargetsAndNoBytes();
    std::cout << "PASS: 6 read-only x86 CALL candidate reference scan groups\n";
}
