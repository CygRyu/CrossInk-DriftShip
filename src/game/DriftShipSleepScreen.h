#pragma once

class GfxRenderer;

namespace DriftShip {
struct State;

// Draws the Drift Ship sleep screen into the frame buffer: the ship, the route
// through the current reach, the latest session's receipt and the ship's
// numbers. The caller owns clearing and the display refresh.
void drawSleepScreen(const GfxRenderer& renderer, const State& state);

}  // namespace DriftShip
