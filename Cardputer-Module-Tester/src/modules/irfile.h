#pragma once
// Flipper ".ir" file parsing and signal encoding.
// Plain C++ with no Arduino or IRremote dependency.
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

// One signal from a .ir file.
struct IrSignal {
  std::string name;
  bool raw = false;            // type: raw (true) or parsed (false)
  std::string protocol;        // parsed signals: "NEC", "NECext", "Samsung32", ...
  uint32_t address = 0;        // parsed signals
  uint32_t command = 0;        // parsed signals
  uint32_t frequency = 38000;  // raw signals, carrier in Hz
  std::vector<uint16_t> data;  // raw signals, microseconds, starting with a mark
};

// Feed it the file one line at a time, then call finish().
class FlipperIrParser {
 public:
  void feedLine(const std::string& line);
  void finish();

  const std::vector<IrSignal>& signals() const { return signals_; }
  bool truncated() const { return truncated_; }  // file had more than we keep

 private:
  void commit();

  bool open_ = false;  // a "name:" line has started a signal
  IrSignal cur_;
  std::vector<IrSignal> signals_;
  bool truncated_ = false;
};

// Turns a signal into mark/space timings in microseconds (mark first) and a
// carrier frequency in kHz, ready for IRremote's sendRaw().
// Supports raw signals plus the parsed protocols NEC, NECext and Samsung32.
// Returns an empty string on success, otherwise a short error message.
std::string irSignalToTimings(const IrSignal& s, std::vector<uint16_t>& out,
                              uint8_t& khz);