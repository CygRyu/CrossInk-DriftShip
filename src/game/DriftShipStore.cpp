#include "DriftShipStore.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iterator>
#include <numeric>

#include "CrossPointSettings.h"
#include "DriftShipStory.h"
#include "DriftShipStoryText.h"

namespace DriftShip {
namespace {

constexpr char SAVE_PATH[] = "/.crosspoint/driftship.bin";
constexpr char SAVE_BAK_PATH[] = "/.crosspoint/driftship.bin.bak";
constexpr char SAVE_TMP_PATH[] = "/.crosspoint/driftship.bin.tmp";
constexpr char SAVE_DIR[] = "/.crosspoint";
// The state from before the open sitting chain was first scored. A merged
// sitting replays the chain from here.
constexpr char BASE_PATH[] = "/.crosspoint/driftship_base.bin";
constexpr char BASE_TMP_PATH[] = "/.crosspoint/driftship_base.bin.tmp";
// The captain's log line for the latest session, plus the no-repeat memory.
constexpr char STORY_PATH[] = "/.crosspoint/driftship_story.bin";
constexpr char STORY_TMP_PATH[] = "/.crosspoint/driftship_story.bin.tmp";
constexpr uint8_t STORY_MAGIC[4] = {'D', 'S', 'S', 'T'};
constexpr uint8_t STORY_VERSION = 1;
// Event packs read per session. More files are ignored, so a crowded folder
// can't stall the sleep screen.
constexpr uint8_t MAX_PACK_FILES = 8;
constexpr size_t PACK_NAME_LEN = 48;
constexpr size_t MAX_PACK_BYTES = 32 * 1024;
constexpr char LOG_PATH[] = "/.crosspoint/driftship_log.bin";
constexpr char LOG_TMP_PATH[] = "/.crosspoint/driftship_log.bin.tmp";
constexpr uint8_t LOG_MAGIC[4] = {'D', 'S', 'L', 'G'};
constexpr uint8_t LOG_VERSION = 1;

// Set when a save from a newer firmware is found, so an older build never
// overwrites (and wipes) progress it can't read.
bool s_blockSave = false;

// Clock ids for sitting times. Times from different clocks are never compared.
constexpr uint32_t CLOCK_NONE = 0;
constexpr uint32_t CLOCK_RTC = 1;
constexpr uint32_t BOOT_CLOCK_MAGIC = 0x44534B31;  // "DSK1"

// Without a working RTC, sitting times come from the system clock, which only
// runs from power-on. RTC_NOINIT memory survives deep sleep and restarts but
// not power loss, so a per-power-on id stored there keeps a sitting from
// before a power cut from ever being merged with one after it.
RTC_NOINIT_ATTR uint32_t s_bootClockMagic;
RTC_NOINIT_ATTR uint32_t s_bootClockId;

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil), so RTC dates can be compared as plain seconds.
int32_t daysFromCivil(int32_t y, const uint32_t m, const uint32_t d) {
  y -= m <= 2 ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// Reads the time a sitting ended. Prefers the RTC (minute resolution, keeps
// time through sleep and power loss); falls back to the system clock.
void readSittingClock(const State& state, uint32_t& now, uint32_t& clockId) {
  now = 0;
  clockId = CLOCK_NONE;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (halClock.isAvailable() && halClock.getDateTime(year, month, day, hour, minute) && year >= 2024 && year <= 2099 &&
      month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour < 24 && minute < 60) {
    const int32_t days = daysFromCivil(year, month, day);
    now = static_cast<uint32_t>(days) * 86400u + hour * 3600u + minute * 60u;
    clockId = CLOCK_RTC;
    return;
  }
  const time_t t = time(nullptr);
  if (t <= 0) return;
  if (s_bootClockMagic != BOOT_CLOCK_MAGIC || s_bootClockId <= CLOCK_RTC) {
    // A new power-on: derive an id that differs from the previous one. The
    // saved chain end and session counts are mixed in, so even an identical
    // boot timing gives a different id.
    const uint32_t mix[] = {state.sittingEnd,
                            state.sittingClock,
                            state.sessions,
                            state.creditedSeconds,
                            static_cast<uint32_t>(micros()),
                            static_cast<uint32_t>(t)};
    const uint32_t h = std::accumulate(std::begin(mix), std::end(mix), 2166136261u,
                                       [](const uint32_t acc, const uint32_t v) { return (acc ^ v) * 16777619u; });
    s_bootClockId = h > CLOCK_RTC ? h : h + 2;
    s_bootClockMagic = BOOT_CLOCK_MAGIC;
  }
  now = static_cast<uint32_t>(t);
  clockId = s_bootClockId;
}

enum class LoadResult : uint8_t { Ok, Missing, Corrupt, NewerFormat };

LoadResult loadFrom(const char* path, State& out) {
  FsFile f;
  if (!Storage.openFileForRead("DSHIP", path, f)) return LoadResult::Missing;
  // The save holds the whole crew roster (up to SAVE_BUFFER_SIZE bytes), too
  // much for the caller's stack, so the read buffer lives on the heap briefly.
  auto bufOwner = makeUniqueNoThrow<uint8_t[]>(SAVE_BUFFER_SIZE);
  if (!bufOwner) {
    f.close();
    LOG_ERR("DSHIP", "No memory to read the save");
    return LoadResult::Corrupt;
  }
  uint8_t* buf = bufOwner.get();
  const int n = f.read(buf, SAVE_BUFFER_SIZE);
  f.close();
  if (n <= 0) return LoadResult::Corrupt;
  const size_t len = static_cast<size_t>(n);
  if (deserialize(out, buf, len)) return LoadResult::Ok;
  // Byte 4 is the format version (after the 4-byte magic).
  if (len > 4 && std::memcmp(buf, "DSHP", 4) == 0 && buf[4] > SAVE_VERSION) return LoadResult::NewerFormat;
  return LoadResult::Corrupt;
}

// The log is written entry by entry so no buffer larger than one 20-byte
// entry is needed on the stack. Losing it only empties the ship log screen.
bool saveLog(const SessionLog& log) {
  if (Storage.exists(LOG_TMP_PATH)) Storage.remove(LOG_TMP_PATH);
  FsFile f;
  if (!Storage.openFileForWrite("DSHIP", LOG_TMP_PATH, f)) {
    LOG_ERR("DSHIP", "Could not open ship log for writing");
    return false;
  }
  const uint8_t header[6] = {LOG_MAGIC[0], LOG_MAGIC[1], LOG_MAGIC[2], LOG_MAGIC[3], LOG_VERSION, log.count};
  bool ok = f.write(header, sizeof(header)) == sizeof(header);
  for (uint8_t i = 0; ok && i < log.count; i++) {
    uint8_t entry[LOG_ENTRY_SIZE];
    serializeLogEntry(log.entries[i], entry);
    ok = f.write(entry, sizeof(entry)) == sizeof(entry);
  }
  ok = ok && f.sync();
  ok = f.close() && ok;
  if (!ok) {
    LOG_ERR("DSHIP", "Could not write ship log");
    Storage.remove(LOG_TMP_PATH);
    return false;
  }
  if (Storage.exists(LOG_PATH) && !Storage.remove(LOG_PATH)) {
    LOG_ERR("DSHIP", "Could not replace ship log");
    Storage.remove(LOG_TMP_PATH);
    return false;
  }
  if (!Storage.rename(LOG_TMP_PATH, LOG_PATH)) {
    LOG_ERR("DSHIP", "Could not rename ship log");
    return false;
  }
  return true;
}

}  // namespace

bool loadLog(SessionLog& log) {
  log = SessionLog{};
  FsFile f;
  if (!Storage.openFileForRead("DSHIP", LOG_PATH, f)) return false;
  uint8_t header[6];
  const bool headerOk = f.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
                        std::memcmp(header, LOG_MAGIC, sizeof(LOG_MAGIC)) == 0 && header[4] == LOG_VERSION;
  if (!headerOk) {
    f.close();
    LOG_ERR("DSHIP", "Ship log header is invalid");
    return false;
  }
  const uint8_t count = std::min<uint8_t>(header[5], LOG_CAPACITY);
  for (uint8_t i = 0; i < count; i++) {
    uint8_t entry[LOG_ENTRY_SIZE];
    if (f.read(entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) break;
    pushLogEntry(log, deserializeLogEntry(entry));
  }
  f.close();
  return true;
}

bool load(State& state) {
  const LoadResult primary = loadFrom(SAVE_PATH, state);
  if (primary == LoadResult::Ok) return true;
  if (primary == LoadResult::NewerFormat) {
    s_blockSave = true;
    LOG_ERR("DSHIP", "Save is from a newer firmware; leaving it untouched");
    state = State{};
    return false;
  }
  const LoadResult backup = loadFrom(SAVE_BAK_PATH, state);
  if (backup == LoadResult::Ok) {
    LOG_INF("DSHIP", "Recovered save from backup");
    return true;
  }
  if (backup == LoadResult::NewerFormat) s_blockSave = true;
  if (primary == LoadResult::Corrupt) LOG_ERR("DSHIP", "Save is corrupt and no backup is usable");
  state = State{};
  return false;
}

namespace {

// Writes a state through a temp file. With bakPath, the previous file is kept
// as a backup until the new one is in place.
bool writeStateFile(const State& state, const char* path, const char* tmpPath, const char* bakPath) {
  auto bufOwner = makeUniqueNoThrow<uint8_t[]>(SAVE_BUFFER_SIZE);
  if (!bufOwner) {
    LOG_ERR("DSHIP", "No memory to write %s", path);
    return false;
  }
  uint8_t* buf = bufOwner.get();
  const size_t len = serialize(state, buf, SAVE_BUFFER_SIZE);
  if (len == 0) {
    LOG_ERR("DSHIP", "Save buffer too small");
    return false;
  }

  if (!Storage.ensureDirectoryExists(SAVE_DIR)) {
    LOG_ERR("DSHIP", "Could not create %s", SAVE_DIR);
    return false;
  }
  if (Storage.exists(tmpPath) && !Storage.remove(tmpPath)) {
    LOG_ERR("DSHIP", "Could not remove stale %s", tmpPath);
    return false;
  }

  FsFile f;
  if (!Storage.openFileForWrite("DSHIP", tmpPath, f)) {
    LOG_ERR("DSHIP", "Could not open %s for writing", tmpPath);
    return false;
  }
  const bool written = f.write(buf, len) == len && f.sync();
  const bool closed = f.close();
  if (!written || !closed) {
    LOG_ERR("DSHIP", "Could not write %s", tmpPath);
    Storage.remove(tmpPath);
    return false;
  }

  if (bakPath) {
    // Keep the previous file as a backup until the new one is in place.
    if (Storage.exists(bakPath) && !Storage.remove(bakPath)) {
      LOG_ERR("DSHIP", "Could not remove old backup");
      Storage.remove(tmpPath);
      return false;
    }
    if (Storage.exists(path) && !Storage.rename(path, bakPath)) {
      LOG_ERR("DSHIP", "Could not rotate backup");
      Storage.remove(tmpPath);
      return false;
    }
  } else if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR("DSHIP", "Could not replace %s", path);
    Storage.remove(tmpPath);
    return false;
  }
  if (!Storage.rename(tmpPath, path)) {
    LOG_ERR("DSHIP", "Could not replace %s", path);
    if (bakPath && Storage.exists(bakPath) && !Storage.exists(path)) Storage.rename(bakPath, path);
    Storage.remove(tmpPath);
    return false;
  }
  return true;
}

}  // namespace

bool save(const State& state) {
  if (s_blockSave) {
    LOG_ERR("DSHIP", "Refusing to overwrite a newer-format save");
    return false;
  }
  return writeStateFile(state, SAVE_PATH, SAVE_TMP_PATH, SAVE_BAK_PATH);
}

namespace {

// --- Captain's log ---

// Working memory for composing a story, on the heap for the one-off update:
// four pieces, a line buffer and the pack file names come to about 1 KB.
struct StoryScratch {
  StoryMemory memory;
  char names[MAX_PACK_FILES][PACK_NAME_LEN];
  uint8_t nameCount = 0;
  char line[MAX_PIECE_LEN + 1];
  char place[MAX_PIECE_LEN];
  char finding[MAX_PIECE_LEN];
  char twist[MAX_PIECE_LEN];
  char reaction[MAX_PIECE_LEN];
  char crew[32];
  char sentence[STORY_LEN];
};

bool endsWithTxt(const char* name) {
  const size_t n = strlen(name);
  if (n < 4) return false;
  const char* ext = name + n - 4;
  return ext[0] == '.' && (ext[1] == 't' || ext[1] == 'T') && (ext[2] == 'x' || ext[2] == 'X') &&
         (ext[3] == 't' || ext[3] == 'T');
}

void listPacks(StoryScratch& sc) {
  sc.nameCount = 0;
  if (!Storage.exists(EVENT_PACK_DIR)) return;
  for (const auto& name : Storage.listFiles(EVENT_PACK_DIR, 32)) {
    if (sc.nameCount >= MAX_PACK_FILES) break;
    if (name.length() + 1 > PACK_NAME_LEN || !endsWithTxt(name.c_str()) || name[0] == '.') continue;
    strncpy(sc.names[sc.nameCount], name.c_str(), PACK_NAME_LEN - 1);
    sc.names[sc.nameCount][PACK_NAME_LEN - 1] = '\0';
    sc.nameCount++;
  }
}

// Reads a file line by line through a 64-byte buffer. Lines longer than the
// caller's buffer are cut at a UTF-8 character boundary.
class LineReader {
 public:
  explicit LineReader(FsFile& file) : file_(file) {}

  bool next(char* out, const size_t cap) {
    size_t n = 0;
    bool any = false;
    while (true) {
      if (pos_ >= len_) {
        const int got = file_.read(buf_, sizeof(buf_));
        if (got <= 0) break;
        len_ = static_cast<size_t>(got);
        pos_ = 0;
      }
      const char c = static_cast<char>(buf_[pos_++]);
      any = true;
      if (c == '\n') break;
      if (n + 1 < cap) out[n++] = c;
    }
    // Don't leave half a multi-byte character at the end of a cut line.
    if (n + 1 >= cap) {
      while (n > 0 && (static_cast<uint8_t>(out[n - 1]) & 0xC0) == 0x80) n--;
      if (n > 0 && (static_cast<uint8_t>(out[n - 1]) & 0xC0) == 0xC0) n--;
    }
    out[n] = '\0';
    return any;
  }

 private:
  FsFile& file_;
  uint8_t buf_[64] = {};
  size_t pos_ = 0;
  size_t len_ = 0;
};

// One kind of piece the story needs: a pool, narrowed by era or trait.
struct PieceQuery {
  Pool pool = Pool::Place;
  int8_t era = -1;
  int8_t trait = -1;
  uint16_t packCount = 0;  // matching pieces in the event packs
};

// Walks every piece line in the event packs. onPiece(section, line) returns
// true to stop. Only the first MAX_PACK_BYTES of each file are read, so a huge
// pack can't stall the sleep screen.
template <typename OnPiece>
void walkPacks(StoryScratch& sc, OnPiece&& onPiece) {
  char path[sizeof(EVENT_PACK_DIR) + 1 + PACK_NAME_LEN];
  for (uint8_t i = 0; i < sc.nameCount; i++) {
    snprintf(path, sizeof(path), "%s/%s", EVENT_PACK_DIR, sc.names[i]);
    FsFile f;
    if (!Storage.openFileForRead("DSHIP", path, f)) continue;
    LineReader reader(f);
    PackSection section;
    bool inSection = false;
    bool stop = false;
    while (!stop && f.position() <= MAX_PACK_BYTES && reader.next(sc.line, sizeof(sc.line))) {
      if (!cleanPackLine(sc.line)) continue;
      if (sc.line[0] == '[') {
        inSection = parsePackSection(sc.line, section);
        continue;
      }
      if (inSection) stop = onPiece(section, sc.line);
    }
    f.close();
    if (stop) return;
  }
}

// Counts the pack pieces for every query in one pass over the packs.
void countPacks(StoryScratch& sc, PieceQuery* queries, const uint8_t n) {
  for (uint8_t q = 0; q < n; q++) queries[q].packCount = 0;
  if (sc.nameCount == 0) return;
  walkPacks(sc, [&](const PackSection& section, const char*) {
    for (uint8_t q = 0; q < n; q++) {
      if (sectionMatches(section, queries[q].pool, queries[q].era, queries[q].trait) &&
          queries[q].packCount < UINT16_MAX) {
        queries[q].packCount++;
      }
    }
    return false;
  });
}

// Picks one piece for a query from the built-in pack and the event packs.
void pickInto(StoryScratch& sc, const PieceQuery& query, uint32_t& seed, char* out, const size_t outLen) {
  const uint16_t builtin = builtinPieceCount(query.pool, query.era, query.trait);
  const uint16_t total = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, builtin + query.packCount));
  const uint16_t index = pickPiece(sc.memory, query.pool, total, seed);
  if (index >= builtin) {
    // A pack piece: walk to it.
    uint16_t at = 0;
    bool found = false;
    walkPacks(sc, [&](const PackSection& section, const char* line) {
      if (!sectionMatches(section, query.pool, query.era, query.trait)) return false;
      if (at++ != index - builtin) return false;
      strncpy(out, line, outLen - 1);
      out[outLen - 1] = '\0';
      found = true;
      return true;
    });
    if (found) return;
  }
  strncpy(out, builtinPiece(query.pool, query.era, query.trait, index < builtin ? index : 0), outLen - 1);
  out[outLen - 1] = '\0';
}

void appendSentence(char* buf, const size_t len, const char* sentence) {
  const size_t used = strlen(buf);
  if (!sentence[0] || used + 2 >= len) return;
  snprintf(buf + used, len - used, "%s%s", used ? " " : "", sentence);
}

// Builds the captain's log line for the state's latest session.
void composeStory(const State& s, StoryScratch& sc) {
  listPacks(sc);
  uint32_t seed = storySeed(s);
  const SessionReport& r = s.last;

  // Most sessions, someone on duty has something to say about it.
  const CrewMember* who = nullptr;
  uint8_t onDuty = 0;
  for (uint8_t i = 0; i < s.crewCount; i++) {
    if (s.crew[i].injured == 0) onDuty++;
  }
  if (onDuty > 0 && nextStoryRandom(seed) % 100 < 60) {
    uint8_t pick = static_cast<uint8_t>(nextStoryRandom(seed) % onDuty);
    for (uint8_t i = 0; i < s.crewCount && !who; i++) {
      if (s.crew[i].injured == 0 && pick-- == 0) who = &s.crew[i];
    }
  }
  Trait trait = Trait::None;
  if (who) trait = who->traits[1] != Trait::None && (nextStoryRandom(seed) & 1) ? who->traits[1] : who->traits[0];
  const bool twist = r.twist == Twist::Good || r.twist == Twist::Bad;

  enum : uint8_t { PLACE = 0, FINDING, TWIST, REACTION, QUERY_COUNT };
  PieceQuery queries[QUERY_COUNT];
  queries[PLACE].pool = Pool::Place;
  queries[PLACE].era = static_cast<int8_t>(eraForReach(s.reach));
  queries[FINDING].pool = Pool::Finding;
  queries[TWIST].pool = r.twist == Twist::Bad ? Pool::Bad : Pool::Good;
  queries[REACTION].pool = Pool::Reaction;
  queries[REACTION].trait = static_cast<int8_t>(trait);
  countPacks(sc, queries, QUERY_COUNT);

  pickInto(sc, queries[PLACE], seed, sc.place, sizeof(sc.place));
  pickInto(sc, queries[FINDING], seed, sc.finding, sizeof(sc.finding));
  StorySlots slots;
  slots.day = r.sessionNumber;
  slots.place = sc.place;
  slots.finding = sc.finding;
  const char* tmpl = (r.watches & WATCH_EXPEDITION) ? tr(STR_DRIFT_SHIP_LOG_EXPEDITION)
                     : (r.watches & WATCH_SURVEY)   ? tr(STR_DRIFT_SHIP_LOG_SURVEY)
                                                    : tr(STR_DRIFT_SHIP_LOG_QUIET);
  expandTemplate(tmpl, slots, sc.sentence, sizeof(sc.sentence));

  if (twist) {
    pickInto(sc, queries[TWIST], seed, sc.twist, sizeof(sc.twist));
    appendSentence(sc.sentence, sizeof(sc.sentence), sc.twist);
  }
  if (who) {
    crewName(who->nameSeed, sc.crew, sizeof(sc.crew));
    pickInto(sc, queries[REACTION], seed, sc.reaction, sizeof(sc.reaction));
    StorySlots crewSlots;
    crewSlots.crew = sc.crew;
    crewSlots.reaction = sc.reaction;
    char reactionLine[MAX_PIECE_LEN + 40];
    expandTemplate(tr(STR_DRIFT_SHIP_LOG_REACTION), crewSlots, reactionLine, sizeof(reactionLine));
    appendSentence(sc.sentence, sizeof(sc.sentence), reactionLine);
  }
}

// Story file: magic, version, the no-repeat memory, the session number, the line.
bool readStoryFile(StoryMemory& memory, uint32_t& sessionNumber, char* line, const size_t lineLen) {
  if (line && lineLen) line[0] = '\0';
  FsFile f;
  if (!Storage.openFileForRead("DSHIP", STORY_PATH, f)) return false;
  uint8_t header[5];
  bool ok = f.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
            std::memcmp(header, STORY_MAGIC, sizeof(STORY_MAGIC)) == 0 && header[4] == STORY_VERSION;
  StoryMemory m;
  ok = ok && f.read(&m, sizeof(m)) == static_cast<int>(sizeof(m));
  uint8_t tail[6];
  ok = ok && f.read(tail, sizeof(tail)) == static_cast<int>(sizeof(tail));
  if (ok) {
    sessionNumber = static_cast<uint32_t>(tail[0]) | static_cast<uint32_t>(tail[1]) << 8 |
                    static_cast<uint32_t>(tail[2]) << 16 | static_cast<uint32_t>(tail[3]) << 24;
    const size_t n = static_cast<size_t>(tail[4]) | static_cast<size_t>(tail[5]) << 8;
    if (line && lineLen) {
      const size_t want = std::min(n, lineLen - 1);
      ok = f.read(line, want) == static_cast<int>(want);
      line[ok ? want : 0] = '\0';
    }
  }
  f.close();
  if (!ok) return false;
  // The memory is only a hint; clamp it so a damaged file can't index out of range.
  std::transform(std::begin(m.next), std::end(m.next), std::begin(m.next),
                 [](const uint8_t next) { return static_cast<uint8_t>(next % STORY_RECENT); });
  memory = m;
  return true;
}

bool writeStoryFile(const StoryMemory& memory, const uint32_t sessionNumber, const char* line) {
  if (Storage.exists(STORY_TMP_PATH)) Storage.remove(STORY_TMP_PATH);
  FsFile f;
  if (!Storage.openFileForWrite("DSHIP", STORY_TMP_PATH, f)) {
    LOG_ERR("DSHIP", "Could not open the captain's log for writing");
    return false;
  }
  const size_t n = strlen(line);
  const uint8_t header[5] = {STORY_MAGIC[0], STORY_MAGIC[1], STORY_MAGIC[2], STORY_MAGIC[3], STORY_VERSION};
  const uint8_t tail[6] = {static_cast<uint8_t>(sessionNumber),
                           static_cast<uint8_t>(sessionNumber >> 8),
                           static_cast<uint8_t>(sessionNumber >> 16),
                           static_cast<uint8_t>(sessionNumber >> 24),
                           static_cast<uint8_t>(n),
                           static_cast<uint8_t>(n >> 8)};
  bool ok = f.write(header, sizeof(header)) == sizeof(header);
  // The memory is a plain array of integers, written in the device's byte order;
  // a mismatch only affects which recent pieces are avoided.
  ok = ok && f.write(&memory, sizeof(memory)) == sizeof(memory);
  ok = ok && f.write(tail, sizeof(tail)) == sizeof(tail);
  ok = ok && f.write(line, n) == n;
  ok = ok && f.sync();
  ok = f.close() && ok;
  if (!ok || (Storage.exists(STORY_PATH) && !Storage.remove(STORY_PATH)) ||
      !Storage.rename(STORY_TMP_PATH, STORY_PATH)) {
    LOG_ERR("DSHIP", "Could not write the captain's log");
    Storage.remove(STORY_TMP_PATH);
    return false;
  }
  return true;
}

void writeStory(const State& state) {
  auto sc = makeUniqueNoThrow<StoryScratch>();
  if (!sc) {
    LOG_ERR("DSHIP", "No memory for the captain's log");
    return;
  }
  uint32_t previousSession = 0;
  readStoryFile(sc->memory, previousSession, nullptr, 0);
  composeStory(state, *sc);
  writeStoryFile(sc->memory, state.last.sessionNumber, sc->sentence);
}

}  // namespace

bool loadStory(const State& state, char* out, const size_t len) {
  if (!out || len == 0) return false;
  out[0] = '\0';
  if (state.version == 0) return false;
  StoryMemory memory;
  uint32_t sessionNumber = 0;
  if (!readStoryFile(memory, sessionNumber, out, len)) return false;
  // A line from another session (e.g. after a reset) is not shown.
  if (sessionNumber != state.last.sessionNumber) {
    out[0] = '\0';
    return false;
  }
  return out[0] != '\0';
}

void onReadingSession(const uint32_t activeSeconds) {
  if (activeSeconds < MIN_SITTING_SECONDS) return;
  // State is ~600 bytes with a full crew, and merging needs a second copy (the
  // snapshot to replay from, or the pre-session state to snapshot). Both live
  // on the heap for this one-off update rather than on the reader's stack.
  auto state = makeUniqueNoThrow<State>();
  auto scratch = makeUniqueNoThrow<State>();
  if (!state || !scratch) {
    LOG_ERR("DSHIP", "No memory for the game state");
    return;
  }
  load(*state);
  if (s_blockSave) return;

  uint32_t now = 0;
  uint32_t clockId = 0;
  readSittingClock(*state, now, clockId);
  bool snapshotLoaded = false;
  if (sittingNeedsSnapshot(*state, activeSeconds, now, clockId)) {
    snapshotLoaded = loadFrom(BASE_PATH, *scratch) == LoadResult::Ok;
    if (!snapshotLoaded) LOG_ERR("DSHIP", "Merge snapshot missing; scoring the sitting on its own");
  }
  const auto doctrine = static_cast<Doctrine>(SETTINGS.driftShipDoctrine);
  const SittingOutcome outcome = applySitting(*state, *scratch, snapshotLoaded, activeSeconds, now, clockId, doctrine);
  if (!outcome.save) return;
  if (outcome.writeSnapshot && !writeStateFile(*scratch, BASE_PATH, BASE_TMP_PATH, nullptr)) {
    // Without a snapshot this session can't be replayed, so don't let the
    // next sitting merge into it.
    closeSitting(*state);
  }
  if (!save(*state)) return;
  if (!outcome.scored) return;

  // Built after the save succeeds so the log never shows a session the game
  // itself lost. A replayed merge replaces the entry it re-scored.
  auto log = makeUniqueNoThrow<SessionLog>();
  if (log) {
    loadLog(*log);
    upsertLogEntry(*log, makeLogEntry(*state));
    saveLog(*log);
  } else {
    LOG_ERR("DSHIP", "No memory for the ship log");
  }
  writeStory(*state);
  LOG_INF("DSHIP", "Session %lu: %lus in %u sittings, +%llu fuel, star %u/%u, crew %u",
          static_cast<unsigned long>(state->sessions), static_cast<unsigned long>(state->last.seconds),
          static_cast<unsigned>(state->last.sittings), static_cast<unsigned long long>(state->last.fuel),
          static_cast<unsigned>(state->star + 1), static_cast<unsigned>(STARS_PER_REACH),
          static_cast<unsigned>(state->crewCount));
}

bool hardReset() {
  bool ok = true;
  for (const char* path : {SAVE_PATH, SAVE_BAK_PATH, SAVE_TMP_PATH, BASE_PATH, BASE_TMP_PATH, LOG_PATH, LOG_TMP_PATH,
                           STORY_PATH, STORY_TMP_PATH}) {
    if (Storage.exists(path) && !Storage.remove(path)) {
      LOG_ERR("DSHIP", "Could not delete %s", path);
      ok = false;
    }
  }
  if (ok) {
    s_blockSave = false;
    LOG_INF("DSHIP", "Voyage reset");
  }
  return ok;
}

}  // namespace DriftShip
