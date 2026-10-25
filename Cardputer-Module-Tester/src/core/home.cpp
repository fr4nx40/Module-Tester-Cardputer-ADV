#include "core/home.h"

#include <M5Cardputer.h>
#include <stdio.h>
#include <string.h>

#include "core/input.h"
#include "core/ui.h"

namespace {

// "MODULE" / "TESTER" in ASCII art. Drawn as pixel blocks, not text, so the
// 83 columns fit the 240 px screen. Each word is centred by the code, so the
// rows need no padding.
const char* const kLogo[] = {
    R"X(ooo        ooooo   .oooooo.   oooooooooo.   ooooo     ooo ooooo        oooooooooooo)X",
    R"X(`88.       .888'  d8P'  `Y8b  `888'   `Y8b  `888'     `8' `888'        `888'     `8)X",
    R"X( 888b     d'888  888      888  888      888  888       8   888          888        )X",
    R"X( 8 Y88. .P  888  888      888  888      888  888       8   888          888oooo8   )X",
    R"X( 8  `888'   888  888      888  888      888  888       8   888          888    "   )X",
    R"X( 8    Y     888  `88b    d88'  888     d88'  `88.    .8'   888       o  888       o)X",
    R"X(o8o        o888o  `Y8bood8P'  o888bood8P'      `YbodP'    o888ooooood8 o888ooooood8)X",
    R"X(ooooooooooooo oooooooooooo  .oooooo..o ooooooooooooo oooooooooooo ooooooooo.)X",
    R"X(8'   888   `8 `888'     `8 d8P'    `Y8 8'   888   `8 `888'     `8 `888   `Y88.)X",
    R"X(     888       888         Y88bo.           888       888          888   .d88')X",
    R"X(     888       888oooo8     `"Y8888o.       888       888oooo8     888ooo88P')X",
    R"X(     888       888    "         `"Y88b      888       888    "     888`88b.)X",
    R"X(     888       888       o oo     .d8P      888       888       o  888  `88b.)X",
    R"X(    o888o     o888ooooood8 8""88888P'      o888o     o888ooooood8 o888o  o888o)X",
};
const int kLogoCols = 83;  // width of the widest word, MODULE
const int kWordRows = 7;
const int kWords = 2;

const uint16_t kGreen = TFT_GREEN;
const uint16_t kDim = 0x0400;  // dark green

// Heavy characters become solid blocks, punctuation becomes a thin dim mark.
// Both words use the same character size and are centred on their own.
void drawLogo(int top) {
  auto& c = ui::canvas();
  const int width = ui::W - 8;  // pixels used by MODULE

  for (int w = 0; w < kWords; w++) {
    // Leftmost and rightmost used column of this word.
    int minC = 1000;
    int maxC = 0;
    for (int r = 0; r < kWordRows; r++) {
      const char* line = kLogo[w * kWordRows + r];
      for (int i = 0; line[i]; i++) {
        if (line[i] == ' ') continue;
        if (i < minC) minC = i;
        if (i + 1 > maxC) maxC = i + 1;
      }
    }
    const int wordW = (maxC - minC) * width / kLogoCols;
    const int x0 = (ui::W - wordW) / 2;

    for (int r = 0; r < kWordRows; r++) {
      const char* line = kLogo[w * kWordRows + r];
      int y = top + r * 4 + w * (kWordRows * 4 + 6);
      for (int i = 0; line[i]; i++) {
        char ch = line[i];
        if (ch == ' ') continue;
        int xa = x0 + (i - minC) * width / kLogoCols;
        int xb = x0 + (i + 1 - minC) * width / kLogoCols;
        int cw = xb - xa;
        if (ch == '.' || ch == ',') {
          c.fillRect(xa, y + 1, cw, 2, kDim);
        } else if (ch == '\'' || ch == '`' || ch == '"') {
          c.fillRect(xa, y, cw, 2, kDim);
        } else {
          c.fillRect(xa, y, cw, 3, kGreen);
        }
      }
    }
  }
}

void drawHome(bool sdReady, bool cursorOn) {
  ui::drawFrame("", "Enter menu|A about");
  auto& c = ui::canvas();
  c.fillRect(0, 0, ui::W, ui::HEADER_H, TFT_BLACK);  // no title bar on this screen

  drawLogo(6);

  c.setFont(&fonts::Font2);
  c.setTextDatum(middle_left);
  c.setTextColor(kGreen, TFT_BLACK);
  char line[48];
  snprintf(line, sizeof(line), "Cardputer ADV test bench   v%s", ui::kVersion);
  c.drawString(line, 6, 80);
  c.setTextColor(sdReady ? kGreen : TFT_ORANGE, TFT_BLACK);
  c.drawString(sdReady ? "SD card: ready" : "SD card: not found", 6, 95);
  ui::present();
}

// Lines are written to fit 38 characters in the small font. A line in capitals
// is drawn as a heading.
const char* const kAbout[] = {
    "A test bench for M5Stack units or",
    "components, running on the",
    "Cardputer ADV.",
    "",
    "MODULES",
    "CC1101, External Display, GPS, IR,",
    "M5Stack LoRa Cap (with its GNSS),",
    "M5Stack RFID 2, NRF24L01, PN532 and",
    "Speaker. Pick a unit and open its",
    "test screen.",
    "",
    "SD BROWSER",
    "Browse the card. IR files live in",
    "/ModuleTests/ir, RFID in /rfid and",
    "melodies (.fmf) in /music.",
    "",
    "SPEAKER",
    "Plays RTTTL melodies",
    ".fmf from SD) through the",
    "onboard codec.",
    "",
    "SCREENSHOTS",
    "Fn+S from anywhere saves a BMP to",
    "/ModuleTests/screenshots.",
    "",
    "KEYS",
    "Arrows move, Enter selects, Del goes",
    "back. Hold an arrow to scroll fast.",
    "",
    "WIRING",
    "Grove port: white wire = G1, yellow",
    "wire = G2. Only one unit that uses",
    "those pins can run at a time.",
    "The LoRa Cap has its own pins, so it",
    "does not clash with the Grove port.",
    "",
    "Everything here is a test tool, not a",
    "finished product. Check the serial",
    "monitor (115200) for detailed logs.",
    "",
    "CREDITS",
    "Thank you to:",
    "",
    "https://github.com/lshaf",
    "- Unigeek",
    "",
    "https://github.com/BruceDevices",
    "- Bruce Firmware",
    "",
    "https://github.com/anthropics/",
    "claude-desktop-buddy",
    "- Claude Desktop Buddy",
    "",
    "https://github.com/Lucaslhm/",
    "Flipper-IRDB",
    "- Flipper IR Files",
    "",
    "https://github.com/UberGuidoZ/",
    "- Flipper FMF files",
    "",
    "CREATED BY",
    "Fr4nx40",
    "https://github.com/fr4nx40",
};
const int kAboutCount = sizeof(kAbout) / sizeof(kAbout[0]);

bool isHeading(const char* s) {
  if (!s[0]) return false;
  for (const char* p = s; *p; p++) {
    if (*p >= 'a' && *p <= 'z') return false;
  }
  return true;
}

}  // namespace

void aboutScreen() {
  const int lineH = 10;
  const int visible = (ui::H - ui::HEADER_H - ui::FOOTER_H - 4) / lineH;
  int top = 0;
  const int maxTop = kAboutCount > visible ? kAboutCount - visible : 0;
  bool dirty = true;

  while (true) {
    if (dirty) {
      char right[16];
      snprintf(right, sizeof(right), "v%s", ui::kVersion);
      ui::drawFrame("About", "UpDown scroll|Del back", right);
      auto& c = ui::canvas();
      c.setFont(&fonts::Font0);
      c.setTextDatum(top_left);
      for (int i = 0; i < visible && top + i < kAboutCount; i++) {
        const char* s = kAbout[top + i];
        c.setTextColor(isHeading(s) ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
        c.drawString(s, 4, ui::HEADER_H + 4 + i * lineH);
      }
      if (maxTop > 0) {  // scroll bar
        int area = ui::H - ui::HEADER_H - ui::FOOTER_H;
        int barH = area * visible / kAboutCount;
        int barY = ui::HEADER_H + (area - barH) * top / maxTop;
        c.fillRect(ui::W - 3, barY, 2, barH, TFT_DARKGREY);
      }
      ui::present();
      dirty = false;
    }

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back || ev.key == Key::Select) return;
    if (ev.key == Key::Up && top > 0) {
      top--;
      dirty = true;
    } else if (ev.key == Key::Down && top < maxTop) {
      top++;
      dirty = true;
    }
    delay(10);
  }
}

void homeScreen(bool sdReady) {
  bool cursorOn = true;
  uint32_t lastBlink = millis();
  bool dirty = true;

  while (true) {
    if (dirty) {
      drawHome(sdReady, cursorOn);
      dirty = false;
    }

    InputEvent ev = inputPoll();
    if (ev.key == Key::Select) return;
    if (ev.key == Key::Char && (ev.ch == 'a' || ev.ch == 'A')) {
      aboutScreen();
      dirty = true;
    }

    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      cursorOn = !cursorOn;
      dirty = true;
    }
    delay(10);
  }
}