#pragma once
// Host stub — routes are recorded so a harness can invoke a handler directly,
// and the last response is kept so it can be asserted on.
#include <Arduino.h>
#include <map>
#include <vector>
#define HTTP_GET  1
#define HTTP_POST 2
#define HTTP_ANY  0
typedef void (*HttpHandler)();
class WebServer {
public:
  std::map<std::string, HttpHandler> routes;   // "GET /api/state"
  HttpHandler notFound = nullptr;
  std::map<std::string, std::string> params;   // request args
  int    lastCode = 0;
  String lastBody, lastType;
  int    reqMethod = HTTP_GET;

  WebServer(int = 80) {}
  void begin() {}
  void handleClient() {}
  void on(const char* uri, HttpHandler h)         { routes[std::string("ANY ") + uri] = h; }
  void on(const char* uri, int m, HttpHandler h)  {
    routes[std::string(m == HTTP_GET ? "GET " : "POST ") + uri] = h;
  }
  void onNotFound(HttpHandler h) { notFound = h; }
  String arg(const char* k)  { auto i = params.find(k); return i == params.end() ? String("") : String(i->second); }
  bool   hasArg(const char* k) { return params.count(k) > 0; }
  int    args() { return (int)params.size(); }
  int    method() { return reqMethod; }
  void   sendHeader(const char*, const String&, bool = false) {}
  void   send(int code, const char* type, const String& body) {
    lastCode = code; lastType = String(type); lastBody = body;
  }
  void   send(int code, const char* type, const char* body) { send(code, type, String(body)); }
  void   send_P(int code, const char* type, const char* body) { send(code, type, String(body)); }
};
