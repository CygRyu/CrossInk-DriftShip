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

// Star type weights in percent for each era, in StarType order. static const
// keeps the table in flash.
static const uint8_t ERA_STAR_WEIGHTS[static_cast<uint8_t>(Era::Count)][static_cast<uint8_t>(StarType::Count)] = {
    // Red, Ice, Neb, Gas, Gate, Pulsar, Ruins, Binary, Hole, Void
    {25, 20, 20, 20, 5, 10, 0, 0, 0, 0},      // Outer rim
    {15, 10, 20, 10, 5, 10, 30, 0, 0, 0},     // Ruin belt
    {15, 10, 5, 15, 5, 15, 0, 25, 10, 0},     // Dense core
    {15, 15, 0, 10, 10, 0, 10, 0, 0, 40},     // Deep void
    {10, 25, 25, 10, 5, 15, 0, 5, 5, 0},      // Shattered arm
    {10, 10, 10, 10, 5, 10, 15, 10, 10, 10},  // Far galaxy
};

StarType rollStarType(State& s) {
  const auto era = static_cast<uint8_t>(eraForReach(s.reach));
  const uint8_t r = rollPercent(s);
  // Walked in this order so the Outer rim rolls exactly as it did before eras.
  static const StarType ORDER[static_cast<uint8_t>(StarType::Count)] = {
      StarType::RedDwarf, StarType::IceField, StarType::Nebula,     StarType::GasGiant,  StarType::Pulsar,
      StarType::DeadGate, StarType::Ruins,    StarType::BinaryStar, StarType::BlackHole, StarType::Void,
  };
  uint8_t at = 0;
  for (const StarType t : ORDER) {
    at = static_cast<uint8_t>(at + ERA_STAR_WEIGHTS[era][static_cast<uint8_t>(t)]);
    if (r < at) return t;
  }
  return StarType::RedDwarf;
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

// --- Crew ---

// The best uninjured crew member for a role, by their main stat. The Steward
// puts them on watch, so only they give the role's bonus.
const CrewMember* bestOnDuty(const State& s, const Role role) {
  const CrewMember* best = nullptr;
  for (uint8_t i = 0; i < s.crewCount; i++) {
    const CrewMember& c = s.crew[i];
    if (c.injured > 0 || c.role != role) continue;
    if (!best || mainStat(c) > mainStat(*best)) best = &c;
  }
  return best;
}

uint8_t dutyStat(const State& s, const Role role) {
  const CrewMember* c = bestOnDuty(s, role);
  return c ? mainStat(*c) : 0;
}

bool hasTrait(const CrewMember& c, const Trait trait) { return c.traits[0] == trait || c.traits[1] == trait; }

// How many uninjured crew carry a trait.
uint8_t traitOnDuty(const State& s, const Trait trait) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < s.crewCount; i++) {
    if (s.crew[i].injured == 0 && hasTrait(s.crew[i], trait)) n++;
  }
  return n;
}

uint8_t bestLuckOnDuty(const State& s) {
  uint8_t best = 0;
  for (uint8_t i = 0; i < s.crewCount; i++) {
    if (s.crew[i].injured == 0) best = std::max(best, s.crew[i].luck);
  }
  return best;
}

// Net Fuel bonus from the crew, in permille: morale, the Quartermaster,
// traits and the permanent legacy of retired crew.
uint32_t crewFuelPermille(const State& s, const uint32_t seconds) {
  int32_t pm = s.legacyPermille;
  for (uint8_t i = 0; i < s.crewCount; i++) {
    if (s.crew[i].injured == 0) pm += 10;  // morale: every crew member on duty
  }
  pm += 20 * dutyStat(s, Role::Quartermaster);
  if (seconds >= LONG_WATCH_SECONDS) pm += 100 * traitOnDuty(s, Trait::NightOwl);
  if (s.crewCount <= 2) pm += 100 * traitOnDuty(s, Trait::Loner);
  pm -= 50 * traitOnDuty(s, Trait::Dreamer);
  return static_cast<uint32_t>(std::max<int32_t>(0, std::min<int32_t>(60000, pm)));
}

uint8_t crewStrength(const CrewMember& c) { return static_cast<uint8_t>(c.skill + c.grit + c.luck + c.level); }

Trait rollTrait(State& s, const Trait other) {
  const uint8_t count = static_cast<uint8_t>(Trait::Count) - 1;  // excludes None
  Trait t = static_cast<Trait>(1 + nextRandom(s) % count);
  if (t == other) t = static_cast<Trait>(1 + (static_cast<uint8_t>(t) % count));
  return t;
}

CrewMember rollCrew(State& s) {
  static const uint8_t STAT_RANGE[static_cast<uint8_t>(Rarity::Count)][2] = {{1, 5}, {3, 7}, {5, 9}, {8, 10}};
  CrewMember c;
  const uint8_t legendary = legendaryChancePercent(s);
  const uint8_t r = rollPercent(s);
  if (r < legendary) {
    c.rarity = Rarity::Legendary;
  } else if (r < legendary + 9) {
    c.rarity = Rarity::Veteran;
  } else if (r < legendary + 39) {
    c.rarity = Rarity::Skilled;
  } else {
    c.rarity = Rarity::Common;
  }
  const uint8_t lo = STAT_RANGE[static_cast<uint8_t>(c.rarity)][0];
  const uint8_t hi = STAT_RANGE[static_cast<uint8_t>(c.rarity)][1];
  c.nameSeed = static_cast<uint16_t>(nextRandom(s) & 0x3FF);
  c.role = static_cast<Role>(nextRandom(s) % static_cast<uint8_t>(Role::Count));
  c.skill = static_cast<uint8_t>(rollRange(s, lo, hi));
  c.grit = static_cast<uint8_t>(rollRange(s, lo, hi));
  c.luck = static_cast<uint8_t>(rollRange(s, lo, hi));
  c.traits[0] = rollTrait(s, Trait::None);
  if (c.rarity >= Rarity::Veteran) c.traits[1] = rollTrait(s, c.traits[0]);
  c.quirk = static_cast<uint8_t>(nextRandom(s) % QUIRK_COUNT);
  c.joinedReach = s.reach;

  if (c.rarity == Rarity::Legendary) {
    s.wakesSinceLegendary = 0;
    s.legendaryWoken = satAdd32(s.legendaryWoken, 1);
  } else if (s.wakesSinceLegendary < UINT16_MAX) {
    s.wakesSinceLegendary++;
  }
  return c;
}

// A new crew member takes an empty bunk, replaces the weakest crew member if
// stronger, or goes back to cryo.
CrewEvent placeCrew(State& s, const CrewMember& woken) {
  if (s.crewCount < crewCapacity(s)) {
    s.crew[s.crewCount++] = woken;
    return CrewEvent::Joined;
  }
  uint8_t weakest = 0;
  for (uint8_t i = 1; i < s.crewCount; i++) {
    if (crewStrength(s.crew[i]) < crewStrength(s.crew[weakest])) weakest = i;
  }
  if (s.crewCount > 0 && crewStrength(woken) > crewStrength(s.crew[weakest])) {
    s.crew[weakest] = woken;
    return CrewEvent::Replaced;
  }
  return CrewEvent::Cryo;
}

// A sleeper wakes and is rolled.
CrewEvent wakeSleeper(State& s, CrewMember& woken) {
  if (s.sleepersWoken < UINT16_MAX) s.sleepersWoken++;
  woken = rollCrew(s);
  return placeCrew(s, woken);
}

// A bad twist may injure one crew member. Grit resists it.
void maybeInjure(State& s, SessionReport& r) {
  if (s.crewCount == 0) return;
  CrewMember& c = s.crew[nextRandom(s) % s.crewCount];
  if (c.injured > 0 || hasTrait(c, Trait::SteadyHands)) return;
  uint32_t chance = c.grit >= 11 ? 5 : std::max<uint32_t>(5, 60u - 5u * c.grit);
  if (hasTrait(c, Trait::Reckless)) chance = std::min<uint32_t>(95, chance * 2);
  if (!rollChance(s, chance)) return;
  c.injured = static_cast<uint8_t>(rollRange(s, 2, 5));
  r.crewInjured = true;
  r.injuredNameSeed = c.nameSeed;
}

void healCrew(State& s) {
  const uint8_t heal = static_cast<uint8_t>(1 + dutyStat(s, Role::Medic) / 5);
  for (uint8_t i = 0; i < s.crewCount; i++) {
    CrewMember& c = s.crew[i];
    c.injured = c.injured > heal ? static_cast<uint8_t>(c.injured - heal) : 0;
  }
}

// Crew on duty earn experience from the session's minutes.
void trainCrew(State& s, const uint32_t seconds, SessionReport& r) {
  const uint16_t minutes = static_cast<uint16_t>(seconds / 60);
  for (uint8_t i = 0; i < s.crewCount; i++) {
    CrewMember& c = s.crew[i];
    if (c.injured > 0 || c.level >= MAX_CREW_LEVEL) continue;
    c.xpMinutes = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, c.xpMinutes + minutes));
    const uint16_t need = static_cast<uint16_t>(120u * c.level);
    if (c.xpMinutes < need) continue;
    c.xpMinutes = static_cast<uint16_t>(c.xpMinutes - need);
    c.level++;
    uint8_t* stats[3] = {&c.skill, &c.grit, &c.luck};
    uint8_t& stat = *stats[nextRandom(s) % 3];
    if (stat < MAX_STAT) stat++;
    if (r.levelUps < UINT8_MAX) r.levelUps++;
  }
}

// Crew who have served for CREW_RETIRE_AFTER_REACHES reaches settle in the
// colony just founded and leave a permanent Fuel bonus behind. Returns how
// many retired; their main stats are added to founderStats.
uint8_t retireCrew(State& s, SessionReport& r, uint16_t& founderStats) {
  uint8_t kept = 0;
  uint8_t retired = 0;
  for (uint8_t i = 0; i < s.crewCount; i++) {
    const CrewMember& c = s.crew[i];
    if (s.reach >= c.joinedReach + CREW_RETIRE_AFTER_REACHES) {
      s.legacyPermille = static_cast<uint16_t>(std::min<uint32_t>(30000, s.legacyPermille + 5u * mainStat(c)));
      s.crewRetired = satAdd32(s.crewRetired, 1);
      if (r.retired < UINT8_MAX) r.retired++;
      retired++;
      founderStats = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, founderStats + mainStat(c)));
    } else {
      s.crew[kept++] = c;
    }
  }
  s.crewCount = kept;
  return retired;
}

// --- Living colonies ---

uint8_t populationDigits(uint64_t population) {
  uint8_t digits = 1;
  while (population >= 10) {
    population /= 10;
    digits++;
  }
  return digits;
}

// Founds the colony for the reach just completed, with the crew who retired
// there. Its starting population is the sleepers the ship woke on the reach.
void foundColony(State& s, SessionReport& r, const uint8_t founders, const uint16_t founderStats) {
  const uint16_t wokenThisReach =
      s.sleepersWoken > s.sleepersAtReachStart ? static_cast<uint16_t>(s.sleepersWoken - s.sleepersAtReachStart) : 0;
  s.sleepersAtReachStart = s.sleepersWoken;
  const uint64_t settlers = 1000ULL * (1 + wokenThisReach) + 500ULL * founders;

  if (s.colonyCount < MAX_COLONIES) {
    Colony& c = s.colony[s.colonyCount++];
    c = Colony{};
    c.nameSeed = static_cast<uint16_t>(nextRandom(s) & 0xFF);
    c.specialty = static_cast<Specialty>(nextRandom(s) % static_cast<uint8_t>(Specialty::Count));
    c.founders = founders;
    c.founderStats = founderStats;
    c.foundedReach = s.reach;
    c.population = settlers;
    r.foundedSeed = c.nameSeed;
    r.foundedJoined = false;
    return;
  }
  // The network is full: the settlers join the smallest colony.
  uint8_t smallest = 0;
  for (uint8_t i = 1; i < s.colonyCount; i++) {
    if (s.colony[i].population < s.colony[smallest].population) smallest = i;
  }
  Colony& c = s.colony[smallest];
  c.population = std::min(MAX_POPULATION, satAdd(c.population, settlers));
  c.founders = static_cast<uint8_t>(std::min<uint32_t>(UINT8_MAX, c.founders + founders));
  c.founderStats = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, c.founderStats + founderStats));
  r.foundedSeed = c.nameSeed;
  r.foundedJoined = true;
}

// Every colony grows by compound interest on the minutes read.
void growColonies(State& s, const uint32_t minutes) {
  for (uint8_t i = 0; i < s.colonyCount; i++) {
    Colony& c = s.colony[i];
    const uint64_t growth = std::max<uint64_t>(1, scaleRatio(c.population, colonyGrowthPerMille(c, minutes), 1000));
    c.population = std::min(MAX_POPULATION, satAdd(c.population, growth));
  }
}

// Now and then one colony sends a report: a boom, a fever or a gift.
void rollColonyEvent(State& s, SessionReport& r, uint32_t& parts, uint32_t& data) {
  if (s.colonyCount == 0 || !rollChance(s, 12)) return;
  Colony& c = s.colony[nextRandom(s) % s.colonyCount];
  const uint8_t digits = populationDigits(c.population);
  const uint8_t roll = rollPercent(s);
  ColonyEvent event;
  if (roll < 35) {
    event = ColonyEvent::Boom;
    c.population = std::min(MAX_POPULATION, satAdd(c.population, c.population / 10));
  } else if (roll < 65) {
    event = ColonyEvent::Gift;
    parts += 5u * digits;
    data += 3u * digits;
    r.colonyParts += 5u * digits;
    r.colonyData += 3u * digits;
  } else if (roll < 80) {
    event = ColonyEvent::Fever;
    c.population = std::max<uint64_t>(1, c.population - c.population / 10);
  } else if (roll < 92 || r.sleeperWoken) {
    // A trained crew member only comes on a session that didn't wake one.
    event = ColonyEvent::HullSection;
    s.condition = static_cast<uint8_t>(std::min<uint32_t>(100, s.condition + 15u));
  } else {
    event = ColonyEvent::TrainedCrew;
  }
  r.colonyEvent = event;
  r.colonyEventSeed = c.nameSeed;
  s.colonyEvents = satAdd32(s.colonyEvents, 1);
}

// --- Salvaged modules ---

// Value range of each effect when it is the module's main benefit.
static const uint8_t MODULE_RANGE[static_cast<uint8_t>(ModuleEffect::Count)][2] = {
    {0, 0},    // None
    {5, 15},   // Fuel %
    {10, 30},  // Parts %
    {10, 30},  // Data %
    {3, 8},    // Derelict chance points
    {2, 5},    // Jump wear points
    {5, 12},   // Jump charge %
};

// How much the Steward values one point of each effect.
static const uint8_t MODULE_WEIGHT[static_cast<uint8_t>(ModuleEffect::Count)] = {0, 4, 1, 1, 2, 3, 2};

ModuleEffect rollEffect(State& s, const ModuleEffect other) {
  const uint8_t count = static_cast<uint8_t>(ModuleEffect::Count) - 1;
  auto e = static_cast<ModuleEffect>(1 + nextRandom(s) % count);
  if (e == other) e = static_cast<ModuleEffect>(1 + (static_cast<uint8_t>(e) % count));
  return e;
}

int8_t rollEffectValue(State& s, const ModuleEffect e) {
  const uint8_t i = static_cast<uint8_t>(e);
  return static_cast<int8_t>(rollRange(s, MODULE_RANGE[i][0], MODULE_RANGE[i][1]));
}

Module rollModule(State& s) {
  Module m;
  m.effect[0] = rollEffect(s, ModuleEffect::None);
  m.value[0] = rollEffectValue(s, m.effect[0]);
  const uint8_t shape = rollPercent(s);
  if (shape < 40) {
    // A drawback, paid for with a stronger main effect.
    m.value[0] = static_cast<int8_t>(m.value[0] + m.value[0] / 2);
    m.effect[1] = rollEffect(s, m.effect[0]);
    m.value[1] = static_cast<int8_t>(-std::max<int>(1, rollEffectValue(s, m.effect[1]) / 2));
  } else if (shape < 65) {
    // A small second benefit.
    m.effect[1] = rollEffect(s, m.effect[0]);
    m.value[1] = static_cast<int8_t>(std::max<int>(1, rollEffectValue(s, m.effect[1]) / 2));
  }
  return m;
}

// Installs into a free slot, or over the worst module if the new one scores
// higher. Otherwise the module is broken down for scrap.
ModuleEvent installModule(State& s, const Module& m) {
  if (s.moduleCount < MODULE_SLOTS) {
    s.modules[s.moduleCount++] = m;
    return ModuleEvent::Installed;
  }
  uint8_t worst = 0;
  for (uint8_t i = 1; i < s.moduleCount; i++) {
    if (moduleScore(s.modules[i]) < moduleScore(s.modules[worst])) worst = i;
  }
  if (moduleScore(m) > moduleScore(s.modules[worst])) {
    s.modules[worst] = m;
    return ModuleEvent::Installed;
  }
  s.parts = satAdd(s.parts, 5);
  return ModuleEvent::Discarded;
}

// Applies a signed percentage to a resource amount, never below zero.
uint32_t applyPercent(const uint32_t v, const int32_t pct) {
  if (pct >= 0) return v + static_cast<uint32_t>(static_cast<uint64_t>(v) * static_cast<uint32_t>(pct) / 100);
  const uint32_t cut = static_cast<uint32_t>(static_cast<uint64_t>(v) * static_cast<uint32_t>(-pct) / 100);
  return cut >= v ? 0 : v - cut;
}

void applyTwist(State& s, SessionReport& r, uint32_t& parts, uint32_t& data) {
  // Each point of the luckiest crew member's Luck turns 1% of neutral twists good.
  const uint8_t goodFrom = static_cast<uint8_t>(50 - std::min<uint8_t>(10, bestLuckOnDuty(s)));
  const uint8_t roll = rollPercent(s);
  if (roll < goodFrom) {
    r.twist = Twist::None;
  } else if (roll < 80) {
    r.twist = Twist::Good;
    parts += parts / 2;
    data += data / 2;
  } else if (traitOnDuty(s, Trait::Lucky) > 0 && rollChance(s, 50)) {
    r.twist = Twist::None;  // a Lucky crew member saw it coming
  } else {
    r.twist = Twist::Bad;
    // Hull upgrades soften bad twists: 5 condition lost, minus 1 per 2 levels.
    const uint8_t hull = level(s, System::Hull);
    const uint8_t loss = hull >= 8 ? 1 : static_cast<uint8_t>(5 - hull / 2);
    wear(s, loss);
    parts -= parts / 10;
    maybeInjure(s, r);
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
  const int32_t discount = static_cast<int32_t>(std::min<uint32_t>(60, 6u * level(s, System::JumpDrive))) +
                           moduleBonus(s, ModuleEffect::JumpCharge);
  const int32_t clamped = std::max<int32_t>(-50, std::min<int32_t>(75, discount));
  v = scaleRatio(v, static_cast<uint32_t>(100 - clamped), 100);
  // A black hole's gravity slings the ship out for less charge.
  if (s.starType == static_cast<uint8_t>(StarType::BlackHole)) v = scaleRatio(v, 60, 100);
  return std::max<uint64_t>(v, 1);
}

uint8_t crewCapacity(const State& s) {
  return static_cast<uint8_t>(std::min<uint16_t>(MAX_CREW, 2 + level(s, System::CryoBay)));
}

uint16_t crewAwake(const State& s) { return s.crewCount; }

int32_t moduleBonus(const State& s, const ModuleEffect effect) {
  int32_t total = 0;
  for (uint8_t i = 0; i < s.moduleCount; i++) {
    for (uint8_t k = 0; k < 2; k++) {
      if (s.modules[i].effect[k] == effect) total += s.modules[i].value[k];
    }
  }
  return total;
}

int32_t moduleScore(const Module& m) {
  int32_t score = 0;
  for (uint8_t k = 0; k < 2; k++) {
    const auto e = static_cast<uint8_t>(m.effect[k]);
    if (e < static_cast<uint8_t>(ModuleEffect::Count)) score += MODULE_WEIGHT[e] * m.value[k];
  }
  return score;
}

Era eraForReach(const uint16_t reach) {
  return static_cast<Era>((reach / REACHES_PER_ERA) % static_cast<uint8_t>(Era::Count));
}

uint16_t eraNumber(const uint16_t reach) { return static_cast<uint16_t>(reach / REACHES_PER_ERA + 1); }

uint8_t starTypeChance(const Era era, const StarType type) {
  if (era >= Era::Count || type >= StarType::Count) return 0;
  return ERA_STAR_WEIGHTS[static_cast<uint8_t>(era)][static_cast<uint8_t>(type)];
}

uint16_t colonyFuelPermille(const Colony& c) {
  const uint32_t digits = populationDigits(c.population);
  return static_cast<uint16_t>((c.specialty == Specialty::Shipyard ? 20u : 10u) * digits);
}

uint32_t colonyGoods(const Colony& c) {
  const uint32_t digits = populationDigits(c.population);
  switch (c.specialty) {
    case Specialty::Mining:
      return digits;
    case Specialty::Science:
      return (digits + 1) / 2;
    case Specialty::Shipyard:
      return digits / 2;
    case Specialty::Count:
      break;
  }
  return 0;
}

uint32_t colonyGrowthPerMille(const Colony& c, const uint32_t minutes) {
  // 1% per 30 minutes read, up to twice as fast with strong founders.
  const uint32_t founderBonus = std::min<uint32_t>(100, 5u * c.founderStats);
  return std::min(SESSION_CAP_SECONDS / 60, minutes) * (100 + founderBonus) / 300;
}

uint64_t totalPopulation(const State& s) {
  uint64_t total = 0;
  for (uint8_t i = 0; i < s.colonyCount; i++) total = satAdd(total, s.colony[i].population);
  return total;
}

uint8_t legendaryChancePercent(const State& s) {
  // 1% until 75 wakes without a Legendary, then +2% per wake: guaranteed by the 150th.
  if (s.wakesSinceLegendary < 75) return 1;
  return static_cast<uint8_t>(std::min<uint32_t>(100, 1u + 2u * (s.wakesSinceLegendary - 74u)));
}

uint8_t jumpWear(const State& s) {
  const uint8_t hull = level(s, System::Hull);
  const int32_t base = hull >= 10 ? 5 : 25 - 2 * hull;
  const int32_t reduced = base - dutyStat(s, Role::Pilot) / 2 - moduleBonus(s, ModuleEffect::HullWear);
  return static_cast<uint8_t>(std::max<int32_t>(1, std::min<int32_t>(60, reduced)));
}

uint8_t derelictChancePercent(const State& s) {
  const int32_t chance = 20 + 5 * level(s, System::Sensors) + dutyStat(s, Role::Scientist) +
                         10 * traitOnDuty(s, Trait::Dreamer) + moduleBonus(s, ModuleEffect::Derelict);
  return static_cast<uint8_t>(std::max<int32_t>(0, std::min<int32_t>(80, chance)));
}

bool canJump(const State& s) {
  return s.surveysHere >= SURVEYS_TO_PLOT_COURSE && s.condition >= MIN_JUMP_CONDITION &&
         s.jumpCharge >= jumpChargeNeeded(s);
}

void resetState(State& s) {
  s = State{};
  s.version = SAVE_VERSION;
}

void startVoyage(State& s, const uint32_t seed) {
  resetState(s);
  s.rng = 0x9E3779B9u ^ seed;
  s.starType = static_cast<uint8_t>(rollStarType(s));
}

bool applySession(State& s, const uint32_t activeSeconds, const Doctrine doctrine) {
  if (activeSeconds < MIN_SESSION_SECONDS) return false;

  if (s.version == 0) startVoyage(s, activeSeconds);

  SessionReport r;
  const uint32_t t = std::min(activeSeconds, SESSION_CAP_SECONDS);
  s.sessions = satAdd32(s.sessions, 1);
  s.creditedSeconds = satAdd32(s.creditedSeconds, t);
  r.sessionNumber = s.sessions;
  r.seconds = t;
  const StarType star = currentStar(s);
  const uint8_t drones = level(s, System::Drones);
  const uint8_t sensors = level(s, System::Sensors);

  // Injured crew recover a little before this session's watches.
  healCrew(s);

  // --- Fuel: the session-length curve, then the multiplier stack ---
  r.baseFuel = baseFuelForSeconds(t);
  r.reactorPermille = static_cast<uint16_t>(80u * level(s, System::Reactor));
  r.crewPermille = static_cast<uint16_t>(crewFuelPermille(s, t));
  uint32_t colonyPermille = 0;
  for (uint8_t i = 0; i < s.colonyCount; i++) colonyPermille += colonyFuelPermille(s.colony[i]);
  r.colonyPermille = static_cast<uint16_t>(std::min<uint32_t>(5000, colonyPermille));
  // A gas giant refuels expeditions; the empty void leaves nothing to do but read.
  if (star == StarType::GasGiant && t >= EXPEDITION_SECONDS) {
    r.starPermille = 300;
  } else if (star == StarType::Void) {
    r.starPermille = 250;
  }
  r.bonusStar = static_cast<uint8_t>(star);
  r.modulePermille =
      static_cast<int16_t>(std::max<int32_t>(-500, std::min<int32_t>(5000, 10 * moduleBonus(s, ModuleEffect::Fuel))));
  r.lucky = rollChance(s, 5u + bestLuckOnDuty(s) / 2);
  uint64_t fuel = addPermille(r.baseFuel, r.reactorPermille);
  fuel = addPermille(fuel, r.crewPermille);
  fuel = addPermille(fuel, r.colonyPermille);
  fuel = addPermille(fuel, r.starPermille);
  if (r.modulePermille >= 0) {
    fuel = addPermille(fuel, static_cast<uint32_t>(r.modulePermille));
  } else {
    fuel -= scaleRatio(fuel, static_cast<uint32_t>(-r.modulePermille), 1000);
  }
  // Every session's Fuel varies by up to 15% either way, so the same reading
  // time never pays exactly the same twice.
  r.variancePermille = static_cast<int16_t>(10 * (static_cast<int32_t>(rollRange(s, 85, 115)) - 100));
  if (r.variancePermille >= 0) {
    fuel = addPermille(fuel, static_cast<uint32_t>(r.variancePermille));
  } else {
    fuel -= scaleRatio(fuel, static_cast<uint32_t>(-r.variancePermille), 1000);
  }
  if (r.lucky) fuel = satMul(fuel, 2);
  r.fuel = fuel;
  s.lifetimeFuel = satAdd(s.lifetimeFuel, fuel);
  // A quarter of every session's Fuel charges the jump drive directly.
  s.jumpCharge = satAdd(s.jumpCharge, fuel / 4);
  s.fuel = satAdd(s.fuel, fuel - fuel / 4);

  uint32_t parts = 0;
  uint32_t data = 0;
  bool sleeper = false;
  const uint8_t hoarders = traitOnDuty(s, Trait::Hoarder);
  const uint8_t medic = dutyStat(s, Role::Medic);

  // --- Maintenance: always runs; repairs come first ---
  r.watches |= WATCH_MAINTENANCE;
  const uint32_t repair = rollRange(s, 8, 12) + 2u * drones + dutyStat(s, Role::Engineer) / 2;
  s.condition = static_cast<uint8_t>(std::min<uint32_t>(100, s.condition + repair));
  parts += rollRange(s, 1, 3) + drones;
  if (rollChance(s, boostedChance(15, t, MIN_SESSION_SECONDS, SURVEY_SECONDS))) parts += 5 + hoarders;

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
    if (star == StarType::Void) found -= found / 2;
    data += found;
    if (s.surveysHere < UINT8_MAX) s.surveysHere++;
    // Bad-luck protection: +5 points for every survey since the last derelict.
    uint32_t derelictChance = derelictChancePercent(s) + 5u * s.surveysSinceDerelict;
    if (star == StarType::Nebula) derelictChance *= 2;
    if (star == StarType::Ruins) derelictChance *= 3;
    if (rollChance(s, boostedChance(std::min<uint32_t>(95, derelictChance), t, SURVEY_SECONDS, EXPEDITION_SECONDS))) {
      r.derelict = true;
      s.surveysSinceDerelict = 0;
      parts += 5 + hoarders;
      data += 3;
      if (rollChance(s, 25u + medic)) sleeper = true;
      // Bad-luck protection: +10 points for every derelict without a module.
      const uint32_t ruinsBonus = star == StarType::Ruins ? 20 : 0;
      if (rollChance(s, std::min<uint32_t>(95, 30u + ruinsBonus + 10u * s.derelictsSinceModule))) {
        s.derelictsSinceModule = 0;
        s.modulesFound = satAdd32(s.modulesFound, 1);
        r.module = rollModule(s);
        r.moduleEvent = installModule(s, r.module);
      } else if (s.derelictsSinceModule < UINT8_MAX) {
        s.derelictsSinceModule++;
      }
    } else if (s.surveysSinceDerelict < UINT8_MAX) {
      s.surveysSinceDerelict++;
    }
  }

  // --- Expedition: 30+ minutes ---
  uint32_t expeditionParts = 0;
  uint32_t expeditionData = 0;
  if (t >= EXPEDITION_SECONDS) {
    r.watches |= WATCH_EXPEDITION;
    expeditionParts = rollRange(s, 3, 8) + drones;
    expeditionData = rollRange(s, 2, 5);
    // The Scout adds 3% per point of Luck; each Reckless crew member adds 20%.
    const int32_t bonusPct = 3 * dutyStat(s, Role::Scout) + 20 * traitOnDuty(s, Trait::Reckless);
    expeditionParts = applyPercent(expeditionParts, bonusPct);
    expeditionData = applyPercent(expeditionData, bonusPct);
    parts += expeditionParts;
    data += expeditionData;
    wear(s, static_cast<uint8_t>(rollRange(s, 3, 7)));
    if (rollChance(s, boostedChance(30u + medic, t, EXPEDITION_SECONDS, LONG_WATCH_SECONDS))) sleeper = true;
  }

  if (survey || t >= EXPEDITION_SECONDS) applyTwist(s, r, parts, data);
  if (star == StarType::RedDwarf) parts += parts / 5;
  if (star == StarType::BinaryStar) {
    parts += parts / 4;
    data += data / 4;
  }
  // The Engineer and Scientist on duty add 2% per point; modules add or take away.
  parts = applyPercent(parts, 2 * dutyStat(s, Role::Engineer) + moduleBonus(s, ModuleEffect::Parts));
  data = applyPercent(data, 2 * dutyStat(s, Role::Scientist) + moduleBonus(s, ModuleEffect::Data));

  const bool longWatch = t >= LONG_WATCH_SECONDS;
  if (longWatch) r.watches |= WATCH_LONG;

  if (sleeper) {
    r.sleeperWoken = true;
    r.crewEvent = wakeSleeper(s, r.newCrew);
  }

  // --- Colonies: growth, shipments and the odd report ---
  growColonies(s, t / 60);
  for (uint8_t i = 0; i < s.colonyCount; i++) {
    const Colony& c = s.colony[i];
    const uint32_t goods = colonyGoods(c);
    if (c.specialty == Specialty::Mining) {
      parts += goods;
      r.colonyParts += goods;
    } else if (c.specialty == Specialty::Science) {
      data += goods;
      r.colonyData += goods;
    } else {
      r.colonyRepair = static_cast<uint8_t>(std::min<uint32_t>(100, r.colonyRepair + goods));
    }
  }
  s.condition = static_cast<uint8_t>(std::min<uint32_t>(100, s.condition + r.colonyRepair));
  rollColonyEvent(s, r, parts, data);
  if (r.colonyEvent == ColonyEvent::TrainedCrew) {
    // Raised in the colony: never Common, and already a few levels in.
    CrewMember trained = rollCrew(s);
    if (trained.rarity == Rarity::Common) trained.rarity = Rarity::Skilled;
    trained.level = 3;
    trained.skill = static_cast<uint8_t>(std::min<uint32_t>(MAX_STAT, trained.skill + 2u));
    r.newCrew = trained;
    r.crewEvent = placeCrew(s, trained);
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
      // jumpWear() is the average; a real jump lands within 3 points of it.
      const int32_t wearRoll = static_cast<int32_t>(jumpWear(s)) + static_cast<int32_t>(rollRange(s, 0, 6)) - 3;
      wear(s, static_cast<uint8_t>(std::max<int32_t>(1, wearRoll)));
    }
    if (star == StarType::Pulsar) wear(s, 15);
    if (star == StarType::BlackHole) wear(s, 10);
    s.starsVisited = satAdd32(s.starsVisited, 1);
    s.distanceDeciLy = satAdd32(s.distanceDeciLy, rollRange(s, 40, 99));
    s.surveysHere = 0;
    if (++s.star >= STARS_PER_REACH) {
      s.star = 0;
      if (s.reach < UINT16_MAX) s.reach++;
      if (s.colonies < UINT16_MAX) s.colonies++;
      r.reachCompleted = true;
      uint16_t founderStats = 0;
      const uint8_t founders = retireCrew(s, r, founderStats);
      foundColony(s, r, founders, founderStats);
      r.newEra = s.reach % REACHES_PER_ERA == 0;
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

  trainCrew(s, t, r);

  r.parts = parts;
  r.data = data;
  s.bestSessionFuel = std::max(s.bestSessionFuel, r.fuel);
  s.longestSessionSeconds = std::max(s.longestSessionSeconds, t);
  if (r.derelict) s.derelictsFound = satAdd32(s.derelictsFound, 1);
  if (r.lucky) s.luckyWatches = satAdd32(s.luckyWatches, 1);
  s.upgradesBought = satAdd32(s.upgradesBought, r.upgradesBought);
  s.last = r;
  return true;
}

LogEntry makeLogEntry(const State& s) {
  const SessionReport& r = s.last;
  LogEntry e;
  e.sessionNumber = r.sessionNumber;
  e.minutes = static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, r.seconds / 60));
  e.watches = r.watches;
  e.flags = (r.lucky ? LOG_LUCKY : 0) | (r.derelict ? LOG_DERELICT : 0) | (r.sleeperWoken ? LOG_SLEEPER : 0) |
            (r.jumped ? LOG_JUMPED : 0) | (r.reachCompleted ? LOG_COLONY : 0) |
            (r.twist == Twist::Bad ? LOG_BAD_TWIST : 0) | (r.twist == Twist::Good ? LOG_GOOD_TWIST : 0) |
            (r.crewEvent == CrewEvent::Joined || r.crewEvent == CrewEvent::Replaced ? LOG_NEW_CREW : 0);
  e.fuel = r.fuel;
  e.reach = s.reach;
  e.star = s.star;
  e.starType = s.starType;
  return e;
}

bool continuesSitting(const State& s, const uint32_t activeSeconds, const uint32_t now, const uint32_t clockId) {
  if (clockId == 0 || s.sittingClock != clockId || s.sittingCount == 0) return false;
  if (now < s.sittingEnd) return false;  // the clock went backwards
  if (activeSeconds > SESSION_CAP_SECONDS || s.sittingSeconds > SESSION_CAP_SECONDS - activeSeconds) return false;
  const uint32_t sinceEnd = now - s.sittingEnd;
  const uint32_t gap = sinceEnd > activeSeconds ? sinceEnd - activeSeconds : 0;
  return gap < MERGE_GAP_SECONDS;
}

uint32_t sittingSessionSeconds(const State& previous, const bool merged, const uint32_t activeSeconds) {
  return merged ? satAdd32(previous.sittingSeconds, activeSeconds) : activeSeconds;
}

void recordSitting(State& target, const State& previous, const bool merged, const uint32_t activeSeconds,
                   const uint32_t now, const uint32_t clockId, const bool scored) {
  // Read everything from previous before writing: they may be the same object.
  const uint32_t seconds = sittingSessionSeconds(previous, merged, activeSeconds);
  const uint8_t count = merged ? static_cast<uint8_t>(std::min<uint32_t>(UINT8_MAX, previous.sittingCount + 1u)) : 1;
  target.sittingEnd = now;
  target.sittingClock = clockId;
  target.sittingSeconds = seconds;
  target.sittingCount = clockId == 0 ? 0 : count;
  target.sittingScored = scored;
  if (scored) target.last.sittings = count;
}

void closeSitting(State& s) {
  s.sittingClock = 0;
  s.sittingCount = 0;
  s.sittingSeconds = 0;
  s.sittingScored = false;
}

void pushLogEntry(SessionLog& log, const LogEntry& entry) {
  if (log.count >= LOG_CAPACITY) {
    for (uint8_t i = 1; i < LOG_CAPACITY; i++) log.entries[i - 1] = log.entries[i];
    log.count = LOG_CAPACITY - 1;
  }
  log.entries[log.count++] = entry;
}

bool sittingNeedsSnapshot(const State& s, const uint32_t activeSeconds, const uint32_t now, const uint32_t clockId) {
  return s.sittingScored && continuesSitting(s, activeSeconds, now, clockId);
}

SittingOutcome applySitting(State& s, State& scratch, const bool snapshotLoaded, const uint32_t activeSeconds,
                            const uint32_t now, const uint32_t clockId, const Doctrine doctrine) {
  SittingOutcome out;
  if (activeSeconds < MIN_SITTING_SECONDS) return out;
  bool merged = continuesSitting(s, activeSeconds, now, clockId);
  const bool replay = merged && s.sittingScored && snapshotLoaded;
  if (merged && s.sittingScored && !snapshotLoaded) merged = false;  // can't replay: score on its own
  const uint32_t seconds = sittingSessionSeconds(s, merged, activeSeconds);

  if (s.version == 0) {
    // Nothing to save until the first counted session. Start the voyage
    // before the snapshot is taken so a replay starts from the same seed.
    if (seconds < MIN_SESSION_SECONDS) return out;
    startVoyage(s, seconds);
  }

  if (replay) {
    // scratch is the snapshot from before the chain. Re-score the whole chain
    // on it, then record the sitting using the chain totals from s.
    const bool scored = applySession(scratch, seconds, doctrine);
    recordSitting(scratch, s, true, activeSeconds, now, clockId, true);
    s = scratch;
    out.save = true;
    out.scored = scored;
    return out;
  }

  scratch = s;  // the state before this sitting: the snapshot if it scores
  out.scored = applySession(s, seconds, doctrine);
  recordSitting(s, scratch, merged, activeSeconds, now, clockId, out.scored);
  out.save = true;
  out.writeSnapshot = out.scored && clockId != 0;
  return out;
}

void upsertLogEntry(SessionLog& log, const LogEntry& entry) {
  if (log.count > 0 && log.entries[log.count - 1].sessionNumber == entry.sessionNumber) {
    log.entries[log.count - 1] = entry;
    return;
  }
  pushLogEntry(log, entry);
}

void serializeLogEntry(const LogEntry& e, uint8_t (&buf)[LOG_ENTRY_SIZE]) {
  size_t i = 0;
  const auto put = [&](const uint64_t v, const int bytes) {
    for (int b = 0; b < bytes; b++) buf[i++] = static_cast<uint8_t>(v >> (8 * b));
  };
  put(e.sessionNumber, 4);
  put(e.minutes, 2);
  put(e.watches, 1);
  put(e.flags, 1);
  put(e.fuel, 8);
  put(e.reach, 2);
  put(e.star, 1);
  put(e.starType, 1);
}

LogEntry deserializeLogEntry(const uint8_t (&buf)[LOG_ENTRY_SIZE]) {
  size_t i = 0;
  const auto get = [&](const int bytes) {
    uint64_t v = 0;
    for (int b = 0; b < bytes; b++) v |= static_cast<uint64_t>(buf[i++]) << (8 * b);
    return v;
  };
  LogEntry e;
  e.sessionNumber = static_cast<uint32_t>(get(4));
  e.minutes = static_cast<uint16_t>(get(2));
  e.watches = static_cast<uint8_t>(get(1));
  e.flags = static_cast<uint8_t>(get(1));
  e.fuel = get(8);
  e.reach = static_cast<uint16_t>(get(2));
  e.star = static_cast<uint8_t>(get(1));
  e.starType = static_cast<uint8_t>(get(1));
  if (e.star >= STARS_PER_REACH) e.star = 0;
  if (e.starType >= static_cast<uint8_t>(StarType::Count)) e.starType = 0;
  return e;
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

void writeCrew(Writer& w, const CrewMember& c) {
  w.u16(c.nameSeed);
  w.u8(static_cast<uint8_t>(c.role));
  w.u8(static_cast<uint8_t>(c.rarity));
  w.u8(c.skill);
  w.u8(c.grit);
  w.u8(c.luck);
  w.u8(static_cast<uint8_t>(c.traits[0]));
  w.u8(static_cast<uint8_t>(c.traits[1]));
  w.u8(c.quirk);
  w.u8(c.level);
  w.u16(c.xpMinutes);
  w.u8(c.injured);
  w.u16(c.joinedReach);
}

uint8_t clampStat(const uint8_t v) { return std::max<uint8_t>(1, std::min<uint8_t>(MAX_STAT, v)); }

CrewMember readCrew(Reader& rd) {
  CrewMember c;
  c.nameSeed = rd.u16();
  const uint8_t role = rd.u8();
  const uint8_t rarity = rd.u8();
  c.role = role < static_cast<uint8_t>(Role::Count) ? static_cast<Role>(role) : Role::Engineer;
  c.rarity = rarity < static_cast<uint8_t>(Rarity::Count) ? static_cast<Rarity>(rarity) : Rarity::Common;
  c.skill = clampStat(rd.u8());
  c.grit = clampStat(rd.u8());
  c.luck = clampStat(rd.u8());
  for (auto& trait : c.traits) {
    const uint8_t t = rd.u8();
    trait = t < static_cast<uint8_t>(Trait::Count) ? static_cast<Trait>(t) : Trait::None;
  }
  c.quirk = static_cast<uint8_t>(rd.u8() % QUIRK_COUNT);
  c.level = std::max<uint8_t>(1, std::min<uint8_t>(MAX_CREW_LEVEL, rd.u8()));
  c.xpMinutes = rd.u16();
  c.injured = rd.u8();
  c.joinedReach = rd.u16();
  return c;
}

void writeModule(Writer& w, const Module& m) {
  for (uint8_t k = 0; k < 2; k++) {
    w.u8(static_cast<uint8_t>(m.effect[k]));
    w.u8(static_cast<uint8_t>(m.value[k]));
  }
}

void writeColony(Writer& w, const Colony& c) {
  w.u16(c.nameSeed);
  w.u8(static_cast<uint8_t>(c.specialty));
  w.u8(c.founders);
  w.u16(c.founderStats);
  w.u16(c.foundedReach);
  w.u64(c.population);
}

Colony readColony(Reader& rd) {
  Colony c;
  c.nameSeed = rd.u16();
  const uint8_t specialty = rd.u8();
  c.specialty =
      specialty < static_cast<uint8_t>(Specialty::Count) ? static_cast<Specialty>(specialty) : Specialty::Mining;
  c.founders = rd.u8();
  c.founderStats = rd.u16();
  c.foundedReach = rd.u16();
  c.population = std::min(MAX_POPULATION, std::max<uint64_t>(1, rd.u64()));
  return c;
}

Module readModule(Reader& rd) {
  Module m;
  for (uint8_t k = 0; k < 2; k++) {
    const uint8_t e = rd.u8();
    m.effect[k] = e < static_cast<uint8_t>(ModuleEffect::Count) ? static_cast<ModuleEffect>(e) : ModuleEffect::None;
    m.value[k] = static_cast<int8_t>(rd.u8());
    if (m.effect[k] == ModuleEffect::None) m.value[k] = 0;
  }
  return m;
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
  w.u64(s.bestSessionFuel);
  w.u32(s.longestSessionSeconds);
  w.u32(s.derelictsFound);
  w.u32(s.luckyWatches);
  w.u32(s.upgradesBought);

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

  // Version 3: crew and modules.
  w.u16(s.legacyPermille);
  w.u16(s.wakesSinceLegendary);
  w.u8(s.surveysSinceDerelict);
  w.u8(s.derelictsSinceModule);
  w.u32(s.legendaryWoken);
  w.u32(s.modulesFound);
  w.u32(s.crewRetired);
  const uint8_t crewCount = std::min(s.crewCount, MAX_CREW);
  w.u8(crewCount);
  for (uint8_t i = 0; i < crewCount; i++) writeCrew(w, s.crew[i]);
  const uint8_t moduleCount = std::min(s.moduleCount, MODULE_SLOTS);
  w.u8(moduleCount);
  for (uint8_t i = 0; i < moduleCount; i++) writeModule(w, s.modules[i]);
  w.u16(static_cast<uint16_t>(r.modulePermille));
  w.u8(static_cast<uint8_t>(r.crewEvent));
  writeCrew(w, r.newCrew);
  w.u8(r.crewInjured ? 1 : 0);
  w.u16(r.injuredNameSeed);
  w.u8(r.levelUps);
  w.u8(static_cast<uint8_t>(r.moduleEvent));
  writeModule(w, r.module);
  w.u8(r.retired);

  // Version 4: Fuel variance.
  w.u16(static_cast<uint16_t>(r.variancePermille));

  // Version 5: session merging.
  w.u32(s.sittingEnd);
  w.u32(s.sittingClock);
  w.u32(s.sittingSeconds);
  w.u8(s.sittingCount);
  w.u8(s.sittingScored ? 1 : 0);
  w.u8(r.sittings);

  // Version 6: living colonies.
  const uint8_t colonyCount = std::min(s.colonyCount, MAX_COLONIES);
  w.u8(colonyCount);
  for (uint8_t i = 0; i < colonyCount; i++) writeColony(w, s.colony[i]);
  w.u16(s.sleepersAtReachStart);
  w.u32(s.colonyEvents);
  w.u16(r.colonyPermille);
  w.u32(r.colonyParts);
  w.u32(r.colonyData);
  w.u8(r.colonyRepair);
  w.u8(static_cast<uint8_t>(r.colonyEvent));
  w.u16(r.colonyEventSeed);
  w.u16(r.foundedSeed);
  w.u8(r.foundedJoined ? 1 : 0);

  // Version 7: galaxy eras.
  w.u8(r.bonusStar);
  w.u8(r.newEra ? 1 : 0);

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
  if (version == 0 || version > SAVE_VERSION) return false;

  State s;
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
  if (version >= 2) {
    s.bestSessionFuel = rd.u64();
    s.longestSessionSeconds = rd.u32();
    s.derelictsFound = rd.u32();
    s.luckyWatches = rd.u32();
    s.upgradesBought = rd.u32();
  }

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

  if (version >= 3) {
    s.legacyPermille = rd.u16();
    s.wakesSinceLegendary = rd.u16();
    s.surveysSinceDerelict = rd.u8();
    s.derelictsSinceModule = rd.u8();
    s.legendaryWoken = rd.u32();
    s.modulesFound = rd.u32();
    s.crewRetired = rd.u32();
    s.crewCount = std::min(rd.u8(), MAX_CREW);
    for (uint8_t i = 0; i < s.crewCount; i++) s.crew[i] = readCrew(rd);
    s.moduleCount = std::min(rd.u8(), MODULE_SLOTS);
    for (uint8_t i = 0; i < s.moduleCount; i++) s.modules[i] = readModule(rd);
    r.modulePermille = static_cast<int16_t>(rd.u16());
    const uint8_t crewEvent = rd.u8();
    r.crewEvent =
        crewEvent <= static_cast<uint8_t>(CrewEvent::Cryo) ? static_cast<CrewEvent>(crewEvent) : CrewEvent::None;
    r.newCrew = readCrew(rd);
    r.crewInjured = rd.u8() != 0;
    r.injuredNameSeed = rd.u16();
    r.levelUps = rd.u8();
    const uint8_t moduleEvent = rd.u8();
    r.moduleEvent = moduleEvent <= static_cast<uint8_t>(ModuleEvent::Discarded) ? static_cast<ModuleEvent>(moduleEvent)
                                                                                : ModuleEvent::None;
    r.module = readModule(rd);
    r.retired = rd.u8();
  }
  if (version >= 4) r.variancePermille = static_cast<int16_t>(rd.u16());
  if (version >= 5) {
    s.sittingEnd = rd.u32();
    s.sittingClock = rd.u32();
    s.sittingSeconds = rd.u32();
    s.sittingCount = rd.u8();
    s.sittingScored = rd.u8() != 0;
    r.sittings = std::max<uint8_t>(1, rd.u8());
  }
  if (version >= 6) {
    s.colonyCount = std::min(rd.u8(), MAX_COLONIES);
    for (uint8_t i = 0; i < s.colonyCount; i++) s.colony[i] = readColony(rd);
    s.sleepersAtReachStart = rd.u16();
    s.colonyEvents = rd.u32();
    r.colonyPermille = rd.u16();
    r.colonyParts = rd.u32();
    r.colonyData = rd.u32();
    r.colonyRepair = rd.u8();
    const uint8_t colonyEvent = rd.u8();
    r.colonyEvent = colonyEvent < static_cast<uint8_t>(ColonyEvent::Count) ? static_cast<ColonyEvent>(colonyEvent)
                                                                           : ColonyEvent::None;
    r.colonyEventSeed = rd.u16();
    r.foundedSeed = rd.u16();
    r.foundedJoined = rd.u8() != 0;
  }
  if (version >= 7) {
    r.bonusStar = rd.u8();
    r.newEra = rd.u8() != 0;
  } else if (r.starPermille > 0) {
    r.bonusStar = static_cast<uint8_t>(StarType::GasGiant);  // the only bonus star before eras
  }
  if (r.bonusStar >= static_cast<uint8_t>(StarType::Count)) r.bonusStar = 0;

  const size_t payloadLen = rd.pos;
  const uint32_t checksum = rd.u32();
  if (!rd.ok || checksum != fnv1a(buf, payloadLen)) return false;

  if (s.star >= STARS_PER_REACH) s.star = 0;
  if (s.starType >= static_cast<uint8_t>(StarType::Count)) s.starType = 0;
  if (s.condition > 100) s.condition = 100;
  if (r.lastUpgrade >= SYSTEM_COUNT) r.lastUpgrade = 0;
  if (version < 2) {
    // v1 had no records: seed them from what the save does know.
    s.bestSessionFuel = r.fuel;
    s.longestSessionSeconds = r.seconds;
  }
  if (version < 3) {
    // Before crew were rolled, woken sleepers were only a count. Give the
    // ship a crew to match, up to the cryo bay's capacity.
    const uint16_t toWake = std::min<uint16_t>(s.sleepersWoken, crewCapacity(s));
    for (uint16_t i = 0; i < toWake; i++) s.crew[s.crewCount++] = rollCrew(s);
  }
  if (version < 6) {
    // Before colonies were alive they were only a count. Found one for each
    // past reach (the newest MAX_COLONIES), with a modest population.
    const uint16_t first = s.colonies > MAX_COLONIES ? static_cast<uint16_t>(s.colonies - MAX_COLONIES) : 0;
    for (uint16_t reach = first; reach < s.colonies; reach++) {
      Colony& c = s.colony[s.colonyCount++];
      c.nameSeed = static_cast<uint16_t>(nextRandom(s) & 0xFF);
      c.specialty = static_cast<Specialty>(nextRandom(s) % static_cast<uint8_t>(Specialty::Count));
      c.foundedReach = static_cast<uint16_t>(reach + 1);
      c.population = 2000;
    }
    s.sleepersAtReachStart = s.sleepersWoken;
  }
  s.version = SAVE_VERSION;
  out = s;
  return true;
}

}  // namespace DriftShip
