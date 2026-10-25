#include "modules/nrf24drv.h"

#include <Arduino.h>
#include <string.h>

namespace nrf24 {

namespace {

// Registers
constexpr uint8_t REG_CONFIG = 0x00;
constexpr uint8_t REG_EN_AA = 0x01;
constexpr uint8_t REG_EN_RXADDR = 0x02;
constexpr uint8_t REG_SETUP_AW = 0x03;
constexpr uint8_t REG_SETUP_RETR = 0x04;
constexpr uint8_t REG_RF_CH = 0x05;
constexpr uint8_t REG_RF_SETUP = 0x06;
constexpr uint8_t REG_STATUS = 0x07;
constexpr uint8_t REG_RPD = 0x09;
constexpr uint8_t REG_RX_ADDR_P0 = 0x0A;
constexpr uint8_t REG_TX_ADDR = 0x10;
constexpr uint8_t REG_RX_PW_P0 = 0x11;
constexpr uint8_t REG_FIFO_STATUS = 0x17;
constexpr uint8_t REG_DYNPD = 0x1C;
constexpr uint8_t REG_FEATURE = 0x1D;

// Commands
constexpr uint8_t CMD_W_REGISTER = 0x20;
constexpr uint8_t CMD_R_RX_PAYLOAD = 0x61;
constexpr uint8_t CMD_W_TX_PAYLOAD = 0xA0;
constexpr uint8_t CMD_FLUSH_TX = 0xE1;
constexpr uint8_t CMD_FLUSH_RX = 0xE2;

// CONFIG bits
constexpr uint8_t PWR_UP = 0x02;
constexpr uint8_t PRIM_RX = 0x01;
constexpr uint8_t CRC16 = 0x0C;  // EN_CRC | CRCO

// STATUS bits
constexpr uint8_t RX_DR = 0x40;
constexpr uint8_t TX_DS = 0x20;
constexpr uint8_t MAX_RT = 0x10;

SPIClass* gSpi = nullptr;
int gCsn = -1;
int gCe = -1;
Rate gRate = Rate::k2M;
uint8_t gLastRead = 0;

uint8_t xfer(uint8_t cmd, const uint8_t* out, uint8_t* in, size_t n) {
  gSpi->beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(gCsn, LOW);
  uint8_t status = gSpi->transfer(cmd);
  for (size_t i = 0; i < n; i++) {
    uint8_t b = gSpi->transfer(out ? out[i] : 0xFF);
    if (in) in[i] = b;
  }
  digitalWrite(gCsn, HIGH);
  gSpi->endTransaction();
  return status;
}

uint8_t readReg(uint8_t reg) {
  uint8_t v = 0;
  xfer(reg & 0x1F, nullptr, &v, 1);
  return v;
}

void writeReg(uint8_t reg, uint8_t v) { xfer(CMD_W_REGISTER | (reg & 0x1F), &v, nullptr, 1); }

void writeRegs(uint8_t reg, const uint8_t* v, size_t n) {
  xfer(CMD_W_REGISTER | (reg & 0x1F), v, nullptr, n);
}

uint8_t rateBits(Rate r) {
  switch (r) {
    case Rate::k250: return 0x20;  // RF_DR_LOW
    case Rate::k1M: return 0x00;
    default: return 0x08;          // RF_DR_HIGH
  }
}

}  // namespace

bool begin(SPIClass& spi, int csn, int ce) {
  gSpi = &spi;
  gCsn = csn;
  gCe = ce;
  pinMode(gCsn, OUTPUT);
  digitalWrite(gCsn, HIGH);
  pinMode(gCe, OUTPUT);
  digitalWrite(gCe, LOW);
  delay(5);

  // Power up, then prove the chip is really there: registers must hold what
  // was written. (A missing chip reads all 0x00 or all 0xFF.)
  writeReg(REG_CONFIG, CRC16 | PWR_UP);
  delay(5);
  writeReg(REG_SETUP_AW, 0x03);
  writeReg(REG_RF_CH, 0x2A);
  bool ok = readReg(REG_CONFIG) == (CRC16 | PWR_UP) && readReg(REG_SETUP_AW) == 0x03 &&
            readReg(REG_RF_CH) == 0x2A;
  gLastRead = readReg(REG_CONFIG);
  if (!ok) return false;

  writeReg(REG_EN_AA, 0x00);      // no auto-acknowledge
  writeReg(REG_SETUP_RETR, 0x00);
  writeReg(REG_EN_RXADDR, 0x01);  // pipe 0 only
  writeReg(REG_DYNPD, 0x00);
  writeReg(REG_FEATURE, 0x00);
  writeReg(REG_RX_PW_P0, kPayload);
  return true;
}

uint8_t lastRead() { return gLastRead; }

void end() {
  if (!gSpi) return;
  digitalWrite(gCe, LOW);
  writeReg(REG_CONFIG, CRC16);  // power down
}

Info info() {
  Info i;
  i.present = true;
  // 250 kbps only exists on the "+" chip: try to set it and read it back.
  writeReg(REG_RF_SETUP, 0x26);
  i.plus = (readReg(REG_RF_SETUP) & 0x28) == 0x20;
  writeReg(REG_RF_SETUP, rateBits(gRate) | 0x06);
  i.config = readReg(REG_CONFIG);
  i.channel = readReg(REG_RF_CH);
  i.rfSetup = readReg(REG_RF_SETUP);
  return i;
}

void configure(uint8_t channel, Rate rate, const uint8_t addr[5]) {
  gRate = rate;
  writeReg(REG_RF_CH, channel > 125 ? 125 : channel);
  writeReg(REG_RF_SETUP, rateBits(rate) | 0x06);  // 0 dBm
  writeRegs(REG_RX_ADDR_P0, addr, 5);
  writeRegs(REG_TX_ADDR, addr, 5);
  writeReg(REG_STATUS, RX_DR | TX_DS | MAX_RT);
  xfer(CMD_FLUSH_RX, nullptr, nullptr, 0);
  xfer(CMD_FLUSH_TX, nullptr, nullptr, 0);
}

void scanStart() {
  digitalWrite(gCe, LOW);
  writeReg(REG_RF_SETUP, rateBits(Rate::k2M) | 0x06);
  writeReg(REG_CONFIG, CRC16 | PWR_UP | PRIM_RX);
  delay(5);
}

bool carrierAt(uint8_t channel) {
  writeReg(REG_RF_CH, channel);
  digitalWrite(gCe, HIGH);
  delayMicroseconds(170);  // 130 us settling plus listening time
  bool hit = (readReg(REG_RPD) & 1) != 0;
  digitalWrite(gCe, LOW);
  return hit;
}

void scanStop() {
  digitalWrite(gCe, LOW);
  writeReg(REG_CONFIG, CRC16 | PWR_UP);
}

void rxStart() {
  digitalWrite(gCe, LOW);
  writeReg(REG_CONFIG, CRC16 | PWR_UP | PRIM_RX);
  delay(5);
  writeReg(REG_STATUS, RX_DR | TX_DS | MAX_RT);
  xfer(CMD_FLUSH_RX, nullptr, nullptr, 0);
  digitalWrite(gCe, HIGH);
}

bool rxAvailable() { return (readReg(REG_FIFO_STATUS) & 0x01) == 0; }  // RX FIFO not empty

void rxRead(uint8_t* buf32) {
  xfer(CMD_R_RX_PAYLOAD, nullptr, buf32, kPayload);
  writeReg(REG_STATUS, RX_DR);
}

void rxStop() {
  digitalWrite(gCe, LOW);
  writeReg(REG_CONFIG, CRC16 | PWR_UP);
}

bool tx(const uint8_t* buf32) {
  digitalWrite(gCe, LOW);
  writeReg(REG_CONFIG, CRC16 | PWR_UP);  // transmit mode
  delayMicroseconds(150);
  writeReg(REG_STATUS, RX_DR | TX_DS | MAX_RT);
  xfer(CMD_FLUSH_TX, nullptr, nullptr, 0);
  xfer(CMD_W_TX_PAYLOAD, buf32, nullptr, kPayload);

  digitalWrite(gCe, HIGH);
  delayMicroseconds(20);
  digitalWrite(gCe, LOW);

  uint32_t t0 = millis();
  while (millis() - t0 < 20) {
    uint8_t st = readReg(REG_STATUS);
    if (st & TX_DS) {
      writeReg(REG_STATUS, TX_DS);
      return true;
    }
    if (st & MAX_RT) {
      writeReg(REG_STATUS, MAX_RT);
      xfer(CMD_FLUSH_TX, nullptr, nullptr, 0);
      return false;
    }
  }
  return false;
}

const char* rateName(Rate r) {
  switch (r) {
    case Rate::k250: return "250 kbps";
    case Rate::k1M: return "1 Mbps";
    default: return "2 Mbps";
  }
}

}  // namespace nrf24