#include "DriftShipEngine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace DriftShip {
namespace {

constexpr uint64_t U64_MAX = UINT64_MAX;
constexpr uint16_t MAX_SYSTEM_LEVEL = 250;
constexpr uint8_t SAVE_MAGIC[4] = {'D', 'S', 'H', 'P'};

// Base costs for level 1 of each system, as {fuel, parts, data}. Every level
// after that costs 1.6x the previous one. static const keeps it in flash.
static const uint16_t BASE_COSTS[SYSTEM_COUNT][3] = {
    {20, 0, 0},   // Reactor
    {15, 5, 0},   // Drones
    {15, 0, 5},   // Sensors
    {40, 10, 0},  // Cryo bay
    {25, 10, 0},  // Hull
    {30, 0, 10},  // Jump drive
};

// How much each doctrine wants each system. Higher means bought sooner.
static const uint8_t DOCTRINE_WEIGHTS[static_cast<uint8_t>(Doctrine::Count)][SYSTEM_COUNT] = {
    {3, 3, 3, 2, 2, 3},  // Steady
    {2, 2, 5, 2, 2, 5},  // Explorer
    {5, 5, 2, 2, 3, 1},  // Builder
};

uint64_t satAdd(const uint64_t a, const uint64_t b) { return a > U64_MAX - b ? U64_MAX : a + b; }

uint64_t satMul(const uint64_t a, const uint64_t b) {
  if (a == 0 || b == 0) return 0;
  return a > U64_MAX / b ? U64_MAX : a * b;
}

uint32_t satAdd32(const uint32_t a, const uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }

// v * (1000 + permille) / 1000 without overflowing for large v.
uint64_t addPermille(const uint64_t v, const uint32_t permille) {
  const uint64_t extra = satAdd(satMul(v / 1000, permille), (v % 1000) * permille / 1000);
  return satAdd(v, extra);
}

uint64_t scaleRatio(const uint64_t v, const uint32_t num, const uint32_t den) {
  return satMul(v / den, num) + (v % den) * num / den;
}

uint32_t nextRandom(State& s) {
  // xorshift32: tiny, fast and good enough for game rolls.
  uint32_t x = s.rng ? s.rng : 0x9E3779B9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s.rng = x;
  return x;
}

// Uniform roll in 0..99.
uint8_t rollPercent(State& s) { return static_cast<uint8_t>(nextRandom(s) % 100); }

uint32_t rollRange(State& s, const uint32_t lo, const uint32_t hi) { return lo + nextRandom(s) % (hi - lo + 1); }

// A longer session within a watch's band raises its chances by up to +50%.
// band runs from bandStart until the next watch starts (or the session cap).
uint32_t boostedChance(const uint32_t chance, const uint32_t seconds, const uint32_t bandStart,
                       const uint32_t bandEnd) {
  if (seconds <= bandStart || bandEnd <= bandStart) return chance;
  const uint32_t into = std::min(seconds, bandEnd) - bandStart;
  const uint32_t boost = into * 50 / (bandEnd - bandStart);
  return std::min<uint32_t>(95, chance * (100 + boost) / 100);
}

bool rollChance(State& s, const uint32_t percent) { return rollPercent(s) < percent; }

StarType rollStarType(State& s) {
  // Weights: red dwarf 25, ice field 20, nebula 20, gas giant 20, pulsar 10, dead gate 5.
  const uint8_t r = rollPercent(s);
  if (r < 25) return StarType::RedDwarf;
  if (r < 45) return StarType::IceField;
  if (r < 65) return StarType::Nebula;
  if (r < 85) return StarType::GasGiant;
  if (r < 95) return StarType::Pulsar;
  return StarType::DeadGate;
}

uint8_t level(const State& s, const System sys) { return s.levels[static_cast<uint8_t>(sys)]; }

StarType currentStar(const State& s) {
  return s.starType < static_cast<uint8_t>(StarType::Count) ? static_cast<StarType>(s.starType) : StarType::RedDwarf;
}

void wear(State& s, const uint8_t amount) { s.condition = s.condition > amount ? s.condition - amount : 0; }

bool canAfford(const State& s, const Cost& c, const uint64_t fuelBudget) {
  return c.fuel <= fuelBudget && c.parts <= s.parts && c.data <= s.data;
}

// The Steward's spending: repairs first when the hull is weak, then the best
// value upgrade for the doctrine, repeated until nothing is affordable.
void autoSpend(State& s, const Doctrine doctrine, SessionReport& report) {
  if (s.condition < REPAIR_BELOW_CONDITION && s.parts >= 2) {
    const uint64_t points = std::min<uint64_t>(100 - s.condition, s.parts / 2);
    s.parts -= points * 2;
    s.condition = static_cast<uint8_t>(s.condition + points);
    report.repaired = true;
  }

  const uint8_t d = static_cast<uint8_t>(doctrine) < static_cast<uint8_t>(Doctrine::Count)
                        ? static_cast<uint8_t>(doctrine)
                        : static_cast<uint8_t>(Doctrine::Steady);
  // Keep a 20% Fuel reserve so one upgrade can't empty the tanks.
  const uint64_t reserve = s.fuel / 5;
  for (uint8_t guard = 0; guard < 32; guard++) {
    const uint64_t budget = s.fuel - std::min(s.fuel, reserve);
    // Rank every system by doctrine weight per unit of cost. The Steward buys
    // the top pick, or saves Fuel for it when Fuel is the only thing missing.
    // A pick blocked by Parts or Data is skipped so it can't stall spending.
    uint8_t order[SYSTEM_COUNT];
    uint64_t scores[SYSTEM_COUNT];
    uint8_t count = 0;
    for (uint8_t i = 0; i < SYSTEM_COUNT; i++) {
      if (s.levels[i] >= MAX_SYSTEM_LEVEL) continue;
      const Cost c = upgradeCost(static_cast<System>(i), s.levels[i]);
      const uint64_t weighted = satAdd(satAdd(c.fuel, satMul(c.parts, 2)), satMul(c.data, 2)) + 1;
      const uint64_t score = satMul(DOCTRINE_WEIGHTS[d][i], 1000000) / weighted;
      uint8_t at = count++;
      while (at > 0 && scores[at - 1] < score) {
        scores[at] = scores[at - 1];
        order[at] = order[at - 1];
        at--;
      }
      scores[at] = score;
      order[at] = i;
    }
    int best = -1;
    for (uint8_t k = 0; k < count; k++) {
      const Cost c = upgradeCost(static_cast<System>(order[k]), s.levels[order[k]]);
      if (canAfford(s, c, budget)) {
        best = order[k];
        break;
      }
      if (c.parts <= s.parts && c.data <= s.data) break;  // only Fuel is short: save up
    }
    if (best < 0) break;
    const Cost c = upgradeCost(static_cast<System>(best), s.levels[best]);
    s.fuel -= c.fuel;
    s.parts -= c.parts;
    s.data -= c.data;
    s.levels[best]++;
    if (report.upgradesBought < UINT8_MAX) report.upgradesBought++;
    report.lastUpgrade = static_cast<uint8_t>(best);
  }
}

void applyTwist(State& s, SessionReport& r, uint32_t& parts, uint32_t& data) {
  const uint8_t roll = rollPercent(s);
  if (roll < 50) {
    r.twist = Twist::None;
  } else if (roll < 80) {
    r.twist = Twist::Good;
    parts += parts / 2;
    data += data / 2;
  } else {
    r.twist = Twist::Bad;
    // Hull upgrades soften bad twists: 5 condition lost, minus 1 per 2 levels.
    const uint8_t hull = level(s, System::Hull);
    const uint8_t loss = hull >= 8 ? 1 : static_cast<uint8_t>(5 - hull / 2);
    wear(s, loss);
    parts -= parts / 10;
  }
}

}  // namespace

uint64_t baseFuelForSeconds(const uint32_t seconds) {
  const uint64_t t = std::min(seconds, SESSION_CAP_SECONDS);
  const uint64_t bonus = std::min<uint64_t>(t, BONUS_RAMP_SECONDS);
  return t * (BONUS_RAMP_SECONDS + bonus) / (60ULL * BONUS_RAMP_SECONDS);
}

Cost upgradeCost(const System system, const uint8_t currentLevel) {
  const uint8_t i = static_cast<uint8_t>(system);
  Cost c{BASE_COSTS[i][0], BASE_COSTS[i][1], BASE_COSTS[i][2]};
  for (uint8_t l = 0; l < currentLevel; l++) {
    c.fuel = scaleRatio(c.fuel, 8, 5);
    c.parts = scaleRatio(c.parts, 8, 5);
    c.data = scaleRatio(c.data, 8, 5);
    if (c.fuel == U64_MAX) break;
  }
  return c;
}

uint64_t jumpChargeNeeded(const State& s) {
  if (s.freeJump) return 0;
  uint64_t v = 60;
  for (uint8_t i = 0; i < s.star && v != U64_MAX; i++) v = scaleRatio(v, 5, 4);
  for (uint16_t i = 0; i < s.reach && v != U64_MAX; i++) v = scaleRatio(v, 3, 2);
  const uint32_t discount = std::min<uint32_t>(60, 6u * level(s, System::JumpDrive));
  v = scaleRatio(v, 100 - discount, 100);
  return std::max<uint64_t>(v, 1);
}

uint8_t crewCapacity(const State& s) {
  return static_cast<uint8_t>(std::min<uint16_t>(UINT8_MAX, 2 + level(s, System::CryoBay)));
}

uint16_t crewAwake(const State& s) { return std::min<uint16_t>(s.sleepersWoken, crewCapacity(s)); }

bool canJump(const State& s) {
  return s.surveysHere >= SURVEYS_TO_PLOT_COURSE && s.condition >= MIN_JUMP_CONDITION &&
         s.jumpCharge >= jumpChargeNeeded(s);
}

void resetState(State& s) {
  s = State{};
  s.version = SAVE_VERSION;
}

bool applySession(State& s, const uint32_t activeSeconds, const Doctrine doctrine) {
  if (activeSeconds < MIN_SESSION_SECONDS) return false;

  if (s.version == 0) {
    resetState(s);
    s.rng = 0x9E3779B9u ^ activeSeconds;
    s.starType = static_cast<uint8_t>(rollStarType(s));
  }

  SessionReport r;
  const uint32_t t = std::min(activeSeconds, SESSION_CAP_SECONDS);
  s.sessions = satAdd32(s.sessions, 1);
  s.creditedSeconds = satAdd32(s.creditedSeconds, t);
  r.sessionNumber = s.sessions;
  r.seconds = t;
  const StarType star = currentStar(s);
  const uint8_t drones = level(s, System::Drones);
  const uint8_t sensors = level(s, System::Sensors);

  // --- Fuel: the session-length curve, then the multiplier stack ---
  r.baseFuel = baseFuelForSeconds(t);
  r.reactorPermille = static_cast<uint16_t>(80u * level(s, System::Reactor));
  r.crewPermille = static_cast<uint16_t>(std::min<uint32_t>(60000, 30u * crewAwake(s)));
  r.starPermille = (star == StarType::GasGiant && t >= EXPEDITION_SECONDS) ? 300 : 0;
  r.lucky = rollChance(s, 5);
  uint64_t fuel = addPermille(r.baseFuel, r.reactorPermille);
  fuel = addPermille(fuel, r.crewPermille);
  fuel = addPermille(fuel, r.starPermille);
  if (r.lucky) fuel = satMul(fuel, 2);
  r.fuel = fuel;
  s.lifetimeFuel = satAdd(s.lifetimeFuel, fuel);
  // A quarter of every session's Fuel charges the jump drive directly.
  s.jumpCharge = satAdd(s.jumpCharge, fuel / 4);
  s.fuel = satAdd(s.fuel, fuel - fuel / 4);

  uint32_t parts = 0;
  uint32_t data = 0;
  bool sleeper = false;

  // --- Maintenance: always runs; repairs come first ---
  r.watches |= WATCH_MAINTENANCE;
  s.condition = static_cast<uint8_t>(std::min<uint32_t>(100, s.condition + 10u + 2u * drones));
  parts += rollRange(s, 1, 3) + drones;
  if (rollChance(s, boostedChance(15, t, MIN_SESSION_SECONDS, SURVEY_SECONDS))) parts += 5;

  // --- Survey: 10+ minutes, or every third short session ---
  bool survey = t >= SURVEY_SECONDS;
  if (!survey) {
    if (++s.shortSessionCounter >= 3) {
      s.shortSessionCounter = 0;
      survey = true;
    }
  }
  if (survey) {
    r.watches |= WATCH_SURVEY;
    uint32_t found = rollRange(s, 1, 3) + sensors / 2;
    if (star == StarType::IceField) found += found / 2;
    if (star == StarType::Nebula) found -= found / 4;
    data += found;
    if (s.surveysHere < UINT8_MAX) s.surveysHere++;
    uint32_t derelictChance = std::min<uint32_t>(60, 20u + 5u * sensors);
    if (star == StarType::Nebula) derelictChance *= 2;
    if (rollChance(s, boostedChance(derelictChance, t, SURVEY_SECONDS, EXPEDITION_SECONDS))) {
      r.derelict = true;
      parts += 5;
      data += 3;
      if (rollChance(s, 25)) sleeper = true;
    }
  }

  // --- Expedition: 30+ minutes ---
  uint32_t expeditionParts = 0;
  uint32_t expeditionData = 0;
  if (t >= EXPEDITION_SECONDS) {
    r.watches |= WATCH_EXPEDITION;
    expeditionParts = rollRange(s, 3, 8) + drones;
    expeditionData = rollRange(s, 2, 5);
    parts += expeditionParts;
    data += expeditionData;
    wear(s, 5);
    if (rollChance(s, boostedChance(30, t, EXPEDITION_SECONDS, LONG_WATCH_SECONDS))) sleeper = true;
  }

  if (survey || t >= EXPEDITION_SECONDS) applyTwist(s, r, parts, data);
  if (star == StarType::RedDwarf) parts += parts / 5;

  const bool longWatch = t >= LONG_WATCH_SECONDS;
  if (longWatch) r.watches |= WATCH_LONG;

  if (sleeper) {
    r.sleeperWoken = true;
    if (s.sleepersWoken < UINT16_MAX) s.sleepersWoken++;
  }

  s.parts = satAdd(s.parts, parts);
  s.data = satAdd(s.data, data);

  autoSpend(s, doctrine, r);

  // Once the course is plotted, a long watch tops up the jump drive with half
  // of the spare Fuel so the ship can leave sooner.
  if (longWatch && s.surveysHere >= SURVEYS_TO_PLOT_COURSE) {
    const uint64_t spare = s.fuel / 2;
    s.fuel -= spare;
    s.jumpCharge = satAdd(s.jumpCharge, spare);
  }

  // --- Jump ---
  if (canJump(s)) {
    s.jumpCharge -= jumpChargeNeeded(s);
    s.freeJump = false;
    r.jumped = true;
    r.cleanJump = longWatch;
    if (!longWatch) {
      const uint8_t hull = level(s, System::Hull);
      wear(s, hull >= 10 ? 5 : static_cast<uint8_t>(25 - 2 * hull));
    }
    if (star == StarType::Pulsar) wear(s, 15);
    s.starsVisited = satAdd32(s.starsVisited, 1);
    s.distanceDeciLy = satAdd32(s.distanceDeciLy, rollRange(s, 40, 99));
    s.surveysHere = 0;
    if (++s.star >= STARS_PER_REACH) {
      s.star = 0;
      if (s.reach < UINT16_MAX) s.reach++;
      if (s.colonies < UINT16_MAX) s.colonies++;
      r.reachCompleted = true;
    }
    const StarType next = rollStarType(s);
    s.starType = static_cast<uint8_t>(next);
    s.freeJump = next == StarType::DeadGate;
  } else if (longWatch && expeditionParts > 0) {
    // No jump this time: the long watch doubles the expedition's haul.
    s.parts = satAdd(s.parts, expeditionParts);
    s.data = satAdd(s.data, expeditionData);
    parts += expeditionParts;
    data += expeditionData;
  }

  r.parts = parts;
  r.data = data;
  s.last = r;
  return true;
}

void formatCompactNumber(const uint64_t value, char* buf, const size_t len) {
  if (!buf || len == 0) return;
  if (value < 10000) {
    snprintf(buf, len, "%llu", static_cast<unsigned long long>(value));
    return;
  }
  // Suffix per power of 1000, starting at thousands. After T comes aa, ab...
  static const char* const SUFFIXES[] = {"k", "M", "B", "T", "aa", "ab", "ac"};
  uint8_t tier = 0;
  uint64_t whole = value / 1000;
  uint64_t rem = value % 1000;
  while (whole >= 1000 && static_cast<size_t>(tier) + 1 < sizeof(SUFFIXES) / sizeof(SUFFIXES[0])) {
    rem = whole % 1000;
    whole /= 1000;
    tier++;
  }
  // Three significant digits: 12.3k, 456k, 7.89M.
  if (whole >= 100) {
    snprintf(buf, len, "%llu%s", static_cast<unsigned long long>(whole), SUFFIXES[tier]);
  } else if (whole >= 10) {
    snprintf(buf, len, "%llu.%llu%s", static_cast<unsigned long long>(whole),
             static_cast<unsigned long long>(rem / 100), SUFFIXES[tier]);
  } else {
    snprintf(buf, len, "%llu.%02llu%s", static_cast<unsigned long long>(whole),
             static_cast<unsigned long long>(rem / 10), SUFFIXES[tier]);
  }
}

// --- Persistence ---
namespace {

struct Writer {
  uint8_t* buf;
  size_t len;
  size_t pos = 0;
  bool ok = true;

  void bytes(const uint8_t* src, const size_t n) {
    if (!ok || pos + n > len) {
      ok = false;
      return;
    }
    std::memcpy(buf + pos, src, n);
    pos += n;
  }
  void u8(const uint8_t v) { bytes(&v, 1); }
  void u16(const uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    bytes(b, 2);
  }
  void u32(const uint32_t v) {
    u16(static_cast<uint16_t>(v));
    u16(static_cast<uint16_t>(v >> 16));
  }
  void u64(const uint64_t v) {
    u32(static_cast<uint32_t>(v));
    u32(static_cast<uint32_t>(v >> 32));
  }
};

struct Reader {
  const uint8_t* buf;
  size_t len;
  size_t pos = 0;
  bool ok = true;

  void bytes(uint8_t* dst, const size_t n) {
    if (!ok || pos + n > len) {
      ok = false;
      std::memset(dst, 0, n);
      return;
    }
    std::memcpy(dst, buf + pos, n);
    pos += n;
  }
  uint8_t u8() {
    uint8_t v = 0;
    bytes(&v, 1);
    return v;
  }
  uint16_t u16() {
    const uint16_t lo = u8();
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(u8()) << 8));
  }
  uint32_t u32() {
    const uint32_t lo = u16();
    return lo | (static_cast<uint32_t>(u16()) << 16);
  }
  uint64_t u64() {
    const uint64_t lo = u32();
    return lo | (static_cast<uint64_t>(u32()) << 32);
  }
};

uint32_t fnv1a(const uint8_t* data, const size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

constexpr uint8_t FLAG_LUCKY = 1u << 0;
constexpr uint8_t FLAG_DERELICT = 1u << 1;
constexpr uint8_t FLAG_SLEEPER = 1u << 2;
constexpr uint8_t FLAG_REPAIRED = 1u << 3;
constexpr uint8_t FLAG_JUMPED = 1u << 4;
constexpr uint8_t FLAG_CLEAN_JUMP = 1u << 5;
constexpr uint8_t FLAG_REACH_DONE = 1u << 6;

}  // namespace

size_t serialize(const State& s, uint8_t* buf, const size_t len) {
  Writer w{buf, len};
  w.bytes(SAVE_MAGIC, sizeof(SAVE_MAGIC));
  w.u8(SAVE_VERSION);
  w.u64(s.fuel);
  w.u64(s.parts);
  w.u64(s.data);
  w.u64(s.jumpCharge);
  w.u64(s.lifetimeFuel);
  w.u32(s.sessions);
  w.u32(s.creditedSeconds);
  w.u32(s.starsVisited);
  w.u32(s.distanceDeciLy);
  w.u32(s.rng);
  w.u16(s.reach);
  w.u16(s.sleepersWoken);
  w.u16(s.colonies);
  w.bytes(s.levels, SYSTEM_COUNT);
  w.u8(s.star);
  w.u8(s.starType);
  w.u8(s.surveysHere);
  w.u8(s.shortSessionCounter);
  w.u8(s.condition);
  w.u8(s.freeJump ? 1 : 0);

  const SessionReport& r = s.last;
  w.u32(r.sessionNumber);
  w.u32(r.seconds);
  w.u8(r.watches);
  w.u64(r.baseFuel);
  w.u16(r.reactorPermille);
  w.u16(r.crewPermille);
  w.u16(r.starPermille);
  w.u64(r.fuel);
  w.u32(r.parts);
  w.u32(r.data);
  w.u8(static_cast<uint8_t>(r.twist));
  const uint8_t flags = (r.lucky ? FLAG_LUCKY : 0) | (r.derelict ? FLAG_DERELICT : 0) |
                        (r.sleeperWoken ? FLAG_SLEEPER : 0) | (r.repaired ? FLAG_REPAIRED : 0) |
                        (r.jumped ? FLAG_JUMPED : 0) | (r.cleanJump ? FLAG_CLEAN_JUMP : 0) |
                        (r.reachCompleted ? FLAG_REACH_DONE : 0);
  w.u8(flags);
  w.u8(r.upgradesBought);
  w.u8(r.lastUpgrade);

  if (!w.ok) return 0;
  w.u32(fnv1a(buf, w.pos));
  return w.ok ? w.pos : 0;
}

bool deserialize(State& out, const uint8_t* buf, const size_t len) {
  if (len < sizeof(SAVE_MAGIC) + 1 + 4 || std::memcmp(buf, SAVE_MAGIC, sizeof(SAVE_MAGIC)) != 0) return false;
  Reader rd{buf, len};
  uint8_t magic[4];
  rd.bytes(magic, sizeof(magic));
  const uint8_t version = rd.u8();
  if (version != SAVE_VERSION) return false;

  State s;
  s.version = version;
  s.fuel = rd.u64();
  s.parts = rd.u64();
  s.data = rd.u64();
  s.jumpCharge = rd.u64();
  s.lifetimeFuel = rd.u64();
  s.sessions = rd.u32();
  s.creditedSeconds = rd.u32();
  s.starsVisited = rd.u32();
  s.distanceDeciLy = rd.u32();
  s.rng = rd.u32();
  s.reach = rd.u16();
  s.sleepersWoken = rd.u16();
  s.colonies = rd.u16();
  rd.bytes(s.levels, SYSTEM_COUNT);
  s.star = rd.u8();
  s.starType = rd.u8();
  s.surveysHere = rd.u8();
  s.shortSessionCounter = rd.u8();
  s.condition = rd.u8();
  s.freeJump = rd.u8() != 0;

  SessionReport& r = s.last;
  r.sessionNumber = rd.u32();
  r.seconds = rd.u32();
  r.watches = rd.u8();
  r.baseFuel = rd.u64();
  r.reactorPermille = rd.u16();
  r.crewPermille = rd.u16();
  r.starPermille = rd.u16();
  r.fuel = rd.u64();
  r.parts = rd.u32();
  r.data = rd.u32();
  const uint8_t twist = rd.u8();
  r.twist = twist <= static_cast<uint8_t>(Twist::Bad) ? static_cast<Twist>(twist) : Twist::None;
  const uint8_t flags = rd.u8();
  r.lucky = flags & FLAG_LUCKY;
  r.derelict = flags & FLAG_DERELICT;
  r.sleeperWoken = flags & FLAG_SLEEPER;
  r.repaired = flags & FLAG_REPAIRED;
  r.jumped = flags & FLAG_JUMPED;
  r.cleanJump = flags & FLAG_CLEAN_JUMP;
  r.reachCompleted = flags & FLAG_REACH_DONE;
  r.upgradesBought = rd.u8();
  r.lastUpgrade = rd.u8();

  const size_t payloadLen = rd.pos;
  const uint32_t checksum = rd.u32();
  if (!rd.ok || checksum != fnv1a(buf, payloadLen)) return false;

  if (s.star >= STARS_PER_REACH) s.star = 0;
  if (s.starType >= static_cast<uint8_t>(StarType::Count)) s.starType = 0;
  if (s.condition > 100) s.condition = 100;
  if (r.lastUpgrade >= SYSTEM_COUNT) r.lastUpgrade = 0;
  out = s;
  return true;
}

}  // namespace DriftShip
