#include "DriftShipSleepScreen.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DriftShipEngine.h"
#include "fontIds.h"

namespace DriftShip {
namespace {

constexpr int SIDE_MARGIN = 24;

// All text buffers on this screen are short single lines; 96 bytes covers the
// longest translated line with room to spare and keeps stack use small.
constexpr size_t LINE_LEN = 96;

const char* starTypeName(const uint8_t type) {
  switch (static_cast<StarType>(type)) {
    case StarType::RedDwarf:
      return tr(STR_DRIFT_SHIP_STAR_RED_DWARF);
    case StarType::IceField:
      return tr(STR_DRIFT_SHIP_STAR_ICE_FIELD);
    case StarType::Nebula:
      return tr(STR_DRIFT_SHIP_STAR_NEBULA);
    case StarType::GasGiant:
      return tr(STR_DRIFT_SHIP_STAR_GAS_GIANT);
    case StarType::DeadGate:
      return tr(STR_DRIFT_SHIP_STAR_DEAD_GATE);
    case StarType::Pulsar:
      return tr(STR_DRIFT_SHIP_STAR_PULSAR);
    case StarType::Count:
      break;
  }
  return tr(STR_DRIFT_SHIP_STAR_RED_DWARF);
}

// Appends text to buf, separated by sep when buf already has content.
void append(char* buf, const size_t len, const char* sep, const char* text) {
  const size_t used = strlen(buf);
  if (used >= len - 1) return;
  snprintf(buf + used, len - used, "%s%s", used > 0 ? sep : "", text);
}

// "x1.40 reactor" style multiplier, or nothing when the bonus is zero.
void appendMultiplier(char* buf, const size_t len, const uint16_t permille, const char* label) {
  if (permille == 0) return;
  char part[32];
  const unsigned total = 1000u + permille;
  snprintf(part, sizeof(part), "x%u.%02u %s", total / 1000, (total % 1000) / 10, label);
  append(buf, len, "  ", part);
}

// Deterministic starfield: changes only when the ship reaches a new star.
void drawStarfield(const GfxRenderer& r, const State& s, const int x, const int y, const int w, const int h) {
  uint32_t seed = 0x2545F491u ^ (s.starsVisited * 2654435761u) ^ s.reach;
  for (int i = 0; i < 90; i++) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    const int px = x + static_cast<int>(seed % static_cast<uint32_t>(w));
    const int py = y + static_cast<int>((seed >> 12) % static_cast<uint32_t>(h));
    if ((seed >> 28) == 0) {
      r.fillRect(px, py, 3, 3);  // a few bright stars
    } else if ((seed >> 29) == 0) {
      r.fillRect(px, py, 2, 2);
    } else {
      r.drawPixel(px, py);
    }
  }
}

// A simple line-art seed ship, drawn around (cx, cy). Damage marks appear when
// the hull is weak and an exhaust trail when the ship just jumped.
void drawShip(const GfxRenderer& r, const State& s, const int cx, const int cy) {
  // Clear the ship's silhouette so stars don't show through.
  r.fillRect(cx - 140, cy - 42, 275, 84, false);

  // Main body and nose.
  r.drawRect(cx - 110, cy - 18, 200, 36, 2, true);
  r.drawLine(cx + 90, cy - 18, cx + 132, cy, 2, true);
  r.drawLine(cx + 90, cy + 18, cx + 132, cy, 2, true);
  // Cockpit window.
  r.fillRect(cx + 96, cy - 4, 14, 8);
  // Hab ring.
  r.drawRect(cx - 52, cy - 40, 26, 80, 2, true);
  r.drawLine(cx - 52, cy - 18, cx - 26, cy - 18, 1, true);
  r.drawLine(cx - 52, cy + 18, cx - 26, cy + 18, 1, true);
  // Cryo windows along the body: one lit window per awake crew member.
  const uint16_t crew = crewAwake(s);
  for (int i = 0; i < 6; i++) {
    const int wx = cx - 14 + i * 16;
    if (i < crew) {
      r.fillRect(wx, cy - 5, 8, 10);
    } else {
      r.drawRect(wx, cy - 5, 8, 10, 1, true);
    }
  }
  // Fins and engine block.
  r.drawLine(cx - 110, cy - 18, cx - 128, cy - 34, 2, true);
  r.drawLine(cx - 110, cy + 18, cx - 128, cy + 34, 2, true);
  r.fillRect(cx - 132, cy - 12, 22, 24);

  if (s.last.jumped) {
    for (int i = 0; i < 4; i++) {
      const int ly = cy - 9 + i * 6;
      r.drawLine(cx - 200 + i * 8, ly, cx - 140, ly, 1, true);
    }
  }
  if (s.condition < REPAIR_BELOW_CONDITION) {
    r.drawLine(cx + 20, cy - 18, cx + 30, cy - 6, 1, true);
    r.drawLine(cx + 30, cy - 6, cx + 24, cy + 4, 1, true);
    r.drawLine(cx - 70, cy + 18, cx - 62, cy + 8, 1, true);
  }
}

// Route through the current reach: visited stars filled, the current star
// large, upcoming stars outlined, and the colony world as a diamond.
void drawRoute(const GfxRenderer& r, const State& s, const int x, const int y, const int w) {
  const int steps = STARS_PER_REACH;
  const int gap = w / steps;
  r.drawLine(x + gap / 2, y, x + gap / 2 + gap * (steps - 1), y, 1, true);
  for (int i = 0; i < steps; i++) {
    const int nx = x + gap / 2 + gap * i;
    if (i < s.star) {
      r.fillRect(nx - 4, y - 4, 9, 9);
    } else if (i == s.star) {
      r.fillRect(nx - 7, y - 7, 15, 15, false);
      r.drawRect(nx - 7, y - 7, 15, 15, 2, true);
      r.fillRect(nx - 3, y - 3, 7, 7);
    } else {
      r.fillRect(nx - 4, y - 4, 9, 9, false);
      r.drawRect(nx - 4, y - 4, 9, 9, 1, true);
    }
  }
  // Colony world past the last star.
  const int hx = x + w - 2;
  r.drawLine(hx - 8, y, hx, y - 8, 2, true);
  r.drawLine(hx, y - 8, hx + 8, y, 2, true);
  r.drawLine(hx + 8, y, hx, y + 8, 2, true);
  r.drawLine(hx, y + 8, hx - 8, y, 2, true);
}

// Draws text word-wrapped to maxWidth. Returns the y below the last line.
int drawWrapped(const GfxRenderer& r, const int fontId, const int x, int y, const int maxWidth, const char* text) {
  const int lineHeight = r.getLineHeight(fontId);
  char line[LINE_LEN] = "";
  const char* p = text;
  while (*p) {
    const char* space = strchr(p, ' ');
    const size_t wordLen = space ? static_cast<size_t>(space - p) : strlen(p);
    char candidate[LINE_LEN];
    snprintf(candidate, sizeof(candidate), "%s%s%.*s", line, line[0] ? " " : "", static_cast<int>(wordLen), p);
    if (line[0] && r.getTextWidth(fontId, candidate) > maxWidth) {
      r.drawText(fontId, x, y, line);
      y += lineHeight;
      snprintf(line, sizeof(line), "%.*s", static_cast<int>(wordLen), p);
    } else {
      memcpy(line, candidate, sizeof(line));
    }
    p += wordLen;
    while (*p == ' ') p++;
  }
  if (line[0]) {
    r.drawText(fontId, x, y, line);
    y += lineHeight;
  }
  return y;
}

// One cell of the stats grid: a small label over a bold value.
void drawStat(const GfxRenderer& r, const int x, const int y, const char* label, const char* value) {
  r.drawText(SMALL_FONT_ID, x, y, label);
  r.drawText(UI_12_FONT_ID, x, y + r.getLineHeight(SMALL_FONT_ID), value, true, EpdFontFamily::BOLD);
}

}  // namespace

void drawSleepScreen(const GfxRenderer& r, const State& s, const int bottomReserve) {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  r.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int width = r.getScreenWidth();
  const int height = r.getScreenHeight();
  const int x0 = left + SIDE_MARGIN;
  const int contentWidth = width - left - right - SIDE_MARGIN * 2;
  char line[LINE_LEN];

  r.clearScreen();
  int y = top + 24;
  r.drawCenteredText(UI_12_FONT_ID, y, tr(STR_DRIFT_SHIP), true, EpdFontFamily::BOLD);
  y += r.getLineHeight(UI_12_FONT_ID) + 4;

  if (s.version == 0) {
    drawStarfield(r, s, x0, y + 20, contentWidth, 260);
    drawShip(r, s, width / 2, y + 150);
    drawWrapped(r, UI_10_FONT_ID, x0, y + 320, contentWidth, tr(STR_DRIFT_SHIP_NOT_STARTED));
    return;
  }

  snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_LOCATION), static_cast<unsigned>(s.reach + 1),
           static_cast<unsigned>(s.star + 1), static_cast<unsigned>(STARS_PER_REACH));
  r.drawCenteredText(UI_10_FONT_ID, y, line);
  y += r.getLineHeight(UI_10_FONT_ID);
  r.drawCenteredText(SMALL_FONT_ID, y, starTypeName(s.starType));
  y += r.getLineHeight(SMALL_FONT_ID) + 8;

  // --- Ship art and route ---
  const int artHeight = 170;
  drawStarfield(r, s, x0, y, contentWidth, artHeight);
  drawShip(r, s, width / 2 + 20, y + artHeight / 2);
  y += artHeight + 14;
  drawRoute(r, s, x0, y, contentWidth);
  y += 26;
  r.drawLine(x0, y, x0 + contentWidth, y, 1, true);
  y += 12;

  // --- Session receipt ---
  const SessionReport& rep = s.last;
  snprintf(line, sizeof(line), tr(STR_DRIFT_SHIP_SESSION), static_cast<unsigned long>(rep.sessionNumber),
           static_cast<unsigned long>(rep.seconds / 60));
  r.drawText(UI_10_FONT_ID, x0, y, line, true, EpdFontFamily::BOLD);
  y += r.getLineHeight(UI_10_FONT_ID);

  line[0] = '\0';
  if (rep.watches & WATCH_MAINTENANCE) append(line, sizeof(line), " + ", tr(STR_DRIFT_SHIP_WATCH_MAINTENANCE));
  if (rep.watches & WATCH_SURVEY) append(line, sizeof(line), " + ", tr(STR_DRIFT_SHIP_WATCH_SURVEY));
  if (rep.watches & WATCH_EXPEDITION) append(line, sizeof(line), " + ", tr(STR_DRIFT_SHIP_WATCH_EXPEDITION));
  if (rep.watches & WATCH_LONG) append(line, sizeof(line), " + ", tr(STR_DRIFT_SHIP_WATCH_LONG));
  y = drawWrapped(r, SMALL_FONT_ID, x0, y, contentWidth, line);

  // The multiplier stack: "82 base  x1.40 reactor  x1.15 crew  x2 lucky"
  char number[16];
  formatCompactNumber(rep.baseFuel, number, sizeof(number));
  snprintf(line, sizeof(line), "%s %s", number, tr(STR_DRIFT_SHIP_BASE));
  appendMultiplier(line, sizeof(line), rep.reactorPermille, tr(STR_DRIFT_SHIP_REACTOR));
  appendMultiplier(line, sizeof(line), rep.crewPermille, tr(STR_DRIFT_SHIP_CREW_BONUS));
  appendMultiplier(line, sizeof(line), rep.starPermille, tr(STR_DRIFT_SHIP_GAS_GIANT_BONUS));
  if (rep.lucky) {
    char lucky[32];
    snprintf(lucky, sizeof(lucky), "x2 %s", tr(STR_DRIFT_SHIP_LUCKY));
    append(line, sizeof(line), "  ", lucky);
  }
  y = drawWrapped(r, SMALL_FONT_ID, x0, y, contentWidth, line);

  char parts[16];
  char data[16];
  formatCompactNumber(rep.fuel, number, sizeof(number));
  formatCompactNumber(rep.parts, parts, sizeof(parts));
  formatCompactNumber(rep.data, data, sizeof(data));
  snprintf(line, sizeof(line), "+%s %s   +%s %s   +%s %s", number, tr(STR_DRIFT_SHIP_FUEL), parts,
           tr(STR_DRIFT_SHIP_PARTS), data, tr(STR_DRIFT_SHIP_DATA));
  y += 4;
  r.drawText(UI_10_FONT_ID, x0, y, line, true, EpdFontFamily::BOLD);
  y += r.getLineHeight(UI_10_FONT_ID) + 4;

  // --- What happened ---
  char events[LINE_LEN * 2] = "";
  if (rep.derelict) append(events, sizeof(events), " ", tr(STR_DRIFT_SHIP_EVENT_DERELICT));
  if (rep.twist == Twist::Good) append(events, sizeof(events), " ", tr(STR_DRIFT_SHIP_EVENT_GOOD));
  if (rep.twist == Twist::Bad) append(events, sizeof(events), " ", tr(STR_DRIFT_SHIP_EVENT_BAD));
  if (rep.sleeperWoken) append(events, sizeof(events), " ", tr(STR_DRIFT_SHIP_EVENT_SLEEPER));
  if (rep.repaired) append(events, sizeof(events), " ", tr(STR_DRIFT_SHIP_EVENT_REPAIRED));
  if (rep.upgradesBought > 0) {
    char upgrades[48];
    snprintf(upgrades, sizeof(upgrades), tr(STR_DRIFT_SHIP_EVENT_UPGRADES), static_cast<unsigned>(rep.upgradesBought));
    append(events, sizeof(events), " ", upgrades);
  }
  if (rep.jumped) {
    append(events, sizeof(events), " ",
           rep.cleanJump ? tr(STR_DRIFT_SHIP_EVENT_CLEAN_JUMP) : tr(STR_DRIFT_SHIP_EVENT_JUMPED));
  }
  if (rep.reachCompleted) append(events, sizeof(events), " ", tr(STR_DRIFT_SHIP_EVENT_COLONY));
  if (events[0]) {
    // drawWrapped works line by line, so long event text is split safely.
    y = drawWrapped(r, SMALL_FONT_ID, x0, y, contentWidth, events);
  }

  // --- Ship numbers: a 3x3 grid pinned to the bottom ---
  const int cellHeight = r.getLineHeight(SMALL_FONT_ID) + r.getLineHeight(UI_12_FONT_ID) + 10;
  const int gridTop = std::max(y + 8, height - bottom - bottomReserve - 16 - cellHeight * 3);
  const int colWidth = contentWidth / 3;
  r.drawLine(x0, gridTop - 8, x0 + contentWidth, gridTop - 8, 1, true);

  char value[24];
  formatCompactNumber(s.fuel, value, sizeof(value));
  drawStat(r, x0, gridTop, tr(STR_DRIFT_SHIP_FUEL), value);
  formatCompactNumber(s.parts, value, sizeof(value));
  drawStat(r, x0 + colWidth, gridTop, tr(STR_DRIFT_SHIP_PARTS), value);
  formatCompactNumber(s.data, value, sizeof(value));
  drawStat(r, x0 + colWidth * 2, gridTop, tr(STR_DRIFT_SHIP_DATA), value);

  int rowY = gridTop + cellHeight;
  snprintf(value, sizeof(value), "%u/%u", static_cast<unsigned>(crewAwake(s)), static_cast<unsigned>(crewCapacity(s)));
  drawStat(r, x0, rowY, tr(STR_DRIFT_SHIP_CREW), value);
  snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(s.condition));
  drawStat(r, x0 + colWidth, rowY, tr(STR_DRIFT_SHIP_HULL), value);
  const uint64_t needed = jumpChargeNeeded(s);
  uint64_t pct = 100;
  if (s.jumpCharge < needed) {
    pct = needed > UINT64_MAX / 100 ? s.jumpCharge / (needed / 100) : s.jumpCharge * 100 / needed;
  }
  snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(pct));
  drawStat(r, x0 + colWidth * 2, rowY, tr(STR_DRIFT_SHIP_JUMP), value);

  rowY += cellHeight;
  snprintf(value, sizeof(value), "%u/%u",
           static_cast<unsigned>(std::min<uint8_t>(s.surveysHere, SURVEYS_TO_PLOT_COURSE)),
           static_cast<unsigned>(SURVEYS_TO_PLOT_COURSE));
  drawStat(r, x0, rowY, tr(STR_DRIFT_SHIP_COURSE), value);
  snprintf(value, sizeof(value), "%lu.%lu", static_cast<unsigned long>(s.distanceDeciLy / 10),
           static_cast<unsigned long>(s.distanceDeciLy % 10));
  drawStat(r, x0 + colWidth, rowY, tr(STR_DRIFT_SHIP_LIGHT_YEARS), value);
  formatCompactNumber(s.starsVisited, value, sizeof(value));
  drawStat(r, x0 + colWidth * 2, rowY, tr(STR_DRIFT_SHIP_STARS), value);
}

}  // namespace DriftShip
