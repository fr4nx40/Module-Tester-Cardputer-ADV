#pragma once
// Minimal nRF24L01 / nRF24L01+ driver (own code, no library). Fixed 32-byte
// payloads, CRC16, auto-acknowledge off, one receive pipe.
#include <SPI.h>
#include <stddef.h>
#include <stdint.h>

namespace nrf24 {

enum class Rate : uint8_t { k250, k1M, k2M };

struct Info {
  bool present = false;
  bool plus = false;  // supports 250 kbps, i.e. nRF24L01+
  uint8_t config = 0;
  uint8_t channel = 0;
  uint8_t rfSetup = 0;
};

constexpr size_t kPayload = 32;

// Sets the pins and checks that a chip answers over SPI.
bool begin(SPIClass& spi, int csn, int ce);
void end();

// What the CONFIG register read back during the last failed begin() (0x00 = no
// reply, 0xFF = MISO floating).
uint8_t lastRead();

Info info();

// Channel 0-125 (2400 + channel MHz), data rate, 0 dBm.
void configure(uint8_t channel, Rate rate, const uint8_t addr[5]);

// Carrier activity scan: listens ~160 us on `channel` and returns true if
// anything above about -64 dBm was heard.
void scanStart();
bool carrierAt(uint8_t channel);
void scanStop();

// Receive
void rxStart();
bool rxAvailable();
void rxRead(uint8_t* buf32);
void rxStop();

// Transmit one 32-byte packet. Returns true when the chip sent it (there is no
// acknowledge, so this does not prove anyone received it).
bool tx(const uint8_t* buf32);

const char* rateName(Rate r);

}  // namespace nrf24
