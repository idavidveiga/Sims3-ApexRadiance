#pragma once
// Lot lighting while the camera moves (Apex Radiance, feature "LotLightingMotion"; docs/features/performance.md).
//
// Every frame each lot with lighting work runs the lot lighting update (0x00ADB8F0 on Steam 1.67.2), which asks
// 0x00ADB120 for its time budget (ms, returned in ST0: 5 for an ordinary lot, 10 while it loads, 15 for a "priority"
// lot, 30 while that one loads, 1000 in the world tool mode) and relights its levels' rooms until the budget is spent
// (the room solve resumes where it stopped on the next frame). While the camera moves the current lot's 15 ms made the
// medium hitches. This feature redirects the only CALL of 0x00ADB120 (0x00ADB95D) to a wrapper that, while the camera
// moves, scales the game's budget so the priority lot gets `budgetMs` (default 3) and every other lot the same fraction
// of its own budget (5 -> 1 ms at 3 ms): the engine's own order and priorities stay, only the time per frame shrinks.
// The world tool mode's 1000 ms is left alone. When the camera stops (300 ms without motion) the game's budgets return.
// Camera motion: the camera eye ([[root]+0x24]+0x60, the same read WorldManager::Update does at 0x00C6D5C9), sampled at
// each budget call.
#include <string>

namespace LotLightingMotion {

// Redirects the CALL (feature on) / puts it back (feature off). Any thread.
bool Start(std::string* error);
void Stop();
bool Running();

// The priority lot's budget while the camera moves, in ms (1..15; 15 = the game's own)
void SetBudgetMs(int ms);
int BudgetMs();

// The camera moved in the last 300 ms (false when unknown or when the feature is off)
bool CameraMoving();

std::string StatusText();
// Development build: status lines
void RenderDeveloperUI();

} // namespace LotLightingMotion
