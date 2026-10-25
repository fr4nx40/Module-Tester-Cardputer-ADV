#include "modules/grove2grove.h"

#include <M5Cardputer.h>

#include "core/input.h"
#include "core/log.h"
#include "core/ui.h"

static const char* TAG = "g2g";

namespace {

// Cardputer Grove port: white wire = G1, yellow wire = G2.
// Per M5's example for this unit (CoreS3 Port B): the white-wire pin reads the
// current, the yellow-wire pin switches the power. If your readings or the
// switch seem swapped, swap these two numbers.
constexpr int kPinCurrent = 1;
constexpr int kPinPower = 2;

// The unit outputs 1 V per amp, so 1 mV above the offset is 1 mA.
constexpr float kMilliampPerMillivolt = 1.0f;

constexpr int kSamples = 16;  // readings averaged per measurement

float readMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < kSamples; i++) sum += analogReadMilliVolts(kPinCurrent);
  return (float)sum / kSamples;
}

// Baseline reading taken once on entry with the power off (100 samples).
float calibrateOffset() {
  float sum = 0;
  for (int i = 0; i < 100; i++) {
    sum += readMillivolts();
    delay(2);
  }
  return sum / 100.0f;
}

void setPower(bool on) { digitalWrite(kPinPower, on ? HIGH : LOW); }

void draw(bool powerOn, float mA, float peakMa) {
  ui::drawFrame("Grove2Grove", "Enter power|Del back", powerOn ? "POWER ON" : "POWER OFF");
  auto& c = ui::canvas();

  c.setFont(&fonts::Font4);
  c.setTextDatum(middle_center);
  c.setTextColor(powerOn ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f mA", mA);
  c.drawString(buf, ui::W / 2, 56);

  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.drawString("Peak", 8, 100);
  c.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.1f mA", peakMa);
  c.drawString(buf, 56, 100);
  ui::present();
}

}  // namespace

void runGrove2Grove(const Module& self) {
  (void)self;
  pinMode(kPinPower, OUTPUT);
  setPower(false);
  analogSetPinAttenuation(kPinCurrent, ADC_11db);
  pinMode(kPinCurrent, INPUT);

  delay(100);
  float offset = calibrateOffset();
  LOGI(TAG, "zero offset %.1f mV", offset);

  bool powerOn = false;
  float shown = 0;
  float peak = 0;
  uint32_t lastRead = 0;
  uint32_t lastLog = 0;
  bool dirty = true;

  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;

    bool toggle = (ev.key == Key::Select);

    if (toggle) {
      powerOn = !powerOn;
      setPower(powerOn);
      peak = 0;
      LOGI(TAG, "power %s", powerOn ? "ON" : "OFF");
      if (!powerOn) shown = 0;
      dirty = true;
    }

    uint32_t now = millis();
    if (now - lastRead >= 100) {
      lastRead = now;
      float mA = (readMillivolts() - offset) * kMilliampPerMillivolt;
      if (mA < 0) mA = 0;
      shown = shown * 0.6f + mA * 0.4f;  // smooth the display
      if (shown > peak) peak = shown;
      dirty = true;
    }
    if (now - lastLog >= 1000) {
      lastLog = now;
      LOGI(TAG, "%s  %.1f mA", powerOn ? "on " : "off", shown);
    }

    if (dirty) {
      draw(powerOn, shown, peak);
      dirty = false;
    }
    delay(5);
  }

  setPower(false);
}
