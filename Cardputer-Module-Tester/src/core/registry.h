#pragma once
// Module registry. Every testable unit is one entry in this table.
// "Container" modules (PaHub, LoRa Cap) host further screens inside run().
#include <stddef.h>

struct Module {
  const char* name;                  // shown in the list
  const char* bus;                   // small tag: UART / GPIO / I2C / Cap
  void (*run)(const Module& self);   // opens the module's screens
};

const Module* modules();
size_t moduleCount();

// Shown for modules whose test is not written yet.
void runStub(const Module& self);
