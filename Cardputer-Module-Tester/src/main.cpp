#include <M5Cardputer.h>

#include <vector>

#include "core/filebrowser.h"
#include "core/home.h"
#include "core/input.h"
#include "core/log.h"
#include "core/pinfinder.h"
#include "core/registry.h"
#include "core/storage.h"
#include "core/ui.h"

static const char* TAG = "main";
static bool gSdReady = false;

static void screenModules() {
  std::vector<ui::MenuItem> items;
  const size_t n = moduleCount();
  items.reserve(n);
  for (size_t i = 0; i < n; i++) {
    items.push_back({modules()[i].name, modules()[i].bus});
  }

  int last = 0;
  while (true) {
    int i = ui::menu("Modules", items.data(), (int)n, last);
    if (i < 0) return;  // Back
    last = i;
    LOGI(TAG, "run module: %s", modules()[i].name);
    modules()[i].run(modules()[i]);
  }
}

// Stand-alone browser for checking the card. The IR and RFID screens will call
// browseFiles() themselves with a file-type filter.
static void screenSdBrowser() {
  String start = "/";
  String path;
  while (browseFiles("SD card", start.c_str(), nullptr, path)) {
    LOGI(TAG, "browser picked: %s", path.c_str());
    start = storage::parentDir(path);  // reopen where the file lives
    String name = path.substring(path.lastIndexOf('/') + 1);
    ui::message(name.c_str(), start.c_str());
  }
}
static void screenPinFinder() {
  pinfinder::run(40, 39, 14, -1, -1);
}


void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;   // no audio amp: keeps noise off the 3.3 V rail
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  M5Cardputer.begin(cfg, true);  // true = enable keyboard
  Serial.begin(115200);

  // Give the serial monitor a moment to attach so boot logs are not missed.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);

  LOGI(TAG, "Module Tester v%s starting", ui::kVersion);
  ui::begin();

  gSdReady = storage::begin();
  if (gSdReady) {
    storage::ensureDirs();
    LOGI(TAG, "SD ready");
  }
}

void loop() {
  static const ui::MenuItem kHome[] = {
      {"Modules", ""},
      {"SD browser", ""},
      {"Find chip pins", "SPI"}
  };
  static int last = 0;

  homeScreen(gSdReady);  // logo; Enter continues, A shows About

  int i = ui::menu("Module Tester", kHome, 3, last, "UpDown move|Enter select|Esc back");
  if (i < 0) return;  // Back returns to the logo screen
  last = i;

  switch (i) {
    case 0: screenModules(); break;
    case 1: screenSdBrowser(); break;
    case 2: screenPinFinder(); break;
  }
}