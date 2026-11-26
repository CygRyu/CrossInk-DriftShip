#pragma once

#include <cstddef>
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

// Loads the ship log (the most recent sessions). Missing or damaged entries
// are skipped; returns false only when the log file can't be read at all.
bool loadLog(SessionLog& log);

// Called by the readers when a reading session is committed. activeSeconds is
// idle-filtered reading time that did not come from automatic page turns.
void onReadingSession(uint32_t activeSeconds);

// Loads the captain's log line written for the state's latest session.
// Returns false (and writes an empty string) when there is none.
bool loadStory(const State& state, char* out, size_t len);

// Event packs are plain-text files in this folder on the SD card.
constexpr char EVENT_PACK_DIR[] = "/driftship/events";

// Hard reset: deletes the save, its backup, the ship log and the captain's log. The next session starts a new voyage.
bool hardReset();

}  // namespace DriftShip
