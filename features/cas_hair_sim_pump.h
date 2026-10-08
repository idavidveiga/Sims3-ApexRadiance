#pragma once
// Apex-owned CAS Hair/Hats simulator task pump.
// The single ASI will eventually call Arm from a VERIFIED native interpreter
// interception of CASHair.PopulateTypesGrid(bool). The pump then resumes the
// existing HairNativeBridge on the engine's simulator thread.
//
// Not a Mono hook by itself. No automatic installation; see Start's explicit
// experimental build/ABI gates. No managed objects or UI handles are cached.
#include "cas_hair_native_bridge.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace ApexCasHairSimPump {
using Generation = ApexCasSchedule::HairNativeBridge::Generation;

// Installs a native inline detour ONLY in explicitly compiled experimental
// builds, with an independently verified EA ProcessTasks calling convention.
// processTasksAddress must come from an unambiguous loaded-image signature
// match; the function prologue is checked again immediately before detouring.
bool Start(std::uintptr_t processTasksAddress,
           std::size_t verifiedSignatureMatches,
           bool nativeAbiIndependentlyVerified,
           std::string* error);
void Stop();
bool Running();

// Stable native session belongs to this ASI, not to the managed GC.
ApexCasSchedule::HairNativeBridge& Session() noexcept;

// Arm only after a successful Session().Begin on the simulation thread;
// failure means callers MUST fall back to original CAS behavior.
bool Arm(Generation generation) noexcept;
void Disarm() noexcept;
std::string StatusText();
}
