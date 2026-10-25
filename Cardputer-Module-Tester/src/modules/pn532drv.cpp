#include "modules/pn532drv.h"

#include <Arduino.h>
#include <string.h>

#include "core/radiospi.h"

namespace pn532 {

namespace {

constexpr uint8_t kI2cAddr = 0x24;
constexpr size_t kBuf = 300;

Bus gBus = Bus::I2C;
Pins gPins;
SPIClass* gSpi = nullptr;
bool gReady = false;

// ================================================================ soft I2C

inline void sclLow() { pinMode(gPins.scl, OUTPUT); digitalWrite(gPins.scl, LOW); }
inline void sdaLow() { pinMode(gPins.sda, OUTPUT); digitalWrite(gPins.sda, LOW); }
inline void sdaRelease() { pinMode(gPins.sda, INPUT_PULLUP); }
inline bool sdaRead() { return digitalRead(gPins.sda) != 0; }

// Releases SCL and waits while the PN532 stretches the clock.
void sclRelease() {
  pinMode(gPins.scl, INPUT_PULLUP);
  uint32_t t0 = micros();
  while (!digitalRead(gPins.scl) && micros() - t0 < 2000) {
  }
}

constexpr int kHalf = 5;  // microseconds, about 100 kHz

void i2cStart() {
  sdaRelease();
  sclRelease();
  delayMicroseconds(kHalf);
  sdaLow();
  delayMicroseconds(kHalf);
  sclLow();
}

void i2cStop() {
  sdaLow();
  delayMicroseconds(kHalf);
  sclRelease();
  delayMicroseconds(kHalf);
  sdaRelease();
  delayMicroseconds(kHalf);
}

// Returns true if the receiver acknowledged.
bool i2cWriteByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    if (b & (1 << i)) sdaRelease(); else sdaLow();
    delayMicroseconds(kHalf);
    sclRelease();
    delayMicroseconds(kHalf);
    sclLow();
  }
  sdaRelease();
  delayMicroseconds(kHalf);
  sclRelease();
  delayMicroseconds(kHalf);
  bool ack = !sdaRead();
  sclLow();
  return ack;
}

uint8_t i2cReadByte(bool ack) {
  uint8_t b = 0;
  sdaRelease();
  for (int i = 0; i < 8; i++) {
    delayMicroseconds(kHalf);
    sclRelease();
    delayMicroseconds(kHalf);
    b = (uint8_t)((b << 1) | (sdaRead() ? 1 : 0));
    sclLow();
  }
  if (ack) sdaLow(); else sdaRelease();
  delayMicroseconds(kHalf);
  sclRelease();
  delayMicroseconds(kHalf);
  sclLow();
  sdaRelease();
  return b;
}

bool i2cWrite(const uint8_t* d, size_t n) {
  i2cStart();
  bool ok = i2cWriteByte(kI2cAddr << 1);
  for (size_t i = 0; ok && i < n; i++) ok = i2cWriteByte(d[i]);
  i2cStop();
  return ok;
}

// Reads n bytes; the first byte the chip sends is its ready flag.
bool i2cRead(uint8_t* d, size_t n) {
  i2cStart();
  if (!i2cWriteByte((kI2cAddr << 1) | 1)) {
    i2cStop();
    return false;
  }
  for (size_t i = 0; i < n; i++) d[i] = i2cReadByte(i + 1 < n);
  i2cStop();
  return true;
}

// ================================================================== SPI

// The PN532 wants least significant bit first.
const SPISettings kSpiSettings(1000000, LSBFIRST, SPI_MODE0);

void spiSelect() {
  gSpi->beginTransaction(kSpiSettings);
  digitalWrite(gPins.cs, LOW);
  delayMicroseconds(10);
}

void spiDeselect() {
  digitalWrite(gPins.cs, HIGH);
  gSpi->endTransaction();
}

// ================================================================ UART

HardwareSerial& uart() { return Serial2; }

// ============================================================ transport

bool txBytes(const uint8_t* d, size_t n) {
  switch (gBus) {
    case Bus::I2C:
      return i2cWrite(d, n);
    case Bus::SPI:
      spiSelect();
      gSpi->transfer(0x01);  // data write
      for (size_t i = 0; i < n; i++) gSpi->transfer(d[i]);
      spiDeselect();
      return true;
    default:
      uart().write(d, n);
      uart().flush();
      return true;
  }
}

bool isReady() {
  switch (gBus) {
    case Bus::I2C: {
      uint8_t s = 0;
      return i2cRead(&s, 1) && (s & 1);
    }
    case Bus::SPI: {
      spiSelect();
      gSpi->transfer(0x02);  // status read
      uint8_t s = gSpi->transfer(0);
      spiDeselect();
      return s & 1;
    }
    default:
      return uart().available() > 0;
  }
}

// Reads n frame bytes once the chip is ready.
bool rxBytes(uint8_t* d, size_t n) {
  switch (gBus) {
    case Bus::I2C: {
      uint8_t tmp[kBuf + 1];
      if (n > kBuf) n = kBuf;
      if (!i2cRead(tmp, n + 1)) return false;
      memcpy(d, tmp + 1, n);  // tmp[0] is the ready flag
      return true;
    }
    case Bus::SPI:
      spiSelect();
      gSpi->transfer(0x03);  // data read
      for (size_t i = 0; i < n; i++) d[i] = gSpi->transfer(0);
      spiDeselect();
      return true;
    default: {
      uint32_t t0 = millis();
      size_t got = 0;
      while (got < n && millis() - t0 < 100) {
        if (uart().available()) d[got++] = (uint8_t)uart().read();
      }
      return got == n;
    }
  }
}

bool waitReady(uint32_t ms, bool (*stop)()) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    if (isReady()) return true;
    if (stop && stop()) return false;
    delay(2);
  }
  return false;
}

// ================================================================ frames

const uint8_t kAck[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

bool sendFrame(uint8_t cmd, const uint8_t* params, size_t plen) {
  uint8_t f[kBuf];
  size_t len = plen + 2;  // TFI + command + params
  if (len + 7 > sizeof(f)) return false;
  size_t i = 0;
  f[i++] = 0x00;
  f[i++] = 0x00;
  f[i++] = 0xFF;
  f[i++] = (uint8_t)len;
  f[i++] = (uint8_t)(0x100 - len);
  f[i++] = 0xD4;
  f[i++] = cmd;
  uint8_t sum = (uint8_t)(0xD4 + cmd);
  for (size_t k = 0; k < plen; k++) {
    f[i++] = params[k];
    sum = (uint8_t)(sum + params[k]);
  }
  f[i++] = (uint8_t)(0x100 - sum);
  f[i++] = 0x00;
  return txBytes(f, i);
}

// Sends a command and waits for its answer. Returns the payload length (after
// the response code), -1 if the chip did not acknowledge, -2 on timeout, -3 on
// a bad answer. `stop` is polled while waiting; it may be null.
int command(uint8_t cmd, const uint8_t* params, size_t plen, uint8_t* resp, size_t respMax,
            uint32_t timeoutMs, bool (*stop)() = nullptr) {
  if (!sendFrame(cmd, params, plen)) return -1;

  // ACK
  if (!waitReady(100, nullptr)) return -1;
  uint8_t ack[6];
  if (!rxBytes(ack, 6) || memcmp(ack, kAck, 6) != 0) return -1;

  // Answer
  if (!waitReady(timeoutMs, stop)) return -2;

  uint8_t buf[kBuf];
  size_t want = respMax + 9;
  if (want > sizeof(buf)) want = sizeof(buf);
  size_t got = 0;
  if (gBus == Bus::UART) {
    // Read the header first so only the real frame is consumed.
    if (!rxBytes(buf, 6)) return -3;
    got = 6;
    size_t need = (size_t)buf[3] + 2 + 6;  // LEN + DCS + postamble + header
    if (buf[3] == 0xFF || need > sizeof(buf)) return -3;
    if (need > got) {
      if (!rxBytes(buf + got, need - got)) return -3;
      got = need;
    }
  } else {
    if (!rxBytes(buf, want)) return -3;
    got = want;
  }

  size_t p = 0;
  while (p + 7 <= got && !(buf[p] == 0 && buf[p + 1] == 0 && buf[p + 2] == 0xFF)) p++;
  if (p + 7 > got) return -3;
  size_t len = buf[p + 3];
  if ((uint8_t)(buf[p + 3] + buf[p + 4]) != 0) return -3;
  if (len < 2 || p + 5 + len > got) return -3;
  if (buf[p + 5] != 0xD5 || buf[p + 6] != (uint8_t)(cmd + 1)) return -3;
  size_t n = len - 2;
  if (n > respMax) n = respMax;
  if (resp && n) memcpy(resp, buf + p + 7, n);
  return (int)n;
}

void abortCommand() {
  txBytes(kAck, 6);  // an ACK frame cancels the running command
  delay(10);
  if (gBus == Bus::UART) {
    while (uart().available()) uart().read();
  } else if (isReady()) {
    uint8_t junk[kBuf];
    rxBytes(junk, 32);
  }
}

}  // namespace

// ================================================================== API

bool begin(Bus bus, const Pins& pins, Firmware& fw) {
  gBus = bus;
  gPins = pins;
  gReady = false;

  switch (bus) {
    case Bus::I2C:
      sdaRelease();
      sclRelease();
      i2cWrite(nullptr, 0);  // wakes the chip; may not be acknowledged
      delay(3);
      break;
    case Bus::SPI: {
      gSpi = &radioSpi(pins.sck, pins.miso, pins.mosi);
      pinMode(pins.cs, OUTPUT);
      digitalWrite(pins.cs, LOW);  // wakes the chip
      delay(3);
      digitalWrite(pins.cs, HIGH);
      delay(2);
      break;
    }
    default: {
      uart().end();
      uart().begin(115200, SERIAL_8N1, pins.rx, pins.tx);
      const uint8_t wake[] = {0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00};
      uart().write(wake, sizeof(wake));
      uart().flush();
      delay(10);
      while (uart().available()) uart().read();
      break;
    }
  }

  uint8_t resp[8];
  int n = command(0x02, nullptr, 0, resp, sizeof(resp), 300);
  if (n < 4) {
    // The first command after power-up can be lost while the chip wakes.
    delay(20);
    n = command(0x02, nullptr, 0, resp, sizeof(resp), 300);
  }
  if (n < 4) return false;
  fw.ic = resp[0];
  fw.ver = resp[1];
  fw.rev = resp[2];

  const uint8_t sam[] = {0x01, 0x14, 0x01};  // normal mode
  if (command(0x14, sam, sizeof(sam), nullptr, 0, 300) < 0) return false;

  // Give up on a passive-target search quickly so the caller stays responsive.
  const uint8_t retries[] = {0x05, 0xFF, 0x01, 0x02};
  command(0x32, retries, sizeof(retries), nullptr, 0, 300);

  gReady = true;
  return true;
}

void end() {
  if (gBus == Bus::UART) uart().end();
  gReady = false;
}

bool pollTag(Tag& tag) {
  const uint8_t params[] = {0x01, 0x00};  // one target, 106 kbps type A
  uint8_t resp[40];
  int n = command(0x4A, params, sizeof(params), resp, sizeof(resp), 600);
  if (n < 6 || resp[0] < 1) return false;

  // [targets][tg][ATQA x2][SAK][uid length][uid...]
  tag.atqa[0] = resp[2];
  tag.atqa[1] = resp[3];
  tag.sak = resp[4];
  uint8_t ul = resp[5];
  if (ul > sizeof(tag.uid) || n < 6 + ul) return false;
  tag.uidLen = ul;
  memcpy(tag.uid, resp + 6, ul);
  return true;
}

namespace {

// InDataExchange: sends `out` to the selected tag and returns the answer length
// (without the status byte), or -1 on failure.
int exchange(const uint8_t* out, size_t n, uint8_t* in, size_t maxIn) {
  uint8_t params[kBuf];
  if (n + 1 > sizeof(params)) return -1;
  params[0] = 0x01;  // target 1
  memcpy(params + 1, out, n);
  uint8_t resp[kBuf];
  int r = command(0x40, params, n + 1, resp, sizeof(resp), 500);
  if (r < 1 || resp[0] != 0x00) return -1;
  size_t len = (size_t)r - 1;
  if (len > maxIn) len = maxIn;
  memcpy(in, resp + 1, len);
  return (int)len;
}

bool sw9000(const uint8_t* d, int n) { return n >= 2 && d[n - 2] == 0x90 && d[n - 1] == 0x00; }

// Type 4 tag: select the NDEF application, find the file, read it.
int readType4(uint8_t* out, size_t maxLen) {
  uint8_t r[kBuf];
  const uint8_t selApp[] = {0x00, 0xA4, 0x04, 0x00, 0x07, 0xD2, 0x76, 0x00,
                            0x00, 0x85, 0x01, 0x01, 0x00};
  int n = exchange(selApp, sizeof(selApp), r, sizeof(r));
  if (!sw9000(r, n)) return 0;  // no NDEF application

  const uint8_t selCc[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03};
  n = exchange(selCc, sizeof(selCc), r, sizeof(r));
  if (!sw9000(r, n)) return 0;

  const uint8_t readCc[] = {0x00, 0xB0, 0x00, 0x00, 0x0F};
  n = exchange(readCc, sizeof(readCc), r, sizeof(r));
  if (n < 17 || !sw9000(r, n)) return -1;
  uint8_t fileId[2] = {r[9], r[10]};  // NDEF file control TLV: 04 06 <file id> ...

  const uint8_t selNdef[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, fileId[0], fileId[1]};
  n = exchange(selNdef, sizeof(selNdef), r, sizeof(r));
  if (!sw9000(r, n)) return -1;

  const uint8_t readLen[] = {0x00, 0xB0, 0x00, 0x00, 0x02};
  n = exchange(readLen, sizeof(readLen), r, sizeof(r));
  if (n < 4 || !sw9000(r, n)) return -1;
  size_t total = ((size_t)r[0] << 8) | r[1];
  if (total == 0) return 0;
  if (total > maxLen) return -1;

  size_t got = 0;
  while (got < total) {
    size_t chunk = total - got;
    if (chunk > 0x30) chunk = 0x30;
    size_t off = 2 + got;
    uint8_t rd[] = {0x00, 0xB0, (uint8_t)(off >> 8), (uint8_t)off, (uint8_t)chunk};
    n = exchange(rd, sizeof(rd), r, sizeof(r));
    if (n < 2 || !sw9000(r, n) || (size_t)(n - 2) != chunk) return -1;
    memcpy(out + got, r, chunk);
    got += chunk;
  }
  return (int)total;
}

// Type 2 tag (NTAG / Ultralight): read pages from 4 and pull out the NDEF TLV.
int readType2(uint8_t* out, size_t maxLen) {
  uint8_t mem[kBuf];
  size_t have = 0;
  for (uint8_t page = 4; have < sizeof(mem) - 16 && page < 130; page += 4) {
    const uint8_t rd[] = {0x30, page};
    uint8_t r[32];
    int n = exchange(rd, sizeof(rd), r, sizeof(r));
    if (n < 16) break;
    memcpy(mem + have, r, 16);
    have += 16;

    // Walk the TLVs we have so far.
    size_t i = 0;
    while (i < have) {
      uint8_t t = mem[i];
      if (t == 0x00) { i++; continue; }          // NULL TLV
      if (t == 0xFE) return 0;                   // terminator: no NDEF found
      if (i + 1 >= have) break;
      size_t len = mem[i + 1];
      size_t hdr = 2;
      if (len == 0xFF) {
        if (i + 3 >= have) break;
        len = ((size_t)mem[i + 2] << 8) | mem[i + 3];
        hdr = 4;
      }
      if (t == 0x03) {
        if (len == 0) return 0;
        if (len > maxLen) return -1;
        if (i + hdr + len <= have) {
          memcpy(out, mem + i + hdr, len);
          return (int)len;
        }
        break;  // need more pages
      }
      i += hdr + len;  // some other TLV: skip it
    }
  }
  return 0;
}

}  // namespace

int readNdef(const Tag& tag, uint8_t* out, size_t maxLen) {
  if (tag.sak & 0x20) return readType4(out, maxLen);
  if (tag.sak == 0x00) return readType2(out, maxLen);
  return 0;  // MIFARE Classic and others: no NDEF reading
}

const char* tagType(const Tag& tag) {
  if (tag.sak & 0x20) return "ISO-DEP (Type 4)";
  switch (tag.sak) {
    case 0x00: return tag.uidLen == 7 ? "NTAG / Ultralight" : "Type 2 tag";
    case 0x08:
    case 0x88: return "MIFARE Classic 1K";
    case 0x09: return "MIFARE Mini";
    case 0x18: return "MIFARE Classic 4K";
    default: return "ISO 14443A tag";
  }
}

// ============================================================== emulation

namespace {

struct Emu {
  const uint8_t* ndef;
  size_t len;
  uint8_t cc[15];
  int selected;  // 0 none, 1 CC file, 2 NDEF file
  bool appSelected;
  bool ndefRead;
};

void emuInit(Emu& e, const uint8_t* ndef, size_t len) {
  e.ndef = ndef;
  e.len = len;
  e.selected = 0;
  e.appSelected = false;
  e.ndefRead = false;
  size_t maxSize = len + 2 > 0xFF ? len + 2 : 0xFF;
  const uint8_t cc[15] = {0x00, 0x0F,         // CC length
                          0x20,               // mapping version 2.0
                          0x00, 0x3B,         // max read size
                          0x00, 0x34,         // max write size
                          0x04, 0x06,         // NDEF file control TLV
                          0xE1, 0x04,         // NDEF file id
                          (uint8_t)(maxSize >> 8), (uint8_t)maxSize,
                          0x00,               // read access: free
                          0xFF};              // write access: none
  memcpy(e.cc, cc, sizeof(cc));
}

// Answers one APDU. Returns the response length.
size_t emuApdu(Emu& e, const uint8_t* a, size_t n, uint8_t* rsp) {
  auto sw = [&](size_t at, uint8_t s1, uint8_t s2) {
    rsp[at] = s1;
    rsp[at + 1] = s2;
    return at + 2;
  };
  if (n < 4 || a[0] != 0x00) return sw(0, 0x6E, 0x00);

  if (a[1] == 0xA4) {  // SELECT
    if (a[2] == 0x04) {
      static const uint8_t aid[7] = {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};
      if (n >= 5 + 7 && a[4] == 7 && memcmp(a + 5, aid, 7) == 0) {
        e.appSelected = true;
        e.selected = 0;
        return sw(0, 0x90, 0x00);
      }
      return sw(0, 0x6A, 0x82);
    }
    if (a[2] == 0x00 && n >= 7 && a[4] == 2 && e.appSelected) {
      if (a[5] == 0xE1 && a[6] == 0x03) { e.selected = 1; return sw(0, 0x90, 0x00); }
      if (a[5] == 0xE1 && a[6] == 0x04) { e.selected = 2; return sw(0, 0x90, 0x00); }
    }
    return sw(0, 0x6A, 0x82);
  }

  if (a[1] == 0xB0 && e.selected) {  // READ BINARY
    size_t off = ((size_t)a[2] << 8) | a[3];
    size_t le = n >= 5 ? a[4] : 256;
    if (le == 0) le = 256;
    size_t size = e.selected == 1 ? sizeof(e.cc) : e.len + 2;
    if (off >= size) return sw(0, 0x6B, 0x00);
    if (off + le > size) le = size - off;
    for (size_t i = 0; i < le; i++) {
      size_t p = off + i;
      if (e.selected == 1) {
        rsp[i] = e.cc[p];
      } else if (p == 0) {
        rsp[i] = (uint8_t)(e.len >> 8);
      } else if (p == 1) {
        rsp[i] = (uint8_t)e.len;
      } else {
        rsp[i] = e.ndef[p - 2];
      }
    }
    if (e.selected == 2 && off + le >= size) e.ndefRead = true;
    return sw(le, 0x90, 0x00);
  }
  return sw(0, 0x6D, 0x00);
}

}  // namespace

EmuResult emulateOnce(const uint8_t* ndef, size_t len, const uint8_t uidTail[3],
                      bool (*stop)()) {
  EmuResult res;
  if (!gReady || len + 2 > 0x7FFF) {
    res.error = true;
    return res;
  }
  Emu emu;
  emuInit(emu, ndef, len);

  // TgInitAsTarget: passive, PICC only, ISO 14443-4 (SEL_RES 0x20).
  uint8_t p[40];
  size_t i = 0;
  p[i++] = 0x05;                         // mode
  p[i++] = 0x04; p[i++] = 0x00;          // SENS_RES
  p[i++] = uidTail[0]; p[i++] = uidTail[1]; p[i++] = uidTail[2];  // NFCID1t (3 bytes)
  p[i++] = 0x20;                         // SEL_RES
  for (int k = 0; k < 18; k++) p[i++] = 0x00;  // FeliCa parameters (unused)
  for (int k = 0; k < 10; k++) p[i++] = 0x00;  // NFCID3t
  p[i++] = 0x00;                         // length of general bytes
  p[i++] = 0x00;                         // length of historical bytes

  uint8_t resp[kBuf];
  int n = command(0x8C, p, i, resp, sizeof(resp), 600000, stop);
  if (n == -2) {  // stopped, or no reader within the wait
    abortCommand();
    res.cancelled = true;
    return res;
  }
  if (n < 1) {
    res.error = true;
    return res;
  }

  // resp[0] is the mode; the rest is the reader's first command.
  uint8_t cmdBuf[kBuf];
  size_t cmdLen = (size_t)n - 1;
  memcpy(cmdBuf, resp + 1, cmdLen);

  for (int guard = 0; guard < 200; guard++) {
    uint8_t rsp[kBuf];
    size_t rl = emuApdu(emu, cmdBuf, cmdLen, rsp + 1);
    if (cmdLen >= 5 && cmdBuf[1] == 0xA4 && cmdBuf[2] == 0x04) res.selected = emu.appSelected;
    res.ndefRead = emu.ndefRead;
    if (emu.appSelected) res.selected = true;

    rsp[0] = 0x00;  // TgResponseToInitiator takes the data directly
    int r = command(0x90, rsp + 1, rl, resp, sizeof(resp), 500);
    if (r < 1 || resp[0] != 0x00) return res;  // reader left

    n = command(0x88, nullptr, 0, resp, sizeof(resp), 600000, stop);
    if (n == -2) {
      abortCommand();
      res.cancelled = stop && stop();
      return res;
    }
    if (n < 1 || resp[0] != 0x00) return res;  // reader left
    cmdLen = (size_t)n - 1;
    memcpy(cmdBuf, resp + 1, cmdLen);
  }
  return res;
}

}  // namespace pn532
