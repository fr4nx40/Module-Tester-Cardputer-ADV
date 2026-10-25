#include "modules/nrf24.h"

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
#include "modules/nrf24drv.h"

static const char* TAG = "nrf24";

namespace {

// Defaults for GPIO board (CE G6, CSN G4). Change them on the Pins
// screen and they are remembered.
const pins::Def kDefs[] = {
    {"SCK", 40, false},
    {"MISO", 39, false},
    {"MOSI", 14, false},
    {"CE", 6, false},
    {"CSN", 4, false},
    {"Other CS", 13, true},
};
pins::Config gCfg = {"nrf24", "NRF24L01 pins", kDefs, 6, {}};

enum Pin { kSck, kMiso, kMosi, kCsn, kCe, kOther };

uint8_t gChannel = 76;
nrf24::Rate gRate = nrf24::Rate::k2M;
int gAddr = 0;

const uint8_t kAddrs[3][5] = {
    {0xE7, 0xE7, 0xE7, 0xE7, 0xE7},
    {0xC2, 0xC2, 0xC2, 0xC2, 0xC2},
    {'0', '0', '0', '0', '1'},
};
const char* const kAddrNames[3] = {"E7E7E7E7E7", "C2C2C2C2C2", "\"00001\""};

// Starts the chip with the chosen pins and settings. Shows an error and
// returns false if it does not answer.
bool startRadio(const Module& self) {
  SPIClass& spi = radioSpi(gCfg.value[kSck], gCfg.value[kMiso], gCfg.value[kMosi]);
  holdBusIdle(gCfg.value[kCsn], gCfg.value[kCe]);
  holdHigh(gCfg.value[kOther]);  // any other chip's CS you've told it about
  if (!nrf24::begin(spi, gCfg.value[kCsn], gCfg.value[kCe])) {
    uint8_t v = nrf24::lastRead();
    LOGW(TAG, "chip not detected (CSN G%d, CE G%d) CONFIG read 0x%02X", gCfg.value[kCsn],
         gCfg.value[kCe], v);
    char l2[40];
    if (v == 0x00) {
      snprintf(l2, sizeof(l2), "Read 0x00: check CSN, MISO, 3V3");
    } else if (v == 0xFF) {
      snprintf(l2, sizeof(l2), "Read 0xFF: check MISO, power");
    } else {
      snprintf(l2, sizeof(l2), "Read 0x%02X: add 10uF on VCC", v);
    }
    ui::message(self.name, "NRF24L01 Not Detected", l2);
    return false;
  }
  nrf24::configure(gChannel, gRate, kAddrs[gAddr]);
  return true;
}

void waitBack() {
  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back || ev.key == Key::Select) return;
    delay(10);
  }
}

// ---------------------------------------------------------------- chip status

void statusScreen(const Module& self) {
  if (!startRadio(self)) return;
  nrf24::Info inf = nrf24::info();

  ui::drawFrame("NRF24L01 status", "Del back", "OK");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);
  const int y0 = ui::HEADER_H + 12;
  const int step = 18;
  auto row = [&](int i, const char* label, const char* value, uint16_t color) {
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(label, 4, y0 + i * step);
    c.setTextColor(color, TFT_BLACK);
    c.drawString(value, 84, y0 + i * step);
  };
  char buf[40];
  row(0, "Chip", inf.plus ? "nRF24L01+" : "nRF24L01 (no 250k)", TFT_GREEN);
  row(1, "SPI", "OK", TFT_GREEN);
  snprintf(buf, sizeof(buf), "%u  (%u MHz)", (unsigned)inf.channel, 2400u + inf.channel);
  row(2, "Channel", buf, TFT_WHITE);
  row(3, "Data rate", nrf24::rateName(gRate), TFT_WHITE);
  snprintf(buf, sizeof(buf), "CONFIG 0x%02X  RF_SETUP 0x%02X", inf.config, inf.rfSetup);
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.drawString(buf, 4, y0 + 4 * step + 2);
  ui::present();

  nrf24::end();
  waitBack();
}

// ----------------------------------------------------------- 2.4 GHz spectrum

// A bar jumps to full when a carrier is heard on its channel and then falls
// by half on every sweep it is not. "Max=" names the strongest channel.
void scanScreen(const Module& self) {
  if (!startRadio(self)) return;

  const int n = 126;
  uint8_t bar[n];
  for (int i = 0; i < n; i++) bar[i] = 0;
  nrf24::scanStart();

  uint32_t lastDraw = 0;
  while (true) {
    for (int ch = 0; ch < n; ch++) {
      if (nrf24::carrierAt((uint8_t)ch)) {
        bar[ch] = 100;
      } else {
        bar[ch] = bar[ch] - bar[ch] / 2;
        if (bar[ch] < 3) bar[ch] = 0;
      }
    }

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;

    uint32_t t = millis();
    if (t - lastDraw < 100) continue;
    lastDraw = t;

    int best = 0;
    for (int i = 1; i < n; i++) {
      if (bar[i] > bar[best]) best = i;
    }
    char maxText[24] = "";
    if (bar[best] > 30) snprintf(maxText, sizeof(maxText), "Max=%d", 2400 + best);
    rssichart::drawBars("2.4G Spectrum", "Del back", "", bar, n, "2400", "2525", maxText);
  }
  nrf24::scanStop();
  nrf24::end();
}

}  // namespace

// ------------------------------------------------------------------- entry

void runNrf24(const Module& self) {
  pins::load(gCfg);

  int sel = 0;
  int top = 0;
  char pinTag[28];
  while (true) {
    pins::summary(gCfg, pinTag, sizeof(pinTag));
    const ui::MenuItem items[] = {
        {"Pins", pinTag},
        {"Chip status", ""},
        {"2.4G Spectrum", "ch 0-125"},
        {"Find chip pins", ""},
    };
    ui::MenuAction a = ui::menuAction(self.name, items, 4, sel, top,
                                      "UpDown move|Enter select|Del back");
    if (a.kind == ui::MenuAction::Back) return;
    if (a.kind != ui::MenuAction::Pick) continue;

    switch (a.row) {
      case 0: pins::edit(gCfg); break;
      case 1: statusScreen(self); break;
      case 2: scanScreen(self); break;
      case 3:
        pinfinder::run(gCfg.value[kSck], gCfg.value[kMiso], gCfg.value[kMosi],
                       gCfg.value[kOther], 5);
        break;
      default: break;
    }
  }
}
