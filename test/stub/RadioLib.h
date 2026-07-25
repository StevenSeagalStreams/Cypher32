// Minimal host-side RadioLib stub with a controllable fake SX1262.
// Captures every transmitted frame so tests can assert on the wire.
#pragma once
#include <Arduino.h>
#include <vector>

#define RADIOLIB_ERR_NONE           0
#define RADIOLIB_ERR_CRC_MISMATCH  -7
#define RADIOLIB_CHANNEL_FREE      -2
#define RADIOLIB_LORA_DETECTED     -3

#define FSPI 0
struct SPIClass {
  SPIClass(int) {}
  void begin(int, int, int, int) {}
};

struct Module {
  Module(int, int, int, int, SPIClass&) {}
};

struct Frame {
  std::vector<uint8_t> data;
  uint32_t             atMs;
};

class SX1262 {
public:
  SX1262(Module* m) : mod(m) {}
  Module* mod;

  // ── test hooks ──
  std::vector<Frame> sent;          // every frame handed to startTransmit()
  std::vector<uint8_t> rxBuf;       // frame the radio will hand back
  bool     cadBusy      = false;    // force scanChannel() to report busy
  bool     failTransmit = false;
  bool     failReceive  = false;
  int      crcNext      = 0;        // next readData() returns CRC error
  int      beginCalls   = 0;
  int      startRxCalls = 0;

  int begin(float, float, int, int, uint8_t, int8_t, int) { beginCalls++; return RADIOLIB_ERR_NONE; }
  void setDio1Action(void (*)()) {}
  void clearDio1Action() {}
  int  standby() { return RADIOLIB_ERR_NONE; }

  int startReceive() {
    startRxCalls++;
    return failReceive ? -1 : RADIOLIB_ERR_NONE;
  }

  int scanChannel() { return cadBusy ? RADIOLIB_LORA_DETECTED : RADIOLIB_CHANNEL_FREE; }

  int startTransmit(uint8_t* buf, size_t len) {
    if (failTransmit) return -1;
    Frame f; f.data.assign(buf, buf + len); f.atMs = millis();
    sent.push_back(f);
    return RADIOLIB_ERR_NONE;
  }
  int finishTransmit() { return RADIOLIB_ERR_NONE; }

  size_t getPacketLength() { return rxBuf.size(); }
  int readData(uint8_t* buf, size_t len) {
    if (crcNext) { crcNext = 0; rxBuf.clear(); return RADIOLIB_ERR_CRC_MISMATCH; }
    size_t n = len < rxBuf.size() ? len : rxBuf.size();
    memcpy(buf, rxBuf.data(), n);
    rxBuf.clear();
    return RADIOLIB_ERR_NONE;
  }
  float getRSSI() { return -70.0f; }
  float getSNR()  { return 9.5f; }
};
