#include "core/modmenu.h"

#include "core/input.h"

namespace ui {

MenuAction menuAction(const char* title, const MenuItem* items, int count,
                      int& sel, int& top, const char* footer) {
  top = keepVisible(sel, top);
  drawMenu(title, items, count, sel, top, footer);

  while (true) {
    InputEvent ev = inputPoll();
    switch (ev.key) {
      case Key::Up:
        sel = (sel + count - 1) % count;
        top = keepVisible(sel, top);
        drawMenu(title, items, count, sel, top, footer);
        break;
      case Key::Down:
        sel = (sel + 1) % count;
        top = keepVisible(sel, top);
        drawMenu(title, items, count, sel, top, footer);
        break;
      case Key::Left:
        return {MenuAction::Left, sel};
      case Key::Right:
        return {MenuAction::Right, sel};
      case Key::Select:
        return {MenuAction::Pick, sel};
      case Key::Back:
        return {MenuAction::Back, -1};
      default:
        break;
    }
    delay(10);
  }
}

}  // namespace ui
