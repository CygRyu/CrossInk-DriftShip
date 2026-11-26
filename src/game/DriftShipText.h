#pragma once

#include <cstddef>

#include "DriftShipColony.h"
#include "DriftShipCrew.h"

// Translated names for Drift Ship crew and modules, shared by the sleep screen
// and the Drift Ship activity.
namespace DriftShip {

const char* roleName(Role role);
const char* rarityName(Rarity rarity);
const char* traitName(Trait trait);  // empty string for Trait::None
const char* quirkText(uint8_t quirk);

// Describes a module, e.g. "+18% Fuel, -4% Data".
void describeModule(const Module& module, char* buf, size_t len);

const char* specialtyName(Specialty specialty);

// What a colony sends back, e.g. "+7% Fuel, +7 Parts per session".
void describeColonyTithe(const Colony& colony, char* buf, size_t len);

// The colony report sentence for an event, e.g. "Port Ione is booming."
// Writes an empty string for ColonyEvent::None.
void describeColonyEvent(ColonyEvent event, uint16_t nameSeed, char* buf, size_t len);

// Describes a crew member's traits, e.g. "Night owl, Reckless".
void describeTraits(const CrewMember& crew, char* buf, size_t len);

}  // namespace DriftShip
