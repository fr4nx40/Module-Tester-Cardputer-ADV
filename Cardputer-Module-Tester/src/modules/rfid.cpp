#include "modules/rfid.h"

#include <M5Cardputer.h>
#include <string.h>

#include <string>

#include "core/input.h"
#include "core/log.h"
#include "core/ui.h"
#include "modules/mfrc522.h"

static const char* TAG = "rfid";

namespace {

// "DE AD BE EF"
std::string bytesToHex(const uint8_t* data, size_t len) {
  std::string out;
  char buf[4];
  for (size_t i = 0; i < len; i++) {
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    if (i) out += ' ';
    out += buf;
  }
  return out;
}

struct TagInfo {
  Uid uid;
  uint8_t atqa[2] = {0, 0};  // as printed: high byte first
  uint8_t sak = 0;
};

// ------------------------------------------------------------ tag types

bool isClassic(uint8_t sak) {
  return sak == 0x08 || sak == 0x09 || sak == 0x18 || sak == 0x28 || sak == 0x38;
}

const char* classicName(uint8_t sak) {
  switch (sak) {
    case 0x09: return "MIFARE Mini";
    case 0x18:
    case 0x38: return "MIFARE Classic 4K";
    default: return "MIFARE Classic 1K";
  }
}

const char* baseTypeName(uint8_t sak) {
  if (isClassic(sak)) return classicName(sak);
  if (sak == 0x00) return "Ultralight / NTAG";
  if (sak & 0x20) return "ISO 14443-4 tag";
  return "Unknown tag";
}

// ---------------------------------------------------------------- polling

// Looks for a tag. A halted tag is woken again on the next call.
bool poll(Mfrc522& r, TagInfo& info) {
  uint8_t atqa[2];
  if (r.wakeupA(atqa) != Mfrc522::Ok) return false;

  Uid uid;
  uint8_t sak = 0;
  if (r.select(uid, sak) != Mfrc522::Ok) return false;
  r.haltA();

  info.uid = uid;
  info.sak = sak;
  info.atqa[0] = atqa[1];  // the tag sends the low byte first
  info.atqa[1] = atqa[0];
  return true;
}

// ------------------------------------------------------------- read screen

enum class TagState { None, Present, Removed };

void drawRead(const TagInfo& info, TagState state) {
  const char* status = state == TagState::None      ? "AWAITING TAG"
                       : state == TagState::Present ? "TAG"
                                                    : "LAST TAG";
  ui::drawFrame("RFID 2", "Del back", status);
  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);

  if (state == TagState::None) {
    c.setTextDatum(middle_center);
    c.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    c.drawString("Awaiting tag...", ui::W / 2, 62);
    ui::present();
    return;
  }

  c.setTextDatum(middle_left);
  const int y0 = ui::HEADER_H + 12;
  const int step = 18;
  auto row = [&](int i, const char* label, const char* value) {
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(label, 4, y0 + i * step);
    c.setTextColor(TFT_WHITE, TFT_BLACK);
    c.drawString(value, 46, y0 + i * step);
  };

  row(0, "Type", baseTypeName(info.sak));
  std::string uid = bytesToHex(info.uid.bytes, info.uid.size);
  row(1, "UID", uid.c_str());
  char buf[40];
  snprintf(buf, sizeof(buf), "%02X %02X   SAK %02X", info.atqa[0], info.atqa[1], info.sak);
  row(2, "ATQA", buf);
  snprintf(buf, sizeof(buf), "%u bytes", (unsigned)info.uid.size);
  row(3, "Size", buf);
  ui::present();
}

// Shows the tag on the reader. When it is removed its data stays on screen
// until a new tag is placed, which replaces it.
void readScreen(Mfrc522& r) {
  TagInfo info;
  TagState state = TagState::None;
  int misses = 0;
  uint32_t lastPoll = 0;
  bool dirty = true;

  while (true) {
    uint32_t now = millis();
    if (now - lastPoll >= 150) {
      lastPoll = now;
      TagInfo seen;
      if (poll(r, seen)) {
        if (state != TagState::Present) dirty = true;
        bool same = state != TagState::None && seen.uid.size == info.uid.size &&
                    memcmp(seen.uid.bytes, info.uid.bytes, seen.uid.size) == 0;
        if (!same) {
          LOGI(TAG, "tag %s sak %02X", bytesToHex(seen.uid.bytes, seen.uid.size).c_str(),
               seen.sak);
          dirty = true;
        }
        info = seen;
        state = TagState::Present;
        misses = 0;
      } else if (state == TagState::Present && ++misses >= 3) {  // ignore brief dropouts
        state = TagState::Removed;
        dirty = true;
      }
    }

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;

    if (dirty) {
      drawRead(info, state);
      dirty = false;
    }
    delay(5);
  }
}

}  // namespace

// ------------------------------------------------------------------- entry

void runRfid(const Module& self) {
  static Mfrc522 reader;
  if (!reader.begin()) {
    ui::message(self.name, "RFID 2 not found", "I2C 0x28 on Grove");
    return;
  }
  readScreen(reader);
  reader.antennaOff();
}
