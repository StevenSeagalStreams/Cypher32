// Minimal host-side Arduino stub — just enough to compile and exercise
// cypher32_lora.h / cypher32_packets.h off-target.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <cctype>

#define IRAM_ATTR
#define INPUT       0
#define OUTPUT      1
#define INPUT_PULLUP 2
#define LOW         0
#define HIGH        1
#define PROGMEM
#define pgm_read_byte(p) (*(const uint8_t*)(p))

// ── fake clock, fully under test control ──
extern uint32_t g_millis;
inline uint32_t millis() { return g_millis; }
inline uint32_t micros() { return g_millis * 1000; }
inline void     advance(uint32_t ms) { g_millis += ms; }

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
// Held high so the sketch sees an unpressed button; the reset arming logic
// walks the same path it does on the bench.
inline int  digitalRead(int) { return HIGH; }
inline int  analogRead(int)  { return 2048; }
// ~3.9 V across the divider: a healthy cell, so the render is not a
// permanent low-battery warning.
inline uint32_t analogReadMilliVolts(int) { return 1950; }
inline void analogReadResolution(int) {}
inline void attachInterrupt(int, void (*)(), int) {}
inline int  digitalPinToInterrupt(int p) { return p; }
#define RISING  1
#define FALLING 2
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
// Overloads, not macros: a min() macro breaks every std:: header that follows.
inline int    min(int a, int b)       { return a < b ? a : b; }
inline int    max(int a, int b)       { return a > b ? a : b; }
inline long   min(long a, long b)     { return a < b ? a : b; }
inline long   max(long a, long b)     { return a > b ? a : b; }
inline unsigned long min(unsigned long a, unsigned long b) { return a < b ? a : b; }
inline unsigned long max(unsigned long a, unsigned long b) { return a > b ? a : b; }
inline float  min(float a, float b)   { return a < b ? a : b; }
inline float  max(float a, float b)   { return a > b ? a : b; }
inline long map(long x, long a, long b, long c, long d) {
  return b == a ? c : (x - a) * (d - c) / (b - a) + c;
}
inline float fmap(float x, float a, float b, float c, float d) {
  return b == a ? c : (x - a) * (d - c) / (b - a) + c;
}

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

  // The sketch parses its own NVS lists with these, so the stub has to carry
  // the whole Arduino String surface, not just the parts the link layer used.
  int  indexOf(const String& n) const           { auto i = s.find(n.s); return i == std::string::npos ? -1 : (int)i; }
  int  indexOf(const String& n, size_t from) const { auto i = s.find(n.s, from); return i == std::string::npos ? -1 : (int)i; }
  int  indexOf(char c) const                    { auto i = s.find(c); return i == std::string::npos ? -1 : (int)i; }
  int  lastIndexOf(char c) const                { auto i = s.rfind(c); return i == std::string::npos ? -1 : (int)i; }
  void remove(size_t at, size_t n)              { if (at < s.size()) s.erase(at, n); }
  void remove(size_t at)                        { if (at < s.size()) s.erase(at); }
  long toInt() const                            { return strtol(s.c_str(), nullptr, 10); }
  double toDouble() const                       { return strtod(s.c_str(), nullptr); }
  bool startsWith(const String& p) const        { return s.rfind(p.s, 0) == 0; }
  bool endsWith(const String& p) const          { return s.size() >= p.s.size() && s.compare(s.size() - p.s.size(), p.s.size(), p.s) == 0; }
  bool equals(const String& o) const            { return s == o.s; }
  void trim()                                   { size_t a = s.find_first_not_of(" \t\r\n");
                                                  size_t b = s.find_last_not_of(" \t\r\n");
                                                  s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1); }
  void toUpperCase()                            { for (auto& c : s) c = toupper((unsigned char)c); }
  void toLowerCase()                            { for (auto& c : s) c = tolower((unsigned char)c); }
  void replace(const String& f, const String& t){ size_t i = 0; while ((i = s.find(f.s, i)) != std::string::npos) { s.replace(i, f.s.size(), t.s); i += t.s.size(); } }
  char operator[](size_t i) const               { return i < s.size() ? s[i] : '\0'; }

  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o)   { s += (o ? o : ""); return *this; }
  bool operator==(const char* o) const { return s == std::string(o ? o : ""); }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const char* o) const { return !(*this == o); }
  bool operator!=(const String& o) const { return !(*this == o); }
  bool operator<(const String& o) const { return s < o.s; }
  void reserve(size_t n) { s.reserve(n); }
  void concat(const String& o) { s += o.s; }
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
  void println()                  { if (enabled) std::printf("\n"); }
  void println(const String& s)   { if (enabled) std::printf("%s\n", s.c_str()); }
  void println(const char* s)     { if (enabled) std::printf("%s\n", s); }
  void println(int v)             { if (enabled) std::printf("%d\n", v); }
  void print(const String& s)     { if (enabled) std::printf("%s", s.c_str()); }
  void print(const char* s)       { if (enabled) std::printf("%s", s); }
  void print(int v)               { if (enabled) std::printf("%d", v); }
  void flush() {}
  operator bool() const { return true; }
};
extern SerialStub Serial;

struct ESPStub {
  uint32_t getEfuseMac() { return 0; }
  uint64_t getEfuseMac64() { return 0x1122334455ULL; }
  void restart() {}
  uint32_t getFreeHeap() { return 180000; }
  uint32_t getHeapSize() { return 320000; }
};
extern ESPStub ESP;
inline uint32_t esp_random() { return 0x12345678u; }
