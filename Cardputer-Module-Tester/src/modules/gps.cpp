#include "modules/gps.h"

#include <M5Cardputer.h>
#include <TinyGPSPlus.h>

#include "core/input.h"
#include "core/log.h"
#include "core/modmenu.h"
#include "core/pins.h"
#include "core/ui.h"

static const char* TAG = "gps";

namespace {

// Cardputer Grove port: white wire = G1, yellow wire = G2.
constexpr int kPinWhite = 1;
constexpr int kPinYellow = 2;

struct LinkConfig {
  int rx;  // Cardputer pin that receives from the GPS unit
  int tx;  // Cardputer pin that sends to the GPS unit (unused by the test)
  uint32_t baud;
  uint32_t format;
  const char* formatName;
};

// Candidates tried during detection. M5's docs are ambiguous about which wire
// is the unit's TX, the older GPS unit's page lists 9600 baud, the v1.1
// (AT6668) is 115200, and some v1.1 units report 7N1. So we try them all.
const LinkConfig kGroveLinks[] = {
    {kPinWhite, kPinYellow, 115200, SERIAL_8N1, "8N1"},
    {kPinYellow, kPinWhite, 115200, SERIAL_8N1, "8N1"},
    {kPinWhite, kPinYellow, 9600, SERIAL_8N1, "8N1"},
    {kPinYellow, kPinWhite, 9600, SERIAL_8N1, "8N1"},
    {kPinWhite, kPinYellow, 115200, SERIAL_7N1, "7N1"},
    {kPinYellow, kPinWhite, 115200, SERIAL_7N1, "7N1"},
};
const int kGroveLinkCount = sizeof(kGroveLinks) / sizeof(kGroveLinks[0]);

// Cap LoRa-1262's GNSS: RX = G15, TX = G13, 115200 baud per M5's docs. The
// reversed pair and 9600 baud are tried too, in case a revision differs.
const LinkConfig kCapLinks[] = {
    {15, 13, 115200, SERIAL_8N1, "8N1"},
    {13, 15, 115200, SERIAL_8N1, "8N1"},
    {15, 13, 9600, SERIAL_8N1, "8N1"},
    {13, 15, 9600, SERIAL_8N1, "8N1"},
};
const int kCapLinkCount = sizeof(kCapLinks) / sizeof(kCapLinks[0]);

// The candidate set in use: Grove port (GPS unit) or the Cap's own pins.
const LinkConfig* gLinks = kGroveLinks;
int gLinkCount = kGroveLinkCount;
const char* gTitle = "GPS";
const char* gNotFoundHint = "Check the unit on Grove";

const uint32_t kProbeMs = 2000;  // listening time per candidate

int gLastGood = -1;  // working candidate, remembered until the next rescan


void openLink(const LinkConfig& l) {
  Serial1.end();
  Serial1.setRxBufferSize(2048);  // must be set before begin()
  Serial1.begin(l.baud, l.format, l.rx, l.tx);
}

// Read everything waiting and feed the NMEA parser. At debug log level
// (APP_LOG_LEVEL=4) every raw NMEA line is also printed to the serial monitor.
void feed(TinyGPSPlus& gps) {
#if APP_LOG_LEVEL >= 4
  static char line[120];
  static size_t len = 0;
#endif
  while (Serial1.available()) {
    int ch = Serial1.read();
    if (ch < 0) break;
    gps.encode((char)ch);
#if APP_LOG_LEVEL >= 4
    if (ch == '\n') {
      line[len] = 0;
      if (len > 0) LOGD(TAG, "%s", line);
      len = 0;
    } else if (ch != '\r' && len < sizeof(line) - 1) {
      line[len++] = (char)ch;
    }
#endif
  }
}

void drawDetect(int n, int total, const LinkConfig& l) {
  ui::drawFrame(gTitle, "Del cancel");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_center);
  c.setTextColor(TFT_WHITE, TFT_BLACK);
  c.drawString("Looking for GPS data...", ui::W / 2, 52);

  char buf[48];
  snprintf(buf, sizeof(buf), "Try %d/%d: RX G%d  %lu  %s", n, total,
           l.rx, (unsigned long)l.baud, l.formatName);
  c.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  c.drawString(buf, ui::W / 2, 76);
  ui::present();
  LOGI(TAG, "probing: %s", buf);
}

enum class DetectResult { Found, NotFound, Cancelled };

// Tries each candidate link until two valid NMEA sentences arrive.
DetectResult detect(TinyGPSPlus& gps, int& foundIdx) {
  int start = gLastGood >= 0 ? gLastGood : 0;
  for (int k = 0; k < gLinkCount; k++) {
    int idx = (start + k) % gLinkCount;
    const LinkConfig& l = gLinks[idx];

    drawDetect(k + 1, gLinkCount, l);
    openLink(l);

    uint32_t good0 = gps.passedChecksum();
    uint32_t t0 = millis();
    while (millis() - t0 < kProbeMs) {
      feed(gps);
      if (inputPoll().key == Key::Back) return DetectResult::Cancelled;
      if (gps.passedChecksum() - good0 >= 2) {
        gLastGood = idx;
        foundIdx = idx;
        LOGI(TAG, "link found: RX G%d %lu %s", l.rx,
             (unsigned long)l.baud, l.formatName);
        return DetectResult::Found;
      }
      delay(5);
    }
  }
  return DetectResult::NotFound;
}

void drawStatus(TinyGPSPlus& gps, const LinkConfig& l) {
  bool fix = gps.location.isValid() && gps.location.age() < 5000;
  ui::drawFrame(gTitle, "Enter rescan|Del back", fix ? "FIX" : "SEARCHING");

  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);

  // label (grey) at lx, value (white) at vx
  auto put = [&](int lx, int vx, int y, const char* label, const char* value) {
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(label, lx, y);
    c.setTextColor(TFT_WHITE, TFT_BLACK);
    c.drawString(value, vx, y);
  };

  const int y0 = ui::HEADER_H + 10;
  const int step = 16;
  const int col2Label = 122;
  const int col2Value = 162;
  char buf[24];

  // Lat / Lon
  if (gps.location.isValid()) {
    snprintf(buf, sizeof(buf), "%.6f", gps.location.lat());
    put(4, 44, y0, "Lat", buf);
    snprintf(buf, sizeof(buf), "%.6f", gps.location.lng());
    put(4, 44, y0 + step, "Lon", buf);
  } else {
    put(4, 44, y0, "Lat", "--");
    put(4, 44, y0 + step, "Lon", "--");
  }

  // Altitude / speed
  if (gps.altitude.isValid()) {
    snprintf(buf, sizeof(buf), "%.1f m", gps.altitude.meters());
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  put(4, 34, y0 + 2 * step, "Alt", buf);

  if (gps.speed.isValid()) {
    snprintf(buf, sizeof(buf), "%.1f km/h", gps.speed.kmph());
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  put(col2Label, col2Value, y0 + 2 * step, "Spd", buf);

  // Satellites / HDOP
  if (gps.satellites.isValid()) {
    snprintf(buf, sizeof(buf), "%u", (unsigned)gps.satellites.value());
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  put(4, 34, y0 + 3 * step, "Sat", buf);

  if (gps.hdop.isValid()) {
    snprintf(buf, sizeof(buf), "%.1f", gps.hdop.hdop());
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  put(col2Label, col2Value, y0 + 3 * step, "HDOP", buf);

  // UTC time / date
  if (gps.time.isValid()) {
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned)gps.time.hour(),
             (unsigned)gps.time.minute(), (unsigned)gps.time.second());
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  put(4, 40, y0 + 4 * step, "UTC", buf);

  if (gps.date.isValid()) {
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", (unsigned)gps.date.year(),
             (unsigned)gps.date.month(), (unsigned)gps.date.day());
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  c.setTextColor(TFT_WHITE, TFT_BLACK);
  c.drawString(buf, col2Label, y0 + 4 * step);

  // Link diagnostics
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  char info[48];
  snprintf(info, sizeof(info), "RX G%d %lu %s  ok %lu bad %lu", l.rx,
           (unsigned long)l.baud, l.formatName,
           (unsigned long)gps.passedChecksum(),
           (unsigned long)gps.failedChecksum());
  c.drawString(info, 4, y0 + 5 * step + 2);

  ui::present();
}

enum class MonitorResult { Back, Rescan };

MonitorResult monitor(TinyGPSPlus& gps, const LinkConfig& l) {
  uint32_t lastDraw = 0;
  while (true) {
    feed(gps);

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) return MonitorResult::Back;
    if (ev.key == Key::Select) {
      gLastGood = -1;  // rescan from the first candidate
      return MonitorResult::Rescan;
    }

    uint32_t now = millis();
    if (now - lastDraw >= 250) {
      drawStatus(gps, l);
      lastDraw = now;
    }
    delay(2);
  }
}

}  // namespace

void runGpsOn(const Module& self) {
  TinyGPSPlus gps;
  gLastGood = -1;

  while (true) {
    int idx = 0;
    DetectResult r = detect(gps, idx);

    if (r == DetectResult::Cancelled) break;
    if (r == DetectResult::NotFound) {
      LOGW(TAG, "no GPS data on any candidate link");
      ui::message(self.name, "No GPS data found", gNotFoundHint);
      break;
    }

    if (monitor(gps, gLinks[idx]) == MonitorResult::Back) break;
  }

  Serial1.end();
}

void runGps(const Module& self) {
  gLinks = kGroveLinks;
  gLinkCount = kGroveLinkCount;
  gTitle = "GPS";
  gNotFoundHint = "Check the unit on Grove";
  runGpsOn(self);
}

void runGpsCap(const Module& self) {
  gLinks = kCapLinks;
  gLinkCount = kCapLinkCount;
  gTitle = "Cap GNSS";
  gNotFoundHint = "Check the Cap is fitted";
  runGpsOn(self);
}

// ------------------------------------------------------- generic GPS module

namespace {

// For a GPS module that is not the M5Stack unit: you choose the pins.
// RX is the Cardputer pin that receives from the GPS module's TX.
const pins::Def kGpsDefs[] = {
    {"RX (GPS TX)", 1, false},
    {"TX (GPS RX)", 2, false},
};
pins::Config gGpsCfg = {"gps", "GPS pins", kGpsDefs, 2, {}};

LinkConfig gCustomLinks[4];
char gCustomHint[32];

}  // namespace

void runGpsPins(const Module& self) {
  pins::load(gGpsCfg);

  int sel = 0;
  int top = 0;
  while (true) {
    char pinTag[24];
    pins::summary(gGpsCfg, pinTag, sizeof(pinTag));
    const ui::MenuItem items[] = {
        {"Pins", pinTag},
        {"GPS test", ""},
    };
    ui::MenuAction a = ui::menuAction(self.name, items, 2, sel, top,
                                      "UpDown move|Enter select|Del back");
    if (a.kind == ui::MenuAction::Back) return;
    if (a.kind != ui::MenuAction::Pick) continue;

    if (a.row == 0) {
      pins::edit(gGpsCfg);
      continue;
    }

    // Try the usual baud rates of generic modules on the chosen pins.
    const uint32_t bauds[4] = {9600, 115200, 38400, 4800};
    for (int i = 0; i < 4; i++) {
      gCustomLinks[i] = {gGpsCfg.value[0], gGpsCfg.value[1], bauds[i], SERIAL_8N1, "8N1"};
    }
    gLinks = gCustomLinks;
    gLinkCount = 4;
    gTitle = "GPS";
    snprintf(gCustomHint, sizeof(gCustomHint), "Check RX G%d / TX G%d", gGpsCfg.value[0],
             gGpsCfg.value[1]);
    gNotFoundHint = gCustomHint;
    runGpsOn(self);
  }
}

// One "GPS" entry in the list: pick the M5Stack unit (fixed Grove pins) or a
// generic GPS on pins you choose.
void runGpsMenu(const Module& self) {
  int sel = 0;
  while (true) {
    const ui::MenuItem items[] = {
        {"M5Stack GPS unit", "Grove"},
        {"Other GPS", "choose pins"},
    };
    sel = ui::menu(self.name, items, 2, sel);
    if (sel < 0) return;
    if (sel == 0) runGps(self);
    if (sel == 1) runGpsPins(self);
  }
}
