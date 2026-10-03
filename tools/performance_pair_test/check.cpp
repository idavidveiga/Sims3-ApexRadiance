#include "../../features/performance_mode.h"
#include <cstdio>
#include <cstdint>
#include <limits>

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    for (unsigned a = 0; a < 64; ++a)
        for (unsigned b = 0; b < 64; ++b)
            for (int failBatch = 0; failBatch < 2; ++failBatch) {
                float referenceA[4]{}, referenceB[4]{}, optimizedA[4]{}, optimizedB[4]{};
                int calls = 0;
                auto read = [&](unsigned reg, float* out, unsigned count) {
                    ++calls;
                    if (count == 2 && failBatch) return false;
                    for (unsigned k = 0; k < count * 4; ++k) {
                        const std::uint32_t bits = 0x80000000u + (reg * 4 + k) * 1234567u;
                        std::memcpy(out + k, &bits, sizeof bits);
                    }
                    return true;
                };
                PerformanceMode::ReadConstantPair(read, a, b, referenceA, referenceB, false);
                calls = 0;
                PerformanceMode::ReadConstantPair(read, a, b, optimizedA, optimizedB, true);
                check(std::memcmp(referenceA, optimizedA, sizeof referenceA) == 0);
                check(std::memcmp(referenceB, optimizedB, sizeof referenceB) == 0);
                const bool adjacent = (a < b && b - a == 1) || (b < a && a - b == 1);
                check(calls == (adjacent ? (failBatch ? 3 : 1) : 2));
            }
    float a[4] = {1, 2, 3, 4}, b[4] = {5, 6, 7, 8};
    auto fail = [](unsigned, float*, unsigned) { return false; };
    PerformanceMode::ReadConstantPair(fail, std::numeric_limits<unsigned>::max(), 0, a, b, true);
    check(a[0] == 1 && b[0] == 5);
    std::printf("Constant pair: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
