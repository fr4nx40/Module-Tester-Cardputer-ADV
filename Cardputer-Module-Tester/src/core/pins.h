#pragma once
// Pin selection for modules wired to your own pins (CC1101, NRF24L01, PN532,
// generic GPS). Each module owns a Config; the values are loaded from flash,
// edited on a setup screen, and saved again when you leave it.
#include <stddef.h>
#include <stdint.h>

namespace pins {

constexpr int kNone = -1;

struct Def {
  const char* label;  // shown on the setup screen, e.g. "CS"
  int8_t def;         // default pin, kNone for "not used"
  bool optional;      // may be set to "none"
};

struct Config {
  const char* key;    // short flash key, e.g. "cc1101"
  const char* title;  // setup screen title
  const Def* defs;
  int count;          // at most 8
  int8_t value[8];    // filled by load()
};

// Reads the saved pins, or the defaults for anything not saved yet.
void load(Config& cfg);
void save(const Config& cfg);

// "G40 G39 G14 G5" style one-liner for a menu tag.
void summary(const Config& cfg, char* out, size_t n);

// The setup screen. Up/Down pick a pin, Left/Right change it, or type its
// number; Del clears an optional pin. Saves on exit.
void edit(Config& cfg);

}  // namespace pins
