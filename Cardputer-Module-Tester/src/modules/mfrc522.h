#pragma once
// Minimal MFRC522 / WS1850S driver over I2C, for the M5Stack RFID 2 unit
// (I2C address 0x28). Uses M5's Ex_I2C bus object, which is the Grove port
// (SCL = G1, SDA = G2) and does not clash with the Cardputer ADV's internal
// I2C devices.
//
// Scope: ISO 14443-A card detection, anticollision / select, MIFARE Classic
// authentication and block reads, and Ultralight / NTAG page reads.
#include <stddef.h>
#include <stdint.h>

struct Uid {
  uint8_t bytes[10] = {0};
  uint8_t size = 0;  // 4, 7 or 10
};

class Mfrc522 {
 public:
  enum Status : uint8_t {
    Ok,
    Error,
    Collision,
    Timeout,
    NoRoom,
    CrcWrong,
    Nack,
    AuthFail,
  };

  // MIFARE Classic authentication commands.
  enum : uint8_t { kAuthKeyA = 0x60, kAuthKeyB = 0x61 };

  // Starts the Grove I2C bus and the chip. False if no unit answers at 0x28.
  bool begin();

  uint8_t version();  // VersionReg (0x91 / 0x92 for MFRC522)

  void antennaOn();
  void antennaOff();
  void resetField();  // antenna off then on: puts every card back to idle

  Status requestA(uint8_t atqa[2]);  // wakes idle cards
  Status wakeupA(uint8_t atqa[2]);   // wakes idle and halted cards
  Status select(Uid& uid, uint8_t& sak);  // one card in the field
  Status haltA();

  // keyCmd is kAuthKeyA or kAuthKeyB. `uid` is the selected card's UID.
  Status authenticate(uint8_t keyCmd, uint8_t block, const uint8_t key[6],
                      const Uid& uid);
  void stopCrypto1();

  // Reads 16 bytes. MIFARE Classic: one block. Ultralight / NTAG: 4 pages
  // starting at `block`.
  Status read(uint8_t block, uint8_t out[16]);

 private:
  void writeReg(uint8_t reg, uint8_t value);
  void writeRegs(uint8_t reg, const uint8_t* data, size_t len);
  uint8_t readReg(uint8_t reg);
  void readRegs(uint8_t reg, uint8_t* out, size_t len);
  void setBits(uint8_t reg, uint8_t mask);
  void clearBits(uint8_t reg, uint8_t mask);

  Status calcCrc(const uint8_t* data, size_t len, uint8_t result[2]);
  Status communicate(uint8_t command, uint8_t waitIrq, const uint8_t* send,
                     uint8_t sendLen, uint8_t* back, uint8_t* backLen,
                     uint8_t* validBits, bool checkCrc);
  Status reqaOrWupa(uint8_t command, uint8_t atqa[2]);
};
