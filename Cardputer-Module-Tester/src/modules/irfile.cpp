#include "modules/irfile.h"

#include <ctype.h>
#include <stdlib.h>

#include <utility>

namespace {

// Limits that protect RAM on big files.
const size_t kMaxSignals = 120;
const size_t kMaxRawValues = 1500;

// NEC-family timings in microseconds.
const uint16_t kNecHeaderMark = 9000;
const uint16_t kNecHeaderSpace = 4500;
const uint16_t kSamsungHeaderMark = 4500;
const uint16_t kSamsungHeaderSpace = 4500;
const uint16_t kBitMark = 560;
const uint16_t kZeroSpace = 560;
const uint16_t kOneSpace = 1690;

std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && isspace((unsigned char)s[b])) b++;
  while (e > b && isspace((unsigned char)s[e - 1])) e--;
  return s.substr(b, e - b);
}

// "04 00 00 00" -> 0x00000004 (bytes are stored least significant first).
uint32_t parseHexBytesLE(const std::string& v) {
  uint32_t out = 0;
  int shift = 0;
  size_t i = 0;
  while (i < v.size() && shift < 32) {
    while (i < v.size() && isspace((unsigned char)v[i])) i++;
    if (i >= v.size()) break;
    size_t j = i;
    while (j < v.size() && !isspace((unsigned char)v[j])) j++;
    unsigned long b = strtoul(v.substr(i, j - i).c_str(), nullptr, 16);
    out |= (uint32_t)(b & 0xFF) << shift;
    shift += 8;
    i = j;
  }
  return out;
}

void pushByteLsbFirst(std::vector<uint16_t>& v, uint8_t b) {
  for (int i = 0; i < 8; i++) {
    v.push_back(kBitMark);
    v.push_back(((b >> i) & 1) ? kOneSpace : kZeroSpace);
  }
}

}  // namespace

void FlipperIrParser::feedLine(const std::string& rawLine) {
  std::string line = trim(rawLine);
  if (line.empty() || line[0] == '#') return;

  size_t colon = line.find(':');
  if (colon == std::string::npos) return;
  std::string key = trim(line.substr(0, colon));
  std::string val = trim(line.substr(colon + 1));

  if (key == "name") {
    commit();
    cur_ = IrSignal();
    cur_.name = val;
    open_ = true;
    return;
  }
  if (!open_) return;  // file header lines (Filetype, Version)

  if (key == "type") {
    cur_.raw = (val == "raw");
  } else if (key == "protocol") {
    cur_.protocol = val;
  } else if (key == "address") {
    cur_.address = parseHexBytesLE(val);
  } else if (key == "command") {
    cur_.command = parseHexBytesLE(val);
  } else if (key == "frequency") {
    cur_.frequency = (uint32_t)strtoul(val.c_str(), nullptr, 10);
  } else if (key == "data") {
    // Long raw signals can be split over several "data:" lines, so append.
    size_t i = 0;
    while (i < val.size()) {
      while (i < val.size() && isspace((unsigned char)val[i])) i++;
      if (i >= val.size()) break;
      size_t j = i;
      while (j < val.size() && !isspace((unsigned char)val[j])) j++;
      if (cur_.data.size() >= kMaxRawValues) {
        truncated_ = true;
        break;
      }
      unsigned long us = strtoul(val.substr(i, j - i).c_str(), nullptr, 10);
      cur_.data.push_back((uint16_t)(us > 65535UL ? 65535UL : us));
      i = j;
    }
  }
}

void FlipperIrParser::commit() {
  if (!open_) return;
  if (signals_.size() < kMaxSignals) {
    signals_.push_back(std::move(cur_));
  } else {
    truncated_ = true;
  }
  cur_ = IrSignal();
  open_ = false;
}

void FlipperIrParser::finish() { commit(); }

std::string irSignalToTimings(const IrSignal& s, std::vector<uint16_t>& out,
                              uint8_t& khz) {
  out.clear();
  khz = 38;

  if (s.raw) {
    if (s.data.size() < 2) return "No raw data";
    out = s.data;
    uint32_t k = (s.frequency + 500) / 1000;
    if (k < 20 || k > 60) k = 38;
    khz = (uint8_t)k;
    return "";
  }

  uint8_t bytes[4];
  uint16_t headerMark = 0;
  uint16_t headerSpace = 0;

  if (s.protocol == "NEC") {
    // 8-bit address and command, each followed by its inverse.
    uint8_t a = (uint8_t)(s.address & 0xFF);
    uint8_t c = (uint8_t)(s.command & 0xFF);
    bytes[0] = a;
    bytes[1] = (uint8_t)~a;
    bytes[2] = c;
    bytes[3] = (uint8_t)~c;
    headerMark = kNecHeaderMark;
    headerSpace = kNecHeaderSpace;
  } else if (s.protocol == "NECext") {
    // 16-bit address and 16-bit command, sent as given.
    bytes[0] = (uint8_t)(s.address & 0xFF);
    bytes[1] = (uint8_t)((s.address >> 8) & 0xFF);
    bytes[2] = (uint8_t)(s.command & 0xFF);
    bytes[3] = (uint8_t)((s.command >> 8) & 0xFF);
    headerMark = kNecHeaderMark;
    headerSpace = kNecHeaderSpace;
  } else {
    return "Unsupported: " + (s.protocol.empty() ? std::string("(none)") : s.protocol);
  }

  out.reserve(67);
  out.push_back(headerMark);
  out.push_back(headerSpace);
  for (int i = 0; i < 4; i++) pushByteLsbFirst(out, bytes[i]);
  out.push_back(kBitMark);  // stop bit
  return "";
}