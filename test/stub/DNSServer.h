#pragma once
#include <Arduino.h>
class DNSServer {
public:
  void setErrorReplyCode(int) {}
  template <typename T> bool start(uint16_t, const char*, T) { return true; }
  void processNextRequest() {}
};
#define DNSReplyCode_NoError 0
namespace DNSReplyCode { enum { NoError = 0, ServerFailure = 2 }; }
