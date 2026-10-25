#pragma once
// A menu loop for module screens that mix actions with settings rows
// (Left/Right change a setting, Enter runs an action).
#include "core/ui.h"

namespace ui {

struct MenuAction {
  enum Kind { Pick, Left, Right, Back } kind;
  int row;  // the selected row; -1 for Back
};

// Draws the menu, handles Up/Down itself, and returns on Enter, Left, Right
// or Back. `sel` and `top` keep their values between calls, so call it in a
// loop and rebuild the item tags each time round.
MenuAction menuAction(const char* title, const MenuItem* items, int count,
                      int& sel, int& top, const char* footer);

}  // namespace ui
