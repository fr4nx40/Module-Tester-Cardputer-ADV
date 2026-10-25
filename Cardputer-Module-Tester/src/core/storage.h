#pragma once
// SD card access and the app's folder layout:
//
//   /M5StackModuleTests/
//     ir/        saved IR signals   (.ir,   Flipper format)
//     rfid/      saved cards        (.rfid, simple text)
//     music/     RTTTL melodies     (.fmf,  one "name:d=..,o=..,b=..:notes" line)
//     samples/   sample files written from the firmware on first run
#include <Arduino.h>

namespace storage {

constexpr const char* kAppDir = "/ModuleTests";
constexpr const char* kIrDir = "/ModuleTests/ir";
constexpr const char* kRfidDir = "/ModuleTests/rfid";
constexpr const char* kMusicDir = "/ModuleTests/music";
constexpr const char* kSamplesDir = "/ModuleTests/samples";

// Mount the card. Returns true when a card is ready.
bool begin();
bool mounted();

// Create the app folders if missing. Returns true if all exist afterwards.
bool ensureDirs();

// Write the built-in sample files into kSamplesDir. Existing files are left
// alone, so edits you make on the card are never overwritten.
// Returns the number of files written.
int writeSamples();

// "/a/b/c.ir" -> "/a/b",  "/a" -> "/",  "/" -> "/"
String parentDir(const String& path);

}  // namespace storage
