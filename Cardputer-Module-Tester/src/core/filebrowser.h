#pragma once
// SD card file browser.
//
//   ; .        move up / down
//   Enter or / open a folder, or pick a file
//   `          clear the name filter, otherwise go up one folder;
//              at the SD root it leaves the browser
//   ,          go up one folder
//   letters    type to filter the current folder by name (case-insensitive)
//   Backspace  erase a filter character
//
// Punctuation keys are navigation keys here, so the filter takes letters,
// digits and '-' / '_' only.
#include <Arduino.h>

// Browse the card starting at `startDir` (falls back to "/" if missing).
// `ext` limits which files are listed, e.g. ".ir" (nullptr = all files).
// Folders are always shown. On a pick, writes the full path to `outPath` and
// returns true. Returns false if the user backed out or there is no card.
bool browseFiles(const char* title, const char* startDir, const char* ext,
                 String& outPath);