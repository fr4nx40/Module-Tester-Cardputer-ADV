#include "modules/subfile.h"

#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kMaxPulses = 1500;

std::string trim(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
  return s.substr(a, b - a);
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

void SubParser::flushRun() {
  if (!haveRun_) return;
  long v = run_ > 65535 ? 65535 : run_;
  if (v < 1) v = 1;
  if (sig_.us.empty() && !runMark_) {
    // A leading gap means nothing on air: drop it so the data starts with a mark.
    haveRun_ = false;
    return;
  }
  if (sig_.us.size() < kMaxPulses) {
    sig_.us.push_back((uint16_t)v);
  } else {
    sig_.truncated = true;
  }
  haveRun_ = false;
}

// Positive values are marks, negative values are spaces. Neighbours with the
// same sign are merged so the list alternates.
void SubParser::addValue(long v) {
  if (v == 0) return;
  bool mark = v > 0;
  long len = v > 0 ? v : -v;
  if (haveRun_ && mark == runMark_) {
    run_ += len;
    return;
  }
  flushRun();
  runMark_ = mark;
  run_ = len;
  haveRun_ = true;
}

void SubParser::feedLine(const std::string& rawLine) {
  std::string line = trim(rawLine);
  size_t colon = line.find(':');
  if (colon == std::string::npos) return;
  std::string key = trim(line.substr(0, colon));
  std::string val = trim(line.substr(colon + 1));

  if (key == "Frequency") {
    sig_.frequencyMHz = (float)atof(val.c_str()) / 1000000.0f;
    haveFrequency_ = true;
  } else if (key == "Preset") {
    sig_.preset = val;
    sig_.ook = contains(val, "Ook") || contains(val, "OOK");
    if (contains(val, "270")) sig_.rxBwKhz = 270.0f;
    if (contains(val, "650")) sig_.rxBwKhz = 650.0f;
    if (contains(val, "Dev238")) sig_.devKhz = 2.38f;
    if (contains(val, "Dev476")) sig_.devKhz = 47.6f;
  } else if (key == "Protocol") {
    protocolSeen_ = true;
    raw_ = (val == "RAW");
  } else if (key == "RAW_Data") {
    const char* p = val.c_str();
    char* end = nullptr;
    while (*p) {
      long v = strtol(p, &end, 10);
      if (end == p) break;
      addValue(v);
      p = end;
    }
  }
}

void SubParser::finish() { flushRun(); }

std::string SubParser::error() const {
  if (!haveFrequency_) return "No frequency in file";
  if (protocolSeen_ && !raw_) return "Only RAW .sub files work";
  if (sig_.us.size() < 4) return "No RAW data in file";
  return "";
}
