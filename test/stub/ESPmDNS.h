#pragma once
#include <Arduino.h>
struct MDNSStub {
  bool begin(const char*) { return true; }
  void addService(const char*, const char*, uint16_t) {}
};
extern MDNSStub MDNS;
