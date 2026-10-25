#pragma once
// IR test, reached through one "IR" menu entry (runIrMenu) with two choices:
//   runIr        Grove port, fixed pins (white G1 = receiver, yellow G2 = emitter).
//   runIrBuiltin "IR module": both TX and RX chosen on its own Pins screen
//                (defaults to the Cardputer ADV's onboard IR LED pad, G44, for TX).
#include "core/registry.h"

void runIrMenu(const Module& self);
void runIr(const Module& self);
void runIrBuiltin(const Module& self);
