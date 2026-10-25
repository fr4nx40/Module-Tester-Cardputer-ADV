#include "core/ui.h"

#include <stdio.h>
#include <string.h>

#include "core/input.h"
#include "core/log.h"
#include "core/storage.h"
#include <SD.h>

namespace ui {

static const char* TAG = "ui";
// Default-constructed on purpose: M5Cardputer.Display may not exist yet during
// static initialisation, so the display is only referenced in begin()/present().
static M5Canvas gCanvas;

void begin() {
  M5Cardputer.Display.setRotation(1);  // landscape, 240x135
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  gCanvas.setColorDepth(16);
  if (!gCanvas.createSprite(W, H)) {
    LOGE(TAG, "createSprite(%d,%d) failed (out of memory?)", W, H);
  }
  LOGI(TAG, "display %dx%d", (int)M5Cardputer.Display.width(),
       (int)M5Cardputer.Display.height());
}

M5Canvas& canvas() { return gCanvas; }

void present() { gCanvas.pushSprite(&M5Cardputer.Display, 0, 0); }

// Small arrow, 7 px wide, centred vertically on cy. Returns its width.
static int drawArrow(char dir, int x, int cy, uint16_t color) {
  switch (dir) {
    case 'U':
      gCanvas.fillTriangle(x + 3, cy - 4, x, cy, x + 6, cy, color);
      gCanvas.fillRect(x + 2, cy, 3, 4, color);
      break;
    case 'D':
      gCanvas.fillTriangle(x + 3, cy + 4, x, cy, x + 6, cy, color);
      gCanvas.fillRect(x + 2, cy - 3, 3, 4, color);
      break;
    case 'L':
      gCanvas.fillTriangle(x, cy, x + 4, cy - 3, x + 4, cy + 3, color);
      gCanvas.fillRect(x + 4, cy - 1, 3, 3, color);
      break;
    default:  // 'R'
      gCanvas.fillTriangle(x + 6, cy, x + 2, cy - 3, x + 2, cy + 3, color);
      gCanvas.fillRect(x, cy - 1, 3, 3, color);
      break;
  }
  return 7;
}

// Draws one key: an arrow name becomes arrows, anything else is text.
static int drawKey(const char* key, int x, int cy) {
  const uint16_t c = TFT_WHITE;
  if (!strcmp(key, "Up")) return drawArrow('U', x, cy, c);
  if (!strcmp(key, "Down")) return drawArrow('D', x, cy, c);
  if (!strcmp(key, "Left")) return drawArrow('L', x, cy, c);
  if (!strcmp(key, "Right")) return drawArrow('R', x, cy, c);
  if (!strcmp(key, "UpDown")) {
    int w = drawArrow('U', x, cy, c);
    return w + 1 + drawArrow('D', x + w + 1, cy, c);
  }
  if (!strcmp(key, "LeftRight")) {
    int w = drawArrow('L', x, cy, c);
    return w + 1 + drawArrow('R', x + w + 1, cy, c);
  }
  gCanvas.setTextColor(c, TFT_BLACK);
  gCanvas.drawString(key, x, cy);
  return gCanvas.textWidth(key);
}

// Command text in the footer: mid grey, so the white keys stand out.
static const uint16_t kHintGrey = 0x9CD3;  // about RGB 153,154,153

static void drawHints(const char* footer) {
  const int cy = H - FOOTER_H / 2;
  gCanvas.setFont(&fonts::Font0);
  gCanvas.setTextDatum(middle_left);
  int x = 4;
  const char* p = footer;
  while (*p) {
    char seg[64];
    size_t n = 0;
    while (p[n] && p[n] != '|') n++;
    size_t len = n < sizeof(seg) - 1 ? n : sizeof(seg) - 1;
    memcpy(seg, p, len);
    seg[len] = 0;
    p += n;
    if (*p == '|') p++;

    if (seg[0] == '~') {
      gCanvas.setTextColor(kHintGrey, TFT_BLACK);
      gCanvas.drawString(seg + 1, x, cy);
      x += gCanvas.textWidth(seg + 1) + 8;
      continue;
    }
    char* space = strchr(seg, ' ');
    const char* cmd = "";
    if (space) {
      *space = 0;
      cmd = space + 1;
    }
    x += drawKey(seg, x, cy);
    if (cmd[0]) {
      x += 3;
      gCanvas.setTextColor(kHintGrey, TFT_BLACK);
      gCanvas.drawString(cmd, x, cy);
      x += gCanvas.textWidth(cmd);
    }
    x += 8;
  }
}

void drawFrame(const char* title, const char* footer, const char* right) {
  gCanvas.fillScreen(TFT_BLACK);

  // header
  gCanvas.fillRect(0, 0, W, HEADER_H, kGreenDim);
  gCanvas.setFont(&fonts::Font2);
  gCanvas.setTextColor(TFT_WHITE, kGreenDim);
  gCanvas.setTextDatum(middle_left);
  gCanvas.drawString(title, 4, HEADER_H / 2);
  if (right && right[0]) {
    gCanvas.setTextDatum(middle_right);
    gCanvas.drawString(right, W - 4, HEADER_H / 2);
  }

  // footer
  if (footer && footer[0]) drawHints(footer);
}

int keepVisible(int sel, int top) {
  if (sel < top) top = sel;
  if (sel >= top + ROWS) top = sel - ROWS + 1;
  if (top < 0) top = 0;
  return top;
}

void drawMenu(const char* title, const MenuItem* items, int count, int sel,
              int top, const char* footer, const char* right) {
  char pos[16];
  snprintf(pos, sizeof(pos), "%d/%d", count > 0 ? sel + 1 : 0, count);
  drawFrame(title, footer, right ? right : pos);

  if (count == 0) {
    gCanvas.setFont(&fonts::Font2);
    gCanvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    gCanvas.setTextDatum(middle_center);
    gCanvas.drawString("(empty)", W / 2,
                       HEADER_H + (H - HEADER_H - FOOTER_H) / 2);
    present();
    return;
  }

  for (int i = 0; i < ROWS; i++) {
    int idx = top + i;
    if (idx >= count) break;

    int y = HEADER_H + 2 + i * ROW_H;
    bool selected = (idx == sel);
    uint16_t bg = selected ? kGreen : TFT_BLACK;
    uint16_t fg = selected ? TFT_BLACK : TFT_LIGHTGREY;

    if (selected) gCanvas.fillRect(0, y, W, ROW_H, bg);

    gCanvas.setFont(&fonts::Font2);
    gCanvas.setTextColor(fg, bg);
    gCanvas.setTextDatum(middle_left);
    gCanvas.drawString(items[idx].label, 6, y + ROW_H / 2);

    if (items[idx].tag && items[idx].tag[0]) {
      int labelW = gCanvas.textWidth(items[idx].label);  // Font2, still set
      gCanvas.setFont(&fonts::Font0);
      int tagW = gCanvas.textWidth(items[idx].tag);
      if (6 + labelW + 8 + tagW + 6 <= W) {  // drop the tag if the label is too long
        gCanvas.setTextColor(selected ? TFT_BLACK : TFT_DARKGREY, bg);
        gCanvas.setTextDatum(middle_right);
        gCanvas.drawString(items[idx].tag, W - 6, y + ROW_H / 2);
      }
    }
  }
  present();
}

int menu(const char* title, const MenuItem* items, int count, int initial,
         const char* footer) {
  if (count <= 0) return -1;

  int sel = (initial >= 0 && initial < count) ? initial : 0;
  int top = 0;
  if (sel >= ROWS) top = sel - ROWS + 1;
  bool dirty = true;

  while (true) {
    if (dirty) {
      drawMenu(title, items, count, sel, top, footer);
      dirty = false;
    }

    InputEvent ev = inputPoll();
    switch (ev.key) {
      case Key::Up:
        sel = (sel + count - 1) % count;
        dirty = true;
        break;
      case Key::Down:
        sel = (sel + 1) % count;
        dirty = true;
        break;
      case Key::Select:
        LOGI(TAG, "menu '%s' -> %d (%s)", title, sel, items[sel].label);
        return sel;
      case Key::Back:
        LOGI(TAG, "menu '%s' -> back", title);
        return -1;
      default:
        break;
    }

    if (dirty) top = keepVisible(sel, top);
    delay(10);
  }
}

void message(const char* title, const char* line1, const char* line2) {
  drawFrame(title, "Enter/Del back");

  gCanvas.setFont(&fonts::Font2);
  gCanvas.setTextColor(TFT_WHITE, TFT_BLACK);
  gCanvas.setTextDatum(middle_center);
  int cy = HEADER_H + (H - HEADER_H - FOOTER_H) / 2;
  if (line2 && line2[0]) {
    gCanvas.drawString(line1, W / 2, cy - 9);
    gCanvas.drawString(line2, W / 2, cy + 9);
  } else {
    gCanvas.drawString(line1, W / 2, cy);
  }
  present();

  while (true) {
    InputEvent ev = inputPoll();
    if (ev.key == Key::Select || ev.key == Key::Back) return;
    delay(10);
  }
}

void toast(const char* text, uint32_t ms, uint16_t color) {
  const int bw = 200;
  const int bh = 34;
  const int x = (W - bw) / 2;
  const int y = (H - bh) / 2;
  gCanvas.fillRoundRect(x, y, bw, bh, 6, color);
  gCanvas.drawRoundRect(x, y, bw, bh, 6, TFT_WHITE);
  gCanvas.setFont(&fonts::Font2);
  gCanvas.setTextColor(TFT_WHITE, color);
  gCanvas.setTextDatum(middle_center);
  gCanvas.drawString(text, W / 2, H / 2);
  present();
  delay(ms);
}

bool saveScreenshot() {
  if (!storage::mounted()) {
    LOGW(TAG, "screenshot: no SD card");
    return false;
  }
  String dirStr = String(storage::kAppDir) + "/screenshots";
  const char* dir = dirStr.c_str();
  if (!SD.exists(dir)) {
    SD.mkdir(storage::kAppDir);
    SD.mkdir(dir);
  }

  static int next = 1;
  char path[64];
  do {
    snprintf(path, sizeof(path), "%s/shot_%03d.bmp", dir, next++);
  } while (SD.exists(path) && next < 10000);

  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    LOGE(TAG, "screenshot: cannot create %s", path);
    return false;
  }

  // 24-bit BMP, stored top-down (negative height). A 240-pixel row is exactly
  // 720 bytes, so no row padding is needed.
  const uint32_t rowBytes = W * 3;
  const uint32_t dataSize = rowBytes * H;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B';
  hdr[1] = 'M';
  uint32_t fileSize = 54 + dataSize;
  memcpy(&hdr[2], &fileSize, 4);
  uint32_t dataOffset = 54;
  memcpy(&hdr[10], &dataOffset, 4);
  uint32_t dibSize = 40;
  memcpy(&hdr[14], &dibSize, 4);
  int32_t w = W, h = -H;
  memcpy(&hdr[18], &w, 4);
  memcpy(&hdr[22], &h, 4);
  hdr[26] = 1;   // planes
  hdr[28] = 24;  // bits per pixel
  memcpy(&hdr[34], &dataSize, 4);
  f.write(hdr, sizeof(hdr));

  uint8_t row[W * 3];
  for (int y = 0; y < H; y++) {
    gCanvas.readRectRGB(0, y, W, 1, row);  // BGR order, which is what BMP wants
    f.write(row, sizeof(row));
  }
  f.close();
  LOGI(TAG, "screenshot saved: %s", path);

  // Confirmation drawn straight onto the screen so the saved image is not
  // affected, then the canvas is pushed back. The box is sized to the text
  // (rather than a fixed 60px) and anchored with a margin from the right
  // edge, so a longer message - "saved" vs "screenshot", say - can never
  // run off the edge of the 240-wide screen.
  auto& d = M5Cardputer.Display;
  d.setFont(&fonts::Font0);
  const char* msg = "saved";
  const int margin = 4;
  const int padX = 6;
  int boxW = d.textWidth(msg) + padX * 2;
  int boxX = W - margin - boxW;
  if (boxX < 0) boxX = 0;  // clamp if the message is wider than the screen
  d.fillRect(boxX, 2, boxW, 12, TFT_DARKGREEN);
  d.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  d.setTextDatum(middle_center);
  d.drawString(msg, boxX + boxW / 2, 8);
  delay(600);
  present();
  return true;
}

}  // namespace ui
