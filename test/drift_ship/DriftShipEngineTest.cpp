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

// Rebuilds an older save layout from a current one: drops the version 5 block,
// then the version 4 block, then the version 3 crew block, then for version 1 the records block, and
// re-signs the result.
size_t makeLegacySave(const State& state, const uint8_t version, uint8_t* out) {
  uint8_t cur[SAVE_BUFFER_SIZE];
  const size_t len = serialize(state, cur, sizeof(cur));
  constexpr size_t v5Block = 15;
  constexpr size_t v4Block = 2;
  const size_t v3Block = 18 + 1 + 16u * state.crewCount + 1 + 4u * state.moduleCount + 29;
  size_t payload = len - 4 - v5Block;
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
