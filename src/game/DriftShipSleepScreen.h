#pragma once

class GfxRenderer;

namespace DriftShip {
struct State;

// Draws the Drift Ship sleep screen into the frame buffer: the ship, the route
// through the current reach, the latest session's receipt and the ship's
// numbers. Clears the frame buffer first; the caller owns the display refresh.
// bottomReserve keeps that many pixels free at the bottom (for button hints).
// story is the captain's log line for the latest session, or null for none.
void drawSleepScreen(const GfxRenderer& renderer, const State& state, int bottomReserve = 0,
                     const char* story = nullptr);

// Draws text word-wrapped to maxWidth, stopping before a line would cross
// maxY. Returns the y below the last line drawn. Words longer than a line are
// drawn on their own line rather than cut.
int drawWrappedText(const GfxRenderer& renderer, int fontId, int x, int y, int maxWidth, const char* text, int maxY);

}  // namespace DriftShip
