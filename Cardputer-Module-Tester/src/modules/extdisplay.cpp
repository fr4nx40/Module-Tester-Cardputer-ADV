#include "modules/extdisplay.h"

#include <M5Cardputer.h>
#include <SPI.h>
#include <Preferences.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <lgfx/v1/panel/Panel_ST7735.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "core/input.h"
#include "core/log.h"
#include "core/radiospi.h"
#include "core/ui.h"

static const char* TAG = "extdisp";

// External SPI display (ILI9341 240x320 or ST7735 128x160), driven with
// M5GFX / LovyanGFX as in the working radar sample: 3-wire bus (no MISO),
// SPI2 at 27 MHz, 16-bit colour. The size is chosen in the menu.

namespace {

// ---- fixed wiring (EXT 2.54-14P connector) ----
constexpr int kSck = 40;   // EXT pin 7
constexpr int kMiso = 39;  // not on the display; the shared bus pin only
constexpr int kMosi = 14;  // EXT pin 9
constexpr int kCs = 5;     // EXT pin 13
constexpr int kDc = 6;     // EXT pin 5
constexpr int kRst = 3;    // EXT pin 1

int kW = 320;  // set from the real display size after it starts
int kH = 240;

struct Panel_ILI9341_Local : public lgfx::v1::Panel_LCD {
  Panel_ILI9341_Local(void) {
    _cfg.memory_width = _cfg.panel_width = 240;
    _cfg.memory_height = _cfg.panel_height = 320;
  }

 protected:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0xEF, 3, 0x03, 0x80, 0x02,
        0xCF, 3, 0x00, 0xC1, 0x30,
        0xED, 4, 0x64, 0x03, 0x12, 0x81,
        0xE8, 3, 0x85, 0x00, 0x78,
        0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
        0xF7, 1, 0x20,
        0xEA, 2, 0x00, 0x00,
        0xC0, 1, 0x23,
        0xC1, 1, 0x10,
        0xC5, 2, 0x3E, 0x28,
        0xC7, 1, 0x86,
        0xB1, 2, 0x00, 0x18,
        0xB6, 3, 0x08, 0x82, 0x27,
        0xF2, 1, 0x00,
        0x26, 1, 0x01,
        0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
        0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
        CMD_SLPOUT, CMD_INIT_DELAY, 120,
        CMD_DISPON, CMD_INIT_DELAY, 120,
        0xFF, 0xFF,
    };
    return (listno == 0) ? list0 : nullptr;
  }
};

// The screen sizes you can pick in the menu. Same five wires for all of them;
// only the controller chip and the pixel grid differ.
struct Model {
  const char* label;  // menu text
  const char* chip;   // shown on the test pattern
  int w, h;           // visible pixels
  int rotation;
};
const Model kModels[2] = {
    {"240x320", "ILI9341", 240, 320, 7},  // shown landscape, 320 x 240
    {"128x160", "ST7735", 128, 160, 0},
};
int gModel = 0;

class LGFX_ILI9488 : public lgfx::v1::LGFX_Device {
  Panel_ILI9341_Local p9341;
  lgfx::v1::Panel_ST7735S p7735;  // the usual 1.8" / 1.44" ST7735R modules
  lgfx::v1::Bus_SPI bus;

 public:
  LGFX_ILI9488(spi_host_device_t host, uint32_t hz, int model) {
    auto b = bus.config();
    b.spi_host = host;
    b.spi_mode = 0;
    b.freq_write = (model == 0) ? hz : 16000000;  // ST7735 is happier below ~16 MHz
    b.freq_read = 16000000;
    b.spi_3wire = true;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;  // let the driver pick a free GDMA channel
    b.pin_sclk = kSck;
    b.pin_mosi = kMosi;
    b.pin_miso = -1;
    b.pin_dc = kDc;
    bus.config(b);

    lgfx::v1::Panel_Device* panel = (model == 0) ? (lgfx::v1::Panel_Device*)&p9341
                                                 : (lgfx::v1::Panel_Device*)&p7735;
    panel->setBus(&bus);

    auto p = panel->config();
    p.pin_cs = kCs;
    p.pin_rst = kRst;
    p.bus_shared = true;
    p.readable = false;
    p.invert = false;
    p.rgb_order = false;
    p.dlen_16bit = false;
    p.offset_x = 0;
    p.offset_y = 0;
    p.offset_rotation = 0;
    p.dummy_read_pixel = 8;
    p.dummy_read_bits = 1;
    if (model == 0) {
      p.memory_width = p.panel_width = 240;
      p.memory_height = p.panel_height = 320;
    } else if (model == 1) {  // ST7735, 128 x 160
      p.memory_width = p.panel_width = 128;
      p.memory_height = p.panel_height = 160;
    }
    panel->config(p);

    setPanel(panel);
  }
};

LGFX_ILI9488* gExt = nullptr;

// SPI2 at 27 MHz, as in the radar sample.
constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr uint32_t kClockHz = 27000000;

// Hook for flipping colours in software if the panel ever shows them inverted.
template <typename T>
inline T X(T c) { return c; }  // keeps the colour type (16-bit stays 16-bit)

// Starts the display the first time, then reuses it.
bool openDisplay() {
  // Quiet the other devices on the shared bus. G5 and G6 are the display's own.
  holdHigh(12);  // SD card
  holdHigh(13);  // CC1101 chip select
  holdHigh(4);   // NRF24 chip select
  pinMode(kCs, OUTPUT);
  digitalWrite(kCs, HIGH);

  // Free the Arduino SPI (it owns SPI2 and pads 40/14) before the display takes them.
  SPI.end();
  pinMode(kRst, OUTPUT);
  digitalWrite(kRst, HIGH);
  Serial.println("[extdisp] starting display");
  if (!gExt) {
    gExt = new LGFX_ILI9488(kHost, kClockHz, gModel);
  }
  // init() is safe to repeat: it takes the pads back from the Arduino SPI.
  if (!gExt->init()) {
    Serial.println("[extdisp] display init failed");
    return false;
  }
  Serial.printf("[extdisp] %s %s init ok\n", kModels[gModel].chip, kModels[gModel].label);
  gExt->setRotation(kModels[gModel].rotation);
  kW = gExt->width();
  kH = gExt->height();
  Serial.printf("[extdisp] size %d x %d\n", kW, kH);
  gExt->fillScreen(X(TFT_BLACK));
  return true;
}

// Gives the shared pins back to the normal SPI bus used by the SD card and
// the radios.
void closeDisplay() {
  if (gExt) {
    gExt->endWrite();
    gExt->waitDisplay();
  }
  digitalWrite(kCs, HIGH);
  SPI.end();
  SPI.begin(kSck, kMiso, kMosi, -1);
}

// Same reasoning as openDisplayGuarded() below: endWrite()/waitDisplay() are
// also SPI/LGFX calls, so they get the same timeout guard on the way out.
constexpr uint32_t kCloseTimeoutMs = 1500;
SemaphoreHandle_t gCloseDone = nullptr;

void closeDisplayTask(void*) {
  closeDisplay();
  xSemaphoreGive(gCloseDone);
  vTaskDelete(nullptr);
}

void closeDisplayGuarded() {
  if (!gCloseDone) gCloseDone = xSemaphoreCreateBinary();
  xTaskCreate(closeDisplayTask, "extdisp_close", 4096, nullptr, 1, nullptr);
  if (xSemaphoreTake(gCloseDone, pdMS_TO_TICKS(kCloseTimeoutMs)) != pdTRUE) {
    Serial.println("[extdisp] close timed out - leaving SPI as-is");
    // Best effort: get the shared bus back for the SD card / radios even if
    // the display side of closeDisplay() never finished.
    SPI.begin(kSck, kMiso, kMosi, -1);
  }
}

// openDisplay() can take an unknown amount of time inside the SPI/LGFX
// driver. We've never pinned down a single call that blocks forever with
// nothing wired to the EXT connector, but the symptom is real, so run it on
// its own task with a hard deadline: if it hasn't finished in kOpenTimeoutMs,
// give up and tell the user instead of hanging the whole app. If it really
// was stuck, the task is simply abandoned (SPI calls here don't allocate
// anything that needs freeing); if it finishes late, gExt is left in
// whatever state init() left it, same as if we'd waited.
constexpr uint32_t kOpenTimeoutMs = 1500;
SemaphoreHandle_t gOpenDone = nullptr;
volatile bool gOpenResult = false;

void openDisplayTask(void*) {
  gOpenResult = openDisplay();
  xSemaphoreGive(gOpenDone);
  vTaskDelete(nullptr);
}

bool openDisplayGuarded() {
  if (!gOpenDone) gOpenDone = xSemaphoreCreateBinary();
  gOpenResult = false;
  xTaskCreate(openDisplayTask, "extdisp_open", 4096, nullptr, 1, nullptr);
  if (xSemaphoreTake(gOpenDone, pdMS_TO_TICKS(kOpenTimeoutMs)) == pdTRUE) {
    return gOpenResult;
  }
  Serial.println("[extdisp] open timed out - nothing wired to EXT?");
  return false;
}

// ----------------------------------------------------------------- drawing

uint32_t hueColor(int x) {
  int h = x * 6 * 256 / kW;
  int seg = h >> 8, f = h & 255;
  int r = 0, g = 0, b = 0;
  switch (seg) {
    case 0: r = 255; g = f; break;
    case 1: r = 255 - f; g = 255; break;
    case 2: g = 255; b = f; break;
    case 3: g = 255 - f; b = 255; break;
    case 4: r = f; b = 255; break;
    default: r = 255; b = 255 - f; break;
  }
  return X(gExt->color888(r, g, b));
}

// Colour bars, grey ramp, hue sweep, corner markers, border and a caption.
void drawPattern() {
  auto& d = *gExt;
  d.fillScreen(X(TFT_BLACK));

  const uint16_t bars[8] = {TFT_WHITE, TFT_YELLOW, TFT_CYAN, TFT_GREEN,
                            TFT_MAGENTA, TFT_RED, TFT_BLUE, 0x8410};
  for (int i = 0; i < 8; i++) d.fillRect(i * kW / 8, 0, kW / 8 + 1, kH * 3 / 8, X(bars[i]));

  for (int x = 0; x < kW; x++) {
    int g = x * 255 / (kW - 1);
    d.drawFastVLine(x, kH * 3 / 8, kH / 8, X(d.color888(g, g, g)));
    d.drawFastVLine(x, kH / 2, kH / 8, hueColor(x));
  }

  d.setTextDatum(middle_center);
  d.setTextColor(X(TFT_WHITE), X(TFT_BLACK));
  const bool big = kW >= 240;
  d.setFont(&fonts::Font4);
  d.setTextSize(big ? 2 : 1);
  d.drawString(big ? "DISPLAY OK" : "OK", kW / 2, kH * 3 / 4);
  d.setTextSize(1);
  d.setFont(big ? (const lgfx::IFont*)&fonts::Font2 : (const lgfx::IFont*)&fonts::Font0);
  d.setTextColor(X(TFT_LIGHTGREY), X(TFT_BLACK));
  {
    char sz[32];
    snprintf(sz, sizeof(sz), "%s  %d x %d", kModels[gModel].chip, kW, kH);
    d.drawString(sz, kW / 2, kH * 9 / 10);
  }

  // Border and corner markers (top-left red, top-right green, bottom-left
  // blue, bottom-right yellow) to check orientation.
  d.drawRect(0, 0, kW, kH, X(TFT_WHITE));
  d.drawRect(1, 1, kW - 2, kH - 2, X(TFT_WHITE));
  d.fillRect(6, 6, 14, 14, X(TFT_RED));
  d.fillRect(kW - 20, 6, 14, 14, X(TFT_GREEN));
  d.fillRect(6, kH - 20, 14, 14, X(TFT_BLUE));
  d.fillRect(kW - 20, kH - 20, 14, 14, X(TFT_YELLOW));
}

// ------------------------------------------------------------------ screens

void pinsScreen() {
  const ui::MenuItem items[] = {
      {"VCC", "3.3V or 5V"},
      {"GND", "GND"},
      {"SCK", "G40  EXT 7"},
      {"MOSI", "G14  EXT 9"},
      {"CS", "G5  EXT 13"},
      {"DC", "G6  EXT 5"},
      {"RST", "G3  EXT 1"},
      {"BLK / LED", "3.3V"},
  };
  int sel = 0;
  while (true) {
    sel = ui::menu("Display pins", items, 8, sel, "UpDown scroll|Del back");
    if (sel < 0) return;
  }
}

struct Page {
  const char* name;
  uint16_t fill;  // TFT_ constants are RGB565, so keep them 16-bit.
};
// Pages 0-5 are a flat fill (0 means "run drawPattern() instead of a fill").
// Pages 6-7 are animated and ignore `fill`; they get their own init/step
// lambdas in testScreen() below.
constexpr int kPageMatrix = 6;
constexpr int kPageStarfield = 7;

// startPage picks which page a menu entry opens directly to (0 = Test
// pattern, 1 = Red, ...); Left/Right still cycle through all of them from
// there, so any entry point reaches every page, including Matrix rain and
// Starfield at the end.
void testScreen(const Module& self, int startPage) {
  if (!openDisplayGuarded()) {
    ui::message(self.name, "Display did not start", "Check the wiring");
    return;
  }

  // The SD card shares these pins while the display is running, so S must
  // not try to save a screenshot here.
  inputTextMode(true);

  const Page pages[] = {
      {"Test pattern", 0},
      {"Red", TFT_RED},
      {"Green", TFT_GREEN},
      {"Blue", TFT_BLUE},
      {"White", TFT_WHITE},
      {"Black", TFT_BLACK},
      {"Matrix rain", 0},
      {"Starfield", 0},
  };
  const int count = sizeof(pages) / sizeof(pages[0]);
  int page = (startPage >= 0 && startPage < count) ? startPage : 0;
  bool dirty = true;

  // Matrix rain state.
  constexpr int kCharW = 6, kCharH = 8, kMaxCols = 90, kTrail = 6;
  int16_t dropY[kMaxCols];
  uint8_t mxSpeed[kMaxCols];
  uint8_t mxTick[kMaxCols];
  int mxCols = 0;
  uint32_t mxLast = 0;

  // Starfield state.
  constexpr int kStars = 256;
  uint8_t sx[kStars], sy[kStars], sz[kStars];
  uint8_t spawnDepth = 255;

  auto initMatrix = [&]() {
    auto& d = *gExt;
    d.fillScreen(X(TFT_BLACK));
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    mxCols = kW / kCharW;
    if (mxCols > kMaxCols) mxCols = kMaxCols;
    for (int i = 0; i < mxCols; i++) {
      dropY[i] = (int16_t)(-(int)random(kH));
      mxSpeed[i] = 1 + (uint8_t)random(3);
      mxTick[i] = 0;
    }
    mxLast = 0;
  };

  auto stepMatrix = [&]() {
    if (millis() - mxLast < 35) return;
    mxLast = millis();
    auto& d = *gExt;
    for (int i = 0; i < mxCols; i++) {
      if (++mxTick[i] < mxSpeed[i]) continue;
      mxTick[i] = 0;
      int x = i * kCharW;

      int eraseY = dropY[i] - kTrail * kCharH;
      if (eraseY >= 0 && eraseY < kH) d.fillRect(x, eraseY, kCharW, kCharH, X(TFT_BLACK));

      if (dropY[i] >= 0 && dropY[i] < kH) {
        char ch = (char)(32 + random(95));
        d.setTextColor(X(d.color888(0, 255, 90)), X(TFT_BLACK));
        d.drawChar(ch, x, dropY[i]);
      }
      dropY[i] = (int16_t)(dropY[i] + kCharH);
      if (dropY[i] > kH + kTrail * kCharH) {
        dropY[i] = (int16_t)(-(int)random(kH / 2 + 1));
        mxSpeed[i] = 1 + (uint8_t)random(3);
      }
    }
  };

  auto initStarfield = [&]() {
    gExt->fillScreen(X(TFT_BLACK));
    for (int i = 0; i < kStars; i++) sz[i] = 0;
    spawnDepth = 255;
  };

  auto stepStarfield = [&]() {
    auto& d = *gExt;
    for (int i = 0; i < kStars; i++) {
      if (sz[i] <= 1) {
        sx[i] = (uint8_t)random(0, 256);
        sy[i] = (uint8_t)random(0, 256);
        sz[i] = spawnDepth--;
        continue;
      }
      int ox = ((int)sx[i] - 128) * 256 / sz[i] + kW / 2;
      int oy = ((int)sy[i] - 128) * 256 / sz[i] + kH / 2;
      if (ox >= 0 && oy >= 0 && ox < kW && oy < kH) d.drawPixel(ox, oy, X(TFT_BLACK));

      sz[i] -= 3;
      if (sz[i] <= 1) continue;
      int x = ((int)sx[i] - 128) * 256 / sz[i] + kW / 2;
      int y = ((int)sy[i] - 128) * 256 / sz[i] + kH / 2;
      if (x >= 0 && y >= 0 && x < kW && y < kH) {
        uint8_t v = 255 - sz[i];
        d.drawPixel(x, y, X(d.color888(v, v, v)));
      } else {
        sz[i] = 0;  // off screen: let it respawn
      }
    }
    delay(16);
  };

  while (true) {
    if (dirty) {
      if (page == 0) {
        drawPattern();
      } else if (page == kPageMatrix) {
        initMatrix();
      } else if (page == kPageStarfield) {
        initStarfield();
      } else {
        gExt->fillScreen(X(pages[page].fill));
      }
      char right[16];
      snprintf(right, sizeof(right), "%d/%d", page + 1, count);
      ui::drawFrame("Screen Test", "Enter next|Del back", right);
      auto& c = ui::canvas();
      c.setFont(&fonts::Font2);
      c.setTextDatum(middle_center);
      c.setTextColor(TFT_WHITE, TFT_BLACK);
      c.drawString(pages[page].name, ui::W / 2, 52);
      c.setTextColor(TFT_DARKGREY, TFT_BLACK);
      c.setFont(&fonts::Font0);
      c.drawString("Look at the external screen", ui::W / 2, 80);
      c.drawString("It cannot be read back (no MISO)", ui::W / 2, 94);
      ui::present();
      dirty = false;
    }

    if (page == kPageMatrix) stepMatrix();
    if (page == kPageStarfield) stepStarfield();

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;
    if (ev.key == Key::Select || ev.key == Key::Right) {
      page = (page + 1) % count;
      dirty = true;
    } else if (ev.key == Key::Left) {
      page = (page + count - 1) % count;
      dirty = true;
    }
    // The animated pages already pace themselves (stepMatrix/stepStarfield);
    // the static pages still want the old idle delay so input polling
    // doesn't spin.
    if (page != kPageMatrix && page != kPageStarfield) delay(10);
  }
  inputTextMode(false);
  closeDisplayGuarded();
}

}  // namespace

// ------------------------------------------------------------------- entry

void loadModel() {
  Preferences p;
  p.begin("extdisp", true);
  int m = p.getUChar("model", 0);
  p.end();
  gModel = (m >= 0 && m < 2) ? m : 0;
}

void saveModel() {
  Preferences p;
  p.begin("extdisp", false);
  p.putUChar("model", (uint8_t)gModel);
  p.end();
}

void runExtDisplay(const Module& self) {
  loadModel();
  int sel = 0;
  while (true) {
    const ui::MenuItem items[] = {
        {"Pins", "fixed"},
        {"Screen", kModels[gModel].label},
        {"Screen Test", ""},
    };
    sel = ui::menu(self.name, items, 3, sel);
    if (sel < 0) return;
    if (sel == 0) pinsScreen();
    if (sel == 1) {
      gModel = (gModel + 1) % 2;
      saveModel();
      // Start again with the new panel. The old object stays in place on
      // purpose; deleting it while the bus is shared can hang.
      gExt = nullptr;
    }
    // "Screen Test" always opens on page 0 (Test pattern); Enter/Right (and
    // Left) cycle through Test pattern/Red/Green/Blue/White/Black/Matrix
    // rain/Starfield from there - all in one screen, inside testScreen().
    if (sel == 2) testScreen(self, 0);
  }
}
