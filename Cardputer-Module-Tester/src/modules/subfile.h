#pragma once
// Flipper ".sub" file parsing (RAW signals only).
// Plain C++ with no Arduino dependency.
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

struct SubSignal {
  float frequencyMHz = 433.92f;
  bool ook = true;             // false for the 2-FSK presets
  float devKhz = 47.6f;        // FSK deviation
  float rxBwKhz = 270.0f;      // from the preset name, when it says
  std::string preset;          // as written in the file
  std::vector<uint16_t> us;    // pulse lengths in microseconds, mark first
  bool truncated = false;      // file had more than we keep
};

// Feed it the file one line at a time, then call finish().
class SubParser {
 public:
  void feedLine(const std::string& line);
  void finish();

  const SubSignal& signal() const { return sig_; }
  bool isRaw() const { return raw_; }

  // Empty on success, otherwise a short message for the user.
  std::string error() const;

 private:
  void addValue(long v);
  void flushRun();

  SubSignal sig_;
  bool haveFrequency_ = false;
  bool raw_ = false;
  bool protocolSeen_ = false;
  bool runMark_ = false;   // sign of the run being built
  long run_ = 0;           // its length so far
  bool haveRun_ = false;
  bool startedWithMark_ = false;
};
