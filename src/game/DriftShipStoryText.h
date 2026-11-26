#pragma once

#include <cstdint>

#include "DriftShipStory.h"

// The built-in event pack (translated strings), for the firmware build.
namespace DriftShip {

// Built-in pieces for a pool. Places add the era's own places; reactions add
// the trait's own reaction. era and trait are -1 for none.
uint16_t builtinPieceCount(Pool pool, int8_t era, int8_t trait);

// A built-in piece by index in [0, builtinPieceCount()).
const char* builtinPiece(Pool pool, int8_t era, int8_t trait, uint16_t index);

}  // namespace DriftShip
