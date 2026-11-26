#pragma once

#include <cstddef>
#include <cstdint>

#include "DriftShipEngine.h"

// Drift Ship captain's log: one short story line per session, assembled from
// parts (a place, a finding, a twist and a crew reaction) so it takes a very
// long time to repeat. The parts come from a built-in pack (translated
// strings) plus optional plain-text event packs on the SD card.
//
// This header is the pure part: section headers, piece selection with a
// no-repeat memory, and template expansion. It never touches the game's
// random state, so stories can't change gameplay. File reading lives in
// DriftShipStore.cpp and the built-in pack in DriftShipStoryText.cpp.
namespace DriftShip {

enum class Pool : uint8_t { Place = 0, Finding, Good, Bad, Reaction, Count };

// The longest piece kept from an event pack, in bytes (longer lines are cut).
constexpr size_t MAX_PIECE_LEN = 96;
// The longest assembled story line, including the terminator.
constexpr size_t STORY_LEN = 320;
// How many recent picks each pool remembers.
constexpr uint8_t STORY_RECENT = 8;

// Recent picks per pool, as piece index + 1 (0 = empty slot).
struct StoryMemory {
  uint16_t recent[static_cast<uint8_t>(Pool::Count)][STORY_RECENT] = {};
  uint8_t next[static_cast<uint8_t>(Pool::Count)] = {};
};

// An event pack section. era and trait are -1 when the section is for any.
struct PackSection {
  Pool pool = Pool::Place;
  int8_t era = -1;    // Era index, for [place <era>]
  int8_t trait = -1;  // Trait index, for [reaction <trait>]
};

// Parses a section header such as "[place]", "[place ruin-belt]" or
// "[reaction night-owl]". Returns false for anything else, including unknown
// pools, eras and traits (so a pack written for a newer firmware is skipped,
// not misread).
bool parsePackSection(const char* line, PackSection& out);

// Trims a pack line in place. Returns false for blank lines and # comments.
bool cleanPackLine(char* line);

// True when pieces of section are candidates for the given era and trait.
bool sectionMatches(const PackSection& section, Pool pool, int8_t era, int8_t trait);

// Picks a piece index in [0, count), rerolling a few times to avoid the
// pool's recent picks, and remembers the pick. seed advances. count 0 gives 0.
uint16_t pickPiece(StoryMemory& memory, Pool pool, uint16_t count, uint32_t& seed);

// Values for a template's {day}, {place}, {finding}, {crew} and {reaction}.
struct StorySlots {
  uint32_t day = 0;
  const char* place = "";
  const char* finding = "";
  const char* crew = "";
  const char* reaction = "";
};

// Expands the placeholders in tmpl into out, always null-terminated. Unknown
// placeholders are copied as written. No printf, so pack text is never
// treated as a format string.
void expandTemplate(const char* tmpl, const StorySlots& slots, char* out, size_t len);

// The seed for a session's story. It is derived from the session's results and
// never from the game's random state, so stories can't change gameplay.
uint32_t storySeed(const State& state);

// Small xorshift step for story rolls.
uint32_t nextStoryRandom(uint32_t& seed);

}  // namespace DriftShip
