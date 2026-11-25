#pragma once

#include <cstddef>
#include <cstdint>

// Drift Ship: a zero-button idle game powered by reading time.
//
// This header is the pure game engine. It has no Arduino, HAL or renderer
// dependencies so it can run in native unit tests. Everything is integer math
// on a small fixed-size state (about 150 bytes on disk), and nothing here
// allocates on the heap.
//
// One reading session goes in (active seconds, already idle-filtered by the
// reader), the state is advanced, and a SessionReport comes out for the sleep
// screen. Randomness comes from a persisted xorshift32 state, so the same
// history always produces the same results.
namespace DriftShip {

constexpr uint8_t SYSTEM_COUNT = 6;
enum class System : uint8_t { Reactor = 0, Drones, Sensors, CryoBay, Hull, JumpDrive };

enum class Doctrine : uint8_t { Steady = 0, Explorer, Builder, Count };

enum class StarType : uint8_t { RedDwarf = 0, IceField, Nebula, GasGiant, DeadGate, Pulsar, Count };

enum class Twist : uint8_t { None = 0, Good, Bad };

// Watch bits recorded in SessionReport::watches.
constexpr uint8_t WATCH_MAINTENANCE = 1u << 0;
constexpr uint8_t WATCH_SURVEY = 1u << 1;
constexpr uint8_t WATCH_EXPEDITION = 1u << 2;
constexpr uint8_t WATCH_LONG = 1u << 3;

constexpr uint8_t STARS_PER_REACH = 12;
constexpr uint8_t SURVEYS_TO_PLOT_COURSE = 3;
constexpr uint8_t MIN_JUMP_CONDITION = 30;
constexpr uint8_t REPAIR_BELOW_CONDITION = 50;
constexpr uint32_t MIN_SESSION_SECONDS = 2 * 60;
constexpr uint32_t SURVEY_SECONDS = 10 * 60;
constexpr uint32_t EXPEDITION_SECONDS = 30 * 60;
constexpr uint32_t LONG_WATCH_SECONDS = 60 * 60;
constexpr uint32_t BONUS_RAMP_SECONDS = 60 * 60;
constexpr uint32_t SESSION_CAP_SECONDS = 90 * 60;

// What happened in the latest counted session. Persisted with the state so the
// sleep screen can draw the receipt after a reboot.
struct SessionReport {
  uint32_t sessionNumber = 0;
  uint32_t seconds = 0;          // credited seconds (after the 90-minute cap)
  uint8_t watches = 0;           // WATCH_* bits
  uint64_t baseFuel = 0;         // from the session-length curve alone
  uint16_t reactorPermille = 0;  // +x/1000 from reactor levels
  uint16_t crewPermille = 0;     // +x/1000 from awake crew
  uint16_t starPermille = 0;     // +x/1000 from the current star (gas giant)
  bool lucky = false;            // lucky watch: x2
  uint64_t fuel = 0;             // final Fuel earned this session
  uint32_t parts = 0;
  uint32_t data = 0;
  Twist twist = Twist::None;
  bool derelict = false;
  bool sleeperWoken = false;
  bool repaired = false;
  bool jumped = false;
  bool cleanJump = false;
  bool reachCompleted = false;
  uint8_t upgradesBought = 0;
  uint8_t lastUpgrade = 0;  // System index of the last upgrade bought
};

struct State {
  uint8_t version = 0;  // 0 = never played; set on the first counted session
  uint64_t fuel = 0;
  uint64_t parts = 0;
  uint64_t data = 0;
  uint64_t jumpCharge = 0;
  uint64_t lifetimeFuel = 0;
  uint32_t sessions = 0;
  uint32_t creditedSeconds = 0;
  uint32_t starsVisited = 0;
  uint32_t distanceDeciLy = 0;  // light-years x10
  uint32_t rng = 0;
  uint16_t reach = 0;
  uint16_t sleepersWoken = 0;
  uint16_t colonies = 0;
  uint8_t levels[SYSTEM_COUNT] = {};
  uint8_t star = 0;  // index within the current reach, 0..STARS_PER_REACH-1
  uint8_t starType = 0;
  uint8_t surveysHere = 0;
  uint8_t shortSessionCounter = 0;  // every third short session also surveys
  uint8_t condition = 100;          // hull condition, 0..100
  bool freeJump = false;            // arrived at a dead gate
  // Lifetime records (save version 2+).
  uint64_t bestSessionFuel = 0;
  uint32_t longestSessionSeconds = 0;
  uint32_t derelictsFound = 0;
  uint32_t luckyWatches = 0;
  uint32_t upgradesBought = 0;
  SessionReport last;
};

// One line of the ship log: a compact summary of a past session.
struct LogEntry {
  uint32_t sessionNumber = 0;
  uint16_t minutes = 0;
  uint8_t watches = 0;  // WATCH_* bits
  uint8_t flags = 0;    // LOG_* bits
  uint64_t fuel = 0;
  uint16_t reach = 0;  // where the ship was when the session ended
  uint8_t star = 0;
  uint8_t starType = 0;
};

constexpr uint8_t LOG_LUCKY = 1u << 0;
constexpr uint8_t LOG_DERELICT = 1u << 1;
constexpr uint8_t LOG_SLEEPER = 1u << 2;
constexpr uint8_t LOG_JUMPED = 1u << 3;
constexpr uint8_t LOG_COLONY = 1u << 4;
constexpr uint8_t LOG_BAD_TWIST = 1u << 5;
constexpr uint8_t LOG_GOOD_TWIST = 1u << 6;

// The most recent sessions, oldest first. Fixed size: no heap.
constexpr uint8_t LOG_CAPACITY = 16;
struct SessionLog {
  uint8_t count = 0;
  LogEntry entries[LOG_CAPACITY];
};

// Builds the log line for the session that was just applied.
LogEntry makeLogEntry(const State& state);

// Appends to the log, dropping the oldest entry when full.
void pushLogEntry(SessionLog& log, const LogEntry& entry);

constexpr size_t LOG_ENTRY_SIZE = 20;
void serializeLogEntry(const LogEntry& entry, uint8_t (&buf)[LOG_ENTRY_SIZE]);
LogEntry deserializeLogEntry(const uint8_t (&buf)[LOG_ENTRY_SIZE]);

// --- Tunables exposed for tests and the sleep screen ---

// Fuel from session length alone: t * (1 + min(t, 60) / 60), t capped at 90 min.
uint64_t baseFuelForSeconds(uint32_t seconds);

// Cost of buying the next level of a system, in each resource.
struct Cost {
  uint64_t fuel = 0;
  uint64_t parts = 0;
  uint64_t data = 0;
};
Cost upgradeCost(System system, uint8_t currentLevel);

// Charge the jump drive needs before the ship can leave the current star.
uint64_t jumpChargeNeeded(const State& state);

uint8_t crewCapacity(const State& state);

// Woken sleepers on duty: every woken sleeper, up to the cryo bay's capacity.
uint16_t crewAwake(const State& state);

// Hull condition a normal (not clean) jump costs, after Hull upgrades.
uint8_t jumpWear(const State& state);

// Base chance in percent that a survey finds a derelict, before star effects.
uint8_t derelictChancePercent(const State& state);

// True when the course is plotted, the drive is charged and the hull holds.
bool canJump(const State& state);

// Advances the game by one reading session. Returns false (and leaves the
// state untouched) for sessions shorter than MIN_SESSION_SECONDS.
bool applySession(State& state, uint32_t activeSeconds, Doctrine doctrine);

// Starts a fresh voyage. Used for first play and for the hard reset.
void resetState(State& state);

// Formats a number with idle-game suffixes: 9999, 12.3k, 456k, 7.89M, 1.20B ... then
// aa, ab, ac for numbers past the trillions. Always null-terminates buf.
void formatCompactNumber(uint64_t value, char* buf, size_t len);

// --- Persistence (format is little-endian, field by field) ---

constexpr uint8_t SAVE_VERSION = 2;  // v2 adds lifetime records; v1 saves still load
constexpr size_t SAVE_BUFFER_SIZE = 224;

// Writes the state into buf. Returns bytes written, or 0 if buf is too small.
size_t serialize(const State& state, uint8_t* buf, size_t len);

// Reads a state written by serialize(). Returns false for unknown versions,
// short buffers or a bad checksum, leaving state unchanged.
bool deserialize(State& state, const uint8_t* buf, size_t len);

}  // namespace DriftShip
