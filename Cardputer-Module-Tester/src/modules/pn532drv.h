#pragma once
// PN532 driver (own code, no library) for I2C, SPI or UART (HSU), with tag
// reading and NFC Forum Type 4 tag emulation.
//
// I2C is bit-banged on any two pins, so it never clashes with the Cardputer's
// own I2C buses.
#include <SPI.h>
#include <stddef.h>
#include <stdint.h>

namespace pn532 {

enum class Bus : uint8_t { I2C, SPI, UART };

struct Pins {
  int sda = -1, scl = -1;                // I2C
  int sck = -1, miso = -1, mosi = -1, cs = -1;  // SPI
  int rx = -1, tx = -1;                  // UART (rx = Cardputer pin that receives)
};

struct Firmware {
  uint8_t ic = 0;
  uint8_t ver = 0;
  uint8_t rev = 0;
};

struct Tag {
  uint8_t atqa[2] = {0, 0};
  uint8_t sak = 0;
  uint8_t uid[10] = {0};
  uint8_t uidLen = 0;
};

// Returns true if a PN532 answered. `fw` receives its firmware version.
bool begin(Bus bus, const Pins& pins, Firmware& fw);
void end();

// Looks for a tag for a short time (about 150 ms). True when one is found.
bool pollTag(Tag& tag);

// Reads the NDEF message from a tag (Type 2 / NTAG or Type 4). Returns its
// length, 0 if the tag has none, or -1 if it could not be read.
int readNdef(const Tag& tag, uint8_t* out, size_t maxLen);

// Short name for a tag, e.g. "NTAG / Ultralight".
const char* tagType(const Tag& tag);

// Emulates a Type 4 tag holding `ndef` until a reader has read it and left, or
// until stop() returns true. `uidTail` are three bytes used in the tag's UID.
struct EmuResult {
  bool cancelled = false;  // stop() returned true
  bool error = false;      // the chip refused or lost the link
  bool selected = false;   // a reader selected the NDEF application
  bool ndefRead = false;   // the reader read the message
};
EmuResult emulateOnce(const uint8_t* ndef, size_t len, const uint8_t uidTail[3],
                      bool (*stop)());

}  // namespace pn532
