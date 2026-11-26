#include "DriftShipText.h"

#include <I18n.h>

#include <cstdio>
#include <cstring>

#include "DriftShipEngine.h"

namespace DriftShip {

const char* roleName(const Role role) {
  switch (role) {
    case Role::Engineer:
      return tr(STR_DRIFT_SHIP_ROLE_ENGINEER);
    case Role::Scientist:
      return tr(STR_DRIFT_SHIP_ROLE_SCIENTIST);
    case Role::Pilot:
      return tr(STR_DRIFT_SHIP_ROLE_PILOT);
    case Role::Scout:
      return tr(STR_DRIFT_SHIP_ROLE_SCOUT);
    case Role::Medic:
      return tr(STR_DRIFT_SHIP_ROLE_MEDIC);
    case Role::Quartermaster:
      return tr(STR_DRIFT_SHIP_ROLE_QUARTERMASTER);
    case Role::Count:
      break;
  }
  return tr(STR_DRIFT_SHIP_ROLE_ENGINEER);
}

const char* rarityName(const Rarity rarity) {
  switch (rarity) {
    case Rarity::Common:
      return tr(STR_DRIFT_SHIP_RARITY_COMMON);
    case Rarity::Skilled:
      return tr(STR_DRIFT_SHIP_RARITY_SKILLED);
    case Rarity::Veteran:
      return tr(STR_DRIFT_SHIP_RARITY_VETERAN);
    case Rarity::Legendary:
      return tr(STR_DRIFT_SHIP_RARITY_LEGENDARY);
    case Rarity::Count:
      break;
  }
  return tr(STR_DRIFT_SHIP_RARITY_COMMON);
}

const char* traitName(const Trait trait) {
  switch (trait) {
    case Trait::SteadyHands:
      return tr(STR_DRIFT_SHIP_TRAIT_STEADY_HANDS);
    case Trait::NightOwl:
      return tr(STR_DRIFT_SHIP_TRAIT_NIGHT_OWL);
    case Trait::Hoarder:
      return tr(STR_DRIFT_SHIP_TRAIT_HOARDER);
    case Trait::Lucky:
      return tr(STR_DRIFT_SHIP_TRAIT_LUCKY);
    case Trait::Reckless:
      return tr(STR_DRIFT_SHIP_TRAIT_RECKLESS);
    case Trait::Loner:
      return tr(STR_DRIFT_SHIP_TRAIT_LONER);
    case Trait::Dreamer:
      return tr(STR_DRIFT_SHIP_TRAIT_DREAMER);
    case Trait::None:
    case Trait::Count:
      break;
  }
  return "";
}

const char* quirkText(const uint8_t quirk) {
  switch (quirk % QUIRK_COUNT) {
    case 0:
      return tr(STR_DRIFT_SHIP_QUIRK_0);
    case 1:
      return tr(STR_DRIFT_SHIP_QUIRK_1);
    case 2:
      return tr(STR_DRIFT_SHIP_QUIRK_2);
    case 3:
      return tr(STR_DRIFT_SHIP_QUIRK_3);
    case 4:
      return tr(STR_DRIFT_SHIP_QUIRK_4);
    case 5:
      return tr(STR_DRIFT_SHIP_QUIRK_5);
    case 6:
      return tr(STR_DRIFT_SHIP_QUIRK_6);
    case 7:
      return tr(STR_DRIFT_SHIP_QUIRK_7);
    case 8:
      return tr(STR_DRIFT_SHIP_QUIRK_8);
    case 9:
      return tr(STR_DRIFT_SHIP_QUIRK_9);
    case 10:
      return tr(STR_DRIFT_SHIP_QUIRK_10);
    default:
      return tr(STR_DRIFT_SHIP_QUIRK_11);
  }
}

namespace {

// One module effect as text. Wear and charge show the change itself, so a
// benefit reads as a reduction ("-4 jump wear").
void describeEffect(const ModuleEffect effect, const int value, char* buf, const size_t len) {
  switch (effect) {
    case ModuleEffect::Fuel:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_MOD_FUEL), value);
      return;
    case ModuleEffect::Parts:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_MOD_PARTS), value);
      return;
    case ModuleEffect::Data:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_MOD_DATA), value);
      return;
    case ModuleEffect::Derelict:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_MOD_DERELICT), value);
      return;
    case ModuleEffect::HullWear:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_MOD_HULL_WEAR), -value);
      return;
    case ModuleEffect::JumpCharge:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_MOD_JUMP_CHARGE), -value);
      return;
    case ModuleEffect::None:
    case ModuleEffect::Count:
      break;
  }
  if (len > 0) buf[0] = '\0';
}

void appendPart(char* buf, const size_t len, const char* text) {
  if (!text[0]) return;
  const size_t used = strlen(buf);
  if (used + 1 >= len) return;
  snprintf(buf + used, len - used, "%s%s", used > 0 ? ", " : "", text);
}

}  // namespace

void describeModule(const Module& module, char* buf, const size_t len) {
  if (!buf || len == 0) return;
  buf[0] = '\0';
  for (uint8_t k = 0; k < 2; k++) {
    char part[40];
    describeEffect(module.effect[k], module.value[k], part, sizeof(part));
    appendPart(buf, len, part);
  }
}

void describeTraits(const CrewMember& crew, char* buf, const size_t len) {
  if (!buf || len == 0) return;
  buf[0] = '\0';
  for (const Trait trait : crew.traits) appendPart(buf, len, traitName(trait));
}

const char* specialtyName(const Specialty specialty) {
  switch (specialty) {
    case Specialty::Mining:
      return tr(STR_DRIFT_SHIP_SPEC_MINING);
    case Specialty::Science:
      return tr(STR_DRIFT_SHIP_SPEC_SCIENCE);
    case Specialty::Shipyard:
      return tr(STR_DRIFT_SHIP_SPEC_SHIPYARD);
    case Specialty::Count:
      break;
  }
  return tr(STR_DRIFT_SHIP_SPEC_MINING);
}

void describeColonyTithe(const Colony& colony, char* buf, const size_t len) {
  if (!buf || len == 0) return;
  const unsigned fuelPct = colonyFuelPermille(colony) / 10u;
  const auto goods = static_cast<unsigned>(colonyGoods(colony));
  switch (colony.specialty) {
    case Specialty::Science:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_TITHE_SCIENCE), fuelPct, goods);
      return;
    case Specialty::Shipyard:
      snprintf(buf, len, tr(STR_DRIFT_SHIP_TITHE_SHIPYARD), fuelPct, goods);
      return;
    case Specialty::Mining:
    case Specialty::Count:
      break;
  }
  snprintf(buf, len, tr(STR_DRIFT_SHIP_TITHE_MINING), fuelPct, goods);
}

void describeColonyEvent(const ColonyEvent event, const uint16_t nameSeed, char* buf, const size_t len) {
  if (!buf || len == 0) return;
  buf[0] = '\0';
  const char* fmt = nullptr;
  switch (event) {
    case ColonyEvent::Boom:
      fmt = tr(STR_DRIFT_SHIP_EVENT_COLONY_BOOM);
      break;
    case ColonyEvent::Fever:
      fmt = tr(STR_DRIFT_SHIP_EVENT_COLONY_FEVER);
      break;
    case ColonyEvent::Gift:
      fmt = tr(STR_DRIFT_SHIP_EVENT_COLONY_GIFT);
      break;
    case ColonyEvent::HullSection:
      fmt = tr(STR_DRIFT_SHIP_EVENT_COLONY_HULL);
      break;
    case ColonyEvent::TrainedCrew:
      fmt = tr(STR_DRIFT_SHIP_EVENT_COLONY_CREW);
      break;
    case ColonyEvent::None:
    case ColonyEvent::Count:
      return;
  }
  char name[32];
  colonyName(nameSeed, name, sizeof(name));
  snprintf(buf, len, fmt, name);
}

}  // namespace DriftShip
