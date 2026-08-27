// Render every e-ink screen the device can show, by running the sketch's own
// drawing code into a framebuffer.
//
// Two things fall out of this, and the second one matters more:
//
//   1. the screenshots in the README are the real pixels, not a mock-up of
//      them — if the layout changes, the images change with it
//   2. it compiles cypher32.ino. Nothing else in this repo does. There is no
//      ESP32 toolchain here, so until now the only thing standing between a
//      typo and the user's Arduino IDE was a regex linter.
//
// Anything the sketch touches that is not a display is stubbed in test/stub.
#include <Arduino.h>
#include <cstdio>
#include <vector>
#include <map>

uint32_t   g_millis   = 100000;
uint32_t   g_rngState = 7;
SerialStub Serial;
ESPStub    ESP;
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
WiFiStub   WiFi;
MDNSStub   MDNS;
std::map<std::string, std::string> Preferences::strs;
std::map<std::string, long>        Preferences::ints;

// The sketch is a .ino, so it is included rather than linked: it defines its
// own globals and we need to reach in and pose them before each screen.
#include "../cypher32.ino"

struct Shot { const char* name; void (*draw)(); };

static void poseIdle() {
  myName = "GhostByte"; myFaction = "BLACK"; myLevel = 7; myXP = 42;
  skillBrute = 11; skillStealth = 5; skillFirewall = 8; skillPoints = 2;
  cyMood = 3; knownCount = 2;
}

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";
  displayPtr = new EInkDisplay_WirelessPaperV1_2();

  // Two neighbours: one scouted to the point of being named, one still just a
  // signal. The radar and the screens both key off exactly this.
  memset(knownNodes, 0, sizeof(knownNodes));
  knownNodes[0].chip_id = 0xBEEF0001; knownNodes[0].level = 9;
  knownNodes[0].faction = 'W'; knownNodes[0].last_seen_ms = g_millis;
  knownNodes[0].first_seen_ms = g_millis - 60000;
  knownNodes[0].rssi = -58; knownNodes[0].rssi_hist[0] = -58; knownNodes[0].rssi_n = 1;
  knownNodes[0].intel = RECON_MAX_SEQ; knownNodes[0].recon_score = RECON_MAX_SEQ;
  knownNodes[0].pwned = true;
  knownNodes[1].chip_id = 0xBEEF0002; knownNodes[1].level = 3;
  knownNodes[1].faction = 'R'; knownNodes[1].last_seen_ms = g_millis;
  knownNodes[1].first_seen_ms = g_millis - 5000;
  knownNodes[1].rssi = -88; knownNodes[1].rssi_hist[0] = -88; knownNodes[1].rssi_n = 1;
  knownNodes[1].intel = 0;
  knownCount = 2;
  loraReady = true; loraStatus = "Online"; loraLastRSSI = -58;

  poseIdle();

  const std::vector<Shot> shots = {
    {"idle",        []{ displayIdle(); }},
    {"newnode",     []{ displayNewNode(0xBEEF0001, 9, 'W'); }},
    {"newnode-unknown", []{ displayNewNode(0xBEEF0002, 3, 'R'); }},
    {"hack-win",    []{ displayHackSuccess("beef0001", 45, "Clean in, clean out."); }},
    {"hack-lose",   []{ displayHackFailed("beef0001", 9, "Firewall held."); }},
    {"message",     []{ displayIncomingMsg("beef0001", "north gate in ten"); }},
    {"levelup",     []{ displayLevelUp(); }},
    {"setup-qr",    []{ displaySetup(); }},
    {"wiping",      []{ displayWiping(); }},
    {"armed",       []{ displayArmed(); }},
  };

  for (const auto& s : shots) {
    display.clearMemory();
    s.draw();
    char path[512];
    snprintf(path, sizeof(path), "%s/eink-%s.pbm", dir, s.name);
    display.writePBM(path);
    printf("  %-18s -> eink-%s.pbm\n", s.name, s.name);
  }
  printf("%d e-ink screens rendered, %d panel updates\n",
         (int)shots.size(), display.updates);
  return 0;
}
