#include <heltec-eink-modules.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>
#include "cypher32_packets.h"
#include "cypher32_lora.h"
#include "cypher32_portal.h"

// Heltec Wireless Paper V1.2 — 250x122px landscape
// Pointer: constructor must NOT run at global init time (before Vext is on)
EInkDisplay_WirelessPaperV1_2* displayPtr = nullptr;
#define display (*displayPtr)

Preferences preferences;
DNSServer dnsServer;
WebServer* serverPtr = nullptr;
#define server (*serverPtr)

// Forward declaration — must be before any function that uses it
struct HackResult {
  bool   success;
  int    xpDelta;  // positive = gained, negative = lost
  String note;
};

// Unique chip ID using ALL 6 MAC bytes folded into 32 bits via XOR
// Placed after HackResult to avoid Arduino preprocessor ordering issues
static uint32_t makeChipID() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t lo  = (uint32_t)(mac & 0xFFFFFFFF);
  uint16_t hi  = (uint16_t)((mac >> 32) & 0xFFFF);
  return lo ^ ((uint32_t)hi << 16) ^ (uint32_t)hi;
}
uint32_t myChipID32 = makeChipID();

// ─────────────────────────────────────────────
//  HARDWARE PINS
// ─────────────────────────────────────────────
#define ADC_CTRL    19
#define BATTERY_PIN 20
#define PRG_PIN     0    // Built-in PRG button on Wireless Paper (active LOW)
#define RESET_HOLD_MS 5000  // Hold 5 seconds to trigger factory reset

// ── Two-button factory reset ─────────────────
//
//  The Wireless Paper has exactly two buttons: RST and PRG. RST is wired to
//  the ESP32's EN pin — it is a hardware reset, not a GPIO. Software can never
//  read it, and holding it just holds the chip in reset with nothing running.
//  "Hold both for five seconds" therefore cannot be implemented literally on
//  this board.
//
//  The gesture below uses both buttons and cannot happen by accident:
//
//      tap RST twice quickly, then hold PRG for 5 seconds
//
//  The double tap is inferred from boot history: a boot that lasted less than
//  ARM_WINDOW_MS was cut short by someone pressing RST, so two in a row means
//  a deliberate double tap. That arms the wipe; PRG then confirms it.
//
//  If your board turns out to have a second *readable* button, set
//  RESET_BTN2_PIN to its GPIO number. The code then skips the arming step and
//  requires both buttons genuinely held together instead.
#define RESET_BTN2_PIN  -1        // GPIO of a second readable button, -1 = none
#define ARM_WINDOW_MS    6000UL   // a boot shorter than this counts as an RST tap
#define ARM_TIMEOUT_MS  20000UL   // armed state lapses if PRG isn't held

#ifndef BLACK
  #define BLACK 0x0000
#endif
#ifndef WHITE
  #define WHITE 0xFFFF
#endif

// ─────────────────────────────────────────────
//  DISPLAY LAYOUT  (250 x 122 px, landscape)
// ─────────────────────────────────────────────
#define DISP_W       250
#define DISP_H       122
#define MARGIN_X     3
#define MARGIN_Y     2
#define FONT_W       6
#define FONT_H       8
#define LINE_H       10

// Footer fixed from bottom:
//   y=114  pip squares (7 px)
//   y=105  B: S: F: labels
//   y=99   XP bar (5 px)
//   y=90   XP text
//   y=88   footer separator
#define FOOTER_SEP_Y  88
#define XP_TEXT_Y     90
#define XP_BAR_Y      99
#define XP_BAR_H      5
#define XP_BAR_X      MARGIN_X
#define XP_BAR_W      (DISP_W - MARGIN_X * 2)
#define PIP_LABEL_Y   112   // Y for skill bars (was pip labels)
// PIP_ROW_Y removed — bars replace squares
#define PIP_SIZE      7    // kept for reference
#define PIP_GAP       2
#define MAX_SKILL     6

// Content zone — side-by-side layout:
//   Character: x=1,  y=13..87, width=75px
//   Bubble:    x=78, y=13..87, width=169px
//   Header sep: y=12
//   Footer sep: y=89
//
//   Inside bubble box (x=78..246):
//     Line 1: y=18  (status)
//     Line 2: y=29  (sub)
//     Line 3: y=40  (extra/xp info)
//     Lines can be omitted — bubble auto-sizes to content
//
#define FACE_Y      13   // sprite top Y
#define BUBBLE_X    82   // bubble left edge X
#define BUBBLE_PAD  5    // internal text padding
#define BUBBLE_W    168  // bubble width (78..245)
#define STATUS_Y    18   // first text line inside bubble (absolute Y)
#define SUB_Y       29
#define EXTRA_Y     40

// ─────────────────────────────────────────────
//  PERSONALITY & MOOD SYSTEM
// ─────────────────────────────────────────────
// Inspired by Pwnagotchi / Flipper Zero.
// The face reacts to events and its mood drifts over time.
//
// Mood is stored as an integer. Each event shifts it:
//   Hack success  → +2
//   Hack fail     → -2
//   Level up      → +3
//   Long idle     → drifts toward 0 slowly
//   Low battery   → -1
//
// Mood clamped to -4..+4. Mapped to face + bubble pool at render time.

int  cyMood        = 0;    // -4=tilted, 0=chill, +4=proud/hyped
int  consecutiveLoss = 0;  // track tilt
int  consecutiveWin  = 0;
unsigned long lastEventMs = 0;

void shiftMood(int delta) {
  cyMood = constrain(cyMood + delta, -4, 4);
  lastEventMs = millis();
  consecutiveLoss = (delta < 0) ? consecutiveLoss + 1 : 0;
  consecutiveWin  = (delta > 0) ? consecutiveWin  + 1 : 0;
}

// Drift mood back toward 0 if nothing has happened for a while
void driftMood() {
  if (millis() - lastEventMs > 300000UL) {  // 5 min of quiet
    if (cyMood > 0) cyMood--;
    else if (cyMood < 0) cyMood++;
    lastEventMs = millis();
  }
  // Low battery nudge
  if (getBatteryPercent() < 20 && cyMood > -2) cyMood--;
}

// ── Face eyes by mood ─────────────────────────────────────────────────
// textSize(3): each char ~18px wide. Eyes fit between the parens.
// We vary the eye characters; parens are always ( and ).
const char* getMoodFace() {
  if (consecutiveLoss >= 2) return "( -_- )";   // tilted, done with it
  if (cyMood >=  4)         return "( *o* )";   // hyped
  if (cyMood >=  2)         return "( ^.^ )";   // happy
  if (cyMood >=  1)         return "( o_o )";   // chill+
  if (cyMood ==  0)         return "( o_o )";   // chill
  if (cyMood == -1)         return "( -.- )";   // meh
  if (cyMood == -2)         return "( ._. )";   // sad
  if (cyMood <= -3)         return "( T_T )";   // salty
  return "( o_o )";
}

// ── Idle bubble pools by mood ─────────────────────────────────────────
const char* bubblesHyped[]  = { "Let's GO!", "HACK ALL", "Come at me!", "I'm on fire!" };
const char* bubblesHappy[]  = { "That felt good.", "Easy win.", "Another one.", "Stay winning." };
const char* bubblesChill[]  = { "All quiet...", "Watching...", "Stay sharp.", "Nothing yet." };
const char* bubblesBored[]  = { "..zzzz..", "*yawn*", "Bored.", "Anyone out there?" };
const char* bubblesMeh[]    = { "meh.", "whatever.", "...", "not feeling it." };
const char* bubblesSad[]    = { "That stung.", "Not my day.", "Come on...", "Really?" };
const char* bubblesTilted[] = { "I give up.", "This is fine.", "Whatever.", "done." };

const char* getIdleBubble() {
  const char** pool;
  int count;
  if (consecutiveLoss >= 2)   { pool = bubblesTilted; count = 4; }
  else if (cyMood >=  4)      { pool = bubblesHyped;  count = 4; }
  else if (cyMood >=  2)      { pool = bubblesHappy;  count = 4; }
  else if (cyMood >= -1)      { pool = bubblesChill;  count = 4; }
  else if (cyMood == -2)      { pool = bubblesSad;    count = 4; }
  else                        { pool = bubblesTilted; count = 4; }
  return pool[random(0, count)];
}

// ── Scan bubbles vary with mood too ──────────────────────────────────
const char* getScanBubble() {
  if (cyMood >= 2)  return (random(2) ? "On the hunt!" : "Let's get them!");
  if (cyMood <= -2) return (random(2) ? "Do I have to?" : "Fine, scanning...");
  const char* pool[] = { "Sniffing...", "Searching...", "Ears open...", "Watching..." };
  return pool[random(0, 4)];
}

#define SCAN_BUBBLES 4  // kept for compatibility

// ─────────────────────────────────────────────
//  RANDOM HACKER NAME GENERATOR
// ─────────────────────────────────────────────
const char* namePrefixes[] = {
  "Ghost","Void","Null","Iron","Zero","Dark",
  "Neon","Byte","Hex","Root","Rogue","Shade",
  "Flux","Nano","Grim","Echo","Venom","Pixel",
  "Glitch","Surge","Blaze","Specter","Crypt","Nexus"
};
const int PREFIX_COUNT = 24;

const char* nameSuffixes[] = {
  "Byte","Crypt","Shade","Hex","Wire","Core",
  "Gate","Node","Shell","Mask","Spike","Bit",
  "Link","Pulse","Slash","Probe","Trap","Worm",
  "Key","Lock","Ping","Trace","Frag","Grid"
};
const int SUFFIX_COUNT = 24;

String generateName() {
  int pi = random(0, PREFIX_COUNT);
  int si = random(0, SUFFIX_COUNT);
  // avoid e.g. "CryptCrypt"
  String pre = namePrefixes[pi];
  String suf = nameSuffixes[si];
  while (suf == pre) si = (si + 1) % SUFFIX_COUNT, suf = nameSuffixes[si];
  String n = pre + suf;
  if (n.length() > 10) n = n.substring(0, 10);
  return n;
}

// ─────────────────────────────────────────────
//  GAME STATE
// ─────────────────────────────────────────────
String myName      = "";
String myPassword  = "";
String myUniqueID  = "";
String myFaction   = "NONE";

// hackedList format: "id:uptimeMs,id:uptimeMs,"
// Each entry stores the uptime-ms when the hack occurred.
// We compare against current uptime to enforce the 7-day cooldown.
// Note: millis() resets on reboot, so we persist a "boot epoch" offset
// (bootEpoch) in preferences so timestamps survive reboots.
String hackedList = "";
String failList   = "";   // 12 h retry cooldowns (T4.4)
String hackPendingId = "";   // target of the hack currently awaiting a verdict

// 7 days in milliseconds
#define WEEK_MS      604800000UL
#define HALF_DAY_MS   43200000UL   // 12 hours hack retry cooldown

// Rebooting from inside a request handler cuts the TCP connection before the
// response has flushed, so the browser sees a reset instead of the reply and
// cannot tell success from failure. Schedule it from loop() instead.
bool     restartPending = false;
uint32_t restartAtMs    = 0;
void requestRestart(uint32_t inMs) { restartPending = true; restartAtMs = millis() + inMs; }

// Set by any handler that put a transient screen up; loop() returns the
// display to idle when it expires. Avoids blocking delay() in handlers.
unsigned long revertIdleAtMs = 0;

int myLevel       = 1;
int myXP          = 0;
int skillPoints   = 0;
int skillStealth  = 0;
int skillBrute    = 0;
int skillFirewall = 0;

// Absolute time base: persisted across reboots as seconds since first boot.
// We store/load this so millis()-relative timestamps stay valid after restart.
unsigned long bootEpoch = 0;  // seconds since first-ever boot (incremented each reboot)

// ─────────────────────────────────────────────
//  HARDWARE
// ─────────────────────────────────────────────

void VextON() {
  pinMode(18, OUTPUT);       digitalWrite(18, LOW);
  pinMode(ADC_CTRL, OUTPUT); digitalWrite(ADC_CTRL, LOW);
}

String getChipID() {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lx", (unsigned long)myChipID32);
  return String(buf);
}

float getBatteryVoltage() {
  digitalWrite(ADC_CTRL, LOW); delay(10);
  uint32_t raw = 0;
  for (int i = 0; i < 15; i++) { raw += analogRead(BATTERY_PIN); delay(2); }
  return ((raw / 15) / 4095.0) * 3.3 * 2.0;
}

int getBatteryPercent() {
  float v = getBatteryVoltage();
  if (v < 3.4)   return 0;
  if (v >= 4.15) return 100;
  return constrain((int)((v - 3.4) * 133), 0, 100);
}

// ─────────────────────────────────────────────
//  PERSISTENCE
// ─────────────────────────────────────────────

void saveProgress() {
  preferences.begin("cypher-v8", false);
  preferences.putString("name",   myName);
  preferences.putString("pw",     myPassword);
  preferences.putString("fac",    myFaction);
  preferences.putString("hacked", hackedList);
  preferences.putString("failed", failList);
  preferences.putInt("lvl",    myLevel);
  preferences.putInt("xp",     myXP);
  preferences.putInt("sp",     skillPoints);
  preferences.putInt("s_st",   skillStealth);
  preferences.putInt("s_br",   skillBrute);
  preferences.putInt("s_fi",   skillFirewall);
  preferences.putULong("epoch", nowMs());  // save total ms elapsed
  preferences.putInt("mood",  cyMood);
  preferences.putInt("closs", consecutiveLoss);
  preferences.end();
}

// Total ms elapsed = ms saved before last reboot + ms since this boot
unsigned long nowMs() {
  return bootEpoch + millis();
}

void loadProgress() {
  preferences.begin("cypher-v8", false);
  myName        = preferences.getString("name",   "");
  myPassword    = preferences.getString("pw",     "cypher32");
  myFaction     = preferences.getString("fac",    "NONE");
  hackedList    = preferences.getString("hacked", "");
  failList      = preferences.getString("failed", "");
  myLevel       = preferences.getInt("lvl",    1);
  myXP          = preferences.getInt("xp",     0);
  skillPoints   = preferences.getInt("sp",     0);
  skillStealth  = preferences.getInt("s_st",   0);
  skillBrute    = preferences.getInt("s_br",   0);
  skillFirewall = preferences.getInt("s_fi",   0);
  // Load previous epoch and add current uptime so time is monotonic
  bootEpoch        = preferences.getULong("epoch", 0);  // total ms before this boot
  cyMood           = preferences.getInt("mood",  0);
  consecutiveLoss  = preferences.getInt("closs", 0);
  myUniqueID  = getChipID();
  // myChipID32 set at global init via makeChipID()
  preferences.end();
}

// ── Cooldown lists (T4.4) ──────────────────────────────────────────────────
// Format: "id:timestampMs,id:timestampMs,"
//
// Both lists are persisted to NVS and timestamped with nowMs(), which carries
// across reboots. They are keyed by chip ID rather than living in the node
// table, so neither a power cycle nor a node being aged out of range can be
// used to clear a cooldown.
//
//   hackedList — 7-day lock after a successful hack
//   failList   — 12-hour retry cooldown after a failed one

bool listHas(const String& list, const String& id, unsigned long window) {
  int start = list.indexOf(id + ":");
  if (start == -1) return false;
  int colon = start + id.length() + 1;
  int comma = list.indexOf(",", colon);
  if (comma == -1) return false;
  unsigned long t = list.substring(colon, comma).toInt();
  return (nowMs() - t) < window;
}

void listRecord(String& list, const String& id) {
  int start = list.indexOf(id + ":");
  if (start != -1) {
    int comma = list.indexOf(",", start);
    if (comma != -1) list.remove(start, comma - start + 1);
  }
  list += id + ":" + String(nowMs()) + ",";
}

void listPrune(String& list, unsigned long window) {
  String fresh = "";
  String tmp = list;
  while (tmp.indexOf(',') != -1) {
    String entry = tmp.substring(0, tmp.indexOf(','));
    tmp = tmp.substring(tmp.indexOf(',') + 1);
    int colon = entry.indexOf(':');
    if (colon == -1) continue;
    unsigned long t = entry.substring(colon + 1).toInt();
    if (nowMs() - t < window) fresh += entry + ",";
  }
  list = fresh;
}

bool recentlyHacked(String id) { return listHas(hackedList, id, WEEK_MS); }
void recordHack(String id)     { listRecord(hackedList, id); }
bool recentlyFailed(String id) { return listHas(failList, id, HALF_DAY_MS); }
void recordFail(String id)     { listRecord(failList, id); }

void pruneHackedList() {
  listPrune(hackedList, WEEK_MS);
  listPrune(failList,   HALF_DAY_MS);
}

// Remaining cooldown in ms, 0 if free — drives the portal's live countdowns.
unsigned long hackCooldownLeft(String id) {
  int start = hackedList.indexOf(id + ":");
  if (start != -1) {
    int colon = start + id.length() + 1;
    int comma = hackedList.indexOf(",", colon);
    if (comma != -1) {
      unsigned long e = nowMs() - hackedList.substring(colon, comma).toInt();
      if (e < WEEK_MS) return WEEK_MS - e;
    }
  }
  start = failList.indexOf(id + ":");
  if (start != -1) {
    int colon = start + id.length() + 1;
    int comma = failList.indexOf(",", colon);
    if (comma != -1) {
      unsigned long e = nowMs() - failList.substring(colon, comma).toInt();
      if (e < HALF_DAY_MS) return HALF_DAY_MS - e;
    }
  }
  return 0;
}

// ─────────────────────────────────────────────
//  HACKING ENGINE
// ─────────────────────────────────────────────
//
//  From design docs:
//    Pool size  = 50 + (enemyFW * 5) - (skillBrute * 4), min 5
//    Attempts   = 4 + skillStealth
//    Roll: pick secret 1..pool, make `attempts` guesses — any match = win
//    XP gain    = 30 + enemyFW * 10
//    XP loss    = 15 - (skillFirewall * 2), min 5
//
//  Faction modifiers (from Factions doc):
//    BLACK: attacks anyone; +20% XP on success; full 15 XP loss (no FW reduction) on fail
//    WHITE: only attacks BLACK and RED; fail loss halved; passive XP when others fail vs them
//    RED:   attacks anyone; +25% XP vs GREEN; 15% chance XP loss even on success vs any
//    GREEN: attacks anyone; +10% XP vs BLACK; 25% chance XP loss even on success vs WHITE

HackResult resolveHack(String enemyFaction, int enemyFW) {
  HackResult r;

  // Faction gate: WHITE can only attack BLACK and RED
  if (myFaction == "WHITE" && enemyFaction != "BLACK" && enemyFaction != "RED") {
    r.success = false; r.xpDelta = 0; r.note = "Target immune!";
    return r;
  }

  // Pool & attempts
  int pool     = 50 + (enemyFW * 5) - (skillBrute * 4);
  if (pool < 5) pool = 5;
  int attempts = 4 + skillStealth;

  // Roll
  int secret = random(1, pool + 1);
  bool hit   = false;
  for (int i = 0; i < attempts && !hit; i++)
    if (random(1, pool + 1) == secret) hit = true;

  // Base values
  int baseGain = 15 + (enemyFW * 5);   // reduced: max ~55 XP per hack
  int baseLoss = 15 - (skillFirewall * 2);
  if (baseLoss < 5) baseLoss = 5;

  if (hit) {
    r.success = true;
    r.xpDelta = baseGain;

    if (myFaction == "BLACK") {
      r.xpDelta = (int)(baseGain * 1.2);
      r.note    = "XP stolen! +20%";
    } else if (myFaction == "RED") {
      if (random(100) < 15) {
        r.success = false; r.xpDelta = -baseLoss; r.note = "Traced! Lose XP.";
      } else if (enemyFaction == "GREEN") {
        r.xpDelta = (int)(baseGain * 1.25); r.note = "Easy target! +25%";
      } else {
        r.note = "Hack success.";
      }
    } else if (myFaction == "GREEN") {
      if (enemyFaction == "WHITE" && random(100) < 25) {
        r.success = false; r.xpDelta = -baseLoss; r.note = "Firewall bit back!";
      } else if (enemyFaction == "BLACK") {
        r.xpDelta = (int)(baseGain * 1.10); r.note = "+10% vs Black Hat";
      } else {
        r.note = "Hack success.";
      }
    } else {
      r.note = "Node secured.";
    }
  } else {
    r.success = false;
    if (myFaction == "BLACK") {
      r.xpDelta = -15; r.note = "Exposed! Full loss.";  // no FW reduction for Black
    } else {
      r.xpDelta = -baseLoss; r.note = "Counter-hacked.";
    }
  }
  return r;
}

// ─────────────────────────────────────────────
//  LEVEL & XP SYSTEM
// ─────────────────────────────────────────────
// Max level: 32  (Cypher32!)
// XP to next level = currentLevel * 150
//   LVL 1→2:  150 XP
//   LVL 5→6:  750 XP
//   LVL 16→17: 2400 XP
//   LVL 31→32: 4650 XP
// XP per successful hack: 15 + enemyFW * 5  (max ~55 at FW=8)
// This means even at low level you need several hacks per level.
#define MAX_LEVEL 32

int xpForNextLevel() {
  return myLevel * 150;
}

// Apply XP delta; return true if player levelled up
bool applyXP(int delta) {
  myXP += delta;
  if (myXP < 0) myXP = 0;
  if (myLevel >= MAX_LEVEL) {
    myXP = 0;  // cap at max level, no overflow
    return false;
  }
  if (myXP >= xpForNextLevel()) {
    myXP -= xpForNextLevel();
    myLevel++;
    skillPoints++;
    return true;
  }
  return false;
}






// ─────────────────────────────────────────────
//  HACKER SPRITES (PROGMEM 1-bit bitmaps)
//  Left-aligned x=1, y=FACE_Y(13)
//  Forced 75x75px — fills full content zone
//  Bubble zone starts at x=82
// ─────────────────────────────────────────────

#define SPR_IDLE1_W 75
#define SPR_IDLE1_H 75
const uint8_t PROGMEM spr_idle1[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xC0, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xDF, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xBF, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
  0xFF, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x3F, 0x98,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x7F, 0xC8, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x7F, 0xCC, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x04, 0xFF, 0xE4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x0D, 0xFF, 0xF6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
  0xFF, 0xF2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0xFF, 0xE2,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0xFF, 0xF3, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xFF, 0xE9, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x11, 0xFF, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x23, 0x80, 0x1D, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C,
  0x7F, 0xC4, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0xFF, 0xF0,
  0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0xFF, 0xFC, 0x40, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x4F, 0xFF, 0xFF, 0x40, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x5F, 0xFF, 0xFF, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x9F, 0xFF, 0xFF, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xBF,
  0xFB, 0xFF, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBF, 0xC0, 0x7F,
  0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3F, 0x80, 0x3F, 0xD0, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0xBF, 0x80, 0x3F, 0xB0, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0xBF, 0x80, 0x3F, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x9F, 0x80, 0x7F, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xDF,
  0xC0, 0x7F, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xEF, 0xE0, 0xFE,
  0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xE7, 0xE4, 0xFC, 0xFF, 0x80,
  0x00, 0x00, 0x00, 0x00, 0xFF, 0xF7, 0xFB, 0xFD, 0xFF, 0xE0, 0x00, 0x00,
  0x00, 0x00, 0xFF, 0xF3, 0xFB, 0xF9, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x00,
  0xBF, 0xFB, 0xFF, 0xFB, 0xFF, 0xB0, 0x00, 0x00, 0x00, 0x01, 0x7F, 0xF9,
  0xFF, 0xF3, 0xFF, 0xD0, 0x00, 0x00, 0x00, 0x03, 0x75, 0xED, 0xFF, 0xF6,
  0xFA, 0xD0, 0x00, 0x00, 0x00, 0x02, 0xF5, 0xEC, 0xFF, 0xE6, 0xF5, 0xE8,
  0x00, 0x00, 0x00, 0x06, 0xFA, 0xDE, 0x7F, 0xCF, 0x6D, 0xEC, 0x00, 0x00,
  0x00, 0x00, 0xDE, 0xD7, 0x7F, 0xDE, 0xAB, 0x64, 0x00, 0x00, 0x00, 0x04,
  0xEF, 0xFF, 0xFF, 0xFF, 0xFE, 0xE4, 0x00, 0x00, 0x00, 0x0D, 0x78, 0x00,
  0x00, 0x00, 0x03, 0xDA, 0x00, 0x00, 0x00, 0x09, 0xF3, 0xFF, 0xFF, 0xFF,
  0xF9, 0xF3, 0x00, 0x00, 0x00, 0x1B, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xF3,
  0x00, 0x00, 0x00, 0x31, 0xF7, 0xFF, 0xFF, 0xFF, 0xFC, 0xE9, 0x00, 0x00,
  0x00, 0x2E, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xED, 0x80, 0x00, 0x00, 0x3B,
  0x77, 0xFF, 0xFF, 0xFF, 0xFB, 0xDF, 0x80, 0x00, 0x00, 0x63, 0xF7, 0xFF,
  0xFF, 0xFF, 0xFD, 0xF8, 0x40, 0x00, 0x00, 0x4D, 0xF3, 0xFF, 0xFF, 0xFF,
  0xF9, 0xF7, 0x40, 0x00, 0x00, 0xF3, 0xFB, 0xFF, 0xFF, 0xFF, 0xFB, 0xEF,
  0x60, 0x00, 0x00, 0x97, 0x7B, 0xFF, 0xFF, 0xFF, 0xF9, 0xF8, 0x20, 0x00,
  0x01, 0x8F, 0xFB, 0xFF, 0xFF, 0xFF, 0xFB, 0xFE, 0x30, 0x00, 0x01, 0x15,
  0xEB, 0xFF, 0xF2, 0xFF, 0xF9, 0x75, 0x30, 0x00, 0x01, 0x31, 0x4B, 0xFF,
  0xE0, 0xFF, 0xFA, 0x31, 0x90, 0x00, 0x03, 0xF6, 0x1B, 0xFF, 0xF0, 0xFF,
  0xFB, 0x0D, 0x78, 0x00, 0x03, 0xEC, 0x1B, 0xFF, 0xE5, 0xFF, 0xFB, 0x06,
  0xB8, 0x00, 0x03, 0xDD, 0x3B, 0xFF, 0xFF, 0xFF, 0xFA, 0xA7, 0xFC, 0x00,
  0x07, 0xD9, 0xA9, 0xFF, 0xFF, 0xFF, 0xF2, 0xD7, 0x78, 0x00, 0x03, 0xFF,
  0xAD, 0xFF, 0xFF, 0xFF, 0xF3, 0xBB, 0xFC, 0x00, 0x03, 0xFF, 0xE9, 0xFF,
  0xFF, 0xFF, 0xFE, 0x7F, 0xF8, 0x00, 0x07, 0xFF, 0xDD, 0xFF, 0xFF, 0xFF,
  0xF3, 0x7F, 0xF8, 0x00, 0x03, 0xFF, 0xE9, 0xFF, 0xFF, 0xFF, 0xF7, 0x7F,
  0xF8, 0x00, 0x01, 0xFF, 0xED, 0xFF, 0xFF, 0xFF, 0xF4, 0xFF, 0xF0, 0x00,
  0x00, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF, 0xE7, 0xFF, 0xF0, 0x00, 0x00, 0xE0,
  0x4C, 0xFF, 0xFF, 0xFF, 0xF5, 0x40, 0xE0, 0x00, 0x1E, 0xFF, 0xB4, 0x20,
  0x08, 0x88, 0x06, 0xBF, 0xFF, 0x00, 0x13, 0x49, 0x5E, 0xAE, 0xE6, 0x65,
  0xD7, 0x54, 0x82, 0x80, 0x00, 0xAA, 0xAF, 0xFF, 0xFF, 0xFF, 0xFE, 0xA5,
  0x58, 0x00, 0x07, 0xB6, 0xDA, 0xAA, 0xAA, 0xAA, 0xAB, 0x7B, 0x74, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define SPR_IDLE2_W 75
#define SPR_IDLE2_H 75
const uint8_t PROGMEM spr_idle2[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xC0, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xDF, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0xFF, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
  0xBF, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3F, 0xD0,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x7F, 0xD8, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x7F, 0xC8, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x04, 0xFF, 0xEC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x06, 0xFF, 0xE4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
  0x7F, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0xFF, 0xF2,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xFF, 0xF2, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0xA0, 0x73, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x12, 0x15, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x30, 0xDF, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13,
  0xFF, 0xF1, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0xFF, 0xFC,
  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0xFF, 0xFE, 0x80, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x6F, 0xFF, 0xFE, 0xC0, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x3F, 0x9F, 0x3F, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x5F, 0x8E, 0x3F, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5F,
  0xDE, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xFF,
  0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5F, 0xFF, 0xFF, 0x40, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x6F, 0xFF, 0xFF, 0x40, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x2F, 0xFF, 0xFE, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x6F, 0xFF, 0xFC, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xF7,
  0xFF, 0xFD, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xF7, 0xFF, 0xFD,
  0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFB, 0xFF, 0xFB, 0xFF, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x7F, 0xF9, 0xFF, 0xF7, 0xFF, 0xC0, 0x00, 0x00,
  0x00, 0x00, 0xFF, 0xFD, 0xFF, 0xF7, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x00,
  0xFF, 0xFE, 0xFF, 0xE7, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x01, 0xBB, 0xFE,
  0x7F, 0xEF, 0xFB, 0xB0, 0x00, 0x00, 0x00, 0x01, 0x6D, 0xEE, 0xFF, 0xCE,
  0xF6, 0xD0, 0x00, 0x00, 0x00, 0x03, 0x75, 0xFF, 0x7F, 0xDE, 0xF5, 0xD8,
  0x00, 0x00, 0x00, 0x02, 0x76, 0xE7, 0x3F, 0xBD, 0xFB, 0xC8, 0x00, 0x00,
  0x00, 0x06, 0xFB, 0xFF, 0xFF, 0xBF, 0xEF, 0xEC, 0x00, 0x00, 0x00, 0x04,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE4, 0x00, 0x00, 0x00, 0x0D, 0x78, 0x00,
  0x00, 0x00, 0x03, 0xD6, 0x00, 0x00, 0x00, 0x09, 0xB0, 0x00, 0x00, 0x00,
  0x01, 0xB2, 0x00, 0x00, 0x00, 0x0B, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xFB,
  0x00, 0x00, 0x00, 0x10, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xF1, 0x00, 0x00,
  0x00, 0x32, 0xF3, 0xFF, 0xFF, 0xFF, 0xFD, 0xE9, 0x80, 0x00, 0x00, 0x2F,
  0x77, 0xFF, 0xFF, 0xFF, 0xFD, 0xD6, 0x80, 0x00, 0x00, 0x79, 0xF7, 0xFF,
  0xFF, 0xFF, 0xF9, 0xFB, 0xC0, 0x00, 0x00, 0x62, 0xFB, 0xFF, 0xFF, 0xFF,
  0xFD, 0xC8, 0xC0, 0x00, 0x00, 0xC8, 0x73, 0xFF, 0xFF, 0xFF, 0xFB, 0xC2,
  0x60, 0x00, 0x00, 0xBF, 0xFB, 0xFF, 0xFF, 0xFF, 0xF9, 0xFF, 0xA0, 0x00,
  0x00, 0xC3, 0xFB, 0xFF, 0xFF, 0xFF, 0xFB, 0xF8, 0xF0, 0x00, 0x01, 0x9F,
  0xF3, 0xFF, 0xFD, 0xFF, 0xF9, 0xFE, 0x10, 0x00, 0x03, 0x29, 0xB3, 0xFF,
  0xE0, 0xFF, 0xF9, 0xB3, 0x98, 0x00, 0x03, 0x73, 0x0B, 0xFF, 0xF1, 0xFF,
  0xFA, 0x19, 0xD8, 0x00, 0x03, 0xD6, 0x1B, 0xFF, 0xF1, 0xFF, 0xFB, 0x0C,
  0xE8, 0x00, 0x03, 0xC4, 0xA9, 0xFF, 0xFF, 0xFF, 0xF2, 0xA6, 0x7C, 0x00,
  0x07, 0xDB, 0x6B, 0xFF, 0xFF, 0xFF, 0xF6, 0xB3, 0x78, 0x00, 0x03, 0xFD,
  0x49, 0xFF, 0xFF, 0xFF, 0xF2, 0xDF, 0xFC, 0x00, 0x03, 0xFF, 0xDD, 0xFF,
  0xFF, 0xFF, 0xF7, 0x7F, 0xF8, 0x00, 0x07, 0xFF, 0xD9, 0xFF, 0xFF, 0xFF,
  0xF3, 0x7F, 0xFC, 0x00, 0x03, 0xFF, 0xD5, 0xFF, 0xFF, 0xFF, 0xF5, 0x7F,
  0xF8, 0x00, 0x01, 0xFF, 0xF4, 0xFF, 0xFF, 0xFF, 0xF5, 0xFF, 0xF0, 0x00,
  0x01, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF, 0xE7, 0xFF, 0xF0, 0x00, 0x00, 0xF6,
  0xDD, 0xFF, 0xFF, 0xFF, 0xF7, 0x6A, 0xE0, 0x00, 0x1F, 0xE0, 0x24, 0x04,
  0x21, 0x01, 0x04, 0x09, 0xFF, 0x00, 0x1A, 0xDF, 0xFE, 0x51, 0x08, 0x54,
  0x4F, 0xFE, 0xDB, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
  0x00, 0x00, 0x07, 0xFF, 0xD5, 0x55, 0x55, 0x55, 0x55, 0xFB, 0xFC, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define SPR_BORED_W 75
#define SPR_BORED_H 75
const uint8_t PROGMEM spr_bored[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xC0, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x3F, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
  0x7F, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3F, 0xC8,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x7F, 0xCC, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xFF, 0xE4, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x0D, 0xFF, 0xF6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x08, 0xFF, 0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19,
  0xFF, 0xF3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0xD0, 0xF9,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x00, 0x19, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x7F, 0x84, 0x80, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x21, 0xFF, 0xF0, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x67, 0xFF, 0xFC, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4F,
  0xFF, 0xFE, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDF, 0xFF, 0xFE,
  0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBF, 0x3F, 0x9F, 0x20, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x2D, 0x0E, 0x1E, 0xB0, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x49, 0x8E, 0x32, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x5F, 0xFF, 0xFF, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x4F,
  0xFF, 0xFE, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2F, 0xFF, 0xFE,
  0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xA7, 0xFF, 0xFC, 0xB0, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x07, 0x81, 0xFF, 0xF8, 0x3C, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x1F, 0xC1, 0xFF, 0xF0, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x3F, 0xE0, 0xFF, 0xE0, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xE0,
  0x7F, 0xC0, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xF0, 0x7F, 0xC3,
  0xFF, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x9F, 0xF8, 0x7F, 0xC3, 0xFF, 0x20,
  0x00, 0x00, 0x00, 0x01, 0x3F, 0xCE, 0x7F, 0xCC, 0x7F, 0x90, 0x00, 0x00,
  0x00, 0x01, 0x7F, 0x9B, 0x7F, 0xF3, 0x3E, 0xD0, 0x00, 0x00, 0x00, 0x03,
  0x67, 0x2C, 0xFF, 0xE6, 0x9D, 0xD0, 0x00, 0x00, 0x00, 0x01, 0x3B, 0x3F,
  0xFF, 0xFF, 0x9D, 0xC8, 0x00, 0x00, 0x00, 0x02, 0xBA, 0x7E, 0x7F, 0xCF,
  0xCF, 0xA8, 0x00, 0x00, 0x00, 0x02, 0x5E, 0x7F, 0x7F, 0xBF, 0xCF, 0x48,
  0x00, 0x00, 0x00, 0x04, 0xFE, 0xFF, 0xBF, 0xFF, 0xEF, 0xC4, 0x00, 0x00,
  0x00, 0x04, 0x7B, 0xB7, 0xFB, 0xB7, 0x77, 0xE4, 0x00, 0x00, 0x00, 0x04,
  0x70, 0x00, 0x00, 0x00, 0x01, 0x86, 0x00, 0x00, 0x00, 0x09, 0xA5, 0x55,
  0x55, 0x55, 0x54, 0xF2, 0x00, 0x00, 0x00, 0x0B, 0xE7, 0xFF, 0xFF, 0xFF,
  0xFC, 0xEA, 0x00, 0x00, 0x00, 0x09, 0xEF, 0xFF, 0xFF, 0xFF, 0xFC, 0xF2,
  0x00, 0x00, 0x00, 0x12, 0xE7, 0xFF, 0xFF, 0xFF, 0xFD, 0x71, 0x00, 0x00,
  0x00, 0x17, 0x8F, 0xFF, 0xFF, 0xFF, 0xFC, 0xBD, 0x00, 0x00, 0x00, 0x19,
  0x27, 0xFF, 0xFF, 0xFF, 0xFD, 0x13, 0x00, 0x00, 0x00, 0x22, 0x77, 0xFF,
  0xFF, 0xFF, 0xFD, 0x88, 0x80, 0x00, 0x00, 0x24, 0x97, 0xFF, 0xFF, 0xFF,
  0xFD, 0xA4, 0x80, 0x00, 0x00, 0x20, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xA2,
  0x80, 0x00, 0x00, 0x4A, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xE8, 0x40, 0x00,
  0x00, 0x53, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xF9, 0x40, 0x00, 0x00, 0x77,
  0xF7, 0xFF, 0xE0, 0xFF, 0xFD, 0xFD, 0xC0, 0x00, 0x00, 0x6F, 0xF3, 0xFF,
  0xF1, 0xFF, 0xFD, 0xFE, 0xC0, 0x00, 0x00, 0x7F, 0xF3, 0xFF, 0xE0, 0xFF,
  0xF9, 0xFF, 0xC0, 0x00, 0x00, 0x7F, 0xFB, 0xFF, 0xFF, 0xFF, 0xF9, 0xFF,
  0xC0, 0x00, 0x00, 0x7F, 0xF3, 0xFF, 0xFF, 0xFF, 0xFB, 0xFF, 0xC0, 0x00,
  0x00, 0x7F, 0xFB, 0xFF, 0xFF, 0xFF, 0xF9, 0xFF, 0xC0, 0x00, 0x00, 0x7F,
  0xF3, 0xFF, 0xFF, 0xFF, 0xFB, 0xFF, 0xC0, 0x00, 0x00, 0x3F, 0xFB, 0xFF,
  0xFF, 0xFF, 0xF9, 0xFF, 0x80, 0x00, 0x00, 0x3F, 0xFB, 0xFF, 0xFF, 0xFF,
  0xFB, 0xFF, 0x80, 0x00, 0x00, 0x3F, 0xF9, 0xFF, 0xFF, 0xFF, 0xF3, 0xFF,
  0x80, 0x00, 0x00, 0x1F, 0xFB, 0xFF, 0xFF, 0xFF, 0xFB, 0xFF, 0x00, 0x00,
  0x00, 0x0F, 0xC9, 0xFF, 0xFF, 0xFF, 0xFA, 0x7C, 0x00, 0x00, 0x00, 0x00,
  0x09, 0xFF, 0xFF, 0xFF, 0xF2, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xE8, 0x00,
  0x00, 0x00, 0x03, 0xFF, 0xFF, 0x00, 0x14, 0xA4, 0xBF, 0xFF, 0xFF, 0xFF,
  0xFF, 0x4A, 0xAB, 0x00, 0x02, 0x92, 0x5F, 0xFF, 0xFF, 0xFF, 0xFF, 0xA9,
  0x24, 0x00, 0x02, 0xAD, 0xA8, 0x00, 0x00, 0x00, 0x01, 0x55, 0x50, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define SPR_VICTORY_W 75
#define SPR_VICTORY_H 75
const uint8_t PROGMEM spr_victory[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x80, 0x00, 0x01, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
  0x70, 0x00, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x18, 0x00,
  0x04, 0xBC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xA4, 0x00, 0x0A, 0xEC,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0xEA, 0x00, 0x0B, 0xD2, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x04, 0x76, 0x00, 0x0D, 0xF8, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x07, 0xFC, 0x00, 0x07, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
  0xF6, 0x00, 0x07, 0xF4, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x05, 0xFC, 0x00,
  0x05, 0xE8, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x02, 0xF4, 0x00, 0x05, 0xD8,
  0x00, 0x00, 0x7F, 0xC0, 0x00, 0x03, 0x78, 0x00, 0x03, 0xF4, 0x00, 0x00,
  0xDF, 0x60, 0x00, 0x02, 0xFC, 0x00, 0x06, 0x8C, 0x00, 0x00, 0xBF, 0xA0,
  0x00, 0x06, 0x48, 0x00, 0x07, 0x34, 0x00, 0x01, 0x3F, 0x90, 0x00, 0x05,
  0xBC, 0x00, 0x07, 0xE6, 0x00, 0x01, 0x7F, 0xD8, 0x00, 0x08, 0xFC, 0x00,
  0x07, 0xF2, 0x00, 0x02, 0x7F, 0xC8, 0x00, 0x0B, 0x7C, 0x00, 0x07, 0xE2,
  0x00, 0x06, 0xFF, 0xEC, 0x00, 0x08, 0xFC, 0x00, 0x07, 0xCA, 0x00, 0x04,
  0xFF, 0xC4, 0x00, 0x0A, 0x7C, 0x00, 0x03, 0xF9, 0x00, 0x0C, 0xFF, 0xE6,
  0x00, 0x19, 0xFC, 0x00, 0x07, 0xF1, 0x00, 0x09, 0xE0, 0xF2, 0x00, 0x13,
  0xF8, 0x00, 0x03, 0xFD, 0x00, 0x09, 0x04, 0x32, 0x00, 0x15, 0xF8, 0x00,
  0x03, 0xF9, 0x80, 0x12, 0x7F, 0x89, 0x00, 0x27, 0xF8, 0x00, 0x03, 0xFA,
  0x80, 0x11, 0xFF, 0xE1, 0x00, 0x2B, 0xF8, 0x00, 0x01, 0xFC, 0xC0, 0x13,
  0xFF, 0xF9, 0x00, 0x27, 0xF0, 0x00, 0x01, 0xFC, 0xA0, 0x37, 0xFF, 0xFD,
  0x80, 0x57, 0xF0, 0x00, 0x01, 0xF9, 0xA0, 0x2F, 0xBF, 0xBE, 0x80, 0xD3,
  0xF0, 0x00, 0x01, 0xFB, 0x30, 0x2F, 0x8F, 0x3E, 0x81, 0x9B, 0xF0, 0x00,
  0x00, 0xFF, 0x10, 0x5F, 0x9E, 0x7E, 0x81, 0x3F, 0xE0, 0x00, 0x00, 0xFF,
  0x48, 0x5F, 0xFF, 0xFF, 0x42, 0x2F, 0xF0, 0x00, 0x00, 0xFF, 0xA4, 0x5F,
  0xFF, 0xFF, 0x46, 0xBF, 0xE0, 0x00, 0x00, 0xFF, 0xA2, 0x5F, 0xFF, 0xFF,
  0x48, 0xBF, 0xE0, 0x00, 0x00, 0x7F, 0x6B, 0x2F, 0xFF, 0xFE, 0xDA, 0x9F,
  0xC0, 0x00, 0x00, 0x3F, 0xA9, 0xAF, 0xFF, 0xFE, 0xB2, 0xFF, 0xC0, 0x00,
  0x00, 0x3F, 0xF8, 0xF7, 0xFF, 0xFD, 0xE3, 0xFF, 0x80, 0x00, 0x00, 0x1F,
  0xFC, 0xF3, 0xFF, 0xFD, 0xCF, 0xFF, 0x00, 0x00, 0x00, 0x1F, 0xF5, 0x7B,
  0xFF, 0xFB, 0xD5, 0xFF, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0x79, 0xFF, 0xF7,
  0xD7, 0xFE, 0x00, 0x00, 0x00, 0x0F, 0xFD, 0xBD, 0xFF, 0xF7, 0xBF, 0xFE,
  0x00, 0x00, 0x00, 0x07, 0xFF, 0xBE, 0xFF, 0xEF, 0xAF, 0xFC, 0x00, 0x00,
  0x00, 0x03, 0xFF, 0xDE, 0x7F, 0xCF, 0xBF, 0xFC, 0x00, 0x00, 0x00, 0x03,
  0xFF, 0xFF, 0x7F, 0xDF, 0xFF, 0xF8, 0x00, 0x00, 0x00, 0x01, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x01, 0xFD, 0x56, 0xAA, 0xAA,
  0xAB, 0xF0, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x01, 0xE0,
  0x00, 0x00, 0x00, 0x00, 0x67, 0xFF, 0xFF, 0xFF, 0xF9, 0xC0, 0x00, 0x00,
  0x00, 0x00, 0x77, 0xFF, 0xFF, 0xFF, 0xFC, 0xC0, 0x00, 0x00, 0x00, 0x00,
  0x27, 0xFF, 0xFF, 0xFF, 0xFD, 0x80, 0x00, 0x00, 0x00, 0x00, 0x37, 0xFF,
  0xFF, 0xFF, 0xFC, 0x80, 0x00, 0x00, 0x00, 0x00, 0x17, 0xFF, 0xFF, 0xFF,
  0xFD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0xFF, 0xFF, 0xFF, 0xFD, 0x80,
  0x00, 0x00, 0x00, 0x00, 0x13, 0xFF, 0xFF, 0xFF, 0xF9, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x17, 0xFF, 0xFF, 0xFF, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x13, 0xFF, 0xFF, 0xFF, 0xF9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0xFF,
  0xEA, 0xFF, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xFF, 0xE0, 0xFF,
  0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0xFF, 0xF1, 0xFF, 0xFD, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x0B, 0xFF, 0xEA, 0xFF, 0xFA, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x0B, 0xFF, 0xFF, 0xFF, 0xF2, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0B, 0xFF, 0xFF, 0xFF, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0xFF,
  0xFF, 0xFF, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0xFF, 0xFF, 0xFF,
  0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xFF, 0xFF, 0xFF, 0xF2, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x09, 0xFF, 0xFF, 0xFF, 0xF2, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x0D, 0xFF, 0xFF, 0xFF, 0xF6, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x09, 0xFF, 0xFF, 0xFF, 0xF2, 0x00, 0x00, 0x00, 0x01, 0x11, 0x0D, 0x6D,
  0x6D, 0xB6, 0xB2, 0x49, 0x24, 0x00, 0x1F, 0xFF, 0xFC, 0x00, 0x00, 0x00,
  0x07, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
  0x00, 0x00, 0x07, 0xFF, 0xDB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFC, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define SPR_LOST_W 75
#define SPR_LOST_H 75
const uint8_t PROGMEM spr_lost[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xC0, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xBF, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x03, 0x7F, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
  0xDF, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0xBF, 0xAE,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x7F, 0xD5, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x1A, 0xFF, 0xEB, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x11, 0xBF, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x23, 0x7F, 0xD8, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
  0xFF, 0xE0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xFF, 0xF0,
  0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0xFF, 0xF8, 0x40, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0xC0, 0x78, 0x40, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x87, 0x3F, 0x3C, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x87, 0x7F, 0xCC, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x8C,
  0xFF, 0xEE, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x99, 0xFF, 0xF7,
  0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x7B, 0xFF, 0xFB, 0xD6, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x09, 0x37, 0xFF, 0xF9, 0x92, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x11, 0x9F, 0x9F, 0x3D, 0x71, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x33, 0xF5, 0x8E, 0x2E, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0xED,
  0xDE, 0xE6, 0xF8, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x45, 0xFF, 0xFF, 0xFF,
  0xF4, 0x40, 0x00, 0x00, 0x00, 0x00, 0x4D, 0xFF, 0xFF, 0xFF, 0xF4, 0x40,
  0x00, 0x00, 0x00, 0x00, 0xB7, 0xEF, 0xFF, 0xFE, 0xFF, 0xA0, 0x00, 0x00,
  0x00, 0x01, 0x1F, 0xEF, 0xFF, 0xFC, 0xFD, 0x10, 0x00, 0x00, 0x00, 0x01,
  0x1F, 0xF7, 0xFF, 0xFD, 0xFF, 0x10, 0x00, 0x00, 0x00, 0x03, 0x5F, 0xF3,
  0xFF, 0xF9, 0xFF, 0x48, 0x00, 0x00, 0x00, 0x02, 0x5F, 0xFB, 0xFF, 0xFB,
  0xFF, 0x48, 0x00, 0x00, 0x00, 0x04, 0x2F, 0xF9, 0xFF, 0xF3, 0xFE, 0xC4,
  0x00, 0x00, 0x00, 0x04, 0xBF, 0xFD, 0xFF, 0xF7, 0xFF, 0xA6, 0x00, 0x00,
  0x00, 0x09, 0xDF, 0xBE, 0xFF, 0xEF, 0xBF, 0xAA, 0x00, 0x00, 0x00, 0x09,
  0x7F, 0xBE, 0xFF, 0xEF, 0xBF, 0xF1, 0x00, 0x00, 0x00, 0x12, 0xFF, 0xBE,
  0x7F, 0xDF, 0xBF, 0xF1, 0x00, 0x00, 0x00, 0x15, 0xFF, 0xBF, 0xBF, 0xBF,
  0xBF, 0xED, 0x00, 0x00, 0x00, 0x26, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8,
  0x80, 0x00, 0x00, 0x27, 0xFF, 0xFF, 0xFF, 0xEF, 0xFF, 0xFC, 0x80, 0x00,
  0x00, 0x2F, 0xF8, 0x00, 0x00, 0x00, 0x01, 0xFF, 0x40, 0x00, 0x00, 0x3F,
  0xF0, 0x42, 0x10, 0x88, 0x21, 0xFF, 0x80, 0x00, 0x00, 0x7F, 0xE7, 0xFF,
  0xFF, 0xFF, 0xFC, 0xFF, 0xC0, 0x00, 0x00, 0x3F, 0xF7, 0xFF, 0xFF, 0xFF,
  0xFD, 0xFF, 0x80, 0x00, 0x00, 0x3F, 0xE7, 0xFF, 0xFF, 0xFF, 0xFC, 0xFF,
  0x80, 0x00, 0x00, 0x3F, 0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xFF, 0xC0, 0x00,
  0x00, 0x3F, 0xE7, 0xFF, 0xFF, 0xFF, 0xFC, 0xFF, 0x80, 0x00, 0x00, 0x3F,
  0xF7, 0xFF, 0xFF, 0xFF, 0xFD, 0xFF, 0x80, 0x00, 0x00, 0x3F, 0xF3, 0xFF,
  0xFF, 0xFF, 0xF9, 0xFF, 0x80, 0x00, 0x00, 0x1F, 0xF7, 0xFF, 0xFF, 0xFF,
  0xFD, 0xFF, 0x00, 0x00, 0x00, 0x07, 0xF3, 0xFF, 0xFF, 0xFF, 0xF9, 0xFA,
  0x00, 0x00, 0x00, 0x00, 0x17, 0xFF, 0xFF, 0xFF, 0xFD, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x13, 0xFF, 0xF5, 0xFF, 0xF9, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0B, 0xFF, 0xF0, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0xFF,
  0xE0, 0xFF, 0xF9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0xFF, 0xF5, 0xFF,
  0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0xFF, 0xFF, 0xFF, 0xFA, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x0B, 0xFF, 0xFF, 0xFF, 0xFA, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x0B, 0xFF, 0xFF, 0xFF, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x0B, 0xFF, 0xFF, 0xFF, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xFF,
  0xFF, 0xFF, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0xFF, 0xFF, 0xFF,
  0xF2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xFF, 0xFF, 0xFF, 0xFA, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x09, 0xFF, 0xFF, 0xFF, 0xF2, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x0B, 0xFF, 0xFF, 0xFF, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x09, 0xFF, 0xFF, 0xFF, 0xF2, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFC, 0x00,
  0x00, 0x00, 0x03, 0xFF, 0xFF, 0x00, 0x04, 0x00, 0x3F, 0xFF, 0xFF, 0xFF,
  0xFF, 0x80, 0x02, 0x00, 0x03, 0xFF, 0xDF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF,
  0xF8, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x40, 0x04, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void drawSprite(const uint8_t* bmp, int w, int h, int yTop) {
  display.drawBitmap(1, yTop, bmp, w, h, BLACK);
}

// ─────────────────────────────────────────────
//  DISPLAY PRIMITIVES
// ─────────────────────────────────────────────

void drawSep(int y) {
  display.fillRect(MARGIN_X, y, DISP_W - MARGIN_X * 2, 1, BLACK);
}

// Skill bar: label + horizontal fill bar scaled to MAX_SKILL_PTS
// Max possible skill = 3 (faction start) + 32 (one SP per level) = 35
// We display against that maximum so the bar feels meaningful at all stages.
#define MAX_SKILL_PTS   35   // absolute maximum any skill can reach
#define SKILL_BAR_W     62   // bar track width in px (fits 3 across 250px)
#define SKILL_BAR_H     6    // bar height in px
#define SKILL_LABEL_W   14   // px reserved for "B:" / "S:" / "F:" label

void drawSkillBar(int x, int y, int val) {
  val = constrain(val, 0, MAX_SKILL_PTS);
  int bx     = x + SKILL_LABEL_W;
  int filled = (val * (SKILL_BAR_W - 2)) / MAX_SKILL_PTS;

  // Track outline
  display.drawRect(bx, y, SKILL_BAR_W, SKILL_BAR_H, BLACK);

  // Fill
  if (filled > 0)
    display.fillRect(bx + 1, y + 1, filled, SKILL_BAR_H - 2, BLACK);


}

void drawXPBar(int xp, int maxXP) {
  display.drawRect(XP_BAR_X, XP_BAR_Y, XP_BAR_W, XP_BAR_H, BLACK);
  int f = (maxXP > 0) ? map(constrain(xp, 0, maxXP), 0, maxXP, 0, XP_BAR_W - 2) : 0;
  if (f > 0) display.fillRect(XP_BAR_X + 1, XP_BAR_Y + 1, f, XP_BAR_H - 2, BLACK);
}

void printAt(int x, int y, String t) { display.setCursor(x, y); display.print(t); }

void printRight(int rx, int y, String t) {
  display.setCursor(rx - (int)t.length() * FONT_W, y); display.print(t);
}

void printCenter(int y, String t) {
  int x = (DISP_W - (int)t.length() * FONT_W) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, y); display.print(t);
}

// drawFace() replaced by drawSprite() — see HACKER SPRITES above
// Kept as stub so mood system still compiles
void drawFace(const char* face) { /* unused — sprites used instead */ }

// Right-side speech bubble — fixed zone x=BUBBLE_X, y=FACE_Y, w=BUBBLE_W
// Tail is a small triangle pointing LEFT (toward character on the left)
// Lines array: pass up to 4 lines, terminate with nullptr
// Each line is printed in white on black, 8px font, FONT_H+3 apart
void drawBubbleRight(const char* l1,
                     const char* l2=nullptr,
                     const char* l3=nullptr,
                     const char* l4=nullptr) {
  const char* lines[4]={l1,l2,l3,l4};
  int nLines=0;
  for(int i=0;i<4;i++) if(lines[i]) nLines=i+1;

  int lineH  = FONT_H + 3;
  int boxH   = BUBBLE_PAD*2 + nLines*lineH - 3;
  int boxY   = FACE_Y + 4;   // slight top offset from sprite top
  int boxX   = BUBBLE_X;
  int boxW   = BUBBLE_W;

  // Box fill
  display.fillRect(boxX, boxY, boxW, boxH, BLACK);

  // Tail: three horizontal lines tapering left
  int tailY = boxY + boxH/2;
  display.fillRect(boxX-6, tailY-2, 6, 5, BLACK);  // stem
  display.fillRect(boxX-8, tailY-1, 2, 3, BLACK);  // taper 1
  display.fillRect(boxX-9, tailY,   1, 1, BLACK);  // tip

  // White text
  display.setTextColor(WHITE);
  for(int i=0;i<nLines;i++) {
    if(lines[i]) {
      display.setCursor(boxX + BUBBLE_PAD, boxY + BUBBLE_PAD + i*lineH);
      display.print(lines[i]);
    }
  }
  display.setTextColor(BLACK);
}

// Legacy single-line wrapper (used by mood system)
void drawBubble(const char* msg) {
  drawBubbleRight(msg);
}

// ─────────────────────────────────────────────
//  COMMON HEADER & FOOTER
// ─────────────────────────────────────────────

void drawHeader() {
  String lvlStr = (myLevel >= MAX_LEVEL) ? "LVL:MAX" : "LVL:" + String(myLevel);
  String left  = myName + " [" + myFaction.substring(0, 1) + "] " + lvlStr;
  String right = "BAT:" + String(getBatteryPercent()) + "%";
  printAt(MARGIN_X, MARGIN_Y, left);
  printRight(DISP_W - MARGIN_X, MARGIN_Y, right);
  drawSep(12);
}

void drawFooter() {
  drawSep(FOOTER_SEP_Y);
  String xpStr = "XP " + String(myXP) + "/" + String(xpForNextLevel());
  printAt(MARGIN_X, XP_TEXT_Y, xpStr);
  if (skillPoints > 0)
    printRight(DISP_W - MARGIN_X, XP_TEXT_Y, "SP:" + String(skillPoints) + "!");
  drawXPBar(myXP, xpForNextLevel());
  // Skill bars — three groups, each: label(14) + bar(62) + value(~12) + gap(8) = 96px
  // x positions: 3, 86, 169  (leaves ~81px each which fits comfortably)
  int barY = PIP_LABEL_Y;
  printAt(3,   barY, "B:");  drawSkillBar(3,   barY, skillBrute);
  printAt(86,  barY, "S:");  drawSkillBar(86,  barY, skillStealth);
  printAt(169, barY, "F:");  drawSkillBar(169, barY, skillFirewall);
}

// ─────────────────────────────────────────────
//  DISPLAY STATES
// ─────────────────────────────────────────────

// ── E-ink refresh budget (T4.6) ──────────────
//
//  A full refresh costs roughly two seconds and a visible flash. The device
//  used to redraw on the 10-minute mood timer whether or not anything had
//  actually changed, which is most of the time. Hashing the visible state and
//  skipping identical redraws removes those without touching the avatar
//  rendering itself, which the roadmap puts off limits.
uint32_t displayRefreshes = 0;
uint32_t displaySkipped   = 0;
uint32_t lastIdleSig      = 0xFFFFFFFF;

uint32_t idleSignature() {
  uint32_t h = 2166136261u;
  int vals[] = { myLevel, myXP / 10, skillBrute, skillStealth, skillFirewall,
                 cyMood, getBatteryPercent() / 5, knownCount, skillPoints };
  for (unsigned i = 0; i < sizeof(vals)/sizeof(vals[0]); i++) {
    h ^= (uint32_t)(vals[i] + 128);
    h *= 16777619u;
  }
  return h;
}

// Returns false when the idle screen would be pixel-identical to what is
// already on the panel.
bool idleNeedsRefresh() {
  uint32_t sig = idleSignature();
  if (sig == lastIdleSig) { displaySkipped++; return false; }
  lastIdleSig = sig;
  return true;
}

void displayIdle() {
  displayRefreshes++;
  display.clearMemory(); display.landscape(); drawHeader();
  driftMood();
  // Large sprite fills most of the content zone
  if (cyMood >= 0) drawSprite(spr_idle1, SPR_IDLE1_W, SPR_IDLE1_H, FACE_Y);
  else             drawSprite(spr_idle2, SPR_IDLE2_W, SPR_IDLE2_H, FACE_Y);
  // Only bubble — no status text on idle screen
  drawBubble(getIdleBubble());
  drawFooter(); display.update();
}

void displayScanning() {
  display.clearMemory(); display.landscape(); drawHeader();
  // Large focused sprite, one bubble line only
  drawSprite(spr_idle2, SPR_IDLE2_W, SPR_IDLE2_H, FACE_Y);
  drawBubble(getScanBubble());
  drawFooter(); display.update();
}

void displayTargetFound(String tid, int fw, int att, int pool) {
  display.clearMemory(); display.landscape(); drawHeader();
  drawSprite(spr_idle2, SPR_IDLE2_W, SPR_IDLE2_H, FACE_Y);
  { String nd="Node: "+tid; String od="P:"+String(pool)+" T:"+String(att);
  drawBubbleRight("TARGET LOCKED", nd.c_str(), od.c_str(),
                  cyMood>=0?"Let's crack it!":"Fine, lets go."); }
  drawFooter(); display.update();
}

void displayAttacking(String tid) {
  display.clearMemory(); display.landscape(); drawHeader();
  drawSprite(spr_idle2, SPR_IDLE2_W, SPR_IDLE2_H, FACE_Y);
  drawBubbleRight("BREACH ATTEMPT", "Breaking firewall...", "Go go go!");
  drawFooter(); display.update();
}

void displayHackSuccess(String tid, int xp, String note) {
  display.clearMemory(); display.landscape(); drawHeader();
  shiftMood(+2);
  drawSprite(spr_victory, SPR_VICTORY_W, SPR_VICTORY_H, FACE_Y);
  { String nd="Node "+tid+" owned."; String xs="XP +"+String(xp);
  drawBubbleRight("SYSTEM BREACHED", nd.c_str(), xs.c_str(), note.c_str()); }
  drawFooter(); display.update();
}

void displayHackFailed(String tid, int xp, String note) {
  display.clearMemory(); display.landscape(); drawHeader();
  shiftMood(-2);
  drawSprite(spr_lost, SPR_LOST_W, SPR_LOST_H, FACE_Y);
  { String nd="Node "+tid+" held."; String xs="XP -"+String(xp);
  drawBubbleRight("COUNTER-HACKED", nd.c_str(), xs.c_str(), note.c_str()); }
  drawFooter(); display.update();
}

void displayImmune(String tid) {
  display.clearMemory(); display.landscape(); drawHeader();
  drawSprite(spr_bored, SPR_BORED_W, SPR_BORED_H, FACE_Y);
  drawBubbleRight("TARGET IMMUNE", "Faction block.", "Can't touch this.");
  drawFooter(); display.update();
}

void displayIncomingMsg(String fromId, String msg) {
  display.clearMemory(); display.landscape(); drawHeader();
  drawSprite(spr_idle1, SPR_IDLE1_W, SPR_IDLE1_H, FACE_Y);
  uint32_t fid = (uint32_t)strtoul(fromId.c_str(), nullptr, 16);
  String senderName = nodeNameFromId(fid);
  String nameStr = "MSG: " + senderName;
  // Truncate message to fit bubble (max ~26 chars per line)
  String line1 = msg.length() > 26 ? msg.substring(0, 26) : msg;
  String line2 = msg.length() > 26 ? msg.substring(26, min((int)msg.length(), 52)) : "";
  if (line2.length() > 0)
    drawBubbleRight(nameStr.c_str(), line1.c_str(), line2.c_str());
  else
    drawBubbleRight(nameStr.c_str(), line1.c_str());
  drawFooter(); display.update();
}

void displayLevelUp() {
  display.clearMemory(); display.landscape();
  drawSep(10);
  printCenter(2, "*** LEVEL UP! ***");
  drawSep(12);
  shiftMood(+3);
  drawSprite(spr_victory, SPR_VICTORY_W, SPR_VICTORY_H, FACE_Y);
  { String ls="LVL "+String(myLevel)+" reached!";
  drawBubbleRight(ls.c_str(), "+1 SKILL POINT",
                  "192.168.4.1 to upgrade",
                  cyMood>=4?"LETS GO!!!":"Yes!! LVL up!"); }
  drawSep(FOOTER_SEP_Y);
  display.update();
}

// Shown while PRG is being held, so a factory reset is not silent.
void displayWiping() {
  displayRefreshes++;
  display.clearMemory(); display.landscape();
  printCenter(40, "FACTORY RESET");
  printCenter(56, "KEEP HOLDING TO WIPE");
  printCenter(72, "RELEASE TO CANCEL");
  display.update();
}

// Shown after a double RST tap, so the armed state is never invisible.
void displayArmed() {
  displayRefreshes++;
  display.clearMemory(); display.landscape();
  printCenter(34, "FACTORY RESET ARMED");
  printCenter(52, "HOLD PRG 5s TO WIPE");
  printCenter(70, "OR WAIT TO CANCEL");
  display.update();
}

void displaySetup() {
  display.clearMemory(); display.landscape();
  printCenter(MARGIN_Y, "CYPHER32 // SETUP REQUIRED");
  drawSep(12);
  drawSprite(spr_idle1, SPR_IDLE1_W, SPR_IDLE1_H, FACE_Y);
  drawBubbleRight("Who are you?", "1.WiFi:Cypher32_Setup",
                  "  Pass: cypher32", "2.Open 192.168.4.1");
  drawSep(88);
  display.update();
}

// ─────────────────────────────────────────────
//  WIFI BEACON
// ─────────────────────────────────────────────

void updateBeacon(bool busy) {
  // Open WiFi — no password, anyone can connect and open 192.168.4.1
  String ssid = (myName == "")
    ? "Cypher32"
    : "C32_" + myFaction.substring(0, 1) + "_" + myName;
  WiFi.softAP(ssid.c_str());  // no password = open network
}

// Read faction letter from SSID (C32_F_Name_ID)
String factionFromSSID(String ssid) {
  if (!ssid.startsWith("C32_") || ssid.length() < 6) return "NONE";
  char f = ssid.charAt(4);
  if (f == 'B') return "BLACK";
  if (f == 'W') return "WHITE";
  if (f == 'R') return "RED";
  if (f == 'G') return "GREEN";
  return "NONE";
}

// ─────────────────────────────────────────────
//  WEB PORTAL
// ─────────────────────────────────────────────

// ─────────────────────────────────────────────
//  PORTAL  (Phase 3)
// ─────────────────────────────────────────────
//
//  One page, served once. Everything after that is JSON:
//    GET  /api/state   — whole UI state, polled every 2 s
//    POST /api/action  — every mutation, password-gated
//    POST /api/setup    — first-run wizard (only while unconfigured)
//
//  The old portal regenerated ~20 KB of HTML inside a blocking request handler
//  on every click. Now a request moves a few hundred bytes, so the handler is
//  no longer a meaningful stall for the radio.

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PORTAL_HTML);
}

// Escape for embedding in a JSON string literal.
String jesc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if      (c == '"')  o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c < 0x20)  continue;
    else                o += c;
  }
  return o;
}

String factionName(char f) {
  if (f == 'B') return "BLACK";  if (f == 'W') return "WHITE";
  if (f == 'R') return "RED";    if (f == 'G') return "GREEN";
  return "?";
}

String buildStateJson() {
  bool configured = !(myName == "" || myFaction == "NONE");

  String j = "{";
  j += "\"configured\":" + String(configured ? "true" : "false") + ",";
  j += "\"name\":\""     + jesc(myName) + "\",";
  j += "\"faction\":\""  + jesc(myFaction) + "\",";
  j += "\"id\":\""       + chipIdStr(myChipID32) + "\",";
  j += "\"version\":\""  + String(FIRMWARE_VERSION) + "\",";
  j += "\"level\":"      + String(myLevel) + ",";
  j += "\"xp\":"         + String(myXP) + ",";
  j += "\"xpNext\":"     + String(xpForNextLevel()) + ",";
  j += "\"sp\":"         + String(skillPoints) + ",";
  j += "\"brute\":"      + String(skillBrute) + ",";
  j += "\"stealth\":"    + String(skillStealth) + ",";
  j += "\"firewall\":"   + String(skillFirewall) + ",";
  j += "\"battery\":"    + String(getBatteryPercent()) + ",";

  j += "\"lora\":{";
  j += "\"status\":\""   + jesc(loraStatus) + "\",";
  j += "\"ready\":"      + String(loraReady ? "true" : "false") + ",";
  j += "\"rssi\":"       + String(loraLastRSSI) + ",";
  j += "\"duty\":"       + String(dutyCyclePct(), 3);
  j += "},";

  j += "\"action\":{";
  j += "\"state\":\""    + String(loraActionText()) + "\",";
  j += "\"label\":\""    + jesc(loraActionLabel) + "\",";
  j += "\"tries\":"      + String(loraActionTries) + ",";
  j += "\"pending\":"    + String((loraActionPending() || hackInFlight) ? "true" : "false");
  j += "},";

  j += "\"nodes\":[";
  for (int i = 0; i < knownCount; i++) {
    KnownNode* n = &knownNodes[i];
    String nid = chipIdStr(n->chip_id);
    if (i) j += ",";
    j += "{\"id\":\""       + nid + "\",";
    j += "\"name\":\""      + jesc(nodeNameFromId(n->chip_id)) + "\",";
    j += "\"level\":"       + String(n->level) + ",";
    j += "\"faction\":\""   + String(n->faction) + "\",";
    j += "\"avgRssi\":"     + String(nodeAvgRssi(n)) + ",";
    j += "\"bars\":"        + String(nodeSignalBars(n)) + ",";
    j += "\"proximity\":\"" + String(nodeProximity(n)) + "\",";
    j += "\"status\":\""    + String(nodeStatusText(n)) + "\",";
    j += "\"ageMs\":"       + String(ageMs(n->last_seen_ms)) + ",";
    j += "\"recon\":"       + String(n->recon_count) + ",";
    j += "\"hackWon\":"     + String(recentlyHacked(nid) ? "true" : "false") + ",";
    j += "\"cooldownMs\":"  + String(hackCooldownLeft(nid)) + ",";
    j += "\"unread\":"      + String(n->msg_unread ? "true" : "false") + ",";
    j += "\"msg\":\""       + jesc(String(n->msg_inbox)) + "\"";
    j += "}";
  }
  j += "]}";
  return j;
}

void handleApiState() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buildStateJson());
}

static void apiFail(int code, const String& msg) {
  server.send(code, "application/json", "{\"err\":\"" + msg + "\"}");
}
static void apiOk(const String& msg) {
  server.send(200, "application/json", "{\"instant\":true,\"msg\":\"" + msg + "\"}");
}

// First-run only. Refuses once a character exists, so nobody on the open Wi-Fi
// can re-roll somebody else's device.
void handleApiSetup() {
  String f = server.arg("f"), p = server.arg("p");
  Serial.printf("[SETUP] args=%d faction='%s' pwlen=%u method=%d\n",
                server.args(), f.c_str(), (unsigned)p.length(), (int)server.method());

  if (!(myName == "" || myFaction == "NONE")) {
    Serial.println("[SETUP] refused — already configured");
    apiFail(409, "Already configured — wipe the device first"); return;
  }
  if (f != "BLACK" && f != "WHITE" && f != "RED" && f != "GREEN") {
    Serial.println("[SETUP] refused — faction missing or unrecognised");
    apiFail(400, "Faction missing — did the form reach the device?"); return;
  }
  if (p.length() < 6) {
    Serial.println("[SETUP] refused — password too short");
    apiFail(400, "Password too short"); return;
  }

  myFaction  = f;
  myPassword = p;
  myName     = generateName();
  if      (f == "BLACK") skillBrute    += 3;
  else if (f == "WHITE") skillFirewall += 3;
  else if (f == "RED")   skillStealth  += 3;
  else { skillBrute++; skillStealth++; skillFirewall++; }
  saveProgress();
  Serial.printf("[SETUP] configured as %s (%s) — rebooting\n",
                myName.c_str(), myFaction.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
  requestRestart(800);   // let the response flush before we drop the link
}

// Every mutation goes through here, and every one needs the password. The AP is
// open by design, so without this anyone within radio range could spend your
// skill points or wipe your character.
void handleApiAction() {
  if (myName == "" || myFaction == "NONE") { apiFail(409, "Not configured"); return; }
  if (server.arg("pw") != myPassword) {
    Serial.printf("[AUTH] rejected '%s' — sent %u chars, stored %u\n",
                  server.arg("a").c_str(),
                  (unsigned)server.arg("pw").length(), (unsigned)myPassword.length());
    apiFail(401, "Wrong password"); return;
  }

  String a = server.arg("a");

  if (a == "beacon")     { loraSendBeacon(); apiOk("Beacon sent"); return; }
  if (a == "clearnodes") { knownCount = 0; memset(knownNodes, 0, sizeof(knownNodes));
                           apiOk("Node list cleared"); return; }
  if (a == "reset")      { server.send(200, "application/json", "{\"instant\":true,\"msg\":\"Wiping\"}");
                           preferences.begin("cypher-v8", false); preferences.clear(); preferences.end();
                           requestRestart(800); return; }
  if (a == "setpw") {
    String np = server.arg("np");
    if (np.length() < 6) { apiFail(400, "Password too short"); return; }
    myPassword = np; saveProgress(); apiOk("Password updated"); return;
  }
  if (a == "skill") {
    if (skillPoints < 1) { apiFail(400, "No skill points"); return; }
    String s = server.arg("s");
    if      (s == "brute")    skillBrute++;
    else if (s == "stealth")  skillStealth++;
    else if (s == "firewall") skillFirewall++;
    else { apiFail(400, "Unknown skill"); return; }
    skillPoints--; saveProgress(); apiOk("Skill raised"); return;
  }

  // ── radio actions: fire and let the poll report the outcome (T3.5) ──
  if (!loraReady) { apiFail(503, "Radio offline"); return; }
  uint32_t target = (uint32_t)strtoul(server.arg("id").c_str(), nullptr, 16);
  if (target == 0) { apiFail(400, "Bad target"); return; }
  KnownNode* n = findNode(target);
  if (!n) { apiFail(404, "Node not in range"); return; }

  if (a == "recon") {
    if (n->recon_count >= 3)  { apiFail(400, "Recon already complete"); return; }
    if (loraActionPending())  { apiFail(429, "Another action in flight"); return; }
    loraSendRecon(target);
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (a == "hack") {
    String nid = chipIdStr(target);
    if (hackInFlight)         { apiFail(429, "A hack is already running"); return; }
    if (recentlyHacked(nid))  { apiFail(400, "Already owned — locked for 7 days"); return; }
    if (recentlyFailed(nid))  { apiFail(400, "Locked out — try again later"); return; }
    if (loraActionPending())  { apiFail(429, "Another action in flight"); return; }
    hackPendingId = nid;
    loraHackStart(target, n->recon_count);
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (a == "msg") {
    String txt = server.arg("txt");
    if (txt.length() == 0)   { apiFail(400, "Empty message"); return; }
    if (loraActionPending()) { apiFail(429, "Another action in flight"); return; }
    strncpy(n->msg_sent, txt.c_str(), 32); n->msg_sent[32] = '\0';
    loraSendMsg(target, txt.c_str());
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  apiFail(400, "Unknown action");
}

// T0.3 — machine-readable diagnostics.  curl 192.168.4.1/api/diag
void handleApiDiag() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", loraDiagJson());
}

// T3.7 — reliable no-op probe, reports round-trip time.
void handleApiPing() {
  if (!server.hasArg("id")) { apiFail(400, "id required"); return; }
  uint32_t target = (uint32_t)strtoul(server.arg("id").c_str(), nullptr, 16);
  if (!loraReady || target == 0) { apiFail(503, "Radio offline"); return; }
  unsigned long t0 = millis();
  loraSendPing(target);
  while (loraActionPending() && (uint32_t)(millis() - t0) < 3500) {
    loraTick(); delay(5); yield();
  }
  bool ok = (loraActionState == LA_SUCCESS);
  String j = "{\"ok\":" + String(ok ? "true" : "false") +
             ",\"rttMs\":" + String((uint32_t)(millis() - t0)) +
             ",\"tries\":" + String(loraActionTries) +
             ",\"rssi\":"  + String(loraLastRSSI) + "}";
  server.send(200, "application/json", j);
}

// ── Captive portal (T3.1) ────────────────────
//
//  Joining a Wi-Fi network with no internet makes phones quietly fall back to
//  cellular, and then "the website doesn't load". Answering the OS probe URLs
//  with a redirect is what makes the portal open by itself.
void handleCaptive() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

// Apply a verdict the defender sent us. Called from loop(), never from a
// request handler, so nothing here blocks the radio.
void resolveHackVerdict() {
  String   nid    = hackPendingId;
  uint32_t target = (uint32_t)strtoul(nid.c_str(), nullptr, 16);
  bool     won    = hackVerdictWon;
  int      enemyFW = hackVerdictFirewall;

  String enemyFaction = "NONE";
  if      (hackVerdictFaction=='B') enemyFaction="BLACK";
  else if (hackVerdictFaction=='W') enemyFaction="WHITE";
  else if (hackVerdictFaction=='R') enemyFaction="RED";
  else if (hackVerdictFaction=='G') enemyFaction="GREEN";

  HackResult result;
  if (won) {
    result = resolveHack(enemyFaction, enemyFW);
    result.success = true;
  } else {
    result.success = false;
    result.xpDelta = -(15 - min(10, skillFirewall*2));
    if (result.xpDelta > -5) result.xpDelta = -5;
    result.note = "Counter-hack detected.";
  }

  KnownNode* n = findNode(target);
  if (n) {
    n->hack_attempted = true;
    n->hack_won       = won;
    n->hack_time_ms   = millis();
  }

  // Confirm to the defender, carrying our XP delta. They already know the
  // outcome — they decided it — so this is a report, not the notification.
  if (loraReady) loraSendHackResult(target, won, (int8_t)constrain(result.xpDelta, -128, 127));

  bool lvlUp = false;
  if (won) {
    recordHack(nid);                       // 7-day lock
    displayHackSuccess(nid, result.xpDelta, result.note);
    lvlUp = applyXP(result.xpDelta);
  } else {
    recordFail(nid);                       // 12-hour retry cooldown (T4.4)
    displayHackFailed(nid, abs(result.xpDelta), result.note);
    applyXP(result.xpDelta);
  }

  saveProgress();
  if (lvlUp) { displayLevelUp(); revertIdleAtMs = millis() + 6000; }
  else                            revertIdleAtMs = millis() + 4000;
}

// ─────────────────────────────────────────────
//  ARDUINO SETUP
// ─────────────────────────────────────────────

// ─────────────────────────────────────────────
//  FACTORY RESET via PRG button
// ─────────────────────────────────────────────
//
//  Hold the PRG button (side of the device) for 5 seconds at any time.
//  The display counts down so you know it's working.
//  Release before 5s to cancel with no changes.

void wipeAndReboot() {
  Serial.println("[FACTORY RESET] wiping NVS");
  preferences.begin("cypher-v8", false);
  preferences.clear();
  preferences.end();
  delay(200);
  ESP.restart();
}

bool     resetArmed        = false;
uint32_t resetArmedAt      = 0;
bool     bootCounterClosed = false;

void initFactoryResetButton() {
  // GPIO0 = PRG/BTN-0 on the Wireless Paper schematic, active LOW.
  pinMode(PRG_PIN, INPUT_PULLUP);
#if RESET_BTN2_PIN >= 0
  pinMode(RESET_BTN2_PIN, INPUT_PULLUP);
#endif

  // Count this boot. The counter is zeroed once we have been up for
  // ARM_WINDOW_MS, so it only ever climbs when boots are being cut short by
  // someone tapping RST. Two in a row is a deliberate double tap.
  preferences.begin("cypher-v8", false);
  uint8_t taps = preferences.getUChar("rstc", 0);
  if (taps < 250) taps++;
  preferences.putUChar("rstc", taps);
  preferences.end();

  resetArmed   = (taps >= 2);
  resetArmedAt = millis();
  if (resetArmed) Serial.println("[FACTORY RESET] armed by double RST tap");
}

// Close the arming window: we have been up long enough that this was a normal
// boot, so the next RST tap starts counting from one again.
static void serviceBootCounter() {
  if (bootCounterClosed) return;
  if (millis() < ARM_WINDOW_MS) return;
  bootCounterClosed = true;
  preferences.begin("cypher-v8", false);
  preferences.putUChar("rstc", 0);
  preferences.end();
}

// Hold PRG for 5 seconds *while the device is running* to wipe everything.
//
// This used to be a boot-time check at the top of setup(), which could never
// work: GPIO0 held low during reset puts the ESP32-S3 into serial download
// mode, so the sketch does not run at all. It has to be polled at runtime,
// where GPIO0 is an ordinary input.
//
// This is also the only way back in if you forget the portal password, so it
// deliberately needs no authentication — physically holding the button on the
// device is the authentication.
void serviceFactoryResetButton() {
  static uint32_t heldSince = 0;
  static bool     warned    = false;

  serviceBootCounter();

  bool prgDown = (digitalRead(PRG_PIN) == LOW);

#if RESET_BTN2_PIN >= 0
  // Board has a genuine second readable button: require both held.
  bool combo = prgDown && (digitalRead(RESET_BTN2_PIN) == LOW);
#else
  // RST is not readable, so the double tap arms and PRG confirms.
  bool combo = resetArmed && prgDown;

  // Armed but nobody followed through — go back to normal.
  if (resetArmed && !prgDown &&
      (uint32_t)(millis() - resetArmedAt) > ARM_TIMEOUT_MS) {
    resetArmed  = false;
    lastIdleSig = 0xFFFFFFFF;
    Serial.println("[FACTORY RESET] disarmed — PRG not held in time");
    if (myName == "" || myFaction == "NONE") displaySetup();
    else                                     displayIdle();
  }
#endif

  if (combo) {
    if (heldSince == 0) { heldSince = millis(); warned = false; return; }

    // Confirm it is working, once, partway through. The e-ink update blocks
    // ~2 s; letting go during it simply cancels on the next poll.
    if (!warned && (uint32_t)(millis() - heldSince) > 1500) {
      warned = true;
      displayWiping();
    }
    if ((uint32_t)(millis() - heldSince) >= RESET_HOLD_MS) wipeAndReboot();
  } else {
    if (heldSince != 0 && warned) {   // released before the deadline
      lastIdleSig = 0xFFFFFFFF;
      displayIdle();
    }
    heldSince = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  esp_task_wdt_deinit();

  // Allocate WebServer here — its constructor can crash if run at global init
  serverPtr = new WebServer(80);

  // Set chip ID immediately so LoRa ISR filter works from first packet

  VextON();
  delay(200);  // power rail stabilisation

  // Display allocated AFTER VextON — global constructor crashes before Vext is on
  displayPtr = new EInkDisplay_WirelessPaperV1_2();
  analogReadResolution(12);
  display.landscape();

  initFactoryResetButton();
  loadProgress();

  // Seed the PRNG before the radio comes up. Beacon jitter, CAD backoff and
  // retry jitter all draw from it — if every board booted with the same seed
  // they would back off in lockstep, which is the collision problem (D2) all
  // over again. Chip ID differs per device, so the sequences diverge.
  randomSeed(myChipID32 ^ micros());

  loraSetup();

  WiFi.mode(WIFI_AP_STA);
  updateBeacon(false);

  server.on("/",            handleRoot);
  server.on("/api/state",   HTTP_GET,  handleApiState);
  server.on("/api/action",  HTTP_POST, handleApiAction);
  server.on("/api/setup",   HTTP_POST, handleApiSetup);
  server.on("/api/diag",    HTTP_GET,  handleApiDiag);
  server.on("/api/ping",    HTTP_GET,  handleApiPing);

  // The probe URLs each OS uses to decide whether a network has internet.
  // Answering with a redirect is what makes the portal pop up on its own.
  server.on("/generate_204",         handleCaptive);  // Android
  server.on("/gen_204",              handleCaptive);
  server.on("/hotspot-detect.html",  handleCaptive);  // iOS / macOS
  server.on("/library/test/success.html", handleCaptive);
  server.on("/ncsi.txt",             handleCaptive);  // Windows
  server.on("/connecttest.txt",      handleCaptive);
  server.on("/fwlink",               handleCaptive);
  server.on("/canonical.html",       handleCaptive);
  server.onNotFound(handleCaptive);
  server.begin();

  // Wildcard DNS: every lookup resolves to us, so any URL the phone tries
  // lands on the portal (T3.1).
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  if (MDNS.begin("cypher32")) MDNS.addService("http", "tcp", 80);

  // First beacon immediately; loraTick() follows up at ~3 s and ~8 s, then
  // settles into the adaptive cadence (T1.7 / T2.4).
  if (myName != "" && myFaction != "NONE") {
    loraBeaconEnabled = true;
    loraSendBeacon();
  }

  if      (resetArmed)                          displayArmed();
  else if (myName == "" || myFaction == "NONE") displaySetup();
  else                                          displayIdle();

}

// ─────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  loraTick();
  serviceFactoryResetButton();   // before the early return: must work even
                                 // on an unconfigured or locked-out device

  if (restartPending && (int32_t)(millis() - restartAtMs) >= 0) {
    Serial.println("[SYS] restarting");
    delay(50);
    ESP.restart();
  }

  if (myName == "" || myFaction == "NONE") return;

  // Deferred return-to-idle. Replaces the old delay(5000), which held the
  // radio deaf for five seconds every time a message arrived.
  if (revertIdleAtMs && (int32_t)(millis() - revertIdleAtMs) >= 0) {
    revertIdleAtMs = 0;
    lastIdleSig = 0xFFFFFFFF;   // panel is showing an event screen — force redraw
    displayIdle();
  }

  // Show incoming message — only update display when something arrived
  if (pendingMsg.length() > 0) {
    String msg  = pendingMsg;
    String from = pendingMsgFrom;
    pendingMsg = ""; pendingMsgFrom = "";
    displayIncomingMsg(from, msg);
    revertIdleAtMs = millis() + 5000;
  }

  // Our hack came back with the defender's verdict (T4.3).
  if (hackVerdictReady) {
    hackVerdictReady = false;
    resolveHackVerdict();
  }

  // ...or the target never answered. Say so rather than inventing a result.
  if (hackTimedOut) {
    hackTimedOut = false;
    shiftMood(-1);
    displayHackFailed(hackPendingId, 0, "No response. Out of range?");
    revertIdleAtMs = millis() + 4000;
  }

  // Someone hacked US. The alert is raised when their HACK_REQ arrives and we
  // roll the outcome ourselves, so an attacker cannot suppress it by simply
  // never sending a HACK_RESULT.
  if (pendingHackAlert) {
    pendingHackAlert = false;
    String who = pendingHackFrom;
    if (pendingHackAttackerWon) {
      shiftMood(-1);
      displayHackFailed(who, 0, "Breached by " + nodeNameFromId(
                          (uint32_t)strtoul(who.c_str(), nullptr, 16)));
    } else {
      shiftMood(+1);
      displayHackSuccess(who, 0, "Firewall held.");
    }
    revertIdleAtMs = millis() + 5000;
  }

  // Beaconing is scheduled inside loraTick() now (T1.7 boot burst + T2.4
  // adaptive cadence), so there is nothing to do here.

  // Mood drift every 10 minutes. Only actually redraw if the visible state
  // changed — a full e-ink refresh costs ~2 s and a flash (T4.6).
  static unsigned long lastMood = 0;
  if ((uint32_t)(millis() - lastMood) > 600000UL) {
    lastMood = millis();
    driftMood();
    if (idleNeedsRefresh()) displayIdle();
  }

  // Prune stale hack records
  static unsigned long lastPrune = 0;
  if (millis() - lastPrune > 300000UL) {
    lastPrune = millis();
    pruneHackedList();
  }
}
