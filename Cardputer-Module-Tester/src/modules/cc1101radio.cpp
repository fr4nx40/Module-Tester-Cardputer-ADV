#include "modules/cc1101radio.h"

#include <Arduino.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <driver/gpio.h>

// Ported from Unigeek's CC1101Util (firmware/src/utils/rf/CC1101Util.cpp): same
// library, same start-up order, same OOK receive registers, same sweep timing.

namespace cc1101 {

namespace {

int gCs = -1;
int gGdo0 = -1;
bool gOok = true;
float gDevKhz = 47.6f;
float gRxBw = 270.0f;
bool gReady = false;

bool freqOk(float mhz) {
  return (mhz >= 300.0f && mhz <= 348.0f) || (mhz >= 387.0f && mhz <= 464.0f) ||
         (mhz >= 779.0f && mhz <= 928.0f);
}

// OOK receive sensitivity registers, from Unigeek (originally Bruce). Init()
// resets them to FSK packet defaults, so they are applied after every Init().
void applyOokRxRegs() {
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_FIFOTHR, 0x47);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_FREND1, 0xB6);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_FOCCFG, 0x18);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL2, 0x03);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL1, 0x00);
  ELECHOUSE_cc1101.SpiWriteReg(CC1101_AGCCTRL0, 0x40);
}

// ---- Frequency detect state
const float kFreqList[] = {
    300.000f, 303.875f, 303.900f, 304.250f, 307.000f, 307.500f, 312.000f, 313.000f,
    314.000f, 315.000f, 318.000f, 330.000f, 345.000f, 348.000f, 387.000f, 390.000f,
    418.000f, 430.000f, 431.000f, 433.075f, 433.220f, 433.420f, 433.657f, 433.889f,
    433.920f, 434.075f, 434.390f, 434.420f, 434.775f, 438.900f, 440.175f, 464.000f,
    868.350f, 868.400f, 868.800f, 868.950f, 906.400f, 915.000f, 925.000f, 928.000f,
};
constexpr int kFreqCount = sizeof(kFreqList) / sizeof(kFreqList[0]);
constexpr int kRssiThreshold = -65;   // dBm: a signal is present above this
constexpr int kAnalyzerHold = 16;     // frames to hold the peak after it stops
constexpr uint16_t kSweepSettleUs = 1500;

float gPeakMhz = 0;
int gPeakRssi = -120;
bool gPeakLive = false;
int gHold = 0;

bool validForSweep(float mhz) {
  return (mhz >= 280 && mhz <= 350) || (mhz >= 387 && mhz <= 468) || (mhz >= 779 && mhz <= 928);
}

// Retunes, re-enters RX so the VCO recalibrates at this frequency, waits for the
// PLL and AGC to settle and reads RSSI (Unigeek _tunedRssi).
int tunedRssi(float mhz) {
  ELECHOUSE_cc1101.setMHZ(mhz);
  ELECHOUSE_cc1101.SetRx();
  delayMicroseconds(kSweepSettleUs);
  return ELECHOUSE_cc1101.getRssi();
}

// ---- capture state, shared with the interrupt
constexpr uint32_t kGapUs = 30000;  // silence that ends a burst
constexpr size_t kMinPulses = 24;
volatile uint16_t gBuf[kMaxPulses];
volatile size_t gN = 0;
volatile uint32_t gLast = 0;
volatile bool gActive = false;
uint32_t gHeard = 0, gRejected = 0;
size_t gLastBurst = 0;

void IRAM_ATTR onEdge() {
  uint32_t now = micros();
  uint32_t dt = now - gLast;
  gLast = now;
  if (!gActive) {
    if (gpio_get_level((gpio_num_t)gGdo0)) {  // a burst starts with a rising edge
      gActive = true;
      gN = 0;
    }
    return;
  }
  if (gN < kMaxPulses) gBuf[gN++] = dt > 65535 ? 65535 : (uint16_t)dt;
}

}  // namespace

int begin(SPIClass& spi, int sck, int miso, int mosi, int cs, int gdo0, float mhz,
          float rxBwKhz, bool ook, float devKhz) {
  if (!freqOk(mhz)) return kErrBadFreq;
  gCs = cs;
  gGdo0 = gdo0;
  gOok = ook;
  gDevKhz = devKhz;
  gRxBw = rxBwKhz;
  gReady = false;
  gHeard = gRejected = 0;
  gLastBurst = 0;

  // Unigeek CC1101Util::begin(), step by step.
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  spi.begin(sck, miso, mosi, -1);

  // The SmartRC library waits for the chip with no time limit, so with no
  // module fitted Init() would never return. Check with our own timed read
  // first: an absent chip reads 0x00 or 0xFF.
  int seen = probe(spi, sck, miso, mosi, cs, 100000);
  if (seen == 0x00 || seen == 0xFF) return kErrNotFound;

  ELECHOUSE_cc1101.setSPIinstance(&spi);
  ELECHOUSE_cc1101.setSpiPin(sck, miso, mosi, cs);
  ELECHOUSE_cc1101.setGDO0(gdo0);
  ELECHOUSE_cc1101.Init();
  if (!ELECHOUSE_cc1101.getCC1101()) return kErrNotFound;

  ELECHOUSE_cc1101.setRxBW(256);
  ELECHOUSE_cc1101.setClb(1, 13, 15);
  ELECHOUSE_cc1101.setClb(2, 16, 19);
  ELECHOUSE_cc1101.setModulation(2);  // ASK/OOK
  ELECHOUSE_cc1101.setDRate(50);
  ELECHOUSE_cc1101.setPktFormat(3);  // async serial
  applyOokRxRegs();
  ELECHOUSE_cc1101.setMHZ(mhz);
  gReady = true;
  return 0;
}

int probe(SPIClass& spi, int sck, int miso, int mosi, int cs, uint32_t hz) {
  // Hardware SPI only (never drive the bus pins by hand: that detaches them
  // from the SPI peripheral). CS pulse, wait for SO low, SRES, wait, read.
  auto ready = [&]() {
    uint32_t t = micros();
    while (gpio_get_level((gpio_num_t)miso)) {
      if (micros() - t > 20000) return false;
    }
    return true;
  };

  spi.begin(sck, miso, mosi, -1);
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  delay(2);
  spi.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  delayMicroseconds(10);
  digitalWrite(cs, HIGH);
  delayMicroseconds(60);
  digitalWrite(cs, LOW);
  ready();
  spi.transfer(0x30);  // SRES
  ready();             // reset finished
  digitalWrite(cs, HIGH);
  delayMicroseconds(100);

  digitalWrite(cs, LOW);
  ready();
  spi.transfer(0xF1);  // VERSION status register, burst bit set
  uint8_t v = spi.transfer(0);
  digitalWrite(cs, HIGH);
  spi.endTransaction();
  return v;
}

void end() {
  if (gGdo0 >= 0) detachInterrupt(gGdo0);
  if (gReady) ELECHOUSE_cc1101.setSidle();
  if (gCs >= 0) digitalWrite(gCs, HIGH);
  gReady = false;
}

// ------------------------------------------------------------------ waterfall

void sweepStart(float calibMhz) {
  if (!gReady) return;
  ELECHOUSE_cc1101.setRxBW(200);
  // SetRx() triggers the VCO calibration at the frequency that is loaded, so
  // load the middle of the swept band first.
  if (calibMhz > 0) ELECHOUSE_cc1101.setMHZ(calibMhz);
  ELECHOUSE_cc1101.SetRx();
  delayMicroseconds(kSweepSettleUs);
}

int rssiAt(float mhz) {
  if (!gReady) return -120;
  ELECHOUSE_cc1101.setMHZ(mhz);
  delayMicroseconds(250);  // PLL relock + AGC settle; shorter smears signals
  return ELECHOUSE_cc1101.getRssi();
}

void sweepEnd() {
  if (gReady) ELECHOUSE_cc1101.setSidle();
}

// ------------------------------------------------------------- frequency detect

void analyzeStart() {
  gPeakMhz = 0;
  gPeakRssi = -120;
  gPeakLive = false;
  gHold = 0;
  if (!gReady) return;
  ELECHOUSE_cc1101.SetRx();
}

bool analyzeStep() {
  if (!gReady) return false;

  // Stage 1: coarse sweep of the whole list, strongest channel wins.
  int coarseRssi = -127;
  float coarseFreq = 0;
  for (int i = 0; i < kFreqCount; i++) {
    int r = tunedRssi(kFreqList[i]);
    if (r > coarseRssi) {
      coarseRssi = r;
      coarseFreq = kFreqList[i];
    }
  }

  // Stage 2: refine +/-0.3 MHz around it in 20 kHz steps.
  if (coarseRssi > kRssiThreshold) {
    int fineRssi = -127;
    float fineFreq = coarseFreq;
    for (float f = coarseFreq - 0.30f; f <= coarseFreq + 0.3001f; f += 0.02f) {
      if (!validForSweep(f)) continue;
      int r = tunedRssi(f);
      if (r > fineRssi) {
        fineRssi = r;
        fineFreq = f;
      }
    }
    gPeakMhz = fineFreq;
    gPeakRssi = fineRssi;
    gPeakLive = true;
    gHold = kAnalyzerHold;
    return true;
  }

  // No live signal: hold the last peak for a while, then clear it.
  if (gHold > 0) {
    gHold--;
    gPeakLive = false;
    return true;
  }
  gPeakMhz = 0;
  gPeakRssi = -120;
  gPeakLive = false;
  return false;
}

void analyzeEnd() {
  if (gReady) ELECHOUSE_cc1101.setSidle();
}

float peakMhz() { return gPeakMhz; }
int peakRssi() { return gPeakRssi; }
bool peakLive() { return gPeakLive; }

// ------------------------------------------------------------ capture / replay

bool captureStart() {
  if (!gReady || gGdo0 < 0) return false;
  detachInterrupt(gGdo0);
  ELECHOUSE_cc1101.setSidle();
  // Receive profile for fixed-frequency capture (Unigeek _initRx).
  ELECHOUSE_cc1101.setModulation(gOok ? 2 : 0);
  if (!gOok) ELECHOUSE_cc1101.setDeviation(gDevKhz);
  ELECHOUSE_cc1101.setRxBW(gRxBw);
  ELECHOUSE_cc1101.setDRate(5);
  ELECHOUSE_cc1101.setPktFormat(3);
  ELECHOUSE_cc1101.SetRx();
  pinMode(gGdo0, INPUT);
  gActive = false;
  gN = 0;
  gLast = micros();
  attachInterrupt(digitalPinToInterrupt(gGdo0), onEdge, CHANGE);
  return true;
}

size_t captureTake(uint16_t* out, size_t maxOut) {
  if (!gActive) return 0;
  bool full = gN >= kMaxPulses;
  bool gap = (uint32_t)(micros() - gLast) > kGapUs;
  if (!full && !gap) return 0;

  noInterrupts();
  size_t n = gN;
  if (n > maxOut) n = maxOut;
  for (size_t i = 0; i < n; i++) out[i] = gBuf[i];
  gActive = false;
  gN = 0;
  interrupts();

  gHeard++;
  gLastBurst = n;
  if (full) { gRejected++; return 0; }  // a flood of edges is noise, not a burst
  if (n < kMinPulses) { gRejected++; return 0; }

  // Real remotes do not use huge numbers of very short pulses.
  size_t shortCount = 0;
  for (size_t i = 0; i < n; i++) {
    if (out[i] < 80) shortCount++;
  }
  if (shortCount * 3 > n) { gRejected++; return 0; }
  return n;
}

uint32_t burstsHeard() { return gHeard; }
uint32_t burstsRejected() { return gRejected; }
size_t lastBurstPulses() { return gLastBurst; }
int liveRssi() { return gReady ? ELECHOUSE_cc1101.getRssi() : -128; }

void captureStop() {
  if (gGdo0 >= 0) detachInterrupt(gGdo0);
}

void replay(const uint16_t* us, size_t n, int repeats) {
  if (!gReady || gGdo0 < 0) return;
  detachInterrupt(gGdo0);
  ELECHOUSE_cc1101.setSidle();
  // Transmit profile (Unigeek _initTx): async serial, full power, data on GDO0.
  ELECHOUSE_cc1101.setModulation(gOok ? 2 : 0);
  if (!gOok) ELECHOUSE_cc1101.setDeviation(gDevKhz);
  ELECHOUSE_cc1101.setPktFormat(3);
  ELECHOUSE_cc1101.setPA(12);
  ELECHOUSE_cc1101.SetTx();
  pinMode(gGdo0, OUTPUT);
  digitalWrite(gGdo0, LOW);

  for (int r = 0; r < repeats; r++) {
    for (size_t i = 0; i < n; i++) {
      digitalWrite(gGdo0, (i & 1) ? LOW : HIGH);  // even entries are marks
      delayMicroseconds(us[i]);
    }
    digitalWrite(gGdo0, LOW);
    delay(15);
  }

  digitalWrite(gGdo0, LOW);
  ELECHOUSE_cc1101.setSidle();
  pinMode(gGdo0, INPUT);
}

bool readStatus(Status& o) {
  if (!gReady) return false;
  o.part = ELECHOUSE_cc1101.SpiReadStatus(0x30);
  o.version = ELECHOUSE_cc1101.SpiReadStatus(0x31);
  o.lqi = ELECHOUSE_cc1101.SpiReadStatus(0x33);
  o.rssiRaw = ELECHOUSE_cc1101.SpiReadStatus(0x34);
  o.marc = ELECHOUSE_cc1101.SpiReadStatus(0x35);
  o.pkt = ELECHOUSE_cc1101.SpiReadStatus(0x38);
  return true;
}

const char* errorText(int code) {
  switch (code) {
    case kErrNotFound: return "CC1101 not detected";
    case kErrBadFreq: return "Frequency out of range";
    default: return "Radio error";
  }
}

}  // namespace cc1101