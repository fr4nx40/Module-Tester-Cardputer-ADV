#include "modules/pn532.h"

#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>
#include <string.h>

#include "core/filebrowser.h"
#include "core/input.h"
#include "core/log.h"
#include "core/modmenu.h"
#include "core/pins.h"
#include "core/storage.h"
#include "core/ui.h"
#include "modules/pn532drv.h"

static const char* TAG = "pn532";

namespace {

// One pin set per interface, each remembered separately.
const pins::Def kI2cDefs[] = {{"SDA", 2, false}, {"SCL", 1, false}};
const pins::Def kSpiDefs[] = {
    {"SCK", 40, false}, {"MISO", 39, false}, {"MOSI", 14, false}, {"CS", 5, false}};
const pins::Def kUartDefs[] = {{"RX", 1, false}, {"TX", 2, false}};

pins::Config gI2c = {"pn532i", "PN532 I2C pins", kI2cDefs, 2, {}};
pins::Config gSpi = {"pn532s", "PN532 SPI pins", kSpiDefs, 4, {}};
pins::Config gUart = {"pn532u", "PN532 UART pins", kUartDefs, 2, {}};

const char* const kBusNames[3] = {"I2C", "SPI", "UART"};
int gBus = 0;

pins::Config& cfg() { return gBus == 0 ? gI2c : gBus == 1 ? gSpi : gUart; }

constexpr size_t kMaxNdef = 240;
pn532::Tag gTag;
bool gHaveTag = false;
uint8_t gNdef[kMaxNdef];
size_t gNdefLen = 0;
bool gNdefFromTag = false;

// Text record (language "en").
size_t makeTextRecord(const char* text, uint8_t* out, size_t max) {
  size_t tl = strlen(text);
  size_t plen = 1 + 2 + tl;
  if (plen > 255 || 4 + plen > max) return 0;
  out[0] = 0xD1;
  out[1] = 0x01;
  out[2] = (uint8_t)plen;
  out[3] = 'T';
  out[4] = 0x02;
  out[5] = 'e';
  out[6] = 'n';
  memcpy(out + 7, text, tl);
  return 7 + tl;
}

const char* const kUriPrefix[] = {"", "http://www.", "https://www.", "http://", "https://"};

size_t makeUriRecord(const char* uri, uint8_t* out, size_t max) {
  int code = 0;
  for (int i = 4; i >= 1; i--) {
    if (strncmp(uri, kUriPrefix[i], strlen(kUriPrefix[i])) == 0) {
      code = i;
      break;
    }
  }
  const char* rest = uri + strlen(kUriPrefix[code]);
  size_t rl = strlen(rest);
  size_t plen = 1 + rl;
  if (plen > 255 || 4 + plen > max) return 0;
  out[0] = 0xD1;
  out[1] = 0x01;
  out[2] = (uint8_t)plen;
  out[3] = 'U';
  out[4] = (uint8_t)code;
  memcpy(out + 5, rest, rl);
  return 5 + rl;
}

// Short text for the first record of a message.
void describeNdef(const uint8_t* d, size_t n, char* out, size_t outSize) {
  out[0] = 0;
  if (n < 4) return;
  uint8_t flags = d[0];
  uint8_t typeLen = d[1];
  size_t i = 2;
  size_t plen;
  if (flags & 0x10) {
    plen = d[i++];
  } else {
    if (n < 6) return;
    plen = ((size_t)d[i] << 24) | ((size_t)d[i + 1] << 16) | ((size_t)d[i + 2] << 8) | d[i + 3];
    i += 4;
  }
  if (flags & 0x08) i++;
  if (i + typeLen + plen > n || typeLen == 0) {
    snprintf(out, outSize, "NDEF %u bytes", (unsigned)n);
    return;
  }
  const uint8_t* type = d + i;
  const uint8_t* pl = d + i + typeLen;
  if (typeLen == 1 && type[0] == 'T' && plen > 1) {
    size_t skip = 1 + (pl[0] & 0x3F);
    if (skip <= plen) {
      size_t m = plen - skip;
      if (m > outSize - 8) m = outSize - 8;
      snprintf(out, outSize, "Text: %.*s", (int)m, (const char*)pl + skip);
      return;
    }
  }
  if (typeLen == 1 && type[0] == 'U' && plen > 0) {
    const char* pre = pl[0] < 5 ? kUriPrefix[pl[0]] : "";
    size_t m = plen - 1;
    if (m > outSize - 24) m = outSize - 24;
    snprintf(out, outSize, "URL: %s%.*s", pre, (int)m, (const char*)pl + 1);
    return;
  }
  snprintf(out, outSize, "NDEF %u bytes", (unsigned)n);
}

bool gOpen = false;

bool startChip(const Module& self) {
  pins::Config& c = cfg();
  pn532::Pins p;
  pn532::Bus bus = pn532::Bus::I2C;
  if (gBus == 0) {
    p.sda = c.value[0];
    p.scl = c.value[1];
  } else if (gBus == 1) {
    bus = pn532::Bus::SPI;
    p.sck = c.value[0];
    p.miso = c.value[1];
    p.mosi = c.value[2];
    p.cs = c.value[3];
  } else {
    bus = pn532::Bus::UART;
    p.rx = c.value[0];
    p.tx = c.value[1];
  }
  pn532::Firmware fw;
  if (!pn532::begin(bus, p, fw)) {
    LOGW(TAG, "chip not detected (%s)", kBusNames[gBus]);
    pn532::end();
    ui::message(self.name, "PN532 Not Detected",
                gBus == 0   ? "Check SDA/SCL and I2C switch"
                : gBus == 1 ? "Check SPI pins and switch"
                            : "Check RX/TX (swap?), HSU");
    return false;
  }
  LOGI(TAG, "PN532 fw %u.%u", fw.ver, fw.rev);
  gOpen = true;
  return true;
}

void stopChip() {
  if (gOpen) pn532::end();
  gOpen = false;
}

bool backPressed() {
  InputEvent ev = inputPoll();
  return ev.key == Key::Back;
}

void hex(const uint8_t* d, size_t n, char* out, size_t outSize) {
  size_t pos = 0;
  out[0] = 0;
  for (size_t i = 0; i < n && pos + 4 < outSize; i++) {
    pos += snprintf(out + pos, outSize - pos, i ? ":%02X" : "%02X", d[i]);
  }
}

void drawCapture(bool found, const char* ndefText) {
  ui::drawFrame("PN532 capture", found ? "Enter emulate|Del back" : "Del back",
                found ? "CAPTURED" : "AWAITING TAG");
  auto& c = ui::canvas();
  c.setTextDatum(middle_left);
  if (!found) {
    c.setFont(&fonts::Font2);
    c.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    c.setTextDatum(middle_center);
    c.drawString("Hold a tag on the antenna", ui::W / 2, 68);
    ui::present();
    return;
  }
  const int y0 = ui::HEADER_H + 12;
  const int step = 18;
  char buf[48];
  auto row = [&](int i, const char* label, const char* value, uint16_t color) {
    c.setFont(&fonts::Font2);
    c.setTextColor(TFT_DARKGREY, TFT_BLACK);
    c.drawString(label, 4, y0 + i * step);
    c.setTextColor(color, TFT_BLACK);
    c.drawString(value, 50, y0 + i * step);
  };
  hex(gTag.uid, gTag.uidLen, buf, sizeof(buf));
  row(0, "UID", buf, TFT_WHITE);
  row(1, "Type", pn532::tagType(gTag), TFT_WHITE);
  snprintf(buf, sizeof(buf), "%02X%02X  SAK %02X", gTag.atqa[0], gTag.atqa[1], gTag.sak);
  row(2, "ATQA", buf, TFT_LIGHTGREY);
  row(3, "NDEF", ndefText, gNdefLen ? TFT_GREEN : TFT_DARKGREY);
  ui::present();
}

// Emulates whatever is currently captured (the tag read above, or a plain
// text record with its UID if it had no NDEF message). Stops the chip first,
// since emulate() opens it again itself, and reopens it for capture on return.
void drawEmulate(const char* what, const char* state, uint16_t color) {
  ui::drawFrame("PN532 emulate", "Del stop", "");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font2);
  c.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  c.setTextDatum(middle_center);
  c.drawString(what, ui::W / 2, 52);
  c.setTextColor(color, TFT_BLACK);
  c.drawString(state, ui::W / 2, 80);
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.drawString("Hold a phone or reader on the antenna", ui::W / 2, 108);
  ui::present();
}

void emulate(const Module& self, const uint8_t* ndef, size_t len, const char* what) {
  if (!startChip(self)) return;
  uint8_t tail[3] = {(uint8_t)esp_random(), (uint8_t)esp_random(), (uint8_t)esp_random()};
  bool read = false;
  while (true) {
    drawEmulate(what, read ? "READ - waiting again" : "EMULATING", read ? TFT_GREEN : TFT_YELLOW);
    pn532::EmuResult r = pn532::emulateOnce(ndef, len, tail, backPressed);
    LOGI(TAG, "emu selected %d read %d err %d", r.selected, r.ndefRead, r.error);
    if (r.cancelled) break;
    if (r.error) {
      ui::message(self.name, "Emulation failed", "Chip refused or link lost");
      break;
    }
    if (r.ndefRead) {
      read = true;
    } else if (r.selected) {
      ui::toast("Reader left early", 900, TFT_DARKGREY);
    }
  }
  stopChip();
}

void emulateCaptured(const Module& self) {
  if (!gHaveTag) return;
  if (gNdefLen == 0) {
    char u[32];
    hex(gTag.uid, gTag.uidLen, u, sizeof(u));
    char t[48];
    snprintf(t, sizeof(t), "UID %s", u);
    gNdefLen = makeTextRecord(t, gNdef, sizeof(gNdef));
    gNdefFromTag = false;
  }
  emulate(self, gNdef, gNdefLen, gNdefFromTag ? "Captured NDEF" : "Captured UID as text");
}

// Capture and emulate in one screen, like the IR unit: keep listening for a
// tag (so a different one replaces the last capture), and Enter emulates
// whatever was last captured.
void captureScreen(const Module& self) {
  if (!startChip(self)) return;
  bool found = false;
  char ndefText[40] = "none";
  drawCapture(false, "");
  uint32_t last = 0;
  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;
    if (ev.key == Key::Select && found) {
      stopChip();
      emulateCaptured(self);  // its own loop; returns when the user presses Del
      if (!startChip(self)) break;
      drawCapture(found, ndefText);
    }
    if (millis() - last > 150) {
      last = millis();
      pn532::Tag t;
      if (pn532::pollTag(t)) {
        gTag = t;
        gHaveTag = true;
        int n = pn532::readNdef(t, gNdef, sizeof(gNdef));
        gNdefLen = n > 0 ? (size_t)n : 0;
        gNdefFromTag = gNdefLen > 0;
        if (gNdefLen) {
          describeNdef(gNdef, gNdefLen, ndefText, sizeof(ndefText));
        } else {
          strcpy(ndefText, n < 0 ? "read failed" : "none");
        }
        found = true;
        LOGI(TAG, "captured uid len %u ndef %u", t.uidLen, (unsigned)gNdefLen);
        drawCapture(true, ndefText);
      }
    }
    delay(5);
  }
  stopChip();
  // Wipe on exit: next time in, start fresh rather than showing an old tag.
  gHaveTag = false;
  gNdefLen = 0;
  gNdefFromTag = false;
}

// .nfc file: one line, "Text: hello" or "Url: https://example.com"
bool loadFile(const String& path, uint8_t* out, size_t& len, char* shown, size_t shownSize) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;
  String line;
  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (line.length() && line[0] != '#') break;
  }
  f.close();
  int colon = line.indexOf(':');
  if (colon < 0) return false;
  String kind = line.substring(0, colon);
  String value = line.substring(colon + 1);
  value.trim();
  kind.trim();
  kind.toLowerCase();
  if (value.length() == 0) return false;
  size_t n = 0;
  if (kind == "text") n = makeTextRecord(value.c_str(), out, kMaxNdef);
  else if (kind == "url" || kind == "uri") n = makeUriRecord(value.c_str(), out, kMaxNdef);
  if (n == 0) return false;
  len = n;
  describeNdef(out, n, shown, shownSize);
  return true;
}

void emulateFromFile(const Module& self) {
  String dir = String(storage::kAppDir) + "/nfc";
  while (true) {
    String path;
    if (!browseFiles("NFC files (.nfc)", dir.c_str(), ".nfc", path)) return;
    dir = storage::parentDir(path);
    uint8_t buf[kMaxNdef];
    size_t len = 0;
    char shown[40];
    if (!loadFile(path, buf, len, shown, sizeof(shown))) {
      ui::message("NFC file", "Cannot read file", "Use: Text: hello  or  Url: https://...");
      continue;
    }
    emulate(self, buf, len, shown);
  }
}

void saveBus() {
  Preferences p;
  if (p.begin("pn532", false)) {
    p.putUChar("bus", (uint8_t)gBus);
    p.end();
  }
}

}  // namespace

void runPn532(const Module& self) {
  pins::load(gI2c);
  pins::load(gSpi);
  pins::load(gUart);
  {
    Preferences p;
    if (p.begin("pn532", true)) {
      gBus = p.getUChar("bus", 0);
      p.end();
    }
    if (gBus < 0 || gBus > 2) gBus = 0;
  }

  int sel = 0;
  int top = 0;
  char busTag[16];
  char pinTag[28];
  while (true) {
    snprintf(busTag, sizeof(busTag), "< %s >", kBusNames[gBus]);
    pins::summary(cfg(), pinTag, sizeof(pinTag));
    const ui::MenuItem items[] = {
        {"Interface", busTag},
        {"Pins", pinTag},
        {"Capture & Emulate", "read a tag"},
        {"Emulate from File", ".nfc"},
    };
    const int count = sizeof(items) / sizeof(items[0]);
    ui::MenuAction a = ui::menuAction(self.name, items, count, sel, top,
                                      "UpDown move|LeftRight edit|Enter select|Del back");
    if (a.kind == ui::MenuAction::Back) return;

    if (a.kind == ui::MenuAction::Left || a.kind == ui::MenuAction::Right) {
      if (a.row == 0) {
        gBus = (gBus + 3 + (a.kind == ui::MenuAction::Right ? 1 : -1)) % 3;
        saveBus();
      }
      continue;
    }

    switch (a.row) {
      case 0: gBus = (gBus + 1) % 3; saveBus(); break;
      case 1: pins::edit(cfg()); break;
      case 2: captureScreen(self); break;
      case 3: emulateFromFile(self); break;
      default: break;
    }
  }
}
