#include "DriftShipStore.h"

#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <iterator>
#include <numeric>

#include "CrossPointSettings.h"

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
  LOG_INF("DSHIP", "Session %lu: %lus in %u sittings, +%llu fuel, star %u/%u, crew %u",
          static_cast<unsigned long>(state->sessions), static_cast<unsigned long>(state->last.seconds),
          static_cast<unsigned>(state->last.sittings), static_cast<unsigned long long>(state->last.fuel),
          static_cast<unsigned>(state->star + 1), static_cast<unsigned>(STARS_PER_REACH),
          static_cast<unsigned>(state->crewCount));
}

bool hardReset() {
  bool ok = true;
  for (const char* path : {SAVE_PATH, SAVE_BAK_PATH, SAVE_TMP_PATH, BASE_PATH, BASE_TMP_PATH, LOG_PATH, LOG_TMP_PATH}) {
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
