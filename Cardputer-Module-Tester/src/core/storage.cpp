#include "core/storage.h"

#include <SD.h>
#include <SPI.h>

#include "core/log.h"
#include "core/samples.h"

namespace storage {

static const char* TAG = "storage";

// Cardputer / Cardputer-Adv microSD pins (from M5's SD example).
// Note: the Cap LoRa-1262 uses the same SCK/MISO/MOSI lines with its own
// chip select, so the bus is shared once the Cap is fitted.
constexpr int kSdSck = 40;
constexpr int kSdMiso = 39;
constexpr int kSdMosi = 14;
constexpr int kSdCs = 12;

static bool gMounted = false;

bool begin() {
  SPI.begin(kSdSck, kSdMiso, kSdMosi, -1);  // no hardware CS: SD.begin() drives the CS pin itself

  // Try the fast clock first, then fall back for fussy cards.
  const uint32_t speeds[] = {25000000, 10000000, 4000000};
  for (uint32_t hz : speeds) {
    if (SD.begin(kSdCs, SPI, hz)) {
      gMounted = true;
      LOGI(TAG, "SD mounted at %u Hz, type=%d, size=%llu MB", (unsigned)hz,
           (int)SD.cardType(), SD.cardSize() / (1024ULL * 1024ULL));
      return true;
    }
    SD.end();
    LOGW(TAG, "SD.begin failed at %u Hz", (unsigned)hz);
  }

  gMounted = false;
  LOGW(TAG, "no usable SD card");
  return false;
}

bool mounted() { return gMounted; }

static bool ensureDir(const char* path) {
  if (SD.exists(path)) return true;
  if (SD.mkdir(path)) {
    LOGI(TAG, "created %s", path);
    return true;
  }
  LOGE(TAG, "mkdir failed: %s", path);
  return false;
}

bool ensureDirs() {
  if (!gMounted) return false;
  bool ok = ensureDir(kAppDir);
  ok = ensureDir(kIrDir) && ok;
  ok = ensureDir(kRfidDir) && ok;
  ok = ensureDir(kMusicDir) && ok;
  return ok;
}



String parentDir(const String& path) {
  if (path.length() <= 1) return "/";
  int i = path.lastIndexOf('/');
  if (i <= 0) return "/";
  return path.substring(0, i);
}

}  // namespace storage
