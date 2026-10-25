#pragma once
// Spectrum screens in Unigeek's style, shared by every radio in the tester.
//
//   draw()       bar chart: bars coloured by level (dark green, green, yellow,
//                orange, red) with a white hold marker per bar, as in Unigeek's
//                NRF24 "2.4G Spectrum".
//   Waterfall    scrolling heat map, as in Unigeek's CC1101 "Waterfall".
//   drawDetect() big peak frequency with a strength bar, as in Unigeek's
//                CC1101 "Detect Freq".
#include <stdint.h>

namespace rssichart {

struct Style {
  const char* title;
  const char* footer;
  const char* right;  // header text on the right, e.g. "Peak:12"
  const char* left;   // axis labels under the chart
  const char* mid;
  const char* end;
  int valueMin;       // value drawn as an empty bar
  int valueMax;       // value drawn as a full bar
  bool showPeaks;     // draw the white hold markers (Unigeek "Peak" mode)
  const char* mode;   // small label top left of the chart, e.g. "Peak"
};

// Bruce-style bar chart (its RSSI range scan): solid bars rising from a baseline
// with tick marks and start/end labels under it, and "Max=" for the strongest
// bar. `level` is 0..100 per bar. `maxText` may be empty.
void drawBars(const char* title, const char* footer, const char* right, const uint8_t* level,
              int n, const char* startLabel, const char* endLabel, const char* maxText);

// Draws the whole bar chart and presents it.
void draw(const Style& style, const int8_t* now, const int8_t* peak, int n);

// Heat colour for a dBm value, as Unigeek's waterfall: black, blue, green,
// yellow, red between -100 and -35 dBm.
uint16_t heatColor(int dbm);

// Scrolling heat map. begin() draws the frame; addRow() paints one row of n
// samples (dBm) and presents; the cursor wraps at the bottom.
class Waterfall {
 public:
  void begin(const char* title, float startMhz, float endMhz);
  // Row sweep helper: the caller fills `dbm[0..width()-1]` then calls addRow().
  int width() const;
  void addRow(const int16_t* dbm);
  // Peak readout line under the frequency ticks.
  void setPeak(int dbm, float mhz);

 private:
  float start_ = 0, end_ = 0;
  int line_ = 0;
};

// Unigeek "Detect Freq" screen. mhz <= 0 means nothing seen yet.
void drawDetect(float mhz, int dbm, bool live);

}  // namespace rssichart
