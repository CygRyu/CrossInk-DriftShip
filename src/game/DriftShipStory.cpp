#include "DriftShipStory.h"

#include <cstring>
#include <iterator>
#include <numeric>

namespace DriftShip {
namespace {

static const char* const POOL_NAMES[static_cast<uint8_t>(Pool::Count)] = {"place", "finding", "good", "bad",
                                                                          "reaction"};
static const char* const ERA_SLUGS[static_cast<uint8_t>(Era::Count)] = {"outer-rim", "ruin-belt",     "dense-core",
                                                                        "deep-void", "shattered-arm", "far-galaxy"};
// Indexed by Trait; None has no slug.
static const char* const TRAIT_SLUGS[static_cast<uint8_t>(Trait::Count)] = {
    "", "steady-hands", "night-owl", "hoarder", "lucky", "reckless", "loner", "dreamer"};

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Matches word against a slug table; returns the index or -1.
int8_t findSlug(const char* word, const size_t len, const char* const* table, const uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (table[i][0] != '\0' && strlen(table[i]) == len && strncmp(word, table[i], len) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

}  // namespace

bool cleanPackLine(char* line) {
  if (!line) return false;
  size_t start = 0;
  while (line[start] && isSpace(line[start])) start++;
  size_t end = strlen(line);
  while (end > start && isSpace(line[end - 1])) end--;
  if (start > 0) memmove(line, line + start, end - start);
  line[end - start] = '\0';
  return line[0] != '\0' && line[0] != '#';
}

bool parsePackSection(const char* line, PackSection& out) {
  if (!line || line[0] != '[') return false;
  const char* close = strchr(line, ']');
  if (!close) return false;
  const char* p = line + 1;
  while (p < close && isSpace(*p)) p++;
  const char* wordEnd = p;
  while (wordEnd < close && !isSpace(*wordEnd)) wordEnd++;
  const int8_t pool = findSlug(p, static_cast<size_t>(wordEnd - p), POOL_NAMES, static_cast<uint8_t>(Pool::Count));
  if (pool < 0) return false;

  PackSection s;
  s.pool = static_cast<Pool>(pool);
  const char* q = wordEnd;
  while (q < close && isSpace(*q)) q++;
  const char* qEnd = close;
  while (qEnd > q && isSpace(*(qEnd - 1))) qEnd--;
  if (q < qEnd) {
    const auto qLen = static_cast<size_t>(qEnd - q);
    if (s.pool == Pool::Place) {
      s.era = findSlug(q, qLen, ERA_SLUGS, static_cast<uint8_t>(Era::Count));
      if (s.era < 0) return false;
    } else if (s.pool == Pool::Reaction) {
      s.trait = findSlug(q, qLen, TRAIT_SLUGS, static_cast<uint8_t>(Trait::Count));
      if (s.trait < 0) return false;
    } else {
      return false;  // other pools take no qualifier
    }
  }
  out = s;
  return true;
}

bool sectionMatches(const PackSection& section, const Pool pool, const int8_t era, const int8_t trait) {
  if (section.pool != pool) return false;
  if (section.era >= 0 && section.era != era) return false;
  if (section.trait >= 0 && section.trait != trait) return false;
  return true;
}

uint32_t nextStoryRandom(uint32_t& seed) {
  uint32_t x = seed ? seed : 0x6D2B79F5u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  seed = x;
  return x;
}

uint16_t pickPiece(StoryMemory& memory, const Pool pool, const uint16_t count, uint32_t& seed) {
  if (count == 0) return 0;
  const auto p = static_cast<uint8_t>(pool);
  // With few pieces, avoiding all recent ones is impossible; avoid as many as the pool allows.
  const uint8_t avoid = static_cast<uint8_t>(count > STORY_RECENT ? STORY_RECENT : count - 1);
  uint16_t pick = 0;
  for (uint8_t attempt = 0; attempt < 8; attempt++) {
    pick = static_cast<uint16_t>(nextStoryRandom(seed) % count);
    bool recent = false;
    for (uint8_t k = 0; k < avoid; k++) {
      const uint8_t slot = static_cast<uint8_t>((memory.next[p] + STORY_RECENT - 1 - k) % STORY_RECENT);
      if (memory.recent[p][slot] == pick + 1) recent = true;
    }
    if (!recent) break;
  }
  memory.recent[p][memory.next[p] % STORY_RECENT] = static_cast<uint16_t>(pick + 1);
  memory.next[p] = static_cast<uint8_t>((memory.next[p] + 1) % STORY_RECENT);
  return pick;
}

void expandTemplate(const char* tmpl, const StorySlots& slots, char* out, const size_t len) {
  if (!out || len == 0) return;
  size_t o = 0;
  const auto put = [&](const char* text) {
    while (*text && o + 1 < len) out[o++] = *text++;
  };
  char day[12];
  for (const char* p = tmpl ? tmpl : ""; *p && o + 1 < len;) {
    if (*p == '{') {
      const char* close = strchr(p, '}');
      if (close) {
        const auto n = static_cast<size_t>(close - p - 1);
        const char* name = p + 1;
        const char* value = nullptr;
        if (n == 3 && strncmp(name, "day", 3) == 0) {
          // Hand-rolled so the template itself is never a format string.
          uint32_t v = slots.day;
          char rev[12];
          size_t k = 0;
          do {
            rev[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
          } while (v > 0 && k < sizeof(rev));
          for (size_t i = 0; i < k; i++) day[i] = rev[k - 1 - i];
          day[k] = '\0';
          value = day;
        } else if (n == 5 && strncmp(name, "place", 5) == 0) {
          value = slots.place;
        } else if (n == 7 && strncmp(name, "finding", 7) == 0) {
          value = slots.finding;
        } else if (n == 4 && strncmp(name, "crew", 4) == 0) {
          value = slots.crew;
        } else if (n == 8 && strncmp(name, "reaction", 8) == 0) {
          value = slots.reaction;
        }
        if (value) {
          put(value);
          p = close + 1;
          continue;
        }
      }
    }
    out[o++] = *p++;
  }
  out[o] = '\0';
}

uint32_t storySeed(const State& s) {
  // Depends only on the session's own results: the same line however often it
  // is drawn, and a merged session gets a fresh line for the whole chain.
  const uint32_t mix[] = {s.last.sessionNumber, s.last.seconds, static_cast<uint32_t>(s.last.fuel), s.creditedSeconds};
  const uint32_t h = std::accumulate(std::begin(mix), std::end(mix), 2166136261u,
                                     [](const uint32_t acc, const uint32_t v) { return (acc ^ v) * 16777619u; });
  return h ? h : 1;
}

}  // namespace DriftShip
