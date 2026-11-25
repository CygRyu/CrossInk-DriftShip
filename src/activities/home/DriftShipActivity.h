#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "game/DriftShipEngine.h"

// Read-only Drift Ship screens: the ship view, systems, the ship log and
// lifetime records. The game itself plays without this screen; it is only for
// checking in. Page buttons flip pages, Back exits.
class DriftShipActivity final : public Activity {
 public:
  DriftShipActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DriftShip", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Ship = 0, Systems, Log, Records, Count };

  // Loaded once on entry. The activity object lives on the heap (owned by
  // ActivityManager), so these ~600 bytes never touch the task stack.
  DriftShip::State state;
  DriftShip::SessionLog log;
  Page page = Page::Ship;

  void turnPage(int delta);
  int drawPageHeader(const char* title) const;
  void drawSystemsPage(int y, int bottom) const;
  void drawLogPage(int y, int bottom) const;
  void drawRecordsPage(int y, int bottom) const;
};
