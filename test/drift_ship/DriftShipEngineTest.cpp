#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "DriftShipEngine.h"

using namespace DriftShip;

namespace {
constexpr uint32_t MIN = 60;

State play(const uint32_t minutes, const int sessions, const Doctrine doctrine = Doctrine::Steady) {
  State s;
  for (int i = 0; i < sessions; i++) applySession(s, minutes * MIN, doctrine);
  return s;
}
}  // namespace

TEST(DriftShipEngine, FuelCurveMatchesDesign) {
  EXPECT_EQ(baseFuelForSeconds(0), 0u);
  EXPECT_EQ(baseFuelForSeconds(30 * MIN), 45u);
  EXPECT_EQ(baseFuelForSeconds(60 * MIN), 120u);
  EXPECT_EQ(baseFuelForSeconds(90 * MIN), 180u);
  EXPECT_EQ(baseFuelForSeconds(120 * MIN), 180u);  // capped at 90 minutes
}

TEST(DriftShipEngine, ShortSessionsAreIgnored) {
  State s;
  EXPECT_FALSE(applySession(s, MIN_SESSION_SECONDS - 1, Doctrine::Steady));
  EXPECT_EQ(s.version, 0u);
  EXPECT_EQ(s.sessions, 0u);
}

TEST(DriftShipEngine, WatchesStackWithSessionLength) {
  State a;
  applySession(a, 5 * MIN, Doctrine::Steady);
  EXPECT_EQ(a.last.watches, WATCH_MAINTENANCE);

  State b;
  applySession(b, 20 * MIN, Doctrine::Steady);
  EXPECT_EQ(b.last.watches, WATCH_MAINTENANCE | WATCH_SURVEY);

  State c;
  applySession(c, 45 * MIN, Doctrine::Steady);
  EXPECT_EQ(c.last.watches, WATCH_MAINTENANCE | WATCH_SURVEY | WATCH_EXPEDITION);

  State d;
  applySession(d, 75 * MIN, Doctrine::Steady);
  EXPECT_EQ(d.last.watches, WATCH_MAINTENANCE | WATCH_SURVEY | WATCH_EXPEDITION | WATCH_LONG);
}

TEST(DriftShipEngine, EveryThirdShortSessionAlsoSurveys) {
  State s;
  applySession(s, 5 * MIN, Doctrine::Steady);
  applySession(s, 5 * MIN, Doctrine::Steady);
  EXPECT_FALSE(s.last.watches & WATCH_SURVEY);
  applySession(s, 5 * MIN, Doctrine::Steady);
  EXPECT_TRUE(s.last.watches & WATCH_SURVEY);
}

TEST(DriftShipEngine, JumpNeedsAPlottedCourse) {
  State s;
  applySession(s, 90 * MIN, Doctrine::Explorer);
  applySession(s, 90 * MIN, Doctrine::Explorer);
  EXPECT_EQ(s.starsVisited, 0u) << "two surveys can't plot a course";
}

TEST(DriftShipEngine, SameHistoryGivesSameResult) {
  const State a = play(37, 50);
  const State b = play(37, 50);
  uint8_t bufA[SAVE_BUFFER_SIZE];
  uint8_t bufB[SAVE_BUFFER_SIZE];
  const size_t lenA = serialize(a, bufA, sizeof(bufA));
  const size_t lenB = serialize(b, bufB, sizeof(bufB));
  ASSERT_GT(lenA, 0u);
  ASSERT_EQ(lenA, lenB);
  EXPECT_EQ(0, memcmp(bufA, bufB, lenA));
}

TEST(DriftShipEngine, SaveRoundTrips) {
  const State a = play(45, 120);
  uint8_t buf[SAVE_BUFFER_SIZE];
  const size_t len = serialize(a, buf, sizeof(buf));
  ASSERT_GT(len, 0u);

  State b;
  ASSERT_TRUE(deserialize(b, buf, len));
  uint8_t again[SAVE_BUFFER_SIZE];
  ASSERT_EQ(serialize(b, again, sizeof(again)), len);
  EXPECT_EQ(0, memcmp(buf, again, len));
  EXPECT_EQ(b.sessions, a.sessions);
  EXPECT_EQ(b.fuel, a.fuel);
  EXPECT_EQ(b.last.fuel, a.last.fuel);
}

TEST(DriftShipEngine, CorruptSaveIsRejected) {
  const State a = play(20, 10);
  uint8_t buf[SAVE_BUFFER_SIZE];
  const size_t len = serialize(a, buf, sizeof(buf));
  buf[20] ^= 0xFF;
  State b;
  EXPECT_FALSE(deserialize(b, buf, len));
  EXPECT_FALSE(deserialize(b, buf, len / 2));
  EXPECT_EQ(b.version, 0u);
}

TEST(DriftShipEngine, ResetStartsAFreshVoyage) {
  State s = play(60, 40);
  ASSERT_GT(s.sessions, 0u);
  resetState(s);
  EXPECT_EQ(s.sessions, 0u);
  EXPECT_EQ(s.fuel, 0u);
  EXPECT_EQ(s.starsVisited, 0u);
}

// The design's sanity table: every reading pattern makes progress, and longer
// reads progress faster.
TEST(DriftShipEngine, EveryReadingPatternProgresses) {
  const int sessions = 400;  // roughly a year of daily reading
  const State five = play(5, sessions);
  const State twenty = play(20, sessions);
  const State fortyFive = play(45, sessions);
  const State ninety = play(90, sessions);

  EXPECT_GT(five.starsVisited, 0u);
  EXPECT_GT(twenty.starsVisited, five.starsVisited);
  EXPECT_GE(fortyFive.starsVisited, twenty.starsVisited);
  EXPECT_GE(ninety.starsVisited, fortyFive.starsVisited);
  EXPECT_GT(ninety.lifetimeFuel, fortyFive.lifetimeFuel);

  for (const State* s : {&five, &twenty, &fortyFive, &ninety}) {
    EXPECT_LE(s->condition, 100u);
    uint32_t totalLevels = 0;
    for (const uint8_t l : s->levels) totalLevels += l;
    EXPECT_GT(totalLevels, 0u) << "the Steward should buy upgrades";
  }
}

TEST(DriftShipEngine, DoctrinesShapeUpgrades) {
  const State builder = play(30, 200, Doctrine::Builder);
  const State explorer = play(30, 200, Doctrine::Explorer);
  EXPECT_GT(builder.levels[static_cast<int>(System::Reactor)], explorer.levels[static_cast<int>(System::Reactor)]);
  EXPECT_GT(explorer.levels[static_cast<int>(System::Sensors)], builder.levels[static_cast<int>(System::Sensors)]);
}

TEST(DriftShipEngine, DISABLED_PrintPacing) {
  for (const uint32_t m : {5u, 20u, 30u, 45u, 90u}) {
    for (const int n : {30, 100, 400}) {
      const State s = play(m, n);
      printf(
          "%2u min x %3d: stars=%u reach=%u fuel=%llu life=%llu parts=%llu data=%llu cond=%u crew=%u "
          "lv=%u,%u,%u,%u,%u,%u\n",
          m, n, s.starsVisited, s.reach, (unsigned long long)s.fuel, (unsigned long long)s.lifetimeFuel,
          (unsigned long long)s.parts, (unsigned long long)s.data, s.condition, crewAwake(s), s.levels[0], s.levels[1],
          s.levels[2], s.levels[3], s.levels[4], s.levels[5]);
    }
  }
}

TEST(DriftShipEngine, CompactNumbers) {
  char buf[16];
  const auto fmt = [&](uint64_t v) {
    formatCompactNumber(v, buf, sizeof(buf));
    return std::string(buf);
  };
  EXPECT_EQ(fmt(0), "0");
  EXPECT_EQ(fmt(9999), "9999");
  EXPECT_EQ(fmt(12345), "12.3k");
  EXPECT_EQ(fmt(456789), "456k");
  EXPECT_EQ(fmt(7890000), "7.89M");
  EXPECT_EQ(fmt(1200000000ULL), "1.20B");
  EXPECT_EQ(fmt(3400000000000000ULL), "3.40aa");
  EXPECT_EQ(fmt(UINT64_MAX), "18.4ab");
}

namespace {
uint32_t testFnv1a(const uint8_t* data, const size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}
}  // namespace

// Rebuilds an older save layout from a current one: drops the version 7 block,
// then the version 6 block,
// then the version 5 block,
// then the version 4 block, then the version 3 crew block, then for version 1 the records block, and
// re-signs the result.
size_t makeLegacySave(const State& state, const uint8_t version, uint8_t* out) {
  uint8_t cur[SAVE_BUFFER_SIZE];
  const size_t len = serialize(state, cur, sizeof(cur));
  const size_t v6Block = 1 + 16u * state.colonyCount + 23;
  constexpr size_t v5Block = 15;
  constexpr size_t v4Block = 2;
  const size_t v3Block = 18 + 1 + 16u * state.crewCount + 1 + 4u * state.moduleCount + 29;
  constexpr size_t v7Block = 2;
  size_t payload = len - 4 - v7Block;
  if (version <= 5) payload -= v6Block;
  if (version <= 4) payload -= v5Block;
  if (version <= 3) payload -= v4Block;
  if (version <= 2) payload -= v3Block;
  memcpy(out, cur, payload);
  if (version == 1) {
    constexpr size_t recordsAt = 83;
    constexpr size_t recordsLen = 24;
    memmove(out + recordsAt, out + recordsAt + recordsLen, payload - recordsAt - recordsLen);
    payload -= recordsLen;
  }
  out[4] = version;
  const uint32_t sum = testFnv1a(out, payload);
  for (int b = 0; b < 4; b++) out[payload + b] = static_cast<uint8_t>(sum >> (8 * b));
  return payload + 4;
}

TEST(DriftShipEngine, VersionOneSavesStillLoad) {
  const State a = play(45, 30);
  uint8_t v1[SAVE_BUFFER_SIZE];
  const size_t len = makeLegacySave(a, 1, v1);

  State b;
  ASSERT_TRUE(deserialize(b, v1, len));
  EXPECT_EQ(b.version, SAVE_VERSION);
  EXPECT_EQ(b.sessions, a.sessions);
  EXPECT_EQ(b.fuel, a.fuel);
  EXPECT_EQ(b.bestSessionFuel, a.last.fuel);
  EXPECT_EQ(b.last.fuel, a.last.fuel);
}

TEST(DriftShipEngine, VersionTwoSavesGetACrew) {
  const State a = play(45, 60);
  ASSERT_GT(a.sleepersWoken, 0u);
  uint8_t v2[SAVE_BUFFER_SIZE];
  const size_t len = makeLegacySave(a, 2, v2);

  State b;
  ASSERT_TRUE(deserialize(b, v2, len));
  EXPECT_EQ(b.version, SAVE_VERSION);
  EXPECT_EQ(b.crewCount, std::min<uint16_t>(a.sleepersWoken, crewCapacity(b)));
  EXPECT_EQ(b.longestSessionSeconds, a.longestSessionSeconds);
  EXPECT_EQ(b.moduleCount, 0u);
}

TEST(DriftShipEngine, RecordsTrackTheBest) {
  State s;
  applySession(s, 20 * MIN, Doctrine::Steady);
  applySession(s, 80 * MIN, Doctrine::Steady);
  applySession(s, 10 * MIN, Doctrine::Steady);
  EXPECT_EQ(s.longestSessionSeconds, 80u * MIN);
  EXPECT_GE(s.bestSessionFuel, baseFuelForSeconds(80 * MIN) * 85 / 100);  // allows the Fuel variance
}

TEST(DriftShipEngine, LogKeepsTheNewestEntries) {
  State s;
  SessionLog log;
  for (int i = 0; i < LOG_CAPACITY + 5; i++) {
    applySession(s, 15 * MIN, Doctrine::Steady);
    pushLogEntry(log, makeLogEntry(s));
  }
  ASSERT_EQ(log.count, LOG_CAPACITY);
  EXPECT_EQ(log.entries[LOG_CAPACITY - 1].sessionNumber, s.sessions);
  EXPECT_EQ(log.entries[0].sessionNumber, s.sessions - LOG_CAPACITY + 1);

  uint8_t buf[LOG_ENTRY_SIZE];
  serializeLogEntry(log.entries[3], buf);
  const LogEntry back = deserializeLogEntry(buf);
  EXPECT_EQ(back.sessionNumber, log.entries[3].sessionNumber);
  EXPECT_EQ(back.fuel, log.entries[3].fuel);
  EXPECT_EQ(back.minutes, 15u);
}

TEST(DriftShipEngine, CrewStaysWithinCapacity) {
  for (const uint32_t m : {20u, 45u, 90u}) {
    const State s = play(m, 300);
    EXPECT_LE(s.crewCount, crewCapacity(s));
    EXPECT_LE(s.moduleCount, MODULE_SLOTS);
    for (uint8_t i = 0; i < s.crewCount; i++) {
      const CrewMember& c = s.crew[i];
      EXPECT_GE(c.skill, 1u);
      EXPECT_LE(c.skill, MAX_STAT);
      EXPECT_LE(c.level, MAX_CREW_LEVEL);
      EXPECT_NE(c.traits[0], Trait::None);
    }
  }
}

TEST(DriftShipEngine, LegendaryIsGuaranteedBy150Wakes) {
  State s;
  s.wakesSinceLegendary = 0;
  EXPECT_EQ(legendaryChancePercent(s), 1u);
  s.wakesSinceLegendary = 149;
  EXPECT_EQ(legendaryChancePercent(s), 100u);
}

TEST(DriftShipEngine, CrewAndModulesRoundTrip) {
  const State a = play(60, 250);
  ASSERT_GT(a.crewCount, 0u);
  uint8_t buf[SAVE_BUFFER_SIZE];
  const size_t len = serialize(a, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  State b;
  ASSERT_TRUE(deserialize(b, buf, len));
  ASSERT_EQ(b.crewCount, a.crewCount);
  ASSERT_EQ(b.moduleCount, a.moduleCount);
  for (uint8_t i = 0; i < a.crewCount; i++) {
    EXPECT_EQ(b.crew[i].nameSeed, a.crew[i].nameSeed);
    EXPECT_EQ(b.crew[i].role, a.crew[i].role);
    EXPECT_EQ(b.crew[i].traits[1], a.crew[i].traits[1]);
    EXPECT_EQ(b.crew[i].xpMinutes, a.crew[i].xpMinutes);
  }
  for (uint8_t i = 0; i < a.moduleCount; i++) {
    EXPECT_EQ(moduleScore(b.modules[i]), moduleScore(a.modules[i]));
  }
}

TEST(DriftShipEngine, FullRosterFitsTheSaveBuffer) {
  State s = play(30, 5);
  s.crewCount = MAX_CREW;
  s.moduleCount = MODULE_SLOTS;
  uint8_t buf[SAVE_BUFFER_SIZE];
  EXPECT_GT(serialize(s, buf, sizeof(buf)), 0u);
}

TEST(DriftShipEngine, DISABLED_PrintCrew) {
  const State s = play(45, 400);
  char name[32];
  for (uint8_t i = 0; i < s.crewCount; i++) {
    const CrewMember& c = s.crew[i];
    crewName(c.nameSeed, name, sizeof(name));
    printf("%-18s role=%d rar=%d S%u G%u L%u lv%u t=%d,%d inj=%u\n", name, (int)c.role, (int)c.rarity, c.skill, c.grit,
           c.luck, c.level, (int)c.traits[0], (int)c.traits[1], c.injured);
  }
  printf("modules=%u legacy=%u retired=%u legendary=%u found=%u\n", s.moduleCount, s.legacyPermille, s.crewRetired,
         s.legendaryWoken, s.modulesFound);
}

TEST(DriftShipEngine, VersionThreeSavesStillLoad) {
  const State a = play(45, 80);
  uint8_t v3[SAVE_BUFFER_SIZE];
  const size_t len = makeLegacySave(a, 3, v3);
  State b;
  ASSERT_TRUE(deserialize(b, v3, len));
  EXPECT_EQ(b.version, SAVE_VERSION);
  EXPECT_EQ(b.crewCount, a.crewCount);
  EXPECT_EQ(b.last.variancePermille, 0);
}

TEST(DriftShipEngine, VersionFourSavesStillLoad) {
  const State a = play(45, 40);
  uint8_t v4[SAVE_BUFFER_SIZE];
  const size_t len = makeLegacySave(a, 4, v4);
  State b;
  ASSERT_TRUE(deserialize(b, v4, len));
  EXPECT_EQ(b.version, SAVE_VERSION);
  EXPECT_EQ(b.fuel, a.fuel);
  EXPECT_EQ(b.last.variancePermille, a.last.variancePermille);
  EXPECT_EQ(b.sittingCount, 0);
  EXPECT_EQ(b.sittingClock, 0u);
  EXPECT_EQ(b.last.sittings, 1);
}

TEST(DriftShipEngine, SameReadingTimeVaries) {
  State s;
  bool sawDifferentFuel = false;
  uint64_t previous = 0;
  for (int i = 0; i < 20; i++) {
    applySession(s, 30 * MIN, Doctrine::Steady);
    EXPECT_GE(s.last.variancePermille, -150);
    EXPECT_LE(s.last.variancePermille, 150);
    if (i > 0 && s.last.fuel != previous) sawDifferentFuel = true;
    previous = s.last.fuel;
  }
  EXPECT_TRUE(sawDifferentFuel);
}

// --- Session merging ---

namespace {
constexpr uint32_t CLOCK = 7;

// Mimics DriftShipStore::onReadingSession with the snapshot file in memory.
struct Device {
  State state;
  State snapshot;
  bool hasSnapshot = false;
  SessionLog log;

  SittingOutcome sit(const uint32_t activeSeconds, const uint32_t endTime, const uint32_t clockId = CLOCK) {
    State scratch;
    const bool loaded = sittingNeedsSnapshot(state, activeSeconds, endTime, clockId) && hasSnapshot;
    if (loaded) scratch = snapshot;
    const SittingOutcome out = applySitting(state, scratch, loaded, activeSeconds, endTime, clockId, Doctrine::Steady);
    if (out.writeSnapshot) {
      snapshot = scratch;
      hasSnapshot = true;
    }
    if (out.scored) upsertLogEntry(log, makeLogEntry(state));
    return out;
  }
};
}  // namespace

TEST(DriftShipMerge, MergingInsideTheCapNeverEarnsLess) {
  for (uint32_t a = 2; a <= 88; a++) {
    for (uint32_t b = 2; a + b <= 90; b++) {
      EXPECT_GE(baseFuelForSeconds((a + b) * MIN), baseFuelForSeconds(a * MIN) + baseFuelForSeconds(b * MIN))
          << a << "+" << b;
    }
  }
}

TEST(DriftShipMerge, CloseSittingsReplayAsOneSession) {
  Device d;
  for (int i = 0; i < 20; i++) d.sit(25 * MIN, 100000u + i * 20000u);  // separate sessions
  const State before = d.state;

  // 20 minutes, a 3-minute break, then 25 more minutes.
  uint32_t t = 600000;
  d.sit(20 * MIN, t);
  t += 3 * MIN + 25 * MIN;
  const SittingOutcome out = d.sit(25 * MIN, t);
  EXPECT_TRUE(out.scored);
  EXPECT_FALSE(out.writeSnapshot);

  // Exactly what one 45-minute session from the same start would have earned.
  State single = before;
  applySession(single, 45 * MIN, Doctrine::Steady);
  EXPECT_EQ(d.state.sessions, single.sessions);
  EXPECT_EQ(d.state.creditedSeconds, single.creditedSeconds);
  EXPECT_EQ(d.state.fuel, single.fuel);
  EXPECT_EQ(d.state.parts, single.parts);
  EXPECT_EQ(d.state.rng, single.rng);
  EXPECT_EQ(d.state.last.fuel, single.last.fuel);
  EXPECT_EQ(d.state.last.seconds, 45 * MIN);
  EXPECT_EQ(d.state.last.sittings, 2);
  EXPECT_TRUE(d.state.last.watches & WATCH_EXPEDITION);

  // The log shows one session, not two.
  EXPECT_EQ(d.log.entries[d.log.count - 1].sessionNumber, 21u);
  EXPECT_EQ(d.log.entries[d.log.count - 1].minutes, 45);
  EXPECT_EQ(d.log.entries[d.log.count - 2].sessionNumber, 20u);
}

TEST(DriftShipMerge, ChainsOfSittingsKeepMerging) {
  Device d;
  d.sit(20 * MIN, 50000);
  uint32_t t = 50000;
  for (int i = 0; i < 3; i++) {
    t += 2 * MIN + 15 * MIN;
    d.sit(15 * MIN, t);
  }
  EXPECT_EQ(d.state.sessions, 1u);
  EXPECT_EQ(d.state.last.seconds, 65 * MIN);
  EXPECT_EQ(d.state.last.sittings, 4);
  EXPECT_EQ(d.log.count, 1);
}

TEST(DriftShipMerge, ShortSittingsAddUpToASession) {
  Device d;
  d.sit(20 * MIN, 1000);  // start the voyage
  const uint32_t sessions = d.state.sessions;
  d.sit(20 * MIN, 1000 + 60 * MIN);  // a separate session, far away
  EXPECT_EQ(d.state.sessions, sessions + 1);

  // Three 50-second sittings a minute apart: too short alone, a session together.
  uint32_t t = 100000;
  EXPECT_FALSE(d.sit(50, t).scored);
  t += 60 + 50;
  EXPECT_FALSE(d.sit(50, t).scored);
  t += 60 + 50;
  const SittingOutcome out = d.sit(50, t);
  EXPECT_TRUE(out.scored);
  EXPECT_EQ(d.state.sessions, sessions + 2);
  EXPECT_EQ(d.state.last.seconds, 150u);
  EXPECT_EQ(d.state.last.sittings, 3);
}

TEST(DriftShipMerge, GapsClocksAndTheCapEndAChain) {
  Device d;
  d.sit(20 * MIN, 10000);
  ASSERT_EQ(d.state.sessions, 1u);

  // 5 minutes or more apart: a new session.
  EXPECT_FALSE(continuesSitting(d.state, 10 * MIN, 10000 + 5 * MIN + 10 * MIN, CLOCK));
  EXPECT_TRUE(continuesSitting(d.state, 10 * MIN, 10000 + 4 * MIN + 10 * MIN, CLOCK));
  // A different clock, no clock, or a clock that went backwards.
  EXPECT_FALSE(continuesSitting(d.state, 10 * MIN, 10000 + 11 * MIN, CLOCK + 1));
  EXPECT_FALSE(continuesSitting(d.state, 10 * MIN, 10000 + 11 * MIN, 0));
  EXPECT_FALSE(continuesSitting(d.state, 10 * MIN, 9000, CLOCK));
  // Past the 90-minute cap together: a new session.
  EXPECT_TRUE(continuesSitting(d.state, 70 * MIN, 10000 + 71 * MIN, CLOCK));
  EXPECT_FALSE(continuesSitting(d.state, 71 * MIN, 10000 + 72 * MIN, CLOCK));

  d.sit(71 * MIN, 10000 + 72 * MIN);
  EXPECT_EQ(d.state.sessions, 2u);
  EXPECT_EQ(d.state.last.sittings, 1);
}

TEST(DriftShipMerge, MissingSnapshotScoresTheSittingAlone) {
  Device d;
  d.sit(20 * MIN, 10000);
  d.hasSnapshot = false;  // e.g. the snapshot file was lost
  d.sit(20 * MIN, 10000 + 22 * MIN);
  EXPECT_EQ(d.state.sessions, 2u);
  EXPECT_EQ(d.state.last.seconds, 20 * MIN);
}

TEST(DriftShipMerge, NoClockNeverMerges) {
  Device d;
  d.sit(20 * MIN, 0, 0);
  const SittingOutcome out = d.sit(20 * MIN, 0, 0);
  EXPECT_FALSE(out.writeSnapshot);
  EXPECT_EQ(d.state.sessions, 2u);
}

TEST(DriftShipMerge, LogUpsertReplacesTheSameSession) {
  SessionLog log;
  LogEntry e;
  e.sessionNumber = 4;
  e.minutes = 10;
  upsertLogEntry(log, e);
  e.minutes = 25;
  upsertLogEntry(log, e);
  ASSERT_EQ(log.count, 1);
  EXPECT_EQ(log.entries[0].minutes, 25);
  e.sessionNumber = 5;
  upsertLogEntry(log, e);
  EXPECT_EQ(log.count, 2);
}

TEST(DriftShipMerge, SittingStateRoundTrips) {
  Device d;
  d.sit(20 * MIN, 10000);
  d.sit(20 * MIN, 10000 + 22 * MIN);
  uint8_t buf[SAVE_BUFFER_SIZE];
  const size_t len = serialize(d.state, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  State loaded;
  ASSERT_TRUE(deserialize(loaded, buf, len));
  EXPECT_EQ(loaded.sittingEnd, d.state.sittingEnd);
  EXPECT_EQ(loaded.sittingClock, CLOCK);
  EXPECT_EQ(loaded.sittingSeconds, 40 * MIN);
  EXPECT_EQ(loaded.sittingCount, 2);
  EXPECT_TRUE(loaded.sittingScored);
  EXPECT_EQ(loaded.last.sittings, 2);
}

// --- Living colonies ---

namespace {
// Plays until the ship completes a reach (or gives up).
State playToReach(const uint16_t reach, const uint32_t minutes = 60) {
  State s;
  for (int i = 0; i < 20000 && s.reach < reach; i++) applySession(s, minutes * MIN, Doctrine::Steady);
  return s;
}
}  // namespace

TEST(DriftShipColonies, EveryReachFoundsAColony) {
  const State s = playToReach(2);
  ASSERT_EQ(s.reach, 2u);
  EXPECT_EQ(s.colonyCount, 2u);
  EXPECT_EQ(s.colonies, 2u);
  EXPECT_EQ(s.colony[0].foundedReach, 1u);
  EXPECT_EQ(s.colony[1].foundedReach, 2u);
  EXPECT_GE(s.colony[1].population, 1000u);
  char name[32];
  colonyName(s.colony[0].nameSeed, name, sizeof(name));
  EXPECT_GT(strlen(name), 3u);
}

TEST(DriftShipColonies, ColoniesGrowByCompoundInterest) {
  State s = playToReach(1);
  ASSERT_EQ(s.colonyCount, 1u);
  s.colony[0].founderStats = 0;
  s.colony[0].population = 100000;
  // About 1% per 30-minute session.
  applySession(s, 30 * MIN, Doctrine::Steady);
  const uint64_t after = s.colony[0].population;
  if (s.last.colonyEvent == ColonyEvent::None) {
    EXPECT_EQ(after, 101000u);
  }
  // A year of daily 30-minute reads compounds well past linear growth.
  s.colony[0].population = 2000;
  for (int i = 0; i < 365; i++) applySession(s, 30 * MIN, Doctrine::Steady);
  EXPECT_GT(s.colony[0].population, 2000u * 20);
  EXPECT_LE(s.colony[0].population, MAX_POPULATION);
}

TEST(DriftShipColonies, StrongFoundersGrowFaster) {
  Colony plain;
  plain.population = 10000;
  Colony founded = plain;
  founded.founderStats = 30;
  EXPECT_EQ(colonyGrowthPerMille(plain, 30), 10u);
  EXPECT_EQ(colonyGrowthPerMille(founded, 30), 20u);
  EXPECT_EQ(colonyGrowthPerMille(plain, 500), colonyGrowthPerMille(plain, 90));  // capped like the session
}

TEST(DriftShipColonies, ColoniesSendFuelAndGoods) {
  Colony c;
  c.population = 1240000;  // 7 digits
  c.specialty = Specialty::Mining;
  EXPECT_EQ(colonyFuelPermille(c), 70u);
  EXPECT_EQ(colonyGoods(c), 7u);
  c.specialty = Specialty::Science;
  EXPECT_EQ(colonyGoods(c), 4u);
  c.specialty = Specialty::Shipyard;
  EXPECT_EQ(colonyFuelPermille(c), 140u);
  EXPECT_EQ(colonyGoods(c), 3u);

  State s = playToReach(1);
  applySession(s, 45 * MIN, Doctrine::Steady);
  EXPECT_EQ(s.last.colonyPermille, colonyFuelPermille(s.colony[0]));
}

TEST(DriftShipColonies, ColonyReportsComeInEveryKind) {
  State s = playToReach(1);
  bool seen[static_cast<uint8_t>(ColonyEvent::Count)] = {};
  uint32_t reports = 0;
  for (int i = 0; i < 2000; i++) {
    applySession(s, 20 * MIN, Doctrine::Steady);
    seen[static_cast<uint8_t>(s.last.colonyEvent)] = true;
    if (s.last.colonyEvent != ColonyEvent::None) reports++;
  }
  for (uint8_t e = 1; e < static_cast<uint8_t>(ColonyEvent::Count); e++) EXPECT_TRUE(seen[e]) << int(e);
  // About 12% of sessions.
  EXPECT_GT(reports, 160u);
  EXPECT_LT(reports, 320u);
  EXPECT_EQ(s.colonyEvents >= reports, true);
}

TEST(DriftShipColonies, AFullNetworkFoldsNewSettlersIn) {
  State s = playToReach(1);
  s.colonyCount = MAX_COLONIES;
  for (uint8_t i = 0; i < MAX_COLONIES; i++) {
    s.colony[i].population = 50000u + i;
    s.colony[i].nameSeed = i;
  }
  s.colony[5].population = 10;
  // Force the end of the reach on the next session.
  s.star = STARS_PER_REACH - 1;
  s.surveysHere = SURVEYS_TO_PLOT_COURSE;
  s.condition = 100;
  s.jumpCharge = jumpChargeNeeded(s) * 4;
  applySession(s, 20 * MIN, Doctrine::Steady);
  ASSERT_TRUE(s.last.reachCompleted);
  EXPECT_EQ(s.colonyCount, MAX_COLONIES);
  EXPECT_TRUE(s.last.foundedJoined);
  EXPECT_EQ(s.last.foundedSeed, 5u);
  EXPECT_GE(s.colony[5].population, 1000u);
}

TEST(DriftShipColonies, ColoniesRoundTrip) {
  const State a = playToReach(2);
  uint8_t buf[SAVE_BUFFER_SIZE];
  const size_t len = serialize(a, buf, sizeof(buf));
  ASSERT_GT(len, 0u);
  State b;
  ASSERT_TRUE(deserialize(b, buf, len));
  ASSERT_EQ(b.colonyCount, a.colonyCount);
  for (uint8_t i = 0; i < a.colonyCount; i++) {
    EXPECT_EQ(b.colony[i].nameSeed, a.colony[i].nameSeed);
    EXPECT_EQ(b.colony[i].specialty, a.colony[i].specialty);
    EXPECT_EQ(b.colony[i].population, a.colony[i].population);
    EXPECT_EQ(b.colony[i].founderStats, a.colony[i].founderStats);
  }
  EXPECT_EQ(b.sleepersAtReachStart, a.sleepersAtReachStart);
  EXPECT_EQ(b.last.colonyPermille, a.last.colonyPermille);
}

TEST(DriftShipColonies, FullNetworkFitsTheSaveBuffer) {
  State s = play(30, 5);
  s.crewCount = MAX_CREW;
  s.moduleCount = MODULE_SLOTS;
  s.colonyCount = MAX_COLONIES;
  uint8_t buf[SAVE_BUFFER_SIZE];
  EXPECT_GT(serialize(s, buf, sizeof(buf)), 0u);
}

TEST(DriftShipColonies, VersionFiveSavesGetLivingColonies) {
  State a = playToReach(2);
  // A v5 save only knew the colony count.
  a.colonyCount = 0;
  uint8_t v5[SAVE_BUFFER_SIZE];
  const size_t len = makeLegacySave(a, 5, v5);
  State b;
  ASSERT_TRUE(deserialize(b, v5, len));
  EXPECT_EQ(b.version, SAVE_VERSION);
  EXPECT_EQ(b.colonyCount, 2u);
  EXPECT_EQ(b.colony[1].foundedReach, 2u);
  EXPECT_EQ(b.colony[0].population, 2000u);
  EXPECT_EQ(b.sleepersAtReachStart, b.sleepersWoken);
}

// --- Galaxy eras ---

TEST(DriftShipEras, EraChangesEveryFiveReaches) {
  EXPECT_EQ(eraForReach(0), Era::OuterRim);
  EXPECT_EQ(eraForReach(4), Era::OuterRim);
  EXPECT_EQ(eraForReach(5), Era::RuinBelt);
  EXPECT_EQ(eraForReach(29), Era::FarGalaxy);
  EXPECT_EQ(eraForReach(30), Era::OuterRim);  // the cycle starts again
  EXPECT_EQ(eraNumber(0), 1u);
  EXPECT_EQ(eraNumber(30), 7u);
}

TEST(DriftShipEras, EveryEraHasAFullStarTable) {
  for (uint8_t e = 0; e < static_cast<uint8_t>(Era::Count); e++) {
    uint32_t total = 0;
    for (uint8_t t = 0; t < static_cast<uint8_t>(StarType::Count); t++) {
      total += starTypeChance(static_cast<Era>(e), static_cast<StarType>(t));
    }
    EXPECT_EQ(total, 100u) << int(e);
  }
  // The Outer rim only has the original six star types.
  for (uint8_t t = static_cast<uint8_t>(StarType::Ruins); t < static_cast<uint8_t>(StarType::Count); t++) {
    EXPECT_EQ(starTypeChance(Era::OuterRim, static_cast<StarType>(t)), 0u);
  }
}

TEST(DriftShipEras, LaterErasBringNewStars) {
  State s;
  applySession(s, 20 * MIN, Doctrine::Steady);
  s.reach = 5;  // the Ruin belt
  uint32_t ruins = 0;
  for (int i = 0; i < 300; i++) {
    s.surveysHere = SURVEYS_TO_PLOT_COURSE;
    s.condition = 100;
    s.star = 0;
    s.jumpCharge = jumpChargeNeeded(s) + 1;
    applySession(s, 20 * MIN, Doctrine::Steady);
    if (s.starType == static_cast<uint8_t>(StarType::Ruins)) ruins++;
    s.reach = 5;
  }
  EXPECT_GT(ruins, 50u);
}

TEST(DriftShipEras, CrossingIntoANewEraIsReported) {
  State s;
  applySession(s, 20 * MIN, Doctrine::Steady);
  s.reach = REACHES_PER_ERA - 1;
  s.star = STARS_PER_REACH - 1;
  s.surveysHere = SURVEYS_TO_PLOT_COURSE;
  s.condition = 100;
  s.jumpCharge = jumpChargeNeeded(s) * 4;
  applySession(s, 20 * MIN, Doctrine::Steady);
  ASSERT_TRUE(s.last.reachCompleted);
  EXPECT_TRUE(s.last.newEra);
  EXPECT_EQ(eraForReach(s.reach), Era::RuinBelt);
}

TEST(DriftShipEras, BlackHolesSlingTheShipOut) {
  State s;
  applySession(s, 20 * MIN, Doctrine::Steady);
  s.freeJump = false;
  s.starType = static_cast<uint8_t>(StarType::RedDwarf);
  const uint64_t normal = jumpChargeNeeded(s);
  s.starType = static_cast<uint8_t>(StarType::BlackHole);
  EXPECT_LT(jumpChargeNeeded(s), normal);
}

TEST(DriftShipEras, TheVoidPaysFuelForQuietReading) {
  State s;
  applySession(s, 20 * MIN, Doctrine::Steady);
  s.starType = static_cast<uint8_t>(StarType::Void);
  applySession(s, 20 * MIN, Doctrine::Steady);
  EXPECT_EQ(s.last.starPermille, 250u);
  EXPECT_EQ(s.last.bonusStar, static_cast<uint8_t>(StarType::Void));
}

TEST(DriftShipEras, VersionSixSavesStillLoad) {
  State a = play(45, 30);
  a.last.starPermille = 300;
  uint8_t v6[SAVE_BUFFER_SIZE];
  const size_t len = makeLegacySave(a, 6, v6);
  State b;
  ASSERT_TRUE(deserialize(b, v6, len));
  EXPECT_EQ(b.version, SAVE_VERSION);
  EXPECT_EQ(b.last.bonusStar, static_cast<uint8_t>(StarType::GasGiant));
  EXPECT_FALSE(b.last.newEra);
}
