#include "core/rssichart.h"

#include <M5Cardputer.h>

#include "core/ui.h"

namespace rssichart {

namespace {

// Bar colour for a 0..100 level (Unigeek NRF24Screen::_specBarColor).
uint16_t barColor(int level) {
  if (level > 85) return TFT_RED;
  if (level > 65) return 0xFD20;  // orange
  if (level > 45) return TFT_YELLOW;
  if (level > 25) return TFT_GREEN;
  return TFT_DARKGREEN;
}

// Strength colour for the detected peak (Unigeek detectStrengthColor).
uint16_t detectColor(int dbm) {
  if (dbm >= -52) return TFT_GREEN;
  if (dbm >= -60) return TFT_YELLOW;
  return TFT_RED;
}

// Heat-map area, in canvas pixels.
constexpr int kMapX = 4;
constexpr int kMapW = 232;
constexpr int kMapTop = ui::HEADER_H + 22;
constexpr int kMapBottom = ui::H - ui::FOOTER_H;

}  // namespace

void draw(const Style& s, const int8_t* now, const int8_t* peak, int n) {
  ui::drawFrame(s.title, s.footer, s.right);
  auto& c = ui::canvas();

  const int chartX = 2;
  const int chartW = ui::W - 4;
  const int chartTop = ui::HEADER_H + 2;
  const int chartBottom = 108;
  const int chartH = chartBottom - chartTop;
  const int range = s.valueMax - s.valueMin;

  c.drawFastHLine(0, chartBottom + 1, ui::W, TFT_DARKGREY);
  for (int i = 0; i < n; i++) {
    int x = chartX + i * chartW / n;
    int w = chartX + (i + 1) * chartW / n - x;
    if (w < 1) w = 1;
    int lvNow = (now[i] - s.valueMin) * 100 / range;
    int lvPk = (peak[i] - s.valueMin) * 100 / range;
    if (lvNow > 100) lvNow = 100;
    if (lvPk > 100) lvPk = 100;
    int hNow = lvNow * chartH / 100;
    int hPk = lvPk * chartH / 100;
    if (hNow > 0) c.fillRect(x, chartBottom - hNow, w, hNow, barColor(lvNow));
    if (s.showPeaks && hPk > hNow && hPk > 0) c.fillRect(x, chartBottom - hPk, w, 1, TFT_WHITE);
  }

  c.setFont(&fonts::Font0);
  c.setTextDatum(top_left);
  if (s.mode && s.mode[0]) {
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(s.mode, chartX + 1, chartTop);
  }
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.drawString(s.left, chartX, chartBottom + 5);
  c.setTextDatum(top_center);
  c.drawString(s.mid, ui::W / 2, chartBottom + 5);
  c.setTextDatum(top_right);
  c.drawString(s.end, ui::W - chartX, chartBottom + 5);
  ui::present();
}

void drawBars(const char* title, const char* footer, const char* right, const uint8_t* level,
              int n, const char* startLabel, const char* endLabel, const char* maxText) {
  ui::drawFrame(title, footer, right);
  auto& c = ui::canvas();

  const int chartX = 2;
  const int chartW = ui::W - 4;
  const int chartTop = ui::HEADER_H + 2;
  const int axisY = 108;
  const int chartH = axisY - chartTop;

  // Baseline with ticks at the start, every quarter and the end.
  c.drawFastHLine(0, axisY + 1, ui::W, TFT_WHITE);
  for (int t = 0; t <= 4; t++) {
    int x = chartX + t * (chartW - 1) / 4;
    c.drawFastVLine(x, axisY + 2, 3, TFT_WHITE);
  }

  for (int i = 0; i < n; i++) {
    int x = chartX + i * chartW / n;
    int w = chartX + (i + 1) * chartW / n - x;
    if (w > 1) w--;  // thin gap between bars, as Bruce leaves
    int lv = level[i] > 100 ? 100 : level[i];
    int h = lv * chartH / 100;
    if (h > 0) c.fillRect(x, axisY - h, w, h, TFT_GREEN);
  }

  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_WHITE, TFT_BLACK);
  c.setTextDatum(top_left);
  c.drawString(startLabel, chartX, axisY + 6);
  c.setTextDatum(top_right);
  c.drawString(endLabel, ui::W - chartX, axisY + 6);
  if (maxText && maxText[0]) {
    c.setTextDatum(top_center);
    c.drawString(maxText, ui::W / 2, axisY + 6);
  }
  ui::present();
}

uint16_t heatColor(int dbm) {
  if (dbm < -100) dbm = -100;
  if (dbm > -35) dbm = -35;
  int level = (dbm + 100) * 255 / 65;  // 0 weak .. 255 strong
  int r, g, b;
  if (level < 64) {
    r = 0;
    g = 0;
    b = level * 4;
  } else if (level < 128) {
    r = 0;
    g = (level - 64) * 4;
    b = 255 - (level - 64) * 4;
  } else if (level < 192) {
    r = (level - 128) * 4;
    g = 255;
    b = 0;
  } else {
    r = 255;
    g = 255 - (level - 192) * 4;
    b = 0;
  }
  return ui::canvas().color565(r, g, b);
}

void Waterfall::begin(const char* title, float startMhz, float endMhz) {
  start_ = startMhz;
  end_ = endMhz;
  line_ = 0;
  ui::drawFrame(title, "Del back");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_left);
  c.setTextColor(TFT_WHITE, TFT_BLACK);
  // Four frequency ticks across the band, as in Unigeek.
  for (int i = 0; i < 4; i++) {
    int x = kMapX + i * (kMapW / 4);
    float f = start_ + (end_ - start_) * i / 4.0f;
    char fb[10];
    snprintf(fb, sizeof(fb), "%.2f", f);
    c.drawString(fb, x + 1, ui::HEADER_H + 2);
    c.drawFastVLine(x, kMapTop, kMapBottom - kMapTop, TFT_DARKGREY);
  }
  ui::present();
}

int Waterfall::width() const { return kMapW; }

void Waterfall::addRow(const int16_t* dbm) {
  auto& c = ui::canvas();
  const int y = kMapTop + line_;
  for (int x = 0; x < kMapW; x++) c.drawPixel(kMapX + x, y, heatColor(dbm[x]));
  // Keep the tick marks visible over the heat map.
  for (int i = 0; i < 4; i++) c.drawPixel(kMapX + i * (kMapW / 4), y, TFT_DARKGREY);
  line_++;
  if (line_ >= kMapBottom - kMapTop) line_ = 0;
  ui::present();
}

void Waterfall::setPeak(int dbm, float mhz) {
  auto& c = ui::canvas();
  c.fillRect(kMapX, ui::HEADER_H + 11, kMapW, 9, TFT_BLACK);
  c.setFont(&fonts::Font0);
  c.setTextDatum(top_left);
  c.setTextColor(TFT_YELLOW, TFT_BLACK);
  char mb[28];
  snprintf(mb, sizeof(mb), "%d dBm @ %.3f", dbm, mhz);
  c.drawString(mb, kMapX + 2, ui::HEADER_H + 11);
}

void drawDetect(float mhz, int dbm, bool live) {
  const bool held = mhz > 0;
  ui::drawFrame("Detect Freq", "Del back");
  auto& c = ui::canvas();

  // Peak frequency colour: dim while searching, strength colour when locked,
  // muted while holding the last peak (Unigeek).
  const uint16_t peakColor = !held ? 0x4208 : (live ? detectColor(dbm) : 0x5AEB);

  char fb[16];
  if (held) {
    snprintf(fb, sizeof(fb), "%.3f", mhz);
  } else {
    snprintf(fb, sizeof(fb), "---.---");
  }
  c.setFont(&fonts::Font6);
  c.setTextDatum(middle_center);
  c.setTextColor(peakColor, TFT_BLACK);
  c.drawString(fb, ui::W / 2, 52);

  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.drawString("MHz", ui::W / 2, 80);

  char line[24];
  uint16_t stat;
  if (!held) {
    snprintf(line, sizeof(line), "scanning...");
    stat = TFT_DARKGREY;
  } else if (live) {
    snprintf(line, sizeof(line), "LIVE  %d dBm", dbm);
    stat = TFT_GREEN;
  } else {
    snprintf(line, sizeof(line), "hold  %d dBm", dbm);
    stat = TFT_ORANGE;
  }
  c.setFont(&fonts::Font2);
  c.setTextColor(stat, TFT_BLACK);
  c.drawString(line, ui::W / 2, 94);

  // RSSI bar between -110 and -30 dBm.
  const int barX = 8, barW = ui::W - 16, barY = 106, barH = 7;
  c.drawRect(barX, barY, barW, barH, TFT_DARKGREY);
  if (held) {
    int v = dbm < -110 ? -110 : (dbm > -30 ? -30 : dbm);
    int fillW = (v + 110) * (barW - 2) / 80;
    if (fillW > 0) c.fillRect(barX + 1, barY + 1, fillW, barH - 2, peakColor);
  }
  ui::present();
}

}  // namespace rssichart
