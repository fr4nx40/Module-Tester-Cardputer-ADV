#pragma once
// PN532 test: interface (I2C / SPI / UART) and pin setup, tag capture,
// emulation of the captured tag, emulation from a file.
#include "core/registry.h"

void runPn532(const Module& self);
