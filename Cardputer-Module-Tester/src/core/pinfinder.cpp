#include "core/pinfinder.h"

#include <M5Cardputer.h>
#include <driver/gpio.h>
#include <string.h>

#include "core/input.h"
#include "core/log.h"
#include "core/radiospi.h"
#include "core/ui.h"
#include "modules/cc1101radio.h"

static const char* TAG = "pinfinder";

namespace pinfinder {

namespace {

// GPIOs worth trying. Left out: display, I2C (8, 9), backlight (38), USB and
// flash pins. 12 is the SD card's own chip select.
const int kCandidates[] = {0,  1,  2,  3,  4,  5,  6,  7,  10, 11, 12, 13, 15,
                           16, 17, 18, 21, 41, 42, 43, 44, 45, 46, 47, 48};
const int kCount = sizeof(kCandidates) / sizeof(kCandidates[0]);

bool isCc1101Version(int v) { return v == 0x04 || v == 0x14 || v == 0x07 || v == 0x17; }

// NRF24: write a value to RF_CH and read it back.
bool nrfAnswers(SPIClass& spi, int cs) {
  uint8_t back = 0;
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  delayMicroseconds(5);
  spi.transfer(0x25);  // W_REGISTER RF_CH
  spi.transfer(0x2A);
  digitalWrite(cs, HIGH);
  delayMicroseconds(5);
  digitalWrite(cs, LOW);
  delayMicroseconds(5);
  spi.transfer(0x05);  // R_REGISTER RF_CH
  back = spi.transfer(0xFF);
  digitalWrite(cs, HIGH);
  spi.endTransaction();
  return back == 0x2A;
}

// --- bit-banged SPI (mode 0), for comparing against the hardware bus
int gSck, gMiso, gMosi;

uint8_t bbXfer(uint8_t out) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(gMosi, (out >> i) & 1);
    delayMicroseconds(3);
    digitalWrite(gSck, HIGH);
    delayMicroseconds(3);
    in = (uint8_t)((in << 1) | (digitalRead(gMiso) ? 1 : 0));
    digitalWrite(gSck, LOW);
    delayMicroseconds(3);
  }
  return in;
}

int bitbangVersion(int cs) {
  pinMode(gSck, OUTPUT);
  digitalWrite(gSck, HIGH);  // datasheet reset state: SCLK high, SI low
  pinMode(gMosi, OUTPUT);
  digitalWrite(gMosi, LOW);
  pinMode(gMiso, INPUT);
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  delay(2);
  digitalWrite(cs, LOW);  // reset: CS pulse, then the SRES strobe
  delayMicroseconds(10);
  digitalWrite(cs, HIGH);
  delayMicroseconds(50);
  digitalWrite(cs, LOW);
  delay(1);
  digitalWrite(gSck, LOW);
  bbXfer(0x30);
  delay(2);
  digitalWrite(cs, HIGH);
  delay(1);
  digitalWrite(cs, LOW);
  delay(1);
  bbXfer(0xF1);
  int v = bbXfer(0);
  digitalWrite(cs, HIGH);
  return v;
}

// How many times the MISO line changes level over about 10 ms with no clock
// running. A line that is not driven (or a chip that is glitching) toggles.
int misoToggles(int cs, bool selected) {
  digitalWrite(cs, selected ? LOW : HIGH);
  delayMicroseconds(100);
  int t = 0;
  int prev = gpio_get_level((gpio_num_t)gMiso);
  for (int i = 0; i < 3000; i++) {
    int v = gpio_get_level((gpio_num_t)gMiso);
    if (v != prev) t++;
    prev = v;
    delayMicroseconds(3);
  }
  digitalWrite(cs, HIGH);
  return t;
}

struct Row {
  const char* label;
  char value[28];
  uint16_t color;
};

void draw(const Row* rows, int n) {
  ui::drawFrame("Find chip pins", "Del back", "");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);
  const int y0 = ui::HEADER_H + 10;
  const int step = 16;
  for (int i = 0; i < n; i++) {
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(rows[i].label, 4, y0 + i * step);
    c.setTextColor(rows[i].color, TFT_BLACK);
    c.drawString(rows[i].value, 96, y0 + i * step);
  }
  ui::present();
}

void set(Row& r, const char* label, uint16_t color, const char* fmt, int a = 0, int b = 0) {
  r.label = label;
  r.color = color;
  snprintf(r.value, sizeof(r.value), fmt, a, b);
}

}  // namespace

void run(int sck, int miso, int mosi, int ccCs, int skipPin) {
  SPIClass& spi = radioSpi(sck, miso, mosi);
  gSck = sck;
  gMiso = miso;
  gMosi = mosi;

  Row rows[6];
  int n = 0;
  set(rows[n++], "SPI", TFT_LIGHTGREY, "G%d G%d", sck, miso);
  snprintf(rows[0].value, sizeof(rows[0].value), "G%d G%d G%d", sck, miso, mosi);
  set(rows[n++], "Scanning...", TFT_YELLOW, "");
  draw(rows, n);

  // Every candidate CS goes high first, so only one chip talks at a time.
  int pinsList[kCount];
  int count = 0;
  for (int i = 0; i < kCount; i++) {
    int p = kCandidates[i];
    if (p == sck || p == miso || p == mosi || p == skipPin) continue;  // skipPin: a chip output
    pinsList[count++] = p;
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
  delay(5);

  // MISO level with nothing selected: read through the input register only,
  // so the SPI pin routing is left alone.
  bool misoHigh = gpio_get_level((gpio_num_t)miso);

  int ccPin = -1, ccVer = 0;
  int oddPin = -1, oddVer = 0;  // a pin that answered, but not with a known ID
  int nrfPins[6];
  int nrfCount = 0;
  for (int i = 0; i < count; i++) {
    int p = pinsList[i];
    // Test every pin for both chips, so two pins wired together show up.
    bool nrf = nrfAnswers(spi, p);
    if (nrf && nrfCount < 6) nrfPins[nrfCount++] = p;
    digitalWrite(p, HIGH);
    if (nrf) {
      LOGI(TAG, "G%d: NRF24 answers", p);
      continue;
    }
    int v = cc1101::probe(spi, sck, miso, mosi, p);
    LOGI(TAG, "G%d: CC1101 probe reads 0x%02X", p, v);
    if (ccPin < 0 && isCc1101Version(v)) {
      ccPin = p;
      ccVer = v;
    } else if (oddPin < 0 && v != 0x00 && v != 0xFF) {
      oddPin = p;
      oddVer = v;
    }
    digitalWrite(p, HIGH);
  }
  int nrfPin = nrfCount ? nrfPins[0] : -1;

  // The configured CS, read with the hardware bus and then bit-banged.
  int good = 0, last = -1, bb = -1, togOff = -1, togOn = -1;
  const int kTries = 16;
  if (ccCs >= 0) {
    for (int i = 0; i < count; i++) digitalWrite(pinsList[i], HIGH);
    for (int k = 0; k < kTries; k++) {
      last = cc1101::probe(spi, sck, miso, mosi, ccCs, 100000);
      if (isCc1101Version(last)) good++;
      LOGI(TAG, "G%d read %d: 0x%02X", ccCs, k, last);
      delay(5);
    }
    bb = bitbangVersion(ccCs);
    // Bit-banging took the bus pins over: give them back to the SPI bus.
    spi.end();
    spi.begin(sck, miso, mosi, -1);
    togOff = misoToggles(ccCs, false);
    togOn = misoToggles(ccCs, true);
  }
  LOGI(TAG, "MISO idle %s, CC1101 CS %d (0x%02X), odd %d (0x%02X), NRF24 CSN %d",
       misoHigh ? "high" : "low", ccPin, ccVer, oddPin, oddVer, nrfPin);
  LOGI(TAG, "CS G%d: %d of %d reads gave a CC1101 ID, bit-banged %02X, MISO toggles: CS high %d, CS low %d",
       ccCs, good, kTries, bb, togOff, togOn);

  n = 1;
  set(rows[n++], "MISO idle", TFT_LIGHTGREY, misoHigh ? "high" : "low");
  if (ccPin >= 0) {
    set(rows[n++], "CC1101 CS", TFT_GREEN, "G%d (ID 0x%02X)", ccPin, ccVer);
  } else if (oddPin >= 0) {
    set(rows[n++], "CC1101 CS", TFT_YELLOW, "G%d? reply 0x%02X", oddPin, oddVer);
  } else {
    set(rows[n++], "CC1101 CS", TFT_RED, "not found");
  }
  if (nrfCount > 0) {
    set(rows[n++], "NRF24 CSN", nrfCount == 1 ? TFT_GREEN : TFT_YELLOW, "");
    size_t pos = 0;
    for (int i = 0; i < nrfCount; i++) {
      pos += snprintf(rows[n - 1].value + pos, sizeof(rows[0].value) - pos, "%sG%d", i ? " " : "",
                      nrfPins[i]);
    }
  } else {
    set(rows[n++], "NRF24 CSN", TFT_RED, "not found");
  }
  if (ccCs >= 0) {
    set(rows[n++], "Correct IDs", good == kTries ? TFT_GREEN : TFT_YELLOW, "%d of 16 reads", good);
    set(rows[n++], "MISO toggles", TFT_WHITE, "off %d  on %d", togOff, togOn);
  }
  draw(rows, n);

  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back || ev.key == Key::Select) return;
    delay(10);
  }
}

}  // namespace pinfinder
