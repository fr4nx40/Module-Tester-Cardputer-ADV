#include "core/input.h"

#include <M5Cardputer.h>

#include "core/log.h"
#include "core/ui.h"

static const char* TAG = "input";
static bool gTextMode = false;

void inputTextMode(bool on) { gTextMode = on; }

InputEvent inputPoll() {
  M5Cardputer.update();

  // Holding an arrow key repeats it: after kRepeatDelayMs, one move every
  // kRepeatEveryMs, until the key is released.
  static const uint32_t kRepeatDelayMs = 400;
  static const uint32_t kRepeatEveryMs = 60;
  static Key heldKey = Key::None;
  static char heldChar = 0;
  static uint32_t heldSince = 0;
  static uint32_t lastRepeat = 0;

  InputEvent ev;
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
    if (heldKey != Key::None) {
      auto hs = M5Cardputer.Keyboard.keysState();
      if (!M5Cardputer.Keyboard.isPressed() || hs.word.empty() || hs.word.front() != heldChar) {
        heldKey = Key::None;  // released, or a different key is down
      } else {
        uint32_t now = millis();
        if (now - heldSince >= kRepeatDelayMs && now - lastRepeat >= kRepeatEveryMs) {
          lastRepeat = now;
          ev.key = heldKey;
          return ev;
        }
      }
    }
    return ev;
  }
  heldKey = Key::None;  // a fresh key press: decide below whether it can repeat

  auto st = M5Cardputer.Keyboard.keysState();

  if (st.enter) {
    ev.key = Key::Select;
  } else if (st.del) {
    ev.key = Key::Back;  // Del is the back button
  } else if (!st.word.empty()) {
    char c = st.word.front();
    // Fn + S saves a screenshot.
    if (st.fn && (c == 's' || c == 'S')) {
      if (!gTextMode) ui::saveScreenshot();
      return ev;  // handled here, not passed on
    }
    switch (c) {
      case ';': ev.key = Key::Up; break;
      case '.': ev.key = Key::Down; break;
      case ',': ev.key = Key::Left; break;
      case '/': ev.key = Key::Right; break;
      default:
        ev.key = Key::Char;
        ev.ch = c;
        break;
    }
  }

  if (ev.key == Key::Up || ev.key == Key::Down || ev.key == Key::Left || ev.key == Key::Right) {
    heldKey = ev.key;
    heldChar = st.word.front();
    heldSince = millis();
    lastRepeat = heldSince;
  }

  if (ev.key != Key::None) {
    LOGD(TAG, "key=%d ch=%c", (int)ev.key, ev.ch ? ev.ch : ' ');
  }
  return ev;
}