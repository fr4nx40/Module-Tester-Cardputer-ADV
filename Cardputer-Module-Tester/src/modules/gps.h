#pragma once
// GPS unit test (UART on the Cardputer's Grove port).
#include "core/registry.h"

void runGps(const Module& self);

// Same screen for the Cap LoRa-1262's GNSS (UART on G15 / G13).
void runGpsCap(const Module& self);

void runGpsPins(const Module& self);

void runGpsMenu(const Module& self);
