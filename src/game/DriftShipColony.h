#pragma once

#include <cstddef>
#include <cstdint>

// Colony types for Drift Ship. Plain data only: growth, tithes and events
// live in DriftShipEngine.cpp.
namespace DriftShip {

enum class Specialty : uint8_t { Mining = 0, Science, Shipyard, Count };

// What a colony sent back or went through this session.
enum class ColonyEvent : uint8_t {
  None = 0,
  Boom,         // population +10%
  Fever,        // population -10%
  Gift,         // a burst of Parts and Data
  HullSection,  // hull condition +15
  TrainedCrew,  // a trained crew member joins the ship
  Count
};

// The newest colonies are kept one by one. Once the network is full, a new
// colony's settlers join the smallest existing one instead.
constexpr uint8_t MAX_COLONIES = 16;
// No colony grows past this, so totals stay far from 64-bit overflow.
constexpr uint64_t MAX_POPULATION = 1000000000000000ULL;

struct Colony {
  uint16_t nameSeed = 0;
  Specialty specialty = Specialty::Mining;
  uint8_t founders = 0;       // crew who retired here
  uint16_t founderStats = 0;  // sum of their main stats; speeds growth
  uint16_t foundedReach = 0;  // the reach it closed, counting from 1
  uint64_t population = 0;
};

// Writes the colony's name for a name seed, e.g. "New Halden".
void colonyName(uint16_t nameSeed, char* buf, size_t len);

}  // namespace DriftShip
