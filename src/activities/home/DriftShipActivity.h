#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "game/DriftShipEngine.h"
#include "game/DriftShipStory.h"

// Read-only Drift Ship screens: the ship view, systems, the ship log and
// lifetime records, plus the crew, salvaged modules and colonies. The game itself plays without this screen; it is only
// for checking in. Page buttons flip pages, Back exits.
class DriftShipActivity final : public Activity {
 public:
  DriftShipActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DriftShip", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Ship = 0, Crew, Systems, Modules, Colonies, Log, Records, Count };

  // Loaded once on entry. The activity object lives on the heap (owned by
  // ActivityManager), so these ~1 KB never touch the task stack.
  DriftShip::State state;
  DriftShip::SessionLog log;
  char story[DriftShip::STORY_LEN] = "";
  Page page = Page::Ship;
  uint8_t crewStart = 0;  // first crew member on the current crew screen
  // How many crew the last crew screen fit; set while drawing.
  mutable uint8_t crewShown = 0;
  uint8_t colonyStart = 0;  // first colony (newest first) on the current colony screen
  mutable uint8_t colonyShown = 0;

  void turnPage(int delta);
  int drawPageHeader(const char* title) const;
  void drawCrewPage(int y, int bottom) const;
  void drawSystemsPage(int y, int bottom) const;
  void drawModulesPage(int y, int bottom) const;
  void drawColoniesPage(int y, int bottom) const;
  void drawLogPage(int y, int bottom) const;
  void drawRecordsPage(int top, int bottom) const;
};
