#pragma once

#include <cstddef>

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

// Describes a crew member's traits, e.g. "Night owl, Reckless".
void describeTraits(const CrewMember& crew, char* buf, size_t len);

}  // namespace DriftShip
