#pragma once
// Thin wrapper around RadioLib's SX1262 for the Cap LoRa-1262.
//
// RadioLib defines its own global class named `Module`, which clashes with the
// registry's `Module` struct, so everything RadioLib related lives in
// caplora.cpp and this header exposes plain functions only.
#include <stddef.h>
#include <stdint.h>

namespace cap {

constexpr int kErrNoCap = -1000;  // returned by begin() when the Cap is absent
constexpr int kErrCrc = -7;       // RadioLib's CRC mismatch code (packet still readable)

struct Preset {
  const char* name;
  float freq;       // MHz
  float bw;         // kHz
  uint8_t sf;       // spreading factor
  uint8_t cr;       // coding rate denominator (5 = 4/5)
  uint8_t sync;     // sync word
  uint16_t preamble;
  int8_t power;     // dBm
};

extern const Preset kPlain;       // 868.0 MHz, SF7, BW125, for your own devices
extern const Preset kMeshtastic;  // EU868 LongFast: 869.525 MHz, SF11, BW250

// True if the Cap's I/O expander (I2C 0x43) answers.
bool present();

// Powers up the Cap (I/O expander pin 0 high), which M5's examples do too.
void power();

// Powers the Cap and configures the radio. 0 = OK, kErrNoCap, or a RadioLib
// error code (-2 = chip not found).
int begin(const Preset& preset);

void standby();

// Receive
int rxStart();
bool rxReady();  // true once per received packet (interrupt flag)
// Reads the packet, then restarts receiving. Returns the RadioLib status
// (0 = OK, -7 = CRC mismatch).
int rxRead(uint8_t* buf, size_t maxLen, size_t& len, float& rssi, float& snr);

// Transmit (blocks until the packet is sent). Returns the RadioLib status.
int tx(const uint8_t* data, size_t len);

// RSSI scan
int scanStart(float firstMHz);
float scanRssi(float mhz);  // dBm at that frequency

// Short text for an error code.
const char* errorText(int code);

}  // namespace cap
