#include <gtest/gtest.h>

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
