#pragma once

#include <cstddef>
#include <cstdint>

// Crew and salvaged-module types for Drift Ship. Plain data only: the rules
// that roll, grow and use them live in DriftShipEngine.cpp.
namespace DriftShip {

enum class Role : uint8_t { Engineer = 0, Scientist, Pilot, Scout, Medic, Quartermaster, Count };

enum class Rarity : uint8_t { Common = 0, Skilled, Veteran, Legendary, Count };

enum class Trait : uint8_t {
  None = 0,
  SteadyHands,  // never injured
  NightOwl,     // +10% Fuel on sessions of 60+ minutes
  Hoarder,      // +1 Parts from every salvage and derelict
  Lucky,        // half of all bad twists are shrugged off
  Reckless,     // +20% expedition finds, double injury chance
  Loner,        // +10% Fuel while the crew is 2 or fewer
  Dreamer,      // -5% Fuel, +10 points of derelict chance
  Count
};

constexpr uint8_t QUIRK_COUNT = 12;
constexpr uint8_t MAX_CREW = 12;
constexpr uint8_t MAX_CREW_LEVEL = 20;
constexpr uint8_t MAX_STAT = 10;
constexpr uint16_t CREW_RETIRE_AFTER_REACHES = 3;

struct CrewMember {
  uint16_t nameSeed = 0;  // picks a given name and a family name
  Role role = Role::Engineer;
  Rarity rarity = Rarity::Common;
  uint8_t skill = 1;  // strength of the role bonus
  uint8_t grit = 1;   // resists injuries; Pilot and Medic main stat
  uint8_t luck = 1;   // lucky watches and Scout finds
  Trait traits[2] = {Trait::None, Trait::None};
  uint8_t quirk = 0;  // flavor only
  uint8_t level = 1;
  uint16_t xpMinutes = 0;
  uint8_t injured = 0;  // sessions left before returning to duty
  uint16_t joinedReach = 0;
};

enum class ModuleEffect : uint8_t { None = 0, Fuel, Parts, Data, Derelict, HullWear, JumpCharge, Count };

constexpr uint8_t MODULE_SLOTS = 4;

// A salvaged module has up to two effects. A value is in benefit points:
// positive helps, negative is the module's drawback. Fuel, Parts, Data and
// JumpCharge are percentages; Derelict and HullWear are percentage points.
struct Module {
  ModuleEffect effect[2] = {ModuleEffect::None, ModuleEffect::None};
  int8_t value[2] = {0, 0};
};

// The crew member's main stat for their role.
uint8_t mainStat(const CrewMember& crew);

// Writes "Given Family" for a name seed.
void crewName(uint16_t nameSeed, char* buf, size_t len);

}  // namespace DriftShip
