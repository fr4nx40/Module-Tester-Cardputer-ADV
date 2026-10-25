#include "modules/caplora.h"

#include <M5Cardputer.h>
#include <RadioLib.h>
#include <SPI.h>

#include "core/log.h"

static const char* TAG = "cap";

namespace cap {

const Preset kPlain = {"Plain 868.0", 868.0f, 125.0f, 7, 5, 0x12, 8, 10};
const Preset kMeshtastic = {"Meshtastic EU868", 869.525f, 250.0f, 11, 5, 0x2B, 16, 10};

namespace {

// Cap LoRa-1262 on the Cardputer-Adv (from M5's docs).
constexpr int kPinNss = 5;
constexpr int kPinIrq = 4;   // SX1262 DIO1
constexpr int kPinRst = 3;
constexpr int kPinBusy = 6;
constexpr int kPinSck = 40;
constexpr int kPinMiso = 39;
constexpr int kPinMosi = 14;

// PI4IOE5V6408 I/O expander on the Cardputer's internal I2C bus.
constexpr uint8_t kExpAddr = 0x43;
constexpr uint32_t kI2cFreq = 400000;
constexpr uint8_t kExpRegId = 0x01;
constexpr uint8_t kExpRegDir = 0x03;      // 1 = output
constexpr uint8_t kExpRegOut = 0x05;      // output level
constexpr uint8_t kExpRegHiZ = 0x07;      // 1 = high impedance
constexpr uint8_t kExpPin0 = 0x01;

Module* gModule = nullptr;
SX1262* gRadio = nullptr;
volatile bool gFlag = false;

void IRAM_ATTR onDio1() { gFlag = true; }

}  // namespace

bool present() {
  if (!M5.In_I2C.scanID(kExpAddr, kI2cFreq)) return false;
  return M5.In_I2C.readRegister8(kExpAddr, kExpRegId, kI2cFreq) != 0;
}

void power() {
  M5.In_I2C.bitOn(kExpAddr, kExpRegDir, kExpPin0, kI2cFreq);   // pin 0 = output
  M5.In_I2C.bitOff(kExpAddr, kExpRegHiZ, kExpPin0, kI2cFreq);  // not high-Z
  M5.In_I2C.bitOn(kExpAddr, kExpRegOut, kExpPin0, kI2cFreq);   // drive high
}

int begin(const Preset& p) {
  if (!present()) return kErrNoCap;
  power();
  delay(20);

  if (!gRadio) {
    // No effect if the SD card code already started SPI with these pins.
    SPI.begin(kPinSck, kPinMiso, kPinMosi, kPinNss);
    gModule = new Module(kPinNss, kPinIrq, kPinRst, kPinBusy, SPI);
    gRadio = new SX1262(gModule);
  }

  gFlag = false;
  // 3.0 V TCXO and the LDO regulator, as in M5's examples for this Cap.
  int16_t st = gRadio->begin(p.freq, p.bw, p.sf, p.cr, p.sync, p.power, p.preamble, 3.0f, true);
  LOGI(TAG, "begin %s -> %d", p.name, (int)st);
  if (st != RADIOLIB_ERR_NONE) return st;

  gRadio->setCurrentLimit(140);
  gRadio->setDio1Action(onDio1);
  return RADIOLIB_ERR_NONE;
}

void standby() {
  if (gRadio) gRadio->standby();
}

// -------------------------------------------------------------------- receive

int rxStart() {
  gFlag = false;
  return gRadio->startReceive();
}

bool rxReady() {
  if (!gFlag) return false;
  gFlag = false;
  return true;
}

int rxRead(uint8_t* buf, size_t maxLen, size_t& len, float& rssi, float& snr) {
  len = gRadio->getPacketLength();
  if (len > maxLen) len = maxLen;
  int16_t st = gRadio->readData(buf, len);
  rssi = gRadio->getRSSI();
  snr = gRadio->getSNR();
  gFlag = false;
  gRadio->startReceive();
  return st;
}

// ------------------------------------------------------------------- transmit

int tx(const uint8_t* data, size_t len) {
  gRadio->standby();
  int16_t st = gRadio->transmit(data, len);
  gFlag = false;
  return st;
}

// ------------------------------------------------------------------- scanning

int scanStart(float firstMHz) {
  gRadio->standby();
  return gRadio->setFrequency(firstMHz);  // full calibration once
}

float scanRssi(float mhz) {
  gRadio->standby();
  gRadio->setFrequency(mhz, true);  // skip re-calibration inside the 863-870 band
  gRadio->startReceive();
  delay(3);                         // let the receiver settle
  return gRadio->getRSSI(false);    // instantaneous, not last-packet
}

// ----------------------------------------------------------------------- text

const char* errorText(int code) {
  switch (code) {
    case kErrNoCap: return "Cap not detected";
    case RADIOLIB_ERR_NONE: return "OK";
    case RADIOLIB_ERR_CHIP_NOT_FOUND: return "SX1262 not found";
    case RADIOLIB_ERR_INVALID_FREQUENCY: return "Bad frequency";
    case RADIOLIB_ERR_TX_TIMEOUT: return "TX timeout";
    case RADIOLIB_ERR_CRC_MISMATCH: return "CRC mismatch";
    default: return "Radio error";
  }
}

}  // namespace cap
