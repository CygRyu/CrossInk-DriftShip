#include "DriftShipStoryText.h"

#include <I18n.h>

namespace DriftShip {
namespace {

// The built-in event pack. Ids live in flash; text comes from the translation
// files, so the pack is translated like any other UI text.
static const StrId PLACES[] = {
    StrId::STR_DRIFT_SHIP_PLACE_0, StrId::STR_DRIFT_SHIP_PLACE_1,  StrId::STR_DRIFT_SHIP_PLACE_2,
    StrId::STR_DRIFT_SHIP_PLACE_3, StrId::STR_DRIFT_SHIP_PLACE_4,  StrId::STR_DRIFT_SHIP_PLACE_5,
    StrId::STR_DRIFT_SHIP_PLACE_6, StrId::STR_DRIFT_SHIP_PLACE_7,  StrId::STR_DRIFT_SHIP_PLACE_8,
    StrId::STR_DRIFT_SHIP_PLACE_9, StrId::STR_DRIFT_SHIP_PLACE_10, StrId::STR_DRIFT_SHIP_PLACE_11,
};
static const StrId PLACES_OUTER_RIM[] = {
    StrId::STR_DRIFT_SHIP_PLACE_OUTER_RIM_0,
    StrId::STR_DRIFT_SHIP_PLACE_OUTER_RIM_1,
    StrId::STR_DRIFT_SHIP_PLACE_OUTER_RIM_2,
    StrId::STR_DRIFT_SHIP_PLACE_OUTER_RIM_3,
};
static const StrId PLACES_RUIN_BELT[] = {
    StrId::STR_DRIFT_SHIP_PLACE_RUIN_BELT_0,
    StrId::STR_DRIFT_SHIP_PLACE_RUIN_BELT_1,
    StrId::STR_DRIFT_SHIP_PLACE_RUIN_BELT_2,
    StrId::STR_DRIFT_SHIP_PLACE_RUIN_BELT_3,
};
static const StrId PLACES_DENSE_CORE[] = {
    StrId::STR_DRIFT_SHIP_PLACE_DENSE_CORE_0,
    StrId::STR_DRIFT_SHIP_PLACE_DENSE_CORE_1,
    StrId::STR_DRIFT_SHIP_PLACE_DENSE_CORE_2,
    StrId::STR_DRIFT_SHIP_PLACE_DENSE_CORE_3,
};
static const StrId PLACES_DEEP_VOID[] = {
    StrId::STR_DRIFT_SHIP_PLACE_DEEP_VOID_0,
    StrId::STR_DRIFT_SHIP_PLACE_DEEP_VOID_1,
    StrId::STR_DRIFT_SHIP_PLACE_DEEP_VOID_2,
    StrId::STR_DRIFT_SHIP_PLACE_DEEP_VOID_3,
};
static const StrId PLACES_SHATTERED_ARM[] = {
    StrId::STR_DRIFT_SHIP_PLACE_SHATTERED_ARM_0,
    StrId::STR_DRIFT_SHIP_PLACE_SHATTERED_ARM_1,
    StrId::STR_DRIFT_SHIP_PLACE_SHATTERED_ARM_2,
    StrId::STR_DRIFT_SHIP_PLACE_SHATTERED_ARM_3,
};
static const StrId PLACES_FAR_GALAXY[] = {
    StrId::STR_DRIFT_SHIP_PLACE_FAR_GALAXY_0,
    StrId::STR_DRIFT_SHIP_PLACE_FAR_GALAXY_1,
    StrId::STR_DRIFT_SHIP_PLACE_FAR_GALAXY_2,
    StrId::STR_DRIFT_SHIP_PLACE_FAR_GALAXY_3,
};
static const StrId* const ERA_PLACES[static_cast<uint8_t>(Era::Count)] = {
    PLACES_OUTER_RIM, PLACES_RUIN_BELT, PLACES_DENSE_CORE, PLACES_DEEP_VOID, PLACES_SHATTERED_ARM, PLACES_FAR_GALAXY};
constexpr uint16_t ERA_PLACE_COUNT = 4;
static const StrId FINDINGS[] = {
    StrId::STR_DRIFT_SHIP_FINDING_0,  StrId::STR_DRIFT_SHIP_FINDING_1,  StrId::STR_DRIFT_SHIP_FINDING_2,
    StrId::STR_DRIFT_SHIP_FINDING_3,  StrId::STR_DRIFT_SHIP_FINDING_4,  StrId::STR_DRIFT_SHIP_FINDING_5,
    StrId::STR_DRIFT_SHIP_FINDING_6,  StrId::STR_DRIFT_SHIP_FINDING_7,  StrId::STR_DRIFT_SHIP_FINDING_8,
    StrId::STR_DRIFT_SHIP_FINDING_9,  StrId::STR_DRIFT_SHIP_FINDING_10, StrId::STR_DRIFT_SHIP_FINDING_11,
    StrId::STR_DRIFT_SHIP_FINDING_12, StrId::STR_DRIFT_SHIP_FINDING_13, StrId::STR_DRIFT_SHIP_FINDING_14,
    StrId::STR_DRIFT_SHIP_FINDING_15, StrId::STR_DRIFT_SHIP_FINDING_16, StrId::STR_DRIFT_SHIP_FINDING_17,
    StrId::STR_DRIFT_SHIP_FINDING_18, StrId::STR_DRIFT_SHIP_FINDING_19,
};
static const StrId GOOD[] = {
    StrId::STR_DRIFT_SHIP_GOOD_0, StrId::STR_DRIFT_SHIP_GOOD_1, StrId::STR_DRIFT_SHIP_GOOD_2,
    StrId::STR_DRIFT_SHIP_GOOD_3, StrId::STR_DRIFT_SHIP_GOOD_4, StrId::STR_DRIFT_SHIP_GOOD_5,
};
static const StrId BAD[] = {
    StrId::STR_DRIFT_SHIP_BAD_0, StrId::STR_DRIFT_SHIP_BAD_1, StrId::STR_DRIFT_SHIP_BAD_2,
    StrId::STR_DRIFT_SHIP_BAD_3, StrId::STR_DRIFT_SHIP_BAD_4, StrId::STR_DRIFT_SHIP_BAD_5,
};
static const StrId REACTIONS[] = {
    StrId::STR_DRIFT_SHIP_REACTION_0,
    StrId::STR_DRIFT_SHIP_REACTION_1,
    StrId::STR_DRIFT_SHIP_REACTION_2,
    StrId::STR_DRIFT_SHIP_REACTION_3,
};
// Indexed by Trait; None has no reaction of its own.
static const StrId TRAIT_REACTIONS[static_cast<uint8_t>(Trait::Count) - 1] = {
    StrId::STR_DRIFT_SHIP_REACTION_STEADY_HANDS, StrId::STR_DRIFT_SHIP_REACTION_NIGHT_OWL,
    StrId::STR_DRIFT_SHIP_REACTION_HOARDER,      StrId::STR_DRIFT_SHIP_REACTION_LUCKY,
    StrId::STR_DRIFT_SHIP_REACTION_RECKLESS,     StrId::STR_DRIFT_SHIP_REACTION_LONER,
    StrId::STR_DRIFT_SHIP_REACTION_DREAMER,
};

template <size_t N>
constexpr uint16_t countOf(const StrId (&)[N]) {
  return static_cast<uint16_t>(N);
}

bool validEra(const int8_t era) { return era >= 0 && era < static_cast<int8_t>(Era::Count); }
bool validTrait(const int8_t trait) { return trait > 0 && trait < static_cast<int8_t>(Trait::Count); }

}  // namespace

uint16_t builtinPieceCount(const Pool pool, const int8_t era, const int8_t trait) {
  switch (pool) {
    case Pool::Place:
      return static_cast<uint16_t>(countOf(PLACES) + (validEra(era) ? ERA_PLACE_COUNT : 0));
    case Pool::Finding:
      return countOf(FINDINGS);
    case Pool::Good:
      return countOf(GOOD);
    case Pool::Bad:
      return countOf(BAD);
    case Pool::Reaction:
      return static_cast<uint16_t>(countOf(REACTIONS) + (validTrait(trait) ? 1 : 0));
    case Pool::Count:
      break;
  }
  return 0;
}

const char* builtinPiece(const Pool pool, const int8_t era, const int8_t trait, const uint16_t index) {
  const I18n& i18n = I18n::getInstance();
  switch (pool) {
    case Pool::Place:
      if (index < countOf(PLACES)) return i18n.get(PLACES[index]);
      if (validEra(era) && index - countOf(PLACES) < ERA_PLACE_COUNT) {
        return i18n.get(ERA_PLACES[era][index - countOf(PLACES)]);
      }
      return i18n.get(PLACES[0]);
    case Pool::Finding:
      return i18n.get(FINDINGS[index % countOf(FINDINGS)]);
    case Pool::Good:
      return i18n.get(GOOD[index % countOf(GOOD)]);
    case Pool::Bad:
      return i18n.get(BAD[index % countOf(BAD)]);
    case Pool::Reaction:
      if (index < countOf(REACTIONS)) return i18n.get(REACTIONS[index]);
      if (validTrait(trait)) return i18n.get(TRAIT_REACTIONS[trait - 1]);
      return i18n.get(REACTIONS[0]);
    case Pool::Count:
      break;
  }
  return "";
}

}  // namespace DriftShip
