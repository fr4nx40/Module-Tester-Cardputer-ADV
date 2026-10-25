#include "modules/speaker.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <cmath>
#include <cctype>
#include <cstring>

#include "core/filebrowser.h"
#include "core/input.h"
#include "core/storage.h"
#include "core/ui.h"

namespace {

// The Cardputer ADV plays audio through an ES8311 I2C-controlled codec
// instead of a plain I2S DAC, and the M5Unified/M5Cardputer library version
// pinned in this project doesn't configure it. So this talks to the codec
// and the I2S bus directly instead of going through M5Cardputer.Speaker.
// Pins and the ES8311 register sequence are from a working reference for
// this exact board: https://gist.github.com/MultiMote/c0be65e1517a9ae136bcacaeb693d71d

constexpr int kI2cSda = 8;
constexpr int kI2cScl = 9;
constexpr uint8_t kCodecAddr = 0x18;

constexpr int kI2sBclk = 41;
constexpr int kI2sWs = 43;
constexpr int kI2sDout = 42;
constexpr uint32_t kSampleRate = 48000;

bool gAudioReady = false;
bool gAudioTried = false;  // only attempt the (slow-ish) init once per boot

bool codecWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kCodecAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool codecPresent() {
  Wire.beginTransmission(kCodecAddr);
  return Wire.endTransmission() == 0;
}

bool codecInit() {
  bool ok = true;
  ok &= codecWrite(0x00, 0x80);  // reset, CSM power on
  delay(2);
  ok &= codecWrite(0x01, 0xB5);  // clock manager: MCLK sourced from BCLK
  ok &= codecWrite(0x02, 0x18);  // clock premultiplier -> internal 256*fs
  ok &= codecWrite(0x0D, 0x01);  // power up analog
  ok &= codecWrite(0x12, 0x00);  // power up DAC
  ok &= codecWrite(0x13, 0x10);  // enable speaker output drive
  ok &= codecWrite(0x32, 0xBF);  // DAC volume, 0 dB (volumeScreen rewrites this)
  ok &= codecWrite(0x37, 0x08);  // bypass DAC equalizer
  return ok;
}

// Starts I2C + the codec + the I2S TX channel, once. Returns false (and
// leaves audio off) if the codec never answers.
bool audioInit() {
  if (gAudioTried) return gAudioReady;
  gAudioTried = true;

  Wire.begin(kI2cSda, kI2cScl, 100000);
  if (!codecPresent()) {
    Serial.println("[speaker] ES8311 not found on internal I2C (G8/G9)");
    return false;
  }
  if (!codecInit()) {
    Serial.println("[speaker] ES8311 register write failed");
    return false;
  }

  // This project's Arduino-ESP32 core (platform espressif32@6.7.0) is on
  // IDF 4.4, which only has the legacy port-based I2S driver (driver/i2s.h),
  // not the newer channel-based driver/i2s_std.h API.
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  // A deeper DMA ring than before (was 4*256): more slack between writes so
  // a slow loop iteration (drawing, SD access) doesn't starve the I2S bus
  // and cause the choppy/dropout sound reported with the old settings.
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 512;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
    Serial.println("[speaker] i2s_driver_install failed");
    return false;
  }

  i2s_pin_config_t pins = {};
  // Some IDF versions add mck_io_num to this struct; zero-init above would
  // otherwise leave it at GPIO0, which the driver could then wrongly try to
  // drive as a clock output. We don't use MCLK (the codec derives its own
  // clock from BCLK), so mark it explicitly unused.
  // If this line fails to compile ("no member named 'mck_io_num'"), this
  // IDF version's struct doesn't have the field at all - just delete it.
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = kI2sBclk;
  pins.ws_io_num = kI2sWs;
  pins.data_out_num = kI2sDout;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    Serial.println("[speaker] i2s_set_pin failed");
    return false;
  }
  Serial.println("[speaker] ES8311 + I2S ready");
  gAudioReady = true;
  return true;
}

int16_t gAmplitude = 8000;  // ~ -6 dBFS headroom at 16-bit; volumeScreen scales this

// Writes one note - `hz` Hz for `ms` milliseconds (hz <= 0 is a rest) -
// straight to the I2S bus, with a short linear fade in/out. Flipper's own
// RTTTL player hard-switches frequency and amplitude between notes, which is
// exactly what caused the loud clicking: each note boundary was a sudden
// jump in the waveform. A few milliseconds of ramp at each end removes that
// without being audible as a separate effect.
void playNote(float hz, uint32_t ms) {
  if (!gAudioReady || ms == 0) return;
  constexpr int kBufFrames = 256;
  static int16_t buf[kBufFrames * 2];
  static float phase = 0.0f;
  constexpr float kTwoPi = 6.2831853f;

  uint32_t totalFrames = (uint32_t)(((uint64_t)kSampleRate * ms) / 1000);
  if (totalFrames == 0) return;
  uint32_t rampFrames = kSampleRate / 250;  // 4 ms fade in/out
  if (rampFrames * 2 > totalFrames) rampFrames = totalFrames / 2;

  float step = (hz > 0) ? (kTwoPi * hz / (float)kSampleRate) : 0.0f;
  if (hz <= 0) phase = 0.0f;  // don't carry phase across a rest

  uint32_t done = 0;
  while (done < totalFrames) {
    uint32_t remain = totalFrames - done;
    int n = (remain < (uint32_t)kBufFrames) ? (int)remain : kBufFrames;
    for (int i = 0; i < n; i++) {
      uint32_t idx = done + (uint32_t)i;
      float env = 1.0f;
      if (rampFrames > 0) {
        if (idx < rampFrames) {
          env = (float)idx / (float)rampFrames;
        } else if (idx >= totalFrames - rampFrames) {
          env = (float)(totalFrames - idx) / (float)rampFrames;
        }
      }
      int16_t s = 0;
      if (hz > 0) {
        s = (int16_t)(sinf(phase) * gAmplitude * env);
        phase += step;
        if (phase > kTwoPi) phase -= kTwoPi;
      }
      buf[i * 2] = s;
      buf[i * 2 + 1] = s;
    }
    size_t written = 0;
    i2s_write(I2S_NUM_0, buf, (size_t)n * sizeof(int16_t) * 2, &written, portMAX_DELAY);
    done += (uint32_t)n;
  }
}

// ------------------------------------------------------------------- RTTTL
//
// Parses the same "name:d=4,o=5,b=125:8d,16p,16d,2g4,..." text Flipper Zero
// plays (and calls .fmf on the SD card), based on Flipper's own
// music_worker.c note-to-frequency math so pitch/timing match.

constexpr int kMaxSongNotes = 600;
struct SongNote {
  float hz;      // 0 = rest
  uint32_t ms;
};
SongNote gSong[kMaxSongNotes];
int gSongLen = 0;
char gSongName[32] = "";

int8_t noteToSemitone(char note) {
  switch (note) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: return 0;
  }
}

size_t extractNumber(const char* s, uint32_t* out) {
  size_t n = 0;
  *out = 0;
  while (isdigit((unsigned char)s[n])) {
    *out = *out * 10 + (uint32_t)(s[n] - '0');
    n++;
  }
  return n;
}

size_t extractDots(const char* s, uint32_t* out) {
  size_t n = 0;
  *out = 0;
  while (s[n] == '.') {
    (*out)++;
    n++;
  }
  return n;
}

size_t skipTill(const char* s, char sym) {
  size_t n = 0;
  while (s[n] != '\0' && s[n] != sym) n++;
  return (s[n] == sym) ? n : 0;
}

// Parses the comma-separated note list after the second ':'.
bool parseRtttlNotes(const char* s, uint32_t defaultDuration, uint32_t defaultOctave,
                     uint32_t bpm) {
  gSongLen = 0;
  const char* cur = s;
  while (*cur != '\0') {
    if (*cur != ' ' && *cur != '\t') {
      uint32_t duration = 0, octave = 0, dots = 0;
      char noteChar = '\0', sharp = '\0';

      cur += extractNumber(cur, &duration);
      if (isalpha((unsigned char)*cur)) {
        noteChar = (char)toupper((unsigned char)*cur);
        cur++;
      }
      if (*cur == '#' || *cur == '_') {
        sharp = '#';
        cur++;
      }
      cur += extractNumber(cur, &octave);
      cur += extractDots(cur, &dots);

      if (!duration) duration = defaultDuration;
      if (!octave) octave = defaultOctave;

      bool valid = duration >= 1 && duration <= 128 &&
                   ((noteChar >= 'A' && noteChar <= 'G') || noteChar == 'P') && octave <= 16 &&
                   dots <= 16;
      if (!valid) return false;

      float hz = 0;
      if (noteChar != 'P') {
        int semitone = (int)(octave * 12) + noteToSemitone(noteChar) + (sharp == '#' ? 1 : 0);
        float fromC4 = (float)semitone - 48.0f;  // 4 * 12
        hz = 261.63f * powf(1.059463094359f, fromC4);
      }
      float ms = 60000.0f * 4.0f / (float)bpm / (float)duration;
      for (uint32_t d = dots; d > 0; d--) ms += ms / 2.0f;

      if (gSongLen < kMaxSongNotes) {
        gSong[gSongLen].hz = hz;
        gSong[gSongLen].ms = (uint32_t)ms;
        gSongLen++;
      }

      cur += skipTill(cur, ',');
    }
    if (*cur != '\0') cur++;
  }
  return gSongLen > 0;
}

// Parses "name:d=..,o=..,b=..:notes" as a whole, filling gSong[]/gSongLen
// and gSongName.
bool parseRtttl(const char* full) {
  const char* cur = full;
  while (*cur == ' ' || *cur == '\t' || *cur == '\r' || *cur == '\n') cur++;

  size_t nameLen = skipTill(cur, ':');
  if (cur[nameLen] != ':') return false;
  size_t copyLen = nameLen < sizeof(gSongName) - 1 ? nameLen : sizeof(gSongName) - 1;
  memcpy(gSongName, cur, copyLen);
  gSongName[copyLen] = '\0';
  cur += nameLen + 1;

  size_t headerLen = skipTill(cur, ':');
  if (cur[headerLen] != ':') return false;

  uint32_t duration = 4, octave = 5, bpm = 63;
  const char* h = cur;
  const char* hEnd = cur + headerLen;
  while (h < hEnd) {
    while (h < hEnd && (*h == ',' || *h == ' ')) h++;
    if (h >= hEnd) break;
    char key = *h;
    h++;
    if (h < hEnd && *h == '=') h++;
    uint32_t val = 0;
    h += extractNumber(h, &val);
    if (key == 'd') duration = val;
    else if (key == 'o') octave = val;
    else if (key == 'b') bpm = val;
  }
  if (!bpm) bpm = 63;
  if (!duration) duration = 4;
  if (!octave) octave = 5;

  cur += headerLen + 1;
  return parseRtttlNotes(cur, duration, octave, bpm);
}

// Tune preloaded so there's always something
// to play without needing the SD card.
const char* kSongDoom =
    "Doom:d=32,o=4,b=56:f,f,f5,f,f,d#5,f,f,c#5,f,f,b,f,f,c5,c#5,f,f,f5,f,f,d#5,f,f,c#5,f,f,8b.,f,f,"
    "f5,f,f,d#5,f,f,c#5,f,f,b,f,f,c5,c#5,f,f,f5,f,f,d#5,f,f,c#5,f,f,8b.,a#,a#,a#5,a#,a#,g#5,a#,a#,"
    "f#5,a#,a#,e5,a#,a#,f5,f#5,a#,a#,a#5,a#,a#,g#5,a#,a#,f#5,a#,a#,8e5";

void drawStatus(const char* title, const char* line1, const char* line2) {
  ui::drawFrame(title, "Del back");
  auto& c = ui::canvas();
  c.setFont(&fonts::Font4);
  c.setTextDatum(middle_center);
  c.setTextColor(TFT_WHITE, TFT_BLACK);
  c.drawString(line1, ui::W / 2, 56);
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_DARKGREY, TFT_BLACK);
  c.drawString(line2, ui::W / 2, 90);
  ui::present();
}

bool noCodec(const Module& self) {
  if (audioInit()) return false;
  ui::message(self.name, "ES8311 codec not found", "Check I2C on G8 (SDA) / G9 (SCL)");
  return true;
}

// Plays whatever's currently parsed into gSong[], one note per screen
// update. Enter restarts from the top; Del stops.
void playParsedSong(const Module& self) {
  if (noCodec(self)) return;
  if (gSongLen == 0) {
    ui::message(self.name, "Nothing to play", "");
    return;
  }
  while (true) {
    for (int i = 0; i < gSongLen; i++) {
      char l1[24];
      if (gSong[i].hz <= 0) {
        snprintf(l1, sizeof(l1), "(rest)");
      } else {
        snprintf(l1, sizeof(l1), "%u Hz", (unsigned)gSong[i].hz);
      }
      char l2[40];
      snprintf(l2, sizeof(l2), "%s  note %d/%d", gSongName, i + 1, gSongLen);
      drawStatus("Speaker: melody", l1, l2);

      playNote(gSong[i].hz, gSong[i].ms);

      InputEvent ev = inputPoll();
      if (ev.key == Key::Back) return;
    }
    // Pause, then loop, unless the user has pressed Del by now.
    uint32_t until = millis() + 600;
    while (millis() < until) {
      InputEvent ev = inputPoll();
      if (ev.key == Key::Back) return;
      delay(5);
    }
  }
}

// Lets you pick a .fmf file from the SD card (storage::kMusicDir), load it
// as one RTTTL-format line, and play it the same way as the preloaded
// tunes.
void loadFromSdScreen(const Module& self) {
  if (!storage::mounted()) {
    ui::message(self.name, "No SD card", "Insert a card and reboot");
    return;
  }
  String path;
  if (!browseFiles("Load melody", storage::kMusicDir, ".fmf", path)) return;

  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) {
    ui::message(self.name, "Cannot open file", path.c_str());
    return;
  }
  static char text[2048];
  size_t n = f.readBytes(text, sizeof(text) - 1);
  text[n] = '\0';
  f.close();

  if (!parseRtttl(text)) {
    ui::message(self.name, "Could not parse file", "Expected one RTTTL line");
    return;
  }
  playParsedSong(self);
}

void melodyMenu(const Module& self) {
  static const ui::MenuItem kItems[] = {
      {"Doom", ""},
      {"Load from SD", ".fmf"},
  };
  int sel = 0;
  while (true) {
    sel = ui::menu("Play melody", kItems, 2, sel);
    if (sel < 0) return;
    if (sel == 0) {
      parseRtttl(kSongDoom);
      playParsedSong(self);
    }
    if (sel == 2) loadFromSdScreen(self);
  }
}

// Sweeps smoothly across the audible range, to check the speaker isn't
// buzzing, cutting out or distorting at any pitch.
void sweepScreen(const Module& self) {
  if (noCodec(self)) return;
  const float kLow = 200.0f, kHigh = 4000.0f;
  const uint32_t kSweepMs = 4000;
  uint32_t start = millis();
  while (true) {
    uint32_t t = millis() - start;
    if (t > kSweepMs) {
      start = millis();
      t = 0;
    }
    float frac = (float)t / (float)kSweepMs;
    float hz = kLow + (kHigh - kLow) * frac;
    playNote(hz, 30);

    char l1[24];
    snprintf(l1, sizeof(l1), "%u Hz", (unsigned)hz);
    drawStatus("Speaker: sweep", l1, "200 Hz - 4000 Hz, repeating");

    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) return;
  }
}

// Left/Right change the volume live so you can hear the effect immediately;
// Enter plays a short test tone at the current level. Scales the sine
// amplitude we generate rather than the codec's own register, so it works
// the same whether or not the codec's own volume register took effect.
void volumeScreen(const Module& self) {
  if (noCodec(self)) return;
  bool dirty = true;
  while (true) {
    if (dirty) {
      char l1[16];
      snprintf(l1, sizeof(l1), "%u / 255", (unsigned)(gAmplitude * 255 / 16000));
      drawStatus("Speaker: volume", l1, "Left/Right change|Enter test tone");
      dirty = false;
    }
    InputEvent ev = inputPoll();
    if (ev.key == Key::Back) break;
    if (ev.key == Key::Left || ev.key == Key::Right) {
      int step = ev.key == Key::Right ? 1000 : -1000;
      int v = (int)gAmplitude + step;
      if (v < 0) v = 0;
      if (v > 16000) v = 16000;
      gAmplitude = (int16_t)v;
      dirty = true;
    }
    if (ev.key == Key::Select) {
      playNote(440.0f, 300);
    }
    delay(10);
  }
}

}  // namespace

// ------------------------------------------------------------------- entry

void runSpeaker(const Module& self) {
  static const ui::MenuItem kItems[] = {
      {"Play melody", ""},
      {"Tone sweep", "200-4000 Hz"},
      {"Volume", ""},
  };

  int sel = 0;
  while (true) {
    sel = ui::menu(self.name, kItems, 3, sel);
    if (sel < 0) return;
    if (sel == 0) melodyMenu(self);
    if (sel == 1) sweepScreen(self);
    if (sel == 2) volumeScreen(self);
  }
}