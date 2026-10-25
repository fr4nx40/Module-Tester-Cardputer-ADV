#include "core/radiospi.h"

#include <Arduino.h>

SPIClass& radioSpi(int sck, int miso, int mosi) {
  if (sck == 40 && miso == 39 && mosi == 14) {
    // The SD card start-up attaches G12 as the bus's hardware chip select, which
    // makes the peripheral pull G12 low during every transfer and wakes the card
    // on the shared MISO line. Restart the bus once without a hardware CS. The
    // SD library uses its own CS pin by hand, so the mounted card keeps working.
    static bool restarted = false;
    if (!restarted) {
      SPI.end();
      SPI.begin(sck, miso, mosi, -1);
      restarted = true;
    }
    holdHigh(12);  // keep the SD card quiet
    return SPI;
  }
  static SPIClass other(HSPI);
  static int cSck = -2, cMiso = -2, cMosi = -2;
  if (sck != cSck || miso != cMiso || mosi != cMosi) {
    if (cSck != -2) other.end();
    other.begin(sck, miso, mosi, -1);
    cSck = sck;
    cMiso = miso;
    cMosi = mosi;
  }
  return other;
}

void holdHigh(int pin) {
  if (pin < 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
}

void holdBusIdle(int ownCs, int ownAux) {
  (void)ownCs;
  (void)ownAux;
  // Bruce does the same at boot: "set GPIO5 HIGH for SD card compatibility".
  // G5 is also the CC1101's GDO0. The library turns GDO0 into an output during
  // Init(), and an output starts LOW, which selects something else on the shared
  // bus and makes MISO return garbage. So every select / data pin starts HIGH,
  // including the radio's own GDO0 / CE line.
  const int pins[] = {4, 5, 6, 12, 13};
  for (int p : pins) holdHigh(p);
}