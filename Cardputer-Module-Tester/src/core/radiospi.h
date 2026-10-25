#pragma once
// SPI bus for radios on your own pins.
#include <SPI.h>

// Returns the SPI bus for the given pins. If they are the Cardputer's SD card
// bus (SCK 40, MISO 39, MOSI 14) the default bus is shared with the card;
// otherwise a separate bus (HSPI) is started on those pins.
SPIClass& radioSpi(int sck, int miso, int mosi);

// Drives a pin high (chip selects of other devices on the same bus, so they
// stay quiet while this radio talks). Ignores pins below 0.
void holdHigh(int pin);

// Parks every other device on the Cardputer ADV's shared bus (LoRa Cap NSS 5,
// ST25R CS 6, SD CS 12, radio CS 13) high, as Unigeek does at boot, except the
// pins this radio uses itself (its own CS and its GDO0/CE line).
void holdBusIdle(int ownCs, int ownAux);