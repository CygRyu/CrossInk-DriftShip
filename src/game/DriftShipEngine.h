#pragma once

#include <cstddef>
#include <cstdint>

#include "DriftShipColony.h"
#include "DriftShipCrew.h"

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
// Sittings less than this far apart are scored as one session.
constexpr uint32_t MERGE_GAP_SECONDS = 5 * 60;
// Shorter sittings are too brief to keep a merge chain open.
constexpr uint32_t MIN_SITTING_SECONDS = 30;

// What happened to a newly woken sleeper.
enum class CrewEvent : uint8_t {
  None = 0,
  Joined,    // took an empty bunk
  Replaced,  // was stronger than the weakest crew member, who went back to cryo
  Cryo,      // the crew was full and stronger, so the sleeper stays in cryo
};

enum class ModuleEvent : uint8_t { None = 0, Installed, Discarded };

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
  // Phase 2 (save version 3+).
  int16_t modulePermille = 0;  // +/- x/1000 Fuel from salvaged modules
  CrewEvent crewEvent = CrewEvent::None;
  CrewMember newCrew;  // valid when crewEvent != None
  bool crewInjured = false;
  uint16_t injuredNameSeed = 0;  // who was hurt, when crewInjured
  uint8_t levelUps = 0;
  ModuleEvent moduleEvent = ModuleEvent::None;
  Module module;  // valid when moduleEvent != None
  uint8_t retired = 0;
  // Save version 4+.
  int16_t variancePermille = 0;  // this session's random Fuel swing, -150..+150
  // Save version 5+.
  uint8_t sittings = 1;  // reading sittings merged into this session
  // Save version 6+: living colonies.
  uint16_t colonyPermille = 0;  // +x/1000 Fuel from the colony network
  uint32_t colonyParts = 0;     // Parts the colonies sent (included in parts)
  uint32_t colonyData = 0;      // Data the colonies sent (included in data)
  uint8_t colonyRepair = 0;     // hull condition the shipyards restored
  ColonyEvent colonyEvent = ColonyEvent::None;
  uint16_t colonyEventSeed = 0;  // name seed of the colony in colonyEvent
  uint16_t foundedSeed = 0;      // name seed of the colony founded or joined, when reachCompleted
  bool foundedJoined = false;    // the network was full: settlers joined an existing colony
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
  // Crew and modules (save version 3+).
  uint8_t crewCount = 0;
  CrewMember crew[MAX_CREW];
  uint8_t moduleCount = 0;
  Module modules[MODULE_SLOTS];
  uint16_t legacyPermille = 0;       // permanent Fuel bonus from retired crew
  uint16_t wakesSinceLegendary = 0;  // bad-luck protection
  uint8_t surveysSinceDerelict = 0;  // bad-luck protection
  uint8_t derelictsSinceModule = 0;  // bad-luck protection
  uint32_t legendaryWoken = 0;
  uint32_t modulesFound = 0;
  uint32_t crewRetired = 0;
  // Session merging (save version 5+): the open chain of sittings that the
  // next sitting may continue. sittingClock 0 means no chain is open.
  uint32_t sittingEnd = 0;      // clock seconds when the latest sitting ended
  uint32_t sittingClock = 0;    // which clock measured sittingEnd
  uint32_t sittingSeconds = 0;  // reading seconds in the chain so far
  uint8_t sittingCount = 0;     // sittings in the chain so far
  bool sittingScored = false;   // the chain was scored, so merging replays it from a snapshot
  // Living colonies (save version 6+), newest last.
  uint8_t colonyCount = 0;
  Colony colony[MAX_COLONIES];
  uint16_t sleepersAtReachStart = 0;  // sleepersWoken when the current reach began
  uint32_t colonyEvents = 0;          // lifetime colony reports
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
constexpr uint8_t LOG_NEW_CREW = 1u << 7;

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

// Like pushLogEntry, but replaces the newest entry when it has the same
// session number (a merged session was re-scored).
void upsertLogEntry(SessionLog& log, const LogEntry& entry);

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

// Crew on the roster (including injured crew).
uint16_t crewAwake(const State& state);

// Sum of a module effect across installed modules, in benefit points.
int32_t moduleBonus(const State& state, ModuleEffect effect);

// How the Steward rates a module: higher is better.
int32_t moduleScore(const Module& module);

// What a colony sends back each session. Bonuses scale with the number of
// digits in its population, so a colony of 1.2M (7 digits) gives +7% Fuel.
uint16_t colonyFuelPermille(const Colony& colony);
// Parts (Mining), Data (Science) or hull repair (Shipyard) per session.
uint32_t colonyGoods(const Colony& colony);
// Growth per session in tenths of a percent for a session of minutes.
uint32_t colonyGrowthPerMille(const Colony& colony, uint32_t minutes);

// Population across every colony.
uint64_t totalPopulation(const State& state);

// Chance in percent that the next woken sleeper is Legendary.
uint8_t legendaryChancePercent(const State& state);

// Average hull condition a normal (not clean) jump costs, after Hull upgrades,
// the Pilot and modules. Each real jump lands within 3 points of it.
uint8_t jumpWear(const State& state);

// Base chance in percent that a survey finds a derelict, before star effects.
uint8_t derelictChancePercent(const State& state);

// True when the course is plotted, the drive is charged and the hull holds.
bool canJump(const State& state);

// Advances the game by one reading session. Returns false (and leaves the
// state untouched) for sessions shorter than MIN_SESSION_SECONDS.
bool applySession(State& state, uint32_t activeSeconds, Doctrine doctrine);

// --- Session merging ---
//
// A sitting is one stretch of reading between opening a book and leaving it
// or sleeping. Sittings less than MERGE_GAP_SECONDS apart are one session, so
// a quick sleep or a book switch doesn't split a long read. The first sitting
// has already been scored by the time the next one ends, so a merge replays
// the whole chain from a snapshot taken before it: the result is exactly what
// one long session would have earned.
//
// clockId names the clock that measured now (0 = no clock). Times from
// different clocks are never compared.

// True when a sitting of activeSeconds that ended at now continues the open
// chain. The sitting started no later than now - activeSeconds, so the gap is
// never under-estimated. Merges stop once the chain would pass the 90-minute
// cap; inside the cap, merging never earns less Fuel than separate sessions.
bool continuesSitting(const State& state, uint32_t activeSeconds, uint32_t now, uint32_t clockId);

// Reading seconds the session will be scored with: the whole chain when the
// sitting merges, otherwise the sitting alone.
uint32_t sittingSessionSeconds(const State& previous, bool merged, uint32_t activeSeconds);

// Records the sitting that just ended into target (the state that will be
// saved), using previous (the state before this sitting) for the chain so far.
// target and previous may be the same object. scored says whether target now
// holds a scored session for this chain; its receipt then counts the sittings.
void recordSitting(State& target, const State& previous, bool merged, uint32_t activeSeconds, uint32_t now,
                   uint32_t clockId, bool scored);

// Closes the chain so the next sitting starts a new session.
void closeSitting(State& state);

// True when the sitting merges into a chain that was already scored, so the
// caller must load the snapshot into scratch before calling applySitting().
bool sittingNeedsSnapshot(const State& state, uint32_t activeSeconds, uint32_t now, uint32_t clockId);

struct SittingOutcome {
  bool save = false;           // state changed and should be saved
  bool scored = false;         // state.last is a new or re-scored session
  bool writeSnapshot = false;  // scratch holds a new snapshot for the chain
};

// Applies one sitting. state is the saved game and receives the result.
// scratch is working memory: on entry it holds the snapshot when
// snapshotLoaded, and on return it holds the new snapshot when
// writeSnapshot. Pass snapshotLoaded = false when the snapshot was needed
// but could not be read; the sitting is then scored on its own.
SittingOutcome applySitting(State& state, State& scratch, bool snapshotLoaded, uint32_t activeSeconds, uint32_t now,
                            uint32_t clockId, Doctrine doctrine);

// Starts the voyage of a never-played ship. applySession() calls it on the
// first counted session; seed is that session's seconds.
void startVoyage(State& state, uint32_t seed);

// Starts a fresh voyage. Used for first play and for the hard reset.
void resetState(State& state);

// Formats a number with idle-game suffixes: 9999, 12.3k, 456k, 7.89M, 1.20B ... then
// aa, ab, ac for numbers past the trillions. Always null-terminates buf.
void formatCompactNumber(uint64_t value, char* buf, size_t len);

// --- Persistence (format is little-endian, field by field) ---

// v2 records, v3 crew and modules, v4 Fuel variance, v5 merging, v6 colonies; older saves load.
constexpr uint8_t SAVE_VERSION = 6;
// Big enough for a full crew roster and colony network. Callers keep it off the stack.
constexpr size_t SAVE_BUFFER_SIZE = 1024;

// Writes the state into buf. Returns bytes written, or 0 if buf is too small.
size_t serialize(const State& state, uint8_t* buf, size_t len);

// Reads a state written by serialize(). Returns false for unknown versions,
// short buffers or a bad checksum, leaving state unchanged.
bool deserialize(State& state, const uint8_t* buf, size_t len);

}  // namespace DriftShip
