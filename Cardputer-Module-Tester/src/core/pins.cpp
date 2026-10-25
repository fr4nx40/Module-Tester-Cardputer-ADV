#include "core/pins.h"

#include <M5Cardputer.h>
#include <Preferences.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/input.h"
#include "core/log.h"
#include "core/ui.h"

namespace pins {

static const char* TAG = "pins";

namespace {

// GPIOs that are free to choose on the ESP32-S3. 19/20 are USB and 26-37 are
// flash / PSRAM, so they are left out. Check your board: pins used by the
// Cardputer itself (display, keyboard, SD card, ...) are not excluded here.
bool usable(int p) {
  return (p >= 0 && p <= 18) || p == 21 || (p >= 38 && p <= 48);
}

// Steps to the next / previous allowed value, wrapping. "none" sits before 0
// for optional pins.
int step(int cur, int dir, bool optional) {
  int list[64];
  int n = 0;
  if (optional) list[n++] = kNone;
  for (int p = 0; p <= 48; p++) {
    if (usable(p)) list[n++] = p;
  }
  int idx = 0;
  for (int i = 0; i < n; i++) {
    if (list[i] == cur) idx = i;
  }
  idx = (idx + dir + n) % n;
  return list[idx];
}

void keyName(const Config& cfg, int i, char* out, size_t n) {
  snprintf(out, n, "%s%d", cfg.key, i);
}

}  // namespace

void load(Config& cfg) {
  Preferences prefs;
  bool open = prefs.begin("pins", true);  // read-only fails until something is saved
  for (int i = 0; i < cfg.count; i++) {
    int v = cfg.defs[i].def;
    if (open) {
      char k[16];
      keyName(cfg, i, k, sizeof(k));
      v = prefs.getChar(k, cfg.defs[i].def);
      if (v != kNone && !usable(v)) v = cfg.defs[i].def;
      if (v == kNone && !cfg.defs[i].optional) v = cfg.defs[i].def;
    }
    cfg.value[i] = (int8_t)v;
  }
  if (open) prefs.end();
}

void save(const Config& cfg) {
  Preferences prefs;
  if (!prefs.begin("pins", false)) {
    LOGW(TAG, "cannot open flash storage");
    return;
  }
  for (int i = 0; i < cfg.count; i++) {
    char k[16];
    keyName(cfg, i, k, sizeof(k));
    prefs.putChar(k, cfg.value[i]);
  }
  prefs.end();
}

void summary(const Config& cfg, char* out, size_t n) {
  size_t pos = 0;
  out[0] = 0;
  for (int i = 0; i < cfg.count && pos + 5 < n; i++) {
    if (cfg.value[i] == kNone) {
      pos += snprintf(out + pos, n - pos, "%s-", i ? " " : "");
    } else {
      pos += snprintf(out + pos, n - pos, "%sG%d", i ? " " : "", cfg.value[i]);
    }
  }
}

void edit(Config& cfg) {
  const int rows = cfg.count + 1;  // last row resets to the defaults
  char tags[9][10];
  ui::MenuItem items[9];

  int sel = 0;
  int top = 0;
  char typed[4] = "";
  uint32_t typedAt = 0;
  bool dirty = true;

  while (true) {
    if (dirty) {
      for (int i = 0; i < cfg.count; i++) {
        bool dup = false;
        for (int j = 0; j < cfg.count; j++) {
          if (j != i && cfg.value[j] != kNone && cfg.value[j] == cfg.value[i]) dup = true;
        }
        if (cfg.value[i] == kNone) {
          snprintf(tags[i], sizeof(tags[i]), "none");
        } else {
          snprintf(tags[i], sizeof(tags[i]), "G%d%s", cfg.value[i], dup ? " !" : "");
        }
        items[i] = {cfg.defs[i].label, tags[i]};
      }
      items[cfg.count] = {"Reset to defaults", ""};
      ui::drawMenu(cfg.title, items, rows, sel, top,
                   "UpDown move|LeftRight change|Del back");
      dirty = false;
    }

    InputEvent ev = inputPoll();
    switch (ev.key) {
      case Key::Up:
        sel = (sel + rows - 1) % rows;
        typed[0] = 0;
        dirty = true;
        break;
      case Key::Down:
        sel = (sel + 1) % rows;
        typed[0] = 0;
        dirty = true;
        break;
      case Key::Left:
      case Key::Right:
        if (sel < cfg.count) {
          cfg.value[sel] = (int8_t)step(cfg.value[sel], ev.key == Key::Right ? 1 : -1,
                                        cfg.defs[sel].optional);
          dirty = true;
        }
        break;
      case Key::Select:
        if (sel == cfg.count) {
          for (int i = 0; i < cfg.count; i++) cfg.value[i] = cfg.defs[i].def;
          dirty = true;
        } else {
          cfg.value[sel] = (int8_t)step(cfg.value[sel], 1, cfg.defs[sel].optional);
          dirty = true;
        }
        break;
      case Key::Del:
        if (sel < cfg.count && cfg.defs[sel].optional) {
          cfg.value[sel] = kNone;
          dirty = true;
        }
        break;
      case Key::Char:
        if (sel < cfg.count && ev.ch >= '0' && ev.ch <= '9') {
          if (millis() - typedAt > 1500 || strlen(typed) >= 2) typed[0] = 0;
          size_t l = strlen(typed);
          typed[l] = ev.ch;
          typed[l + 1] = 0;
          typedAt = millis();
          int v = atoi(typed);
          if (usable(v)) {
            cfg.value[sel] = (int8_t)v;
            dirty = true;
          }
        }
        break;
      case Key::Back:
        save(cfg);
        return;
      default:
        break;
    }
    if (dirty) top = ui::keepVisible(sel, top);
    delay(10);
  }
}

}  // namespace pins
