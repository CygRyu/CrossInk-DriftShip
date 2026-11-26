#include "DriftShipCrew.h"

#include <cstdio>

namespace DriftShip {
namespace {

// Names are proper nouns, so they are not translated. static const keeps the
// tables in flash. 32 x 32 combinations.
static const char* const GIVEN_NAMES[32] = {
    "Ada",  "Bram", "Cass", "Dara", "Emil", "Fen",  "Galen", "Hana",  "Ilya", "Juno", "Kai",
    "Lena", "Milo", "Nia",  "Oren", "Pia",  "Quin", "Rhea",  "Soren", "Tova", "Ula",  "Vik",
    "Wren", "Xia",  "Yuri", "Zola", "Ines", "Taro", "Mara",  "Dev",   "Sana", "Otto",
};

static const char* const FAMILY_NAMES[32] = {
    "Osei",  "Varga",     "Lind",  "Okafor", "Reyes",  "Sato",   "Brandt", "Novak",  "Haddad", "Moreau", "Kaur",
    "Ivers", "Tanaka",    "Quill", "Achebe", "Lund",   "Petrov", "Abara",  "Castel", "Mbeki",  "Holm",   "Rana",
    "Vos",   "Delacroix", "Kim",   "Farr",   "Ostrom", "Nakai",  "Silva",  "Rowe",   "Ueda",   "Marek",
};

}  // namespace

uint8_t mainStat(const CrewMember& crew) {
  switch (crew.role) {
    case Role::Pilot:
    case Role::Medic:
      return crew.grit;
    case Role::Scout:
      return crew.luck;
    case Role::Engineer:
    case Role::Scientist:
    case Role::Quartermaster:
    case Role::Count:
      break;
  }
  return crew.skill;
}

void crewName(const uint16_t nameSeed, char* buf, const size_t len) {
  if (!buf || len == 0) return;
  snprintf(buf, len, "%s %s", GIVEN_NAMES[nameSeed % 32], FAMILY_NAMES[(nameSeed / 32) % 32]);
}

}  // namespace DriftShip
