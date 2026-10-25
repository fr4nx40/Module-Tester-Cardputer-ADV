#include "core/samples.h"

// ---- IR (Flipper .ir format) -------------------------------------------

static const char kIrRaw[] = R"(Filetype: IR signals file
Version: 1
#
# Sample: raw timings in microseconds
name: Raw_sample
type: raw
frequency: 38000
duty_cycle: 0.330000
data: 9000 4500 560 560 560 560 560 1690 560 560 560 1690 560 1690 560 560 560 560 560 39000
)";

// ---- RFID (simple text format) -----------------------------------------

static const char kRfidMifare[] = R"(Filetype: M5ModuleTests RFID card
Version: 1
# Sample card
Type: MIFARE Classic 1K
UID: DE AD BE EF
ATQA: 00 04
SAK: 08
)";

static const char kRfidNtag[] = R"(Filetype: M5ModuleTests RFID card
Version: 1
# Sample card
Type: NTAG215
UID: 04 11 22 33 44 55 66
ATQA: 00 44
SAK: 00
)";

const SampleFile kSampleFiles[] = {
    {"sample_raw.ir", kIrRaw},
    {"sample_mifare1k.rfid", kRfidMifare},
    {"sample_ntag215.rfid", kRfidNtag},
};

const size_t kSampleFileCount = sizeof(kSampleFiles) / sizeof(kSampleFiles[0]);