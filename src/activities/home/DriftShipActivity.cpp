#include "DriftShipActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "game/DriftShipSleepScreen.h"
#include "game/DriftShipStore.h"

namespace {

constexpr int PAGE_COUNT = 4;
constexpr int SIDE_MARGIN = 20;

const char* systemName(const DriftShip::System system) {
  switch (system) {
    case DriftShip::System::Reactor:
      return tr(STR_DRIFT_SHIP_SYS_REACTOR);
    case DriftShip::System::Drones:
      return tr(STR_DRIFT_SHIP_SYS_DRONES);
    case DriftShip::System::Sensors:
      return tr(STR_DRIFT_SHIP_SYS_SENSORS);
    case DriftShip::System::CryoBay:
      return tr(STR_DRIFT_SHIP_SYS_CRYO_BAY);
    case DriftShip::System::Hull:
      return tr(STR_DRIFT_SHIP_HULL);
    case DriftShip::System::JumpDrive:
      return tr(STR_DRIFT_SHIP_SYS_JUMP_DRIVE);
  }
  return "";
}

const char* doctrineName(const uint8_t doctrine) {
  switch (static_cast<DriftShip::Doctrine>(doctrine)) {
    case DriftShip::Doctrine::Explorer:
      return tr(STR_DRIFT_SHIP_EXPLORER);
    case DriftShip::Doctrine::Builder:
      return tr(STR_DRIFT_SHIP_BUILDER);
    case DriftShip::Doctrine::Steady:
    case DriftShip::Doctrine::Count:
      break;
  }
  return tr(STR_DRIFT_SHIP_STEADY);
}

// What the next level of a system will cost, e.g. "Next: 210 Fuel, 12 Parts".
void formatCost(const DriftShip::Cost& cost, char* buf, const size_t len) {
  char fuel[16];
  DriftShip::formatCompactNumber(cost.fuel, fuel, sizeof(fuel));
  int used = snprintf(buf, len, "%s %s %s", tr(STR_DRIFT_SHIP_NEXT_COST), fuel, tr(STR_DRIFT_SHIP_FUEL));
  if (cost.parts > 0 && used > 0 && static_cast<size_t>(used) < len) {
    char parts[16];
    DriftShip::formatCompactNumber(cost.parts, parts, sizeof(parts));
    used += snprintf(buf + used, len - used, ", %s %s", parts, tr(STR_DRIFT_SHIP_PARTS));
  }
  if (cost.data > 0 && used > 0 && static_cast<size_t>(used) < len) {
    char data[16];
    DriftShip::formatCompactNumber(cost.data, data, sizeof(data));
    snprintf(buf + used, len - used, ", %s %s", data, tr(STR_DRIFT_SHIP_DATA));
  }
}

// A label on the left and its value right-aligned on the same row.
void drawRow(const GfxRenderer& r, const int x, const int y, const int width, const char* label, const char* value) {
  r.drawText(UI_10_FONT_ID, x, y, label);
  const int valueWidth = r.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
  r.drawText(UI_10_FONT_ID, x + width - valueWidth, y, value, true, EpdFontFamily::BOLD);
}

}  // namespace

void DriftShipActivity::onEnter() {
  Activity::onEnter();
  DriftShip::load(state);
  DriftShip::loadLog(log);
  page = Page::Ship;
  requestUpdate();
}

void DriftShipActivity::turnPage(const int delta) {
  const int next = (static_cast<int>(page) + delta + PAGE_COUNT) % PAGE_COUNT;
  page = static_cast<Page>(next);
  requestUpdate();
}

void DriftShipActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(Button::Right) || mappedInput.wasPressed(Button::Down) ||
      mappedInput.wasPressed(Button::PageForward) || mappedInput.wasReleased(Button::Confirm)) {
    turnPage(1);
    return;
  }
  if (mappedInput.wasPressed(Button::Left) || mappedInput.wasPressed(Button::Up) ||
      mappedInput.wasPressed(Button::PageBack)) {
    turnPage(-1);
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    turnPage(x < renderer.getScreenWidth() / 2 ? -1 : 1);
  }
}

int DriftShipActivity::drawPageHeader(const char* title) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char subtitle[48];
  snprintf(subtitle, sizeof(subtitle), tr(STR_DRIFT_SHIP_PAGE_OF), title, static_cast<unsigned>(page) + 1,
           static_cast<unsigned>(PAGE_COUNT));
  const Rect header{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight};
  GUI.drawHeader(renderer, header, tr(STR_DRIFT_SHIP), subtitle);
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

void DriftShipActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int height = renderer.getScreenHeight();
  const int contentBottom = height - metrics.buttonHintsHeight - 8;

  if (page == Page::Ship) {
    // The same view as the sleep screen, with room left for the button hints.
    DriftShip::drawSleepScreen(renderer, state, metrics.buttonHintsHeight);
  } else {
    renderer.clearScreen();
    switch (page) {
      case Page::Systems:
        drawSystemsPage(drawPageHeader(tr(STR_DRIFT_SHIP_PAGE_SYSTEMS)), contentBottom);
        break;
      case Page::Log:
        drawLogPage(drawPageHeader(tr(STR_DRIFT_SHIP_PAGE_LOG)), contentBottom);
        break;
      case Page::Records:
        drawRecordsPage(drawPageHeader(tr(STR_DRIFT_SHIP_PAGE_RECORDS)), contentBottom);
        break;
      case Page::Ship:
      case Page::Count:
        break;
    }
  }

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}

void DriftShipActivity::drawSystemsPage(int y, const int bottom) const {
  const int x = SIDE_MARGIN;
  const int width = renderer.getScreenWidth() - SIDE_MARGIN * 2;
  const int rowLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  char line[96];
  char value[24];

  for (uint8_t i = 0; i < DriftShip::SYSTEM_COUNT; i++) {
    if (y + rowLine + smallLine * 2 > bottom) break;
    const auto system = static_cast<DriftShip::System>(i);
    const uint8_t level = state.levels[i];
    snprintf(value, sizeof(value), tr(STR_DRIFT_SHIP_LEVEL), static_cast<unsigned>(level));
    drawRow(renderer, x, y, width, systemName(system), value);
    y += rowLine;

    switch (system) {
      case DriftShip::System::Reactor:
        snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_FX_REACTOR), 8u * level);
        break;
      case DriftShip::System::Drones:
        snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_FX_DRONES), static_cast<unsigned>(level), 2u * level);
        break;
      case DriftShip::System::Sensors:
        snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_FX_SENSORS), static_cast<unsigned>(level / 2),
                 static_cast<unsigned>(DriftShip::derelictChancePercent(state)));
        break;
      case DriftShip::System::CryoBay:
        snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_FX_CRYO_BAY),
                 static_cast<unsigned>(DriftShip::crewCapacity(state)));
        break;
      case DriftShip::System::Hull:
        snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_FX_HULL), static_cast<unsigned>(DriftShip::jumpWear(state)));
        break;
      case DriftShip::System::JumpDrive:
        snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_FX_JUMP_DRIVE), std::min(60u, 6u * level));
        break;
    }
    renderer.drawText(SMALL_FONT_ID, x, y, line);
    y += smallLine;
    formatCost(DriftShip::upgradeCost(system, level), line, sizeof(line));
    renderer.drawText(SMALL_FONT_ID, x, y, line);
    y += smallLine + 8;
  }

  if (y + rowLine <= bottom) {
    renderer.drawLine(x, y, x + width, y, 1, true);
    y += 8;
    drawRow(renderer, x, y, width, tr(STR_DRIFT_SHIP_DOCTRINE), doctrineName(SETTINGS.driftShipDoctrine));
  }
}

void DriftShipActivity::drawLogPage(int y, const int bottom) const {
  const int x = SIDE_MARGIN;
  const int width = renderer.getScreenWidth() - SIDE_MARGIN * 2;
  if (log.count == 0) {
    renderer.drawText(UI_10_FONT_ID, x, y,
                      renderer.truncatedText(UI_10_FONT_ID, tr(STR_DRIFT_SHIP_LOG_EMPTY), width).c_str());
    return;
  }

  const int titleLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  char left[48];
  char right[48];
  char tags[96];
  // Newest first, as many as fit on the page.
  for (int i = log.count - 1; i >= 0; i--) {
    if (y + titleLine + smallLine > bottom) break;
    const DriftShip::LogEntry& e = log.entries[i];

    snprintf(left, sizeof(left), "#%lu  %u min", static_cast<unsigned long>(e.sessionNumber),
             static_cast<unsigned>(e.minutes));
    char fuel[16];
    DriftShip::formatCompactNumber(e.fuel, fuel, sizeof(fuel));
    snprintf(right, sizeof(right), "+%s %s", fuel, tr(STR_DRIFT_SHIP_FUEL));
    drawRow(renderer, x, y, width, left, right);
    y += titleLine;

    tags[0] = '\0';
    const auto addTag = [&](const char* tag) {
      const size_t used = strlen(tags);
      snprintf(tags + used, sizeof(tags) - used, "%s%s", used ? ", " : "", tag);
    };
    if (e.watches & DriftShip::WATCH_EXPEDITION) {
      addTag(tr(STR_DRIFT_SHIP_WATCH_EXPEDITION));
    } else if (e.watches & DriftShip::WATCH_SURVEY) {
      addTag(tr(STR_DRIFT_SHIP_WATCH_SURVEY));
    } else {
      addTag(tr(STR_DRIFT_SHIP_WATCH_MAINTENANCE));
    }
    if (e.flags & DriftShip::LOG_JUMPED) addTag(tr(STR_DRIFT_SHIP_TAG_JUMP));
    if (e.flags & DriftShip::LOG_COLONY) addTag(tr(STR_DRIFT_SHIP_TAG_COLONY));
    if (e.flags & DriftShip::LOG_DERELICT) addTag(tr(STR_DRIFT_SHIP_TAG_DERELICT));
    if (e.flags & DriftShip::LOG_SLEEPER) addTag(tr(STR_DRIFT_SHIP_TAG_SLEEPER));
    if (e.flags & DriftShip::LOG_LUCKY) addTag(tr(STR_DRIFT_SHIP_TAG_LUCKY));
    if (e.flags & DriftShip::LOG_BAD_TWIST) addTag(tr(STR_DRIFT_SHIP_TAG_DAMAGE));
    renderer.drawText(SMALL_FONT_ID, x, y, renderer.truncatedText(SMALL_FONT_ID, tags, width).c_str());
    y += smallLine + 6;
  }
}

void DriftShipActivity::drawRecordsPage(int y, const int bottom) const {
  const int x = SIDE_MARGIN;
  const int width = renderer.getScreenWidth() - SIDE_MARGIN * 2;
  const int step = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  char value[32];

  const auto row = [&](const char* label) {
    if (y + step > bottom) return;
    drawRow(renderer, x, y, width, label, value);
    y += step;
  };

  const uint32_t tenthsOfHours = state.creditedSeconds / 360;
  snprintf(value, sizeof(value), "%lu.%lu", static_cast<unsigned long>(tenthsOfHours / 10),
           static_cast<unsigned long>(tenthsOfHours % 10));
  row(tr(STR_DRIFT_SHIP_REC_HOURS));
  DriftShip::formatCompactNumber(state.sessions, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_SESSIONS));
  DriftShip::formatCompactNumber(state.lifetimeFuel, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_FUEL));
  DriftShip::formatCompactNumber(state.bestSessionFuel, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_BEST));
  snprintf(value, sizeof(value), tr(STR_DRIFT_SHIP_MINUTES_VALUE),
           static_cast<unsigned long>(state.longestSessionSeconds / 60));
  row(tr(STR_DRIFT_SHIP_REC_LONGEST));
  DriftShip::formatCompactNumber(state.starsVisited, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_STARS));
  snprintf(value, sizeof(value), "%lu.%lu", static_cast<unsigned long>(state.distanceDeciLy / 10),
           static_cast<unsigned long>(state.distanceDeciLy % 10));
  row(tr(STR_DRIFT_SHIP_LIGHT_YEARS));
  DriftShip::formatCompactNumber(state.colonies, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_COLONIES));
  DriftShip::formatCompactNumber(state.sleepersWoken, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_SLEEPERS));
  DriftShip::formatCompactNumber(state.derelictsFound, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_DERELICTS));
  DriftShip::formatCompactNumber(state.luckyWatches, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_LUCKY));
  DriftShip::formatCompactNumber(state.upgradesBought, value, sizeof(value));
  row(tr(STR_DRIFT_SHIP_REC_UPGRADES));
}
