// Minimal host-side Arduino stub — just enough to compile and exercise
// cypher32_lora.h / cypher32_packets.h off-target.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

#define IRAM_ATTR
#define INPUT 0
#define PROGMEM

// ── fake clock, fully under test control ──
extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }
inline uint32_t micros() { return g_millis * 1000; }
inline void     advance(uint32_t ms) { g_millis += ms; }

inline void pinMode(int, int) {}
inline void delay(uint32_t ms) { g_millis += ms; }
inline void yield() {}

// ── deterministic PRNG so tests are reproducible ──
extern uint32_t g_rngState;
inline void randomSeed(uint32_t s) { g_rngState = s ? s : 1; }
inline long random(long lo, long hi) {
  if (hi <= lo) return lo;
  g_rngState = g_rngState * 1664525u + 1013904223u;
  return lo + (long)((g_rngState >> 8) % (uint32_t)(hi - lo));
}
inline long random(long hi) { return random(0, hi); }

template <typename T> T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ── String ──
class String {
public:
  std::string s;
  String() {}
  String(const char* p) : s(p ? p : "") {}
  String(const std::string& o) : s(o) {}
  String(int v)          { char b[32]; snprintf(b, sizeof b, "%d", v);  s = b; }
  String(unsigned v)     { char b[32]; snprintf(b, sizeof b, "%u", v);  s = b; }
  String(long v)         { char b[32]; snprintf(b, sizeof b, "%ld", v); s = b; }
  String(unsigned long v){ char b[32]; snprintf(b, sizeof b, "%lu", v); s = b; }
  String(char c)         { s = std::string(1, c); }
  String(float v, int d) { char b[48]; snprintf(b, sizeof b, "%.*f", d, v); s = b; }
  String(double v, int d){ char b[48]; snprintf(b, sizeof b, "%.*f", d, v); s = b; }

  size_t      length() const { return s.size(); }
  const char* c_str() const  { return s.c_str(); }
  char        charAt(size_t i) const { return i < s.size() ? s[i] : '\0'; }
  String      substring(size_t a) const { return String(s.substr(a)); }
  String      substring(size_t a, size_t b) const { return String(s.substr(a, b - a)); }

  String& operator+=(const String& o) { s += o.s; return *this; }
  bool operator==(const char* o) const { return s == std::string(o ? o : ""); }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const char* o) const { return !(*this == o); }
};
inline String operator+(const String& a, const String& b) { return String(a.s + b.s); }
inline String operator+(const String& a, const char* b)   { return String(a.s + std::string(b ? b : "")); }
inline String operator+(const char* a, const String& b)   { return String(std::string(a ? a : "") + b.s); }

// ── Serial ──
struct SerialStub {
  bool enabled = false;
  template <typename... A> void printf(const char* f, A... a) {
    if (enabled) std::printf(f, a...);
  }
  void begin(unsigned long) {}
};
extern SerialStub Serial;

struct ESPStub { uint32_t getEfuseMac() { return 0; } void restart() {} };
extern ESPStub ESP;
inline uint32_t esp_random() { return 0x12345678u; }
