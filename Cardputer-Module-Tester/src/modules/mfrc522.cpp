#include "modules/mfrc522.h"

#include <M5Cardputer.h>

#include "core/log.h"

static const char* TAG = "mfrc522";

namespace {

const uint8_t kAddr = 0x28;
const uint32_t kFreq = 100000;

// Registers
const uint8_t CommandReg = 0x01;
const uint8_t ComIrqReg = 0x04;
const uint8_t DivIrqReg = 0x05;
const uint8_t ErrorReg = 0x06;
const uint8_t Status2Reg = 0x08;
const uint8_t FIFODataReg = 0x09;
const uint8_t FIFOLevelReg = 0x0A;
const uint8_t ControlReg = 0x0C;
const uint8_t BitFramingReg = 0x0D;
const uint8_t CollReg = 0x0E;
const uint8_t ModeReg = 0x11;
const uint8_t TxModeReg = 0x12;
const uint8_t RxModeReg = 0x13;
const uint8_t TxControlReg = 0x14;
const uint8_t TxASKReg = 0x15;
const uint8_t CRCResultRegH = 0x21;
const uint8_t CRCResultRegL = 0x22;
const uint8_t TModeReg = 0x2A;
const uint8_t TPrescalerReg = 0x2B;
const uint8_t TReloadRegH = 0x2C;
const uint8_t TReloadRegL = 0x2D;
const uint8_t VersionReg = 0x37;

// Commands
const uint8_t CmdIdle = 0x00;
const uint8_t CmdCalcCRC = 0x03;
const uint8_t CmdTransceive = 0x0C;
const uint8_t CmdMFAuthent = 0x0E;
const uint8_t CmdSoftReset = 0x0F;

// ISO 14443-A / MIFARE commands
const uint8_t PiccReqA = 0x26;
const uint8_t PiccWupA = 0x52;
const uint8_t PiccHltA = 0x50;
const uint8_t PiccRead = 0x30;

}  // namespace

// ------------------------------------------------------------ register I/O

void Mfrc522::writeReg(uint8_t reg, uint8_t value) {
  M5.Ex_I2C.writeRegister8(kAddr, reg, value, kFreq);
}

void Mfrc522::writeRegs(uint8_t reg, const uint8_t* data, size_t len) {
  M5.Ex_I2C.writeRegister(kAddr, reg, data, len, kFreq);
}

uint8_t Mfrc522::readReg(uint8_t reg) {
  uint8_t v = 0;
  M5.Ex_I2C.readRegister(kAddr, reg, &v, 1, kFreq);
  return v;
}

void Mfrc522::readRegs(uint8_t reg, uint8_t* out, size_t len) {
  M5.Ex_I2C.readRegister(kAddr, reg, out, len, kFreq);
}

void Mfrc522::setBits(uint8_t reg, uint8_t mask) {
  writeReg(reg, readReg(reg) | mask);
}

void Mfrc522::clearBits(uint8_t reg, uint8_t mask) {
  writeReg(reg, readReg(reg) & (uint8_t)~mask);
}

// ------------------------------------------------------------------- setup

bool Mfrc522::begin() {
  M5.Ex_I2C.begin();
  if (!M5.Ex_I2C.scanID(kAddr, kFreq)) {
    LOGW(TAG, "nothing at I2C 0x%02X", kAddr);
    return false;
  }

  writeReg(CommandReg, CmdSoftReset);
  uint32_t t0 = millis();
  while ((readReg(CommandReg) & 0x10) && millis() - t0 < 100) delay(2);
  delay(10);

  writeReg(TxModeReg, 0x00);
  writeReg(RxModeReg, 0x00);
  writeReg(TModeReg, 0x80);       // timer starts automatically after sending
  writeReg(TPrescalerReg, 0xA9);  // ~25 ms timeout with the reload below
  writeReg(TReloadRegH, 0x03);
  writeReg(TReloadRegL, 0xE8);
  writeReg(TxASKReg, 0x40);       // 100% ASK modulation
  writeReg(ModeReg, 0x3D);        // CRC preset 0x6363
  antennaOn();

  LOGI(TAG, "chip version 0x%02X", version());
  return true;
}

uint8_t Mfrc522::version() { return readReg(VersionReg); }

void Mfrc522::antennaOn() {
  if ((readReg(TxControlReg) & 0x03) != 0x03) setBits(TxControlReg, 0x03);
}

void Mfrc522::antennaOff() { clearBits(TxControlReg, 0x03); }

void Mfrc522::resetField() {
  antennaOff();
  delay(8);
  antennaOn();
  delay(8);
}

// ------------------------------------------------------------ transport

Mfrc522::Status Mfrc522::calcCrc(const uint8_t* data, size_t len, uint8_t result[2]) {
  writeReg(CommandReg, CmdIdle);
  writeReg(DivIrqReg, 0x04);     // clear CRCIRq
  writeReg(FIFOLevelReg, 0x80);  // flush FIFO
  writeRegs(FIFODataReg, data, len);
  writeReg(CommandReg, CmdCalcCRC);

  uint32_t t0 = millis();
  while (true) {
    if (readReg(DivIrqReg) & 0x04) break;
    if (millis() - t0 > 90) return Timeout;
  }
  writeReg(CommandReg, CmdIdle);
  result[0] = readReg(CRCResultRegL);
  result[1] = readReg(CRCResultRegH);
  return Ok;
}

Mfrc522::Status Mfrc522::communicate(uint8_t command, uint8_t waitIrq,
                                     const uint8_t* send, uint8_t sendLen,
                                     uint8_t* back, uint8_t* backLen,
                                     uint8_t* validBits, bool checkCrc) {
  uint8_t txLastBits = validBits ? *validBits : 0;

  writeReg(CommandReg, CmdIdle);
  writeReg(ComIrqReg, 0x7F);     // clear all interrupt flags
  writeReg(FIFOLevelReg, 0x80);  // flush FIFO
  writeRegs(FIFODataReg, send, sendLen);
  writeReg(BitFramingReg, txLastBits);
  writeReg(CommandReg, command);
  if (command == CmdTransceive) setBits(BitFramingReg, 0x80);  // StartSend

  uint32_t t0 = millis();
  while (true) {
    uint8_t irq = readReg(ComIrqReg);
    if (irq & waitIrq) break;
    if (irq & 0x01) return Timeout;  // chip timer ran out: no answer
    if (millis() - t0 > 40) return Timeout;
  }

  uint8_t err = readReg(ErrorReg);
  if (err & 0x13) return Error;  // buffer overflow, parity, protocol

  uint8_t valid = 0;
  if (back && backLen) {
    uint8_t n = readReg(FIFOLevelReg);
    if (n > *backLen) return NoRoom;
    *backLen = n;
    if (n) readRegs(FIFODataReg, back, n);
    valid = readReg(ControlReg) & 0x07;
    if (validBits) *validBits = valid;
  }

  if (err & 0x08) return Collision;

  if (back && backLen && checkCrc) {
    if (*backLen == 1 && valid == 4) return Nack;  // 4-bit NAK
    if (*backLen < 2 || valid != 0) return CrcWrong;
    uint8_t crc[2];
    if (calcCrc(back, *backLen - 2, crc) != Ok) return CrcWrong;
    if (back[*backLen - 2] != crc[0] || back[*backLen - 1] != crc[1]) return CrcWrong;
  }
  return Ok;
}

// --------------------------------------------------------------- ISO 14443-A

Mfrc522::Status Mfrc522::reqaOrWupa(uint8_t command, uint8_t atqa[2]) {
  clearBits(CollReg, 0x80);  // bits received after a collision are cleared
  uint8_t valid = 7;         // REQA / WUPA are 7-bit frames
  uint8_t len = 2;
  Status st = communicate(CmdTransceive, 0x30, &command, 1, atqa, &len, &valid, false);
  if (st != Ok) return st;
  if (len != 2 || valid != 0) return Error;
  return Ok;
}

Mfrc522::Status Mfrc522::requestA(uint8_t atqa[2]) { return reqaOrWupa(PiccReqA, atqa); }
Mfrc522::Status Mfrc522::wakeupA(uint8_t atqa[2]) { return reqaOrWupa(PiccWupA, atqa); }

Mfrc522::Status Mfrc522::select(Uid& uid, uint8_t& sak) {
  static const uint8_t kCascade[3] = {0x93, 0x95, 0x97};
  uid.size = 0;
  sak = 0;

  for (int level = 0; level < 3; level++) {
    // Anticollision: the card answers with 4 UID bytes (or CT + 3) and a BCC.
    uint8_t buf[9];
    buf[0] = kCascade[level];
    buf[1] = 0x20;
    uint8_t back[5];
    uint8_t backLen = sizeof(back);
    uint8_t valid = 0;
    Status st = communicate(CmdTransceive, 0x30, buf, 2, back, &backLen, &valid, false);
    if (st != Ok) return st;
    if (backLen != 5) return Error;
    if ((uint8_t)(back[0] ^ back[1] ^ back[2] ^ back[3]) != back[4]) return Error;

    // Select with the 5 bytes we just got, plus CRC.
    buf[0] = kCascade[level];
    buf[1] = 0x70;
    for (int i = 0; i < 5; i++) buf[2 + i] = back[i];
    uint8_t crc[2];
    if (calcCrc(buf, 7, crc) != Ok) return Timeout;
    buf[7] = crc[0];
    buf[8] = crc[1];

    uint8_t resp[3];
    uint8_t respLen = sizeof(resp);
    valid = 0;
    st = communicate(CmdTransceive, 0x30, buf, 9, resp, &respLen, &valid, true);
    if (st != Ok) return st;
    if (respLen != 3) return Error;
    sak = resp[0];

    if (back[0] == 0x88) {  // cascade tag: 3 UID bytes now, more to come
      for (int i = 1; i < 4 && uid.size < 10; i++) uid.bytes[uid.size++] = back[i];
    } else {
      for (int i = 0; i < 4 && uid.size < 10; i++) uid.bytes[uid.size++] = back[i];
    }
    if (!(sak & 0x04)) return Ok;  // UID complete
  }
  return Error;
}

Mfrc522::Status Mfrc522::haltA() {
  uint8_t buf[4] = {PiccHltA, 0x00, 0, 0};
  uint8_t crc[2];
  if (calcCrc(buf, 2, crc) != Ok) return Timeout;
  buf[2] = crc[0];
  buf[3] = crc[1];
  Status st = communicate(CmdTransceive, 0x30, buf, 4, nullptr, nullptr, nullptr, false);
  // A halted card does not answer, so a timeout means success.
  return (st == Timeout) ? Ok : st;
}

// ------------------------------------------------------------------ MIFARE

Mfrc522::Status Mfrc522::authenticate(uint8_t keyCmd, uint8_t block,
                                      const uint8_t key[6], const Uid& uid) {
  uint8_t buf[12];
  buf[0] = keyCmd;
  buf[1] = block;
  for (int i = 0; i < 6; i++) buf[2 + i] = key[i];
  // The card's last four UID bytes take part in the authentication.
  for (int i = 0; i < 4; i++) buf[8 + i] = uid.bytes[uid.size - 4 + i];

  Status st = communicate(CmdMFAuthent, 0x10, buf, sizeof(buf), nullptr, nullptr, nullptr, false);
  if (st != Ok) return AuthFail;
  if (!(readReg(Status2Reg) & 0x08)) return AuthFail;  // MFCrypto1On
  return Ok;
}

void Mfrc522::stopCrypto1() { clearBits(Status2Reg, 0x08); }

Mfrc522::Status Mfrc522::read(uint8_t block, uint8_t out[16]) {
  uint8_t buf[4] = {PiccRead, block, 0, 0};
  uint8_t crc[2];
  if (calcCrc(buf, 2, crc) != Ok) return Timeout;
  buf[2] = crc[0];
  buf[3] = crc[1];

  uint8_t back[18];
  uint8_t backLen = sizeof(back);
  uint8_t valid = 0;
  Status st = communicate(CmdTransceive, 0x30, buf, 4, back, &backLen, &valid, true);
  if (st != Ok) return st;
  if (backLen != 18) return Error;
  for (int i = 0; i < 16; i++) out[i] = back[i];
  return Ok;
}
