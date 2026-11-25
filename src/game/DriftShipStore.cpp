#include "DriftShipStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"

namespace DriftShip {
namespace {

constexpr char SAVE_PATH[] = "/.crosspoint/driftship.bin";
constexpr char SAVE_BAK_PATH[] = "/.crosspoint/driftship.bin.bak";
constexpr char SAVE_TMP_PATH[] = "/.crosspoint/driftship.bin.tmp";
constexpr char SAVE_DIR[] = "/.crosspoint";

// Set when a save from a newer firmware is found, so an older build never
// overwrites (and wipes) progress it can't read.
bool s_blockSave = false;

enum class LoadResult : uint8_t { Ok, Missing, Corrupt, NewerFormat };

LoadResult loadFrom(const char* path, State& out) {
  FsFile f;
  if (!Storage.openFileForRead("DSHIP", path, f)) return LoadResult::Missing;
  uint8_t buf[SAVE_BUFFER_SIZE];
  const int n = f.read(buf, sizeof(buf));
  f.close();
  if (n <= 0) return LoadResult::Corrupt;
  const size_t len = static_cast<size_t>(n);
  if (deserialize(out, buf, len)) return LoadResult::Ok;
  // Byte 4 is the format version (after the 4-byte magic).
  if (len > 4 && std::memcmp(buf, "DSHP", 4) == 0 && buf[4] > SAVE_VERSION) return LoadResult::NewerFormat;
  return LoadResult::Corrupt;
}

}  // namespace

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

bool save(const State& state) {
  if (s_blockSave) {
    LOG_ERR("DSHIP", "Refusing to overwrite a newer-format save");
    return false;
  }
  uint8_t buf[SAVE_BUFFER_SIZE];
  const size_t len = serialize(state, buf, sizeof(buf));
  if (len == 0) {
    LOG_ERR("DSHIP", "Save buffer too small");
    return false;
  }

  if (!Storage.ensureDirectoryExists(SAVE_DIR)) {
    LOG_ERR("DSHIP", "Could not create %s", SAVE_DIR);
    return false;
  }
  if (Storage.exists(SAVE_TMP_PATH) && !Storage.remove(SAVE_TMP_PATH)) {
    LOG_ERR("DSHIP", "Could not remove stale temp save");
    return false;
  }

  FsFile f;
  if (!Storage.openFileForWrite("DSHIP", SAVE_TMP_PATH, f)) {
    LOG_ERR("DSHIP", "Could not open temp save for writing");
    return false;
  }
  const bool written = f.write(buf, len) == len && f.sync();
  const bool closed = f.close();
  if (!written || !closed) {
    LOG_ERR("DSHIP", "Could not write temp save");
    Storage.remove(SAVE_TMP_PATH);
    return false;
  }

  // Keep the previous save as a backup until the new one is in place.
  if (Storage.exists(SAVE_BAK_PATH) && !Storage.remove(SAVE_BAK_PATH)) {
    LOG_ERR("DSHIP", "Could not remove old backup");
    Storage.remove(SAVE_TMP_PATH);
    return false;
  }
  if (Storage.exists(SAVE_PATH) && !Storage.rename(SAVE_PATH, SAVE_BAK_PATH)) {
    LOG_ERR("DSHIP", "Could not rotate backup");
    Storage.remove(SAVE_TMP_PATH);
    return false;
  }
  if (!Storage.rename(SAVE_TMP_PATH, SAVE_PATH)) {
    LOG_ERR("DSHIP", "Could not replace save");
    if (Storage.exists(SAVE_BAK_PATH) && !Storage.exists(SAVE_PATH)) Storage.rename(SAVE_BAK_PATH, SAVE_PATH);
    Storage.remove(SAVE_TMP_PATH);
    return false;
  }
  return true;
}

void onReadingSession(const uint32_t activeSeconds) {
  if (activeSeconds < MIN_SESSION_SECONDS) return;
  State state;
  load(state);
  if (s_blockSave) return;
  const auto doctrine = static_cast<Doctrine>(SETTINGS.driftShipDoctrine);
  if (!applySession(state, activeSeconds, doctrine)) return;
  if (save(state)) {
    LOG_INF("DSHIP", "Session %lu: %lus, +%llu fuel, star %u/%u", static_cast<unsigned long>(state.sessions),
            static_cast<unsigned long>(activeSeconds), static_cast<unsigned long long>(state.last.fuel),
            static_cast<unsigned>(state.star + 1), static_cast<unsigned>(STARS_PER_REACH));
  }
}

bool hardReset() {
  bool ok = true;
  for (const char* path : {SAVE_PATH, SAVE_BAK_PATH, SAVE_TMP_PATH}) {
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
