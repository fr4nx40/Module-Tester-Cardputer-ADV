#pragma once
// CC1101 driver for the tester, built on the same SmartRC library (Bruce fork)
// and the same start-up sequence that Unigeek uses on the Cardputer ADV:
//   CS high -> SPI.begin(sck, miso, mosi, no hardware CS) -> setSPIinstance ->
//   setSpiPin -> setGDO0 -> Init() -> getCC1101()
// followed by Unigeek's OOK receive profile.
//
// Every function is plain C++ (no library types in this header).
#include <SPI.h>
#include <stddef.h>
#include <stdint.h>

namespace cc1101 {

constexpr int kErrNotFound = -2;  // the chip did not answer
constexpr int kErrBadFreq = -4;   // outside 300-348, 387-464, 779-928 MHz

// Largest capture, in pulses (alternating mark / space, mark first).
constexpr size_t kMaxPulses = 1500;

// Starts the SPI bus and the chip. Returns 0, kErrNotFound or kErrBadFreq.
//   rxBwKhz  receive filter used for capture (270 or 650 are the usual picks)
//   ook      true = OOK/ASK, false = 2-FSK
int begin(SPIClass& spi, int sck, int miso, int mosi, int cs, int gdo0, float mhz,
          float rxBwKhz, bool ook, float devKhz = 47.6f);
void end();

// Plain VERSION register read for diagnostics: 0x14 or 0x04 is a CC1101, 0x00
// means no reply, 0xFF means MISO floating. Used only to explain a failure.
int probe(SPIClass& spi, int sck, int miso, int mosi, int cs, uint32_t hz = 100000);

// ---- Waterfall sweep (Unigeek "Waterfall") ----
// sweepStart() calibrates once at the middle of the band, then rssiAt() only
// retunes (fast). sweepEnd() puts the chip back to idle.
void sweepStart(float calibMhz);
int rssiAt(float mhz);
void sweepEnd();

// ---- Frequency detect (Unigeek "Detect Freq") ----
// Sweeps the usual remote frequencies, then refines around the strongest one.
// analyzeStep() returns true while there is something to show (a live peak, or
// the held last peak).
void analyzeStart();
bool analyzeStep();
void analyzeEnd();
float peakMhz();   // 0 when nothing has been seen
int peakRssi();    // dBm
bool peakLive();   // true while the carrier is present now

// ---- Capture / replay on GDO0 ----
bool captureStart();
size_t captureTake(uint16_t* out, size_t maxOut);
void captureStop();
// Counters since begin(): every burst that ended, how many were thrown away as
// noise, the pulse count of the latest burst, and the live signal strength.
uint32_t burstsHeard();
uint32_t burstsRejected();
size_t lastBurstPulses();
int liveRssi();
void replay(const uint16_t* us, size_t n, int repeats);

// Raw chip registers for the "Chip status" screen.
struct Status {
  uint8_t part, version, marc, rssiRaw, lqi, pkt;
};
bool readStatus(Status& out);

const char* errorText(int code);

}  // namespace cc1101
