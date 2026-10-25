#include "modules/cc1101.h"

#include <M5Cardputer.h>
#include <string.h>

#include "core/input.h"
#include "core/log.h"
#include "core/modmenu.h"
#include "core/pinfinder.h"
#include "core/pins.h"
#include "core/radiospi.h"
#include "core/rssichart.h"
#include "core/ui.h"
#include "modules/cc1101radio.h"

static const char* TAG = "cc1101";

namespace {

// Defaults are placeholders: set the pins your board uses on the Pins screen
// and they are remembered.
const pins::Def kDefs[] = {
    {"SCK", 40, false},
    {"MISO", 39, false},
    {"MOSI", 14, false},
    {"CS", 13, false},
    {"GDO0", 5, false},
    {"GDO2", pins::kNone, true},
    {"Other CS", 4, true},
};
pins::Config gCfg = {"cc1101", "CC1101 pins", kDefs, 7, {}};

enum Pin { kSck, kMiso, kMosi, kCs, kGdo0, kGdo2, kOther };

struct Freq {
  const char* name;
  float mhz;
  float scanFrom;
  float scanTo;
};
const Freq kFreqs[] = {
    {"315.00", 315.00f, 310.0f, 320.0f},
    {"433.92", 433.92f, 430.0f, 440.0f},
    {"868.35", 868.35f, 863.0f, 870.0f},
    {"915.00", 915.00f, 902.0f, 928.0f},
};
const int kFreqCount = sizeof(kFreqs) / sizeof(kFreqs[0]);
int gFreq = 1;

struct Filter {
  const char* name;
  float bwKhz;
};
const Filter kFilters[] = {{"AM 270", 270.0f}, {"AM 650", 650.0f}};
int gFilter = 0;

// Starts the chip the way Unigeek does. Shows an error and returns false if it
// fails.
bool startRadio(const Module& self, float mhz, float bw, bool ook, float dev) {
  if (gCfg.value[kCs] == pins::kNone || gCfg.value[kGdo0] == pins::kNone) {
    ui::message(self.name, "CS and GDO0 pins not set", "Set them on the Pins screen");
    return false;
  }
  SPIClass& spi = radioSpi(gCfg.value[kSck], gCfg.value[kMiso], gCfg.value[kMosi]);
  holdBusIdle(gCfg.value[kCs], gCfg.value[kGdo0]);
  holdHigh(gCfg.value[kOther]);  // any other chip's CS you've told it about

  int st = cc1101::begin(spi, gCfg.value[kSck], gCfg.value[kMiso], gCfg.value[kMosi],
                         gCfg.value[kCs], gCfg.value[kGdo0], mhz, bw, ook, dev);
  if (st == 0) return true;

  if (st == cc1101::kErrNotFound) {
    // Read the ID once more by hand so the message says what came back.
    int v = cc1101::probe(spi, gCfg.value[kSck], gCfg.value[kMiso], gCfg.value[kMosi],
                          gCfg.value[kCs]);
    LOGW(TAG, "not detected, CS G%d GDO0 G%d, VERSION read 0x%02X", gCfg.value[kCs],
         gCfg.value[kGdo0], v);
    char l2[40];
    if (v == 0x00) {
      snprintf(l2, sizeof(l2), "Read 0x00: check CS, MISO, 3V3");
    } else if (v == 0xFF) {
      snprintf(l2, sizeof(l2), "Read 0xFF: check MISO, power");
    } else {
      snprintf(l2, sizeof(l2), "ID read 0x%02X (want 14)", v);
    }
    ui::message(self.name, "CC1101 Not Detected", l2);
    return false;
  }
  char l2[32];
  snprintf(l2, sizeof(l2), "Error %d", st);
  ui::message(self.name, cc1101::errorText(st), l2);
  return false;
}

// -------------------------------------------------- Waterfall and Detect Freq

// Unigeek's Waterfall: a scrolling heat map of RSSI across the chosen band.
void waterfallScreen(const Module& self) {
  const Freq& fr = kFreqs[gFreq];
  if (!startRadio(self, fr.mhz, kFilters[gFilter].bwKhz, true, 47.6f)) return;

  rssichart::Waterfall wf;
  wf.begin("Waterfall", fr.scanFrom, fr.scanTo);
  cc1101::sweepStart((fr.scanFrom + fr.scanTo) * 0.5f);

  const int w = wf.width();
  const float step = (fr.scanTo - fr.scanFrom) / (float)w;
  static int16_t row[232];
  int maxRssi = -120;
  float maxFreq = fr.scanFrom;
  uint32_t lastPeak = millis();

  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back || ev.key == Key::Select) break;

    int lineMax = -120;
    float lineFreq = fr.scanFrom;
    for (int x = 0; x < w && x < 232; x++) {
      float f = fr.scanFrom + x * step;
      int r = cc1101::rssiAt(f);
      row[x] = (int16_t)r;
      if (r > lineMax) {
        lineMax = r;
        lineFreq = f;
      }
    }
    if (lineMax > maxRssi) {
      maxRssi = lineMax;
      maxFreq = lineFreq;
    }
    if (millis() - lastPeak >= 1500) {
      wf.setPeak(maxRssi, maxFreq);
      maxRssi = -120;
      lastPeak = millis();
    }
    wf.addRow(row);
  }
  cc1101::sweepEnd();
  cc1101::end();
}

// Unigeek's Detect Freq: finds the strongest carrier and shows its frequency.
void detectScreen(const Module& self) {
  if (!startRadio(self, kFreqs[gFreq].mhz, kFilters[gFilter].bwKhz, true, 47.6f)) return;
  cc1101::analyzeStart();
  rssichart::drawDetect(0, -120, false);

  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back || ev.key == Key::Select) break;
    cc1101::analyzeStep();
    rssichart::drawDetect(cc1101::peakMhz(), cc1101::peakRssi(), cc1101::peakLive());
  }
  cc1101::analyzeEnd();
  cc1101::end();
}

// ---------------------------------------------------------------- chip status

// Live raw registers, so a stuck or garbled chip can be seen on the screen.
void statusScreen(const Module& self) {
  const Freq& fr = kFreqs[gFreq];
  if (!startRadio(self, fr.mhz, 270, true, 47.6f)) return;
  cc1101::sweepStart(fr.mhz);
  cc1101::rssiAt(fr.mhz);

  int minRaw = 255, maxRaw = 0;
  uint32_t lastDraw = 0;
  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back || ev.key == Key::Select) break;
    if (millis() - lastDraw < 200) {
      delay(5);
      continue;
    }
    lastDraw = millis();

    cc1101::Status st;
    if (!cc1101::readStatus(st)) break;
    if (st.rssiRaw < minRaw) minRaw = st.rssiRaw;
    if (st.rssiRaw > maxRaw) maxRaw = st.rssiRaw;
    int dbm = st.rssiRaw >= 128 ? (st.rssiRaw - 256) / 2 - 74 : st.rssiRaw / 2 - 74;

    ui::drawFrame("Chip status", "Del back");
    auto& c = ui::canvas();
    c.setFont(&fonts::Font2);
    c.setTextDatum(top_left);
    c.setTextColor(TFT_WHITE, TFT_BLACK);
    char l[48];
    int y = ui::HEADER_H + 2;
    snprintf(l, sizeof(l), "Part %02X  Version %02X (want 14)", st.part, st.version);
    c.drawString(l, 4, y);
    y += 16;
    snprintf(l, sizeof(l), "State %02X %s", st.marc, st.marc == 0x0D ? "RX" : "(want 0D = RX)");
    c.drawString(l, 4, y);
    y += 16;
    snprintf(l, sizeof(l), "RSSI raw %02X = %d dBm", st.rssiRaw, dbm);
    c.drawString(l, 4, y);
    y += 16;
    snprintf(l, sizeof(l), "raw min %02X  max %02X", minRaw, maxRaw);
    c.drawString(l, 4, y);
    y += 16;
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    snprintf(l, sizeof(l), "%s MHz", fr.name);
    c.drawString(l, 4, y);
    ui::present();
  }
  cc1101::sweepEnd();
  cc1101::end();
}

}  // namespace

// ------------------------------------------------------------------- entry

void runCc1101(const Module& self) {
  pins::load(gCfg);

  int sel = 0;
  int top = 0;
  while (true) {
    char pinTag[32];
    pins::summary(gCfg, pinTag, sizeof(pinTag));
    char freqTag[16];
    snprintf(freqTag, sizeof(freqTag), "< %s >", kFreqs[gFreq].name);
    const ui::MenuItem items[] = {
        {"Pins", pinTag},
        {"Frequency", freqTag},
        {"Waterfall", ""},
        {"Detect Freq", ""},
        {"Find chip pins", ""},
        {"Chip status", "raw regs"},
    };
    const int count = sizeof(items) / sizeof(items[0]);

    ui::MenuAction a = ui::menuAction(self.name, items, count, sel, top,
                                      "UpDown move|LeftRight edit|Enter select|Del back");
    if (a.kind == ui::MenuAction::Back) return;

    if (a.kind == ui::MenuAction::Left || a.kind == ui::MenuAction::Right) {
      int dir = a.kind == ui::MenuAction::Right ? 1 : -1;
      if (a.row == 1) gFreq = (gFreq + kFreqCount + dir) % kFreqCount;
      continue;
    }

    switch (a.row) {
      case 0: pins::edit(gCfg); break;
      case 1: gFreq = (gFreq + 1) % kFreqCount; break;
      case 2: waterfallScreen(self); break;
      case 3: detectScreen(self); break;
      case 4:
        pinfinder::run(gCfg.value[kSck], gCfg.value[kMiso], gCfg.value[kMosi], gCfg.value[kCs], gCfg.value[kGdo0]);
        break;
      case 5: statusScreen(self); break;
      default: break;
    }
  }
}
