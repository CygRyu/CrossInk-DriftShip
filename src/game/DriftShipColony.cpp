#include "DriftShipColony.h"

#include <cstdio>

namespace DriftShip {
namespace {

// Names are proper nouns, so they are not translated. static const keeps the
// tables in flash. 8 x 32 combinations; an empty prefix gives a bare name.
static const char* const PREFIXES[8] = {"", "New ", "Port ", "Fort ", "", "Saint ", "Mount ", "Far "};

static const char* const ROOTS[32] = {
    "Halden", "Ione",  "Corrin", "Vesta",  "Marrow", "Aster",  "Tamsin", "Oduya", "Brightwater", "Kessel",  "Lyra",
    "Sorrel", "Anwen", "Toller", "Mirela", "Haven",  "Ostra",  "Calder", "Nadir", "Ember",       "Quiet",   "Rowan",
    "Selby",  "Thule", "Ubari",  "Verity", "Wick",   "Yarrow", "Zenith", "Abeni", "Cairn",       "Dunmore",
};

}  // namespace

void colonyName(const uint16_t nameSeed, char* buf, const size_t len) {
  if (!buf || len == 0) return;
  snprintf(buf, len, "%s%s", PREFIXES[(nameSeed / 32) % 8], ROOTS[nameSeed % 32]);
}

}  // namespace DriftShip
