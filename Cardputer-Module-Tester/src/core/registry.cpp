#include "core/registry.h"

#include "core/ui.h"
#include "modules/cc1101.h"
#include "modules/extdisplay.h"
#include "modules/gps.h"
#include "modules/ir.h"
#include "modules/lora.h"
#include "modules/nrf24.h"
#include "modules/pn532.h"
#include "modules/rfid.h"
#include "modules/speaker.h"

void runStub(const Module& self) {
  ui::message(self.name, "Not implemented yet", self.bus);
}

// Alphabetical by display name.
static const Module kModules[] = {
    {"CC1101",           "SPI",  runCc1101},
    {"External Display", "SPI",  runExtDisplay},
    {"GPS",              "UART", runGpsMenu},
    {"IR",               "GPIO", runIrMenu},
    {"M5Stack LoRa Cap", "Cap",  runLoraCap},  // container: LoRa / GNSS / Cap Grove
    {"M5Stack RFID 2",   "I2C",  runRfid},
    {"NRF24L01",         "SPI",  runNrf24},
    {"PN532",            "I2C/SPI/UART", runPn532},
    {"Speaker",          "I2S",  runSpeaker},
};

const Module* modules() { return kModules; }
size_t moduleCount() { return sizeof(kModules) / sizeof(kModules[0]); }
