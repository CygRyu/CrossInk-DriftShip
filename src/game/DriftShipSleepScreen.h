#pragma once

class GfxRenderer;

namespace DriftShip {
struct State;

// Draws the Drift Ship sleep screen into the frame buffer: the ship, the route
// through the current reach, the latest session's receipt and the ship's
// numbers. Clears the frame buffer first; the caller owns the display refresh.
// bottomReserve keeps that many pixels free at the bottom (for button hints).
void drawSleepScreen(const GfxRenderer& renderer, const State& state, int bottomReserve = 0);

}  // namespace DriftShip
