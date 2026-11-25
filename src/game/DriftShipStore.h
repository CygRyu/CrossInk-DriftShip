#pragma once

#include <cstdint>

#include "DriftShipEngine.h"

// SD card persistence and app glue for Drift Ship. The engine itself lives in
// DriftShipEngine.h and knows nothing about files or settings.
namespace DriftShip {

// Loads the saved game. Falls back to the backup copy, then to a fresh
// (never played) state. Returns false only when nothing usable was found.
bool load(State& state);

// Writes the game through a temp file, keeping the previous save as a backup.
bool save(const State& state);

// Called by the readers when a reading session is committed. activeSeconds is
// idle-filtered reading time that did not come from automatic page turns.
void onReadingSession(uint32_t activeSeconds);

// Hard reset: deletes the save and its backup. The next session starts a new voyage.
bool hardReset();

}  // namespace DriftShip
