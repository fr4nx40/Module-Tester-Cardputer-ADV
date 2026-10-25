#pragma once
// Drawing helpers for the 240x135 Cardputer screen.
//
//   +------------------------------+  y=0
//   | title                  n/N   |  header (18 px)
//   +------------------------------+
//   | row 0                        |
//   | ...  6 rows x 16 px          |
//   | row 5                        |
//   +------------------------------+
//   | key hints                    |  footer (14 px)
//   +------------------------------+  y=135
#include <M5Cardputer.h>

namespace ui {

constexpr int W = 240;
constexpr int H = 135;
constexpr int HEADER_H = 18;
constexpr int FOOTER_H = 14;
constexpr int ROW_H = 16;
constexpr int ROWS = 6;

// UI greens (RGB565): headers use the dim one, the selected menu row the bright one.
constexpr uint16_t kGreen = 0x07E0;     // same as TFT_GREEN
constexpr uint16_t kGreenDim = 0x0400;  // dark green

constexpr const char* kVersion = "1.0";
// Footer hints: "key command" pairs separated by '|'. The key is drawn white,
// the command light grey. Key names Up, Down, UpDown, Left, Right and LeftRight
// are drawn as arrows. A hint starting with '~' is plain grey text.
constexpr const char* kDefaultFooter = "UpDown move|Enter select|Del back";

struct MenuItem {
  const char* label;
  const char* tag;  // small right-aligned text, e.g. the bus type; may be ""
};

// Call once after M5Cardputer.begin().
void begin();

// The off-screen canvas. Draw on it, then call present().
M5Canvas& canvas();
void present();

// Clear the canvas and draw header + footer. `right` is optional header text.
void drawFrame(const char* title, const char* footer = kDefaultFooter,
               const char* right = nullptr);

// Draw one frame of a list screen. `top` is the first visible row. `right` is
// the header's right-hand text; when null it shows "selected/total".
// menu() uses this; screens with their own input handling (e.g. the file
// browser) call it directly.
void drawMenu(const char* title, const MenuItem* items, int count, int sel,
              int top, const char* footer = kDefaultFooter,
              const char* right = nullptr);

// Returns the `top` value that keeps row `sel` inside the visible window.
int keepVisible(int sel, int top);

// Scrolling list. Returns the chosen index, or -1 if the user pressed Back.
// `initial` lets a caller restore the previous selection.
int menu(const char* title, const MenuItem* items, int count,
         int initial = 0, const char* footer = kDefaultFooter);

// Simple message screen. Blocks until Enter or Back.
void message(const char* title, const char* line1, const char* line2 = nullptr);

// Short notice drawn over whatever is on the canvas, shown for `ms`, then
// returns. The caller redraws its own screen afterwards.
void toast(const char* text, uint32_t ms = 900, uint16_t color = TFT_DARKGREEN);

// Saves what is on the Cardputer screen (the last thing drawn) as a 24-bit
// BMP in <storage::kAppDir>/screenshots on the SD card. Returns false if no
// card is mounted or the file cannot be written. Pressing Fn+S does this from
// anywhere (see input.cpp).
bool saveScreenshot();

}  // namespace ui
