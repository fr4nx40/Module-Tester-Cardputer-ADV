#include "modules/lora.h"

#include <M5Cardputer.h>
#include <string.h>

#include "core/input.h"
#include "core/log.h"
#include "core/ui.h"
#include "modules/caplora.h"
#include "modules/gps.h"

static const char* TAG = "lora";

namespace {

// ------------------------------------------------------------------- regions

struct Region {
  const char* label;     // shown in the menu
  cap::Preset plain;     // for your own devices
  cap::Preset mesh;      // Meshtastic LongFast default channel
  float scanFrom;        // RSSI scan range, MHz
  float scanTo;
};

const Region kRegions[] = {
    {"EU 868", cap::kPlain, cap::kMeshtastic, 863.0f, 870.0f},
    {"US 915",
     {"Plain 915.0", 915.0f, 125.0f, 7, 5, 0x12, 8, 10},
     {"Meshtastic US915", 906.875f, 250.0f, 11, 5, 0x2B, 16, 10},
     902.0f, 928.0f},
};
const int kRegionCount = sizeof(kRegions) / sizeof(kRegions[0]);
int gRegion = 0;  // kept until the next power cycle

const Region& region() { return kRegions[gRegion]; }

// Shows why the radio could not start. Returns true if it started fine.
bool radioReady(int code) {
  if (code == cap::kErrNoCap) {
    ui::message("LoRa Cap", "Cap not detected", "Fit the Cap, I2C 0x43");
    return false;
  }
  if (code != 0) {
    char l2[32];
    snprintf(l2, sizeof(l2), "Error %d", code);
    ui::message("LoRa Cap", cap::errorText(code), l2);
    return false;
  }
  return true;
}

void presetLine(const cap::Preset& p, char* out, size_t n) {
  snprintf(out, n, "%.3f MHz  SF%u  BW%.0f", p.freq, (unsigned)p.sf, p.bw);
}

// --------------------------------------------------------------- status check

void statusScreen() {
  const cap::Preset& plain = region().plain;
  bool hasCap = cap::present();
  int st = hasCap ? cap::begin(plain) : cap::kErrNoCap;
  if (st == 0) cap::standby();

  ui::drawFrame("Cap status", "Del back", st == 0 ? "OK" : "FAIL");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);

  const int y0 = ui::HEADER_H + 12;
  const int step = 18;
  auto row = [&](int i, const char* label, const char* value, uint16_t color) {
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(label, 4, y0 + i * step);
    c.setTextColor(color, TFT_BLACK);
    c.drawString(value, 90, y0 + i * step);
  };

  row(0, "I/O expander", hasCap ? "found 0x43" : "missing", hasCap ? TFT_GREEN : TFT_RED);
  char buf[40];
  if (st == 0) {
    row(1, "SX1262", "OK", TFT_GREEN);
  } else if (st == cap::kErrNoCap) {
    row(1, "SX1262", "not tried", TFT_DARKGREY);
  } else {
    snprintf(buf, sizeof(buf), "%s (%d)", cap::errorText(st), st);
    row(1, "SX1262", buf, TFT_RED);
  }
  presetLine(plain, buf, sizeof(buf));
  row(2, "Region", region().label, TFT_WHITE);
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  c.drawString(buf, 8, y0 + 2 * step + 14);
  c.drawString("SPI shared with the SD card (own chip select)", 8, y0 + 3 * step + 10);
  ui::present();

  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Select || ev.key == Key::Back) return;
    delay(10);
  }
}

// ------------------------------------------------------------------ RSSI scan

void scanScreen() {
  if (!radioReady(cap::begin(region().plain))) return;

  const float f0 = region().scanFrom;
  const int n = 57;
  const float step = (region().scanTo - f0) / (n - 1);
  int8_t now[n];
  int8_t peak[n];
  for (int i = 0; i < n; i++) now[i] = peak[i] = -127;

  cap::scanStart(f0);

  const int chartX = 4;
  const int chartW = 232;
  const int chartTop = 22;
  const int chartBottom = 108;  // baseline
  const int chartH = chartBottom - chartTop;
  const int rssiMin = -127;
  const int rssiMax = -30;

  uint32_t lastDraw = 0;
  while (true) {
    for (int i = 0; i < n; i++) {
      float r = cap::scanRssi(f0 + step * i);
      int v = (int)r;
      if (v < rssiMin) v = rssiMin;
      now[i] = (int8_t)v;
      if (now[i] > peak[i]) peak[i] = now[i];

      InputEvent ev = inputPoll();
      if (ev.key == Key::Back) {
        cap::standby();
        return;
      }
      if (ev.key == Key::Select) {
        for (int k = 0; k < n; k++) peak[k] = now[k];
      }
    }

    uint32_t t = millis();
    if (t - lastDraw < 100) continue;
    lastDraw = t;

    int best = 0;
    for (int i = 1; i < n; i++) {
      if (peak[i] > peak[best]) best = i;
    }
    char right[24];
    snprintf(right, sizeof(right), "pk %d @%.1f", (int)peak[best], f0 + step * best);
    ui::drawFrame("RSSI scan", "Enter reset peak|Del back", right);

    auto& c = ui::canvas();
    c.drawFastHLine(chartX, chartBottom, chartW, TFT_DARKGREY);
    for (int i = 0; i < n; i++) {
      int x = chartX + i * chartW / n;
      int w = (chartW / n) - 0;
      if (w < 1) w = 1;
      int hNow = (now[i] - rssiMin) * chartH / (rssiMax - rssiMin);
      int hPk = (peak[i] - rssiMin) * chartH / (rssiMax - rssiMin);
      if (hNow > chartH) hNow = chartH;
      if (hPk > chartH) hPk = chartH;
      if (hNow > 0) c.fillRect(x, chartBottom - hNow, w, hNow, TFT_DARKCYAN);
      if (hPk > 0) c.fillRect(x, chartBottom - hPk, w, 2, TFT_ORANGE);
    }
    c.setFont(&fonts::Font0);
    c.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    c.setTextDatum(top_left);
    char lab[16];
    snprintf(lab, sizeof(lab), "%.0f", region().scanFrom);
    c.drawString(lab, chartX, chartBottom + 4);
    c.setTextDatum(top_center);
    snprintf(lab, sizeof(lab), "%.1f", (region().scanFrom + region().scanTo) / 2);
    c.drawString(lab, chartX + chartW / 2, chartBottom + 4);
    c.setTextDatum(top_right);
    snprintf(lab, sizeof(lab), "%.0f MHz", region().scanTo);
    c.drawString(lab, chartX + chartW, chartBottom + 4);
    ui::present();
  }
}

// -------------------------------------------------------------------- receive

struct Rx {
  int rssi = 0;
  float snr = 0;
  size_t len = 0;
  bool crcBad = false;
  char text[34] = "";
  uint32_t number = 0;
};

// Printable text if most bytes are, otherwise the first bytes in hex.
void previewPayload(const uint8_t* d, size_t len, char* out, size_t outSize) {
  size_t printable = 0;
  for (size_t i = 0; i < len; i++) {
    if (d[i] >= 32 && d[i] < 127) printable++;
  }
  if (len > 0 && printable * 10 >= len * 7) {
    size_t n = len < outSize - 1 ? len : outSize - 1;
    for (size_t i = 0; i < n; i++) out[i] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.';
    out[n] = 0;
  } else {
    size_t pos = 0;
    for (size_t i = 0; i < len && i < 10 && pos + 3 < outSize; i++) {
      pos += snprintf(out + pos, outSize - pos, "%02X ", d[i]);
    }
    out[pos] = 0;
  }
}

void drawReceive(const cap::Preset& p, const Rx* log, int logCount, uint32_t total) {
  char right[16];
  snprintf(right, sizeof(right), "%lu rx", (unsigned long)total);
  char title[32];
  snprintf(title, sizeof(title), "RX %s", p.name);
  ui::drawFrame(title, "Del back", right);

  auto& c = ui::canvas();
  char line[48];
  presetLine(p, line, sizeof(line));
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.setTextDatum(top_left);
  c.drawString(line, 4, ui::HEADER_H + 3);

  if (logCount == 0) {
    c.setFont(&fonts::Font2);
    c.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    c.setTextDatum(middle_center);
    c.drawString("Awaiting packet...", ui::W / 2, 68);
    ui::present();
    return;
  }

  const int top = ui::HEADER_H + 14;
  const int entryH = 29;
  for (int i = 0; i < logCount; i++) {
    const Rx& r = log[i];
    int y = top + i * entryH;
    c.setFont(&fonts::Font2);
    c.setTextDatum(top_left);
    c.setTextColor(i == 0 ? TFT_WHITE : TFT_LIGHTGREY, TFT_BLACK);
    snprintf(line, sizeof(line), "#%lu  %d dBm  %.1f dB  %uB%s", (unsigned long)r.number,
             r.rssi, r.snr, (unsigned)r.len, r.crcBad ? "  CRC!" : "");
    c.drawString(line, 4, y);
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.setFont(&fonts::Font0);
    c.drawString(r.text, 4, y + 17);
  }
  ui::present();
}

void receiveScreen(const cap::Preset& p) {
  if (!radioReady(cap::begin(p))) return;
  cap::rxStart();

  Rx log[3];
  int logCount = 0;
  uint32_t total = 0;
  bool dirty = true;

  while (true) {
    if (cap::rxReady()) {
      uint8_t buf[256];
      size_t len = 0;
      float rssi = 0;
      float snr = 0;
      int st = cap::rxRead(buf, sizeof(buf), len, rssi, snr);
      if (st == 0 || st == cap::kErrCrc) {
        total++;
        for (int i = 2; i > 0; i--) log[i] = log[i - 1];
        Rx& r = log[0];
        r.rssi = (int)rssi;
        r.snr = snr;
        r.len = len;
        r.crcBad = (st != 0);
        r.number = total;
        previewPayload(buf, len, r.text, sizeof(r.text));
        if (logCount < 3) logCount++;
        LOGI(TAG, "rx #%lu %d dBm %.1f dB %u bytes%s", (unsigned long)total, r.rssi, r.snr,
             (unsigned)len, r.crcBad ? " CRC" : "");
        dirty = true;
      } else {
        LOGW(TAG, "rx read error %d", st);
      }
    }

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;

    if (dirty) {
      drawReceive(p, log, logCount, total);
      dirty = false;
    }
    delay(5);
  }
  cap::standby();
}

// ------------------------------------------------------------------- transmit

void drawTransmit(const cap::Preset& p, uint32_t sent, int lastStatus, uint32_t lastMs,
                  bool autoMode) {
  ui::drawFrame("LoRa transmit", "Enter send|A auto|Del back", autoMode ? "AUTO" : "MANUAL");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);

  const int y0 = ui::HEADER_H + 12;
  const int step = 18;
  char buf[48];
  auto row = [&](int i, const char* label, const char* value, uint16_t color) {
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(label, 4, y0 + i * step);
    c.setTextColor(color, TFT_BLACK);
    c.drawString(value, 56, y0 + i * step);
  };

  presetLine(p, buf, sizeof(buf));
  row(0, "Setup", buf, TFT_WHITE);
  snprintf(buf, sizeof(buf), "%d dBm", (int)p.power);
  row(1, "Power", buf, TFT_WHITE);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)sent);
  row(2, "Sent", buf, TFT_WHITE);
  if (lastStatus == 1) {
    row(3, "Last", "none yet", TFT_DARKGREY);
  } else if (lastStatus == 0) {
    snprintf(buf, sizeof(buf), "OK  %lu ms on air", (unsigned long)lastMs);
    row(3, "Last", buf, TFT_GREEN);
  } else {
    snprintf(buf, sizeof(buf), "%s (%d)", cap::errorText(lastStatus), lastStatus);
    row(3, "Last", buf, TFT_RED);
  }
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.drawString("Another LoRa device required", 4, y0 + 4 * step + 4);
  ui::present();
}

void transmitScreen() {
  const cap::Preset& p = region().plain;
  if (!radioReady(cap::begin(p))) return;

  uint32_t sent = 0;
  int lastStatus = 1;  // 1 = nothing sent yet (RadioLib codes are 0 or negative)
  uint32_t lastMs = 0;
  bool autoMode = false;
  uint32_t lastSend = 0;
  bool dirty = true;

  auto sendOne = [&]() {
    char msg[24];
    snprintf(msg, sizeof(msg), "M5TEST #%lu", (unsigned long)(sent + 1));
    uint32_t t0 = millis();
    int st = cap::tx((const uint8_t*)msg, strlen(msg));
    lastMs = millis() - t0;
    lastStatus = st;
    if (st == 0) sent++;
    lastSend = millis();
    LOGI(TAG, "tx '%s' -> %d (%lu ms)", msg, st, (unsigned long)lastMs);
    dirty = true;
  };

  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;
    if (ev.key == Key::Select) sendOne();
    if (ev.key == Key::Char && (ev.ch == 'a' || ev.ch == 'A')) {
      autoMode = !autoMode;
      lastSend = millis();
      dirty = true;
    }
    if (autoMode && millis() - lastSend >= 3000) sendOne();

    if (dirty) {
      drawTransmit(p, sent, lastStatus, lastMs, autoMode);
      dirty = false;
    }
    delay(5);
  }
  cap::standby();
}

// ----------------------------------------------------------------------- GNSS

void gnssScreen(const Module& self) {
  if (!cap::present()) {
    ui::message("LoRa Cap", "Cap not detected", "Fit the Cap, I2C 0x43");
    return;
  }
  cap::power();
  delay(50);
  runGpsCap(self);
}

}  // namespace

// ------------------------------------------------------------------- entry

void runLoraCap(const Module& self) {
  char regionTag[24];
  char scanTag[16];
  char meshTag[16];
  char plainTag[16];

  int sel = 0;
  int top = 0;
  while (true) {
    const Region& r = region();
    snprintf(regionTag, sizeof(regionTag), "< %s >", r.label);
    snprintf(scanTag, sizeof(scanTag), "%.0f-%.0f", r.scanFrom, r.scanTo);
    snprintf(meshTag, sizeof(meshTag), "%.3f", r.mesh.freq);
    snprintf(plainTag, sizeof(plainTag), "%.3f", r.plain.freq);
    const ui::MenuItem items[] = {
        {"Region", regionTag},
        {"Status check", "Cap"},
        {"RSSI scan", scanTag},
        {"Receive: Meshtastic", meshTag},
        {"Receive: plain", plainTag},
        {"Transmit", plainTag},
        {"GNSS", "UART"},
    };
    const int count = sizeof(items) / sizeof(items[0]);

    ui::drawMenu("LoRa Cap", items, count, sel, top,
                 "UpDown move|LeftRight edit|Enter select|Del back");

    int pick = -1;
    bool redraw = false;
    while (!redraw) {
      InputEvent ev = inputPoll();
      switch (ev.key) {
        case Key::Up:
          sel = (sel + count - 1) % count;
          redraw = true;
          break;
        case Key::Down:
          sel = (sel + 1) % count;
          redraw = true;
          break;
        case Key::Left:
        case Key::Right:
          if (sel == 0) {
            gRegion = (gRegion + (ev.key == Key::Right ? 1 : kRegionCount - 1)) % kRegionCount;
            redraw = true;
          }
          break;
        case Key::Select:
          if (sel == 0) {
            gRegion = (gRegion + 1) % kRegionCount;
            redraw = true;
          } else {
            pick = sel;
            redraw = true;
          }
          break;
        case Key::Back:
          return;
        default:
          break;
      }
      delay(10);
    }
    top = ui::keepVisible(sel, top);

    switch (pick) {
      case 1: statusScreen(); break;
      case 2: scanScreen(); break;
      case 3: receiveScreen(region().mesh); break;
      case 4: receiveScreen(region().plain); break;
      case 5: transmitScreen(); break;
      case 6: gnssScreen(self); break;
      default: break;
    }
  }
}
