#include "modules/ir.h"

#include <M5Cardputer.h>
#include <SD.h>

#include <string>
#include <vector>

// Keep the (unused) feedback LED code out. Must come before the include.
#define NO_LED_FEEDBACK_CODE
// IRremote's .hpp holds the implementation: include it in this file only.
#include <IRremote.hpp>

#include "core/filebrowser.h"
#include "core/input.h"
#include "core/log.h"
#include "core/modmenu.h"
#include "core/pins.h"
#include "core/storage.h"
#include "core/ui.h"
#include "modules/irfile.h"

static const char* TAG = "ir";

namespace {

constexpr uint8_t kCaptureKhz = 38;  // carrier assumed for replaying captures

// Which pins this screen is using right now, set by runIr()/runIrBuiltin()
// before anything else runs. The Grove unit's pins are fixed; the second
// entry below lets you choose both.
uint8_t gRecvPin = 1;
uint8_t gEmitPin = 2;
char gRecvHint[56] = "Receiver G1 (white)   Emitter G2 (yellow)";

// Pins for the second IR entry: both TX and RX are yours to choose (TX
// defaults to G44, the Cardputer ADV's onboard IR LED pad).
const pins::Def kBuiltinDefs[] = {
    {"TX", 44, false},
    {"RX", 15, false},
};
pins::Config gBuiltinCfg = {"irbuilt", "IR (onboard): set TX and RX", kBuiltinDefs, 2, {}};
enum BuiltinPin { kBuiltinTx, kBuiltinRx };

bool gReceiverOn = false;

void receiverStart() {
  IrReceiver.begin(gRecvPin, false);
  // With no IR unit fitted the pin would float and pick up noise.
  pinMode(gRecvPin, INPUT_PULLUP);
  gReceiverOn = true;
}

void receiverStop() {
  if (gReceiverOn) IrReceiver.stop();
  gReceiverOn = false;
}

// Sends mark/space timings (microseconds, mark first). The receiver is paused
// while sending so the unit's own emitter does not feed back into it.
void sendTimings(const uint16_t* us, size_t n, uint8_t khz) {
  bool wasOn = gReceiverOn;
  if (wasOn) IrReceiver.stop();

  LOGI(TAG, "send %u values @ %u kHz", (unsigned)n, (unsigned)khz);
  IrSender.sendRaw(us, (uint_fast16_t)n, khz);

  if (wasOn) {
    IrReceiver.start();
    IrReceiver.resume();
  }
}

// ---------------------------------------------------------------- capture

struct Capture {
  uint16_t us[RAW_BUFFER_LENGTH];
  size_t n = 0;
  char proto[20] = "";
  uint32_t address = 0;
  uint32_t command = 0;
  bool decoded = false;
  bool overflow = false;
  uint32_t totalUs = 0;
  uint32_t count = 0;  // number of frames captured so far
};

// Copies the frame IRremote just received into `c`, converted to microseconds.
// Returns false for repeats and noise, which are ignored.
bool grabFrame(Capture& c) {
  IRData& d = IrReceiver.decodedIRData;

  if (d.flags & IRDATA_FLAGS_IS_REPEAT) return false;
  if (d.flags & IRDATA_FLAGS_WAS_OVERFLOW) {
    c.overflow = true;
    LOGW(TAG, "frame overflowed the raw buffer");
    return false;
  }

  // rawbuf[0] is unused; entries 1.. alternate mark, space, mark, ...
  size_t len = d.rawlen;
  if (len < 5 || len > RAW_BUFFER_LENGTH) return false;  // noise

  size_t n = 0;
  uint32_t total = 0;
  for (size_t i = 1; i < len; i++) {
    int32_t us = (int32_t)IrReceiver.irparams.rawbuf[i] * MICROS_PER_TICK;
    // The receiver stretches marks and shrinks spaces; undo that.
    us += (i & 1) ? -(int32_t)MARK_EXCESS_MICROS : (int32_t)MARK_EXCESS_MICROS;
    if (us < 1) us = 1;
    if (us > 65535) us = 65535;
    c.us[n++] = (uint16_t)us;
    total += (uint32_t)us;
  }

  // Real remotes never use pulses this short. Serial data from another unit
  // on the same pin (a GPS, say) does, so such frames are dropped.
  size_t shortCount = 0;
  for (size_t i = 0; i < n; i++) {
    if (c.us[i] < 150) shortCount++;
  }
  if (shortCount * 4 > n) {
    LOGW(TAG, "ignored noise: %u of %u pulses < 150 us", (unsigned)shortCount, (unsigned)n);
    return false;
  }

  c.n = n;
  c.totalUs = total;
  c.overflow = false;
  c.decoded = (d.protocol != UNKNOWN);
  strncpy(c.proto, getProtocolString(d.protocol), sizeof(c.proto) - 1);
  c.proto[sizeof(c.proto) - 1] = 0;
  c.address = d.address;
  c.command = d.command;
  c.count++;

  LOGI(TAG, "captured %s addr 0x%lX cmd 0x%lX, %u values, %lu us", c.proto,
       (unsigned long)c.address, (unsigned long)c.command, (unsigned)c.n,
       (unsigned long)c.totalUs);
  return true;
}

void drawCapture(const Capture& c, const char* status) {
  ui::drawFrame("IR capture", c.n ? "Enter replay|Del back" : "Del back",
                status);
  auto& cv = ui::canvas();
  cv.setFont(&fonts::Font2);
  cv.setTextDatum(middle_left);

  if (c.n == 0) {
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    cv.setTextDatum(middle_center);
    cv.drawString("Waiting for a signal...", ui::W / 2, 56);
    cv.setFont(&fonts::Font0);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString(gRecvHint, ui::W / 2, 80);
    if (c.overflow) {
      cv.setTextColor(TFT_ORANGE, TFT_BLACK);
      cv.drawString("Last frame too long, ignored", ui::W / 2, 96);
    }
    ui::present();
    return;
  }

  const int y0 = ui::HEADER_H + 10;
  const int step = 16;
  char buf[40];

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("Proto", 4, y0);
  cv.setTextColor(c.decoded ? TFT_WHITE : TFT_YELLOW, TFT_BLACK);
  cv.drawString(c.decoded ? c.proto : "Unknown (raw only)", 50, y0);

  if (c.decoded) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("Addr", 4, y0 + step);
    cv.drawString("Cmd", 122, y0 + step);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(buf, sizeof(buf), "0x%lX", (unsigned long)c.address);
    cv.drawString(buf, 50, y0 + step);
    snprintf(buf, sizeof(buf), "0x%lX", (unsigned long)c.command);
    cv.drawString(buf, 156, y0 + step);
  }

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("Raw", 4, y0 + 2 * step);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%u values, %lu.%lu ms", (unsigned)c.n,
           (unsigned long)(c.totalUs / 1000), (unsigned long)((c.totalUs % 1000) / 100));
  cv.drawString(buf, 50, y0 + 2 * step);

  cv.setFont(&fonts::Font0);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  snprintf(buf, sizeof(buf), "Replays at %u kHz from G%u", (unsigned)kCaptureKhz, (unsigned)gEmitPin);
  cv.drawString(buf, 4, y0 + 3 * step + 4);
  if (c.overflow) {
    cv.setTextColor(TFT_ORANGE, TFT_BLACK);
    cv.drawString("A longer frame was ignored", 4, y0 + 3 * step + 16);
  }
  ui::present();
}

void captureScreen() {
  static Capture cap;  // large buffer: keep it off the task stack
  cap.n = 0;
  cap.decoded = false;
  cap.overflow = false;
  cap.count = 0;

  receiverStart();

  const char* status = "LISTENING";
  bool dirty = true;

  while (true) {
    if (IrReceiver.decode()) {
      if (grabFrame(cap)) status = "CAPTURED";
      IrReceiver.resume();  // rawbuf is copied already
      dirty = true;
    }

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;
    if (ev.key == Key::Select && cap.n > 0) {
      sendTimings(cap.us, cap.n, kCaptureKhz);
      status = "SENT";
      dirty = true;
    }

    if (dirty) {
      drawCapture(cap, cap.n ? status : "LISTENING");
      dirty = false;
    }
    delay(5);
  }

  receiverStop();
  // Wipe on exit too (entry already resets it, but this keeps the two
  // consistent, same as CC1101/PN532): next time in starts fresh.
  cap.n = 0;
  cap.decoded = false;
  cap.overflow = false;
  cap.count = 0;
}

// ------------------------------------------------------------ replay file

// Reads and parses a .ir file. Returns an error string, or "" on success.
std::string loadIrFile(const String& path, FlipperIrParser& parser) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return "Cannot open file";

  while (f.available()) {
    String line = f.readStringUntil('\n');
    parser.feedLine(std::string(line.c_str()));
  }
  f.close();
  parser.finish();

  if (parser.signals().empty()) return "No signals in file";
  return "";
}

// Shows the signals of one file and sends the chosen one. Returns on Back.
void signalList(const String& path) {
  FlipperIrParser parser;
  std::string err = loadIrFile(path, parser);
  if (!err.empty()) {
    ui::message("IR file", err.c_str(), path.c_str());
    return;
  }
  const std::vector<IrSignal>& sigs = parser.signals();
  LOGI(TAG, "%s: %u signals%s", path.c_str(), (unsigned)sigs.size(),
       parser.truncated() ? " (truncated)" : "");

  std::vector<ui::MenuItem> items;
  items.reserve(sigs.size());
  for (const IrSignal& s : sigs) {
    items.push_back({s.name.c_str(), s.raw ? "raw" : s.protocol.c_str()});
  }

  int slash = path.lastIndexOf('/');
  String title = path.substring(slash + 1);
  std::vector<uint16_t> us;

  int sel = 0;
  while (true) {
    sel = ui::menu(title.c_str(), items.data(), (int)items.size(), sel,
                   "UpDown move|Enter send|Del back");
    if (sel < 0) return;

    uint8_t khz = 38;
    std::string e = irSignalToTimings(sigs[sel], us, khz);
    if (!e.empty()) {
      LOGW(TAG, "'%s': %s", sigs[sel].name.c_str(), e.c_str());
      ui::toast(e.c_str(), 1400, TFT_MAROON);
      continue;
    }
    sendTimings(us.data(), us.size(), khz);
    ui::toast("Sent", 500);
  }
}

void replayFromFile() {
  if (!storage::mounted()) {
    ui::message("IR files", "No SD card", "Insert a card and reboot");
    return;
  }

  String dir = storage::kAppDir;
  while (true) {
    String path;
    if (!browseFiles("IR files", dir.c_str(), ".ir", path)) return;
    dir = storage::parentDir(path);  // come back to the same folder
    signalList(path);
  }
}

}  // namespace

// ------------------------------------------------------------------- entry

void runIr(const Module& self) {
  (void)self;
  gRecvPin = 1;
  gEmitPin = 2;
  snprintf(gRecvHint, sizeof(gRecvHint), "Receiver G1 (white)   Emitter G2 (yellow)");
  IrSender.begin(gEmitPin);

  static const ui::MenuItem kItems[] = {
      {"Capture & replay", "live"},
      {"Replay from file", "SD"},
  };

  int sel = 0;
  while (true) {
    sel = ui::menu("IR unit", kItems, 2, sel);
    if (sel < 0) break;
    if (sel == 0) captureScreen();
    if (sel == 1) replayFromFile();
  }
  receiverStop();
}

// A second IR entry where both pins are yours to choose. Defaults to the
// Cardputer ADV's onboard IR LED (G44) for TX and G15 for RX, but either can
// be changed on the Pins screen. The fixed Grove IR unit above is unaffected.
void runIrBuiltin(const Module& self) {
  pins::load(gBuiltinCfg);
  gEmitPin = (uint8_t)gBuiltinCfg.value[kBuiltinTx];
  IrSender.begin(gEmitPin);

  int sel = 0;
  int top = 0;
  while (true) {
    gRecvPin = (uint8_t)gBuiltinCfg.value[kBuiltinRx];
    snprintf(gRecvHint, sizeof(gRecvHint), "Receiver G%u   Emitter G%u",
             (unsigned)gRecvPin, (unsigned)gEmitPin);

    char pinTag[16];
    pins::summary(gBuiltinCfg, pinTag, sizeof(pinTag));
    const ui::MenuItem items[] = {
        {"Pins", pinTag},
        {"Capture & replay", "live"},
        {"Replay from file", "SD"},
    };
    ui::MenuAction a = ui::menuAction(self.name, items, 3, sel, top,
                                      "UpDown move|Enter select|Del back");
    if (a.kind == ui::MenuAction::Back) break;
    if (a.kind != ui::MenuAction::Pick) continue;

    switch (a.row) {
      case 0:
        pins::edit(gBuiltinCfg);
        // TX may have changed; re-attach the sender to the (possibly new) pin.
        gEmitPin = (uint8_t)gBuiltinCfg.value[kBuiltinTx];
        IrSender.begin(gEmitPin);
        break;
      case 1: captureScreen(); break;
      case 2: replayFromFile(); break;
      default: break;
    }
  }
  receiverStop();
}

// Top-level "IR" entry: a GPS-menu-style chooser between the Grove IR unit
// (fixed pins) and the onboard/custom pin version.
void runIrMenu(const Module& self) {
  int sel = 0;
  while (true) {
    const ui::MenuItem items[] = {
        {"M5Stack IR unit", "Grove G1/G2"},
        {"IR module", "choose pins"},
    };
    sel = ui::menu(self.name, items, 2, sel);
    if (sel < 0) return;
    if (sel == 0) runIr(self);
    if (sel == 1) runIrBuiltin(self);
  }
}
