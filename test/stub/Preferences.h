#pragma once
// Host stub — an in-memory NVS. Values survive begin/end within a run, which
// is what the sketch's save/load round trip assumes.
#include <Arduino.h>
#include <map>
class Preferences {
public:
  static std::map<std::string, std::string> strs;
  static std::map<std::string, long>        ints;
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  void clear() { strs.clear(); ints.clear(); }
  void putString(const char* k, const String& v) { strs[k] = v.s; }
  String getString(const char* k, const String& d = String("")) {
    auto it = strs.find(k); return it == strs.end() ? d : String(it->second);
  }
  void putInt(const char* k, int v)               { ints[k] = v; }
  int  getInt(const char* k, int d = 0)           { auto i = ints.find(k); return i == ints.end() ? d : (int)i->second; }
  void putUChar(const char* k, uint8_t v)         { ints[k] = v; }
  uint8_t getUChar(const char* k, uint8_t d = 0)  { auto i = ints.find(k); return i == ints.end() ? d : (uint8_t)i->second; }
  void putULong(const char* k, unsigned long v)   { ints[k] = (long)v; }
  unsigned long getULong(const char* k, unsigned long d = 0) {
    auto i = ints.find(k); return i == ints.end() ? d : (unsigned long)i->second;
  }
};
