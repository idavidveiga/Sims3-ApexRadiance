#pragma once
// Passive Lot LoD streaming probe.
//
// Probe v1 intentionally performs READS ONLY. It verifies that Apex can resolve
// the native "Throttle Lot LoD Transitions" flag and the WorldManager camera
// threshold on Steam 1.67 and EA/Origin 1.69 before any gameplay feature is enabled.

namespace LotLodStreaming {

// Called once after GameAddr::Resolve(). Logs the resolved addresses and current throttle byte.
void StartProbe();

// Called by Apex's existing 10 ms pump. Waits for a live WorldManager, then logs
// WorldManager+0xEC ("Camera speed threshold") once.
void TickProbe();

} // namespace LotLodStreaming
