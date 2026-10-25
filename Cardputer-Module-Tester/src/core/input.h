#pragma once
// Keyboard abstraction for the Cardputer ADV.
//   ;  up        .  down        ,  left        /  right
//   Enter select   `  back      Backspace = Del
// Anything else is reported as Key::Char (for the search box later).
#include <Arduino.h>

enum class Key : uint8_t { None, Up, Down, Left, Right, Select, Back, Del, Char };

struct InputEvent {
  Key key = Key::None;
  char ch = 0;  // valid when key == Key::Char
};

// Call often from loops. Also runs M5Cardputer.update().
// Returns one event per key press, Key::None when nothing happened.
InputEvent inputPoll();

// While true, Fn+S does nothing. Used where the SD card's pins are borrowed
// (the external display test). Otherwise Fn+S saves a screenshot to the SD card.
void inputTextMode(bool on);
