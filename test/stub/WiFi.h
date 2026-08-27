#pragma once
// Host stub — the sketch only ever brings up a soft AP and asks for its SSID.
#include <Arduino.h>
#define WIFI_AP     2
#define WIFI_STA    1
#define WIFI_AP_STA 3
struct IPAddress {
  uint8_t a, b, c, d;
  IPAddress(uint8_t a_=192, uint8_t b_=168, uint8_t c_=4, uint8_t d_=1)
    : a(a_), b(b_), c(c_), d(d_) {}
  String toString() const {
    char t[20]; snprintf(t, sizeof t, "%u.%u.%u.%u", a, b, c, d); return String(t);
  }
};
typedef IPAddress IPAddressStub;
struct WiFiStub {
  String ssid = "C32_B_HexByte";
  bool mode(int) { return true; }
  bool softAP(const char* s, const char* = nullptr) { ssid = String(s); return true; }
  bool softAP(const String& s) { ssid = s; return true; }
  IPAddressStub softAPIP() { return IPAddressStub(); }
  String softAPSSID() { return ssid; }
};
extern WiFiStub WiFi;
