#pragma once
// Sample files embedded in the firmware. They are written to the SD card's
// M5StackModuleTests folder on first run. The data is made up for testing the browser
// and the IR / RFID screens; it is not captured from real devices.
#include <stddef.h>

struct SampleFile {
  const char* name;     // file name inside the samples folder
  const char* content;  // full file text
};

extern const SampleFile kSampleFiles[];
extern const size_t kSampleFileCount;