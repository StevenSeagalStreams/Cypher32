#include <heltec-eink-modules.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
#include "cypher32_packets.h"
#include "cypher32_lora.h"

// Heltec Wireless Paper V1.2 — 250x122px landscape
// Pointer: constructor must NOT run at global init time (before Vext is on)
EInkDisplay_WirelessPaperV1_2* displayPtr = nullptr;
#define display (*displayPtr)

Preferences preferences;
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

// 7 days in milliseconds
#define WEEK_MS      604800000UL
#define HALF_DAY_MS   43200000UL   // 12 hours hack retry cooldown

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

// ── Hacked list helpers ────────────────────────────────────────────────────
// Format: "id:timestampMs,id:timestampMs,"

// Check if we hacked this ID within the last 7 days
bool recentlyHacked(String id) {
  int start = hackedList.indexOf(id + ":");
  if (start == -1) return false;
  int colon = start + id.length() + 1;
  int comma = hackedList.indexOf(",", colon);
  if (comma == -1) return false;
  unsigned long hackTime = hackedList.substring(colon, comma).toInt();
  return (nowMs() - hackTime) < WEEK_MS;
}

// Record a hack (or update existing entry with new timestamp)
void recordHack(String id) {
  // Remove old entry for this id if present
  int start = hackedList.indexOf(id + ":");
  if (start != -1) {
    int comma = hackedList.indexOf(",", start);
    if (comma != -1) hackedList.remove(start, comma - start + 1);
  }
  hackedList += id + ":" + String(nowMs()) + ",";
}

// Clean up entries older than 7 days to keep string short
void pruneHackedList() {
  String fresh = "";
  String tmp = hackedList;
  while (tmp.indexOf(',') != -1) {
    String entry = tmp.substring(0, tmp.indexOf(','));
    tmp = tmp.substring(tmp.indexOf(',') + 1);
    int colon = entry.indexOf(':');
    if (colon == -1) continue;
    String eid  = entry.substring(0, colon);
    unsigned long t = entry.substring(colon + 1).toInt();
    if (nowMs() - t < WEEK_MS) fresh += entry + ",";
  }
  hackedList = fresh;
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

void displayIdle() {
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

void handleRoot() {
  String html = "<html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:'Courier New',monospace;background:#000;color:#0f0;margin:0;padding:0;max-width:480px;margin:auto}"
    ".nav{display:flex;border-bottom:1px solid #0a3a0a;overflow-x:auto;scrollbar-width:none;position:sticky;top:0;background:#000;z-index:9}"
    ".nav::-webkit-scrollbar{display:none}"
    ".tab{flex:0 0 auto;padding:10px 14px;font-size:12px;color:#0a8c0a;cursor:pointer;border-bottom:2px solid transparent;text-decoration:none}"
    ".tab.a{color:#0f0;border-bottom-color:#0f0}"
    ".pg{display:none;padding:12px}"
    ".pg.a{display:block}"
    ".card{border:1px solid #0a3a0a;border-radius:5px;padding:12px;margin-bottom:10px;background:#030a03}"
    ".ct{font-size:10px;color:#0a8c0a;letter-spacing:.05em;margin-bottom:8px}"
    ".row{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}"
    ".lbl{font-size:11px;color:#0a8c0a}"
    ".val{font-size:12px;color:#0f0}"
    ".big{font-size:20px;font-weight:bold}"
    ".btn{display:block;width:100%;padding:10px;background:#0f0;color:#000;font-family:'Courier New',monospace;"
         "font-size:12px;font-weight:bold;border:none;border-radius:4px;cursor:pointer;text-align:center;margin-top:8px;box-sizing:border-box}"
    ".btn.ol{background:transparent;color:#0f0;border:1px solid #0f0}"
    ".btn.dn{background:transparent;color:#f05050;border:1px solid #f05050}"
    ".btn.sm{padding:5px 10px;font-size:11px;width:auto;display:inline-block}"
    "input,select,textarea{width:100%;padding:8px;background:#000;color:#0f0;border:1px solid #0a3a0a;"
                          "border-radius:4px;font-family:'Courier New',monospace;font-size:12px;box-sizing:border-box;margin-top:4px}"
    "textarea{resize:none;height:56px}"
    ".bar{height:5px;background:#0a1a0a;border:1px solid #0a3a0a;border-radius:2px}"
    ".bf{height:100%;background:#0f0;border-radius:2px}"
    ".badge{font-size:10px;padding:1px 6px;border-radius:3px;border:1px solid}"
    ".br{color:#f05050;border-color:#f05050}.bw{color:#a0d0ff;border-color:#a0d0ff}"
    ".bb{color:#5080f0;border-color:#5080f0}.bg{color:#50d050;border-color:#50d050}"
    ".hint{font-size:11px;opacity:.65;margin-top:3px}"
    ".dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#0f0;animation:pulse 2s infinite}"
    ".dot.off{background:#333;animation:none}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}"
    ".nr{display:flex;align-items:flex-start;justify-content:space-between;padding:8px 0;border-bottom:1px solid #041004}"
    ".nr:last-child{border-bottom:none}"
    ".mr{padding:7px 0;border-bottom:1px solid #041004}"
    ".mr:last-child{border-bottom:none}"
    ".mf{font-size:10px;color:#0a8c0a;margin-bottom:2px}"
    ".mt{font-size:12px;color:#0f0}"
    ".me{float:right;font-size:10px;color:#063006}"
    ".grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:10px}"
    ".mc{text-align:center;padding:8px}"
    "</style></head><body>";

  // ── Nav bar always visible ────────────────────────────────────────────────
  if (myName == "" || myFaction == "NONE") {
    // SETUP PAGE
    html += "<div style='padding:16px'>"
            "<h2>CYPHER32 // INITIALIZE</h2>"
            "<div class='card'>"
            "<div class='ct'>SELECT YOUR FACTION</div>"
            "<form action='/setup' method='get'>"
            "Faction:<br>"
            "<select name='f' onchange='showDesc(this.value)'>"
            "<option value='BLACK'>Black Hat  (+3 Brute Force)</option>"
            "<option value='WHITE'>White Hat  (+3 Firewall)</option>"
            "<option value='RED'>Red Hat    (+3 Stealth)</option>"
            "<option value='GREEN'>Green Hat  (+1 each)</option>"
            "</select>"
            "<div class='hint' id='fd' style='margin:6px 0'></div>"
            "Password (min 6 chars):<br>"
            "<input type='password' name='p' id='pw1' minlength='6' required style='margin-bottom:6px'>"
            "<br>Confirm password:<br>"
            "<input type='password' id='pw2' minlength='6' required style='margin-bottom:6px'>"
            "<div class='hint' id='pwe' style='color:#f66'></div>"
            "<input type='submit' class='btn' value='ESTABLISH UPLINK' onclick=\""
            "var a=document.getElementById(\'pw1\').value,"
            "b=document.getElementById(\'pw2\').value;"
            "if(a.length<6){document.getElementById(\'pwe\').textContent=\'Min 6 chars!\';return false;}"
            "if(a!==b){document.getElementById(\'pwe\').textContent=\'Passwords do not match!\';return false;}"
            "return true;\">"
            "</form></div>"
            "<script>"
            "var d={'BLACK':'Offensive. Attacks anyone. +20% XP on success.',"
            "'WHITE':'Defensive. Only attacks Black+Red. Earns XP when defenders fail.',"
            "'RED':'Stealth specialist. +25% XP vs Green.',"
            "'GREEN':'Balanced. +10% XP vs Black.'};"
            "function showDesc(v){document.getElementById('fd').textContent=d[v]||'';}"
            "showDesc('BLACK');"
            "</script></div>";
    html += "</body></html>";
    server.send(200, "text/html", html);
    return;
  }

  // ── Tabbed main UI ────────────────────────────────────────────────────────
  // Faction badge class
  String fcls = "bb";
  if (myFaction=="BLACK") fcls="bb";
  else if (myFaction=="WHITE") fcls="bw";
  else if (myFaction=="RED")   fcls="br";
  else if (myFaction=="GREEN") fcls="bg";

  html += "<div class='nav'>"
          "<a class='tab a' href='#' onclick='show(\"hud\",this)'>HUD</a>"
          "<a class='tab'   href='#' onclick='show(\"nodes\",this)'>Nodes</a>"
          "<a class='tab'   href='#' onclick='show(\"skills\",this)'>Skills</a>"
          "<a class='tab'   href='#' onclick='show(\"msgs\",this)'>Messages</a>"
          "<a class='tab'   href='#' onclick='show(\"cfg\",this)'>Settings</a>"
          "</div>";

  // ── HUD PAGE ─────────────────────────────────────────────────────────────
  html += "<div class='pg a' id='hud'>";

  // Identity card
  html += "<div class='card'>"
          "<div style='display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:10px'>"
          "<div><div style='font-size:20px;font-weight:bold'>" + myName + "</div>"
          "<div style='margin-top:4px'><span class='badge " + fcls + "'>" + myFaction + "</span>"
          " <span class='lbl'>ID: " + myUniqueID + "</span></div></div>"
          "<div style='text-align:right'><div class='big'>" + String(myLevel) + "</div>"
          "<div class='lbl'>LEVEL</div></div></div>";

  // XP bar
  int xpPct = constrain(myXP * 100 / xpForNextLevel(), 0, 100);
  html += "<div class='lbl' style='margin-bottom:3px'>XP " + String(myXP) + " / " + String(xpForNextLevel()) + "</div>"
          "<div class='bar'><div class='bf' style='width:" + String(xpPct) + "%'></div></div>"
          "</div>";

  // Stats grid
  int hackCount = 0;
  { String tmp = hackedList; while(tmp.indexOf(',')!=-1){hackCount++;tmp=tmp.substring(tmp.indexOf(',')+1);} }
  html += "<div class='grid3'>"
          "<div class='card mc'><div class='lbl'>Hacked</div><div class='big' style='font-size:18px'>" + String(hackCount) + "</div></div>"
          "<div class='card mc'><div class='lbl'>Battery</div><div class='big' style='font-size:18px'>" + String(getBatteryPercent()) + "%</div></div>"
          "<div class='card mc'><div class='lbl'>SP</div><div class='big' style='font-size:18px;" + (skillPoints>0?"color:#ff0":"") + "'>" + String(skillPoints) + (skillPoints>0?"!":"") + "</div></div>"
          "</div>";

  // Skills mini
  auto sbar = [](String name, int v) {
    int pct = v * 100 / 35;
    return "<div class='row'><span class='lbl'>" + name + "</span><span class='val'>" + String(v) + "</span></div>"
           "<div class='bar' style='margin-bottom:8px'><div class='bf' style='width:" + String(pct) + "%'></div></div>";
  };
  html += "<div class='card'><div class='ct'>SKILLS</div>"
          + sbar("Brute Force", skillBrute)
          + sbar("Stealth", skillStealth)
          + sbar("Firewall", skillFirewall)
          + "</div>";

  // LoRa diagnostics card
  String dotCls = loraReady ? "dot" : "dot off";
  html += "<div class='card'><div class='ct'>LORA</div>"
          "<div class='row'><span class='lbl'>Status</span>"
          "<span class='val'><span class='" + dotCls + "'></span> " + loraStatus + "</span></div>";
  if (!loraReady && loraInitError != 0)
    html += "<div class='row'><span class='lbl'>Init error</span><span class='val' style='color:#f66'>" + String(loraInitError) + "</span></div>";
  html += "<div class='row'><span class='lbl'>Signal</span><span class='val'>" + loraGetSignal() + "</span></div>"
          "<div class='row'><span class='lbl'>Known nodes</span><span class='val'>" + String(knownCount) + "</span></div>"
          "<div class='row'><span class='lbl'>Beacons sent</span><span class='val'>" + String(loraBeaconsSent) + "</span></div>"
          "<div class='row'><span class='lbl'>Pkts TX / RX</span><span class='val'>" + String(loraPktSent) + " / " + String(loraPktRecv) + "</span></div>"
          "<form action='/beacon' method='get'>"
          "<button type='submit' class='btn ol' style='margin-top:6px'>Send beacon now</button>"
          "</form></div>";

  html += "</div>"; // end HUD

  // ── NODES PAGE ────────────────────────────────────────────────────────────
  html += "<div class='pg' id='nodes'>";

  // Header bar: count + clear button
  html += "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px'>"
          "<span class='lbl'>" + String(knownCount) + " node(s) via LoRa beacon</span>"
          "<form action='/clearnodes' method='get'>"
          "<button type='submit' class='btn ol sm' style='font-size:10px;padding:3px 8px'>Clear list</button>"
          "</form></div>";

  if (knownCount == 0) {
    html += "<div class='card'><div class='lbl'>No nodes discovered yet.</div>"
            "<div class='hint'>Nodes appear when other Cypher32 devices send a LoRa beacon. "
            "Press &ldquo;Send beacon now&rdquo; on HUD to speed up discovery.</div></div>";
  }

  for (int i = 0; i < knownCount; i++) {
    KnownNode& nd = knownNodes[i];
    String nid  = chipIdStr(nd.chip_id);
    String name = nodeNameFromId(nd.chip_id);
    bool locked = recentlyHacked(nid);
    bool hackDone = nd.hack_attempted;

    // Faction badge
    String fBadge = "";
    char fc = nd.faction;
    if      (fc=='B') fBadge="<span class='badge bb'>BLACK</span>";
    else if (fc=='W') fBadge="<span class='badge bw'>WHITE</span>";
    else if (fc=='R') fBadge="<span class='badge br'>RED</span>";
    else if (fc=='G') fBadge="<span class='badge bg'>GREEN</span>";
    else              fBadge="<span class='badge' style='color:#0a3a0a;border-color:#0a3a0a'>UNKNOWN</span>";

    // Last seen
    unsigned long ago = nd.last_seen_ms ? (millis()-nd.last_seen_ms)/1000UL : 9999;
    String seenStr = ago<60 ? String(ago)+"s ago" : ago<3600 ? String(ago/60)+"m ago" : "1h+ ago";

    // Card style
    String cardStyle = locked
      ? "border:1px solid #1a0a0a;border-radius:5px;padding:12px;margin-bottom:10px;background:#0a0303;"
      : "border:1px solid #0a3a0a;border-radius:5px;padding:12px;margin-bottom:10px;background:#030a03;";

    html += "<div style='" + cardStyle + "'>";

    // Header: name + faction + status chip
    html += "<div style='display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:6px'>"
            "<div><div style='font-size:15px;font-weight:bold;color:" + String(locked?"#0a3a0a":"#0f0") + "'>"
            + name + "</div>"
            "<div style='font-size:11px;color:#0a8c0a;margin-top:2px'>ID: " + nid + " &middot; LVL " + String(nd.level)
            + " &middot; " + seenStr + "</div></div>"
            "<div>" + fBadge + "</div></div>";

    if (locked) {
      // Show cooldown
      int idx = hackedList.indexOf(nid + ":");
      String coolStr = "7d 0h";
      if (idx != -1) {
        int col = idx+nid.length()+1;
        int com = hackedList.indexOf(",",col);
        if (com!=-1) {
          unsigned long hackT=(unsigned long)hackedList.substring(col,com).toInt();
          unsigned long elapsed=nowMs()-hackT;
          if (elapsed<WEEK_MS) {
            unsigned long msLeft=WEEK_MS-elapsed;
            int hLeft=(int)(msLeft/3600000UL);
            coolStr=String(hLeft/24)+"d "+String(hLeft%24)+"h";
          }
        }
      }
      html += "<div style='font-size:11px;color:#3a1a0a;margin-top:4px'>"
              "Locked &mdash; re-hackable in " + coolStr + "</div>";
    } else {
      // Show recon results if any
      if (nd.recon_count > 0) {
        html += "<div style='background:#041a04;border:1px solid #0a3a0a;border-radius:4px;"
                "padding:7px;margin:7px 0;font-size:11px'>"
                "<div style='color:#0a8c0a;margin-bottom:4px'>RECON INTEL</div>";
        for (int r=0; r<nd.recon_count; r++) {
          html += "<div>" + statTypeName(nd.recon_types[r])
                + ": <span style='color:#0f0;font-weight:bold'>" + String(nd.recon_values[r]) + "</span></div>";
        }
        if (nd.recon_count < 3)
          html += "<div style='color:#063006;margin-top:3px'>" + String(3-nd.recon_count) + " recon attempt(s) remaining</div>";
        else
          html += "<div style='color:#063006;margin-top:3px'>All stats revealed</div>";
        html += "</div>";
      }

      // Hack result + 12h cooldown
      bool hackOnCooldown = hackDone && nd.hack_time_ms > 0 &&
                            (millis() - nd.hack_time_ms < HALF_DAY_MS);
      bool hackCanRetry   = hackDone && !hackOnCooldown && !nd.hack_won;
      if (hackCanRetry) { nd.hack_attempted = false; nd.hack_won = false; }

      if (hackDone && !hackCanRetry) {
        if (nd.hack_won) {
          html += "<div style='background:#041a04;border:1px solid #0f0;border-radius:4px;"
                  "padding:7px;font-size:11px;color:#0f0;margin-bottom:7px'>"
                  "HACK SUCCEEDED &mdash; node owned.</div>";
        } else if (hackOnCooldown) {
          unsigned long msLeft = HALF_DAY_MS - (millis() - nd.hack_time_ms);
          int hLeft = (int)(msLeft / 3600000UL);
          int mLeft = (int)((msLeft % 3600000UL) / 60000UL);
          html += "<div style='background:#1a0404;border:1px solid #f05050;border-radius:4px;"
                  "padding:7px;font-size:11px;color:#f05050;margin-bottom:7px'>"
                  "HACK FAILED &mdash; retry in " + String(hLeft) + "h " + String(mLeft) + "m</div>";
        }
      }

      // Action buttons
      html += "<div style='display:flex;gap:6px;margin-top:6px'>";

      if (nd.recon_count < 2) {
        String statOptions = "";
        bool hasB=false, hasS=false, hasFW=false;
        for(int r=0;r<nd.recon_count;r++){
          if(nd.recon_types[r]==STAT_BRUTE)    hasB=true;
          if(nd.recon_types[r]==STAT_STEALTH)  hasS=true;
          if(nd.recon_types[r]==STAT_FIREWALL) hasFW=true;
        }
        html += "<form action='/recon' method='get' style='display:flex;gap:4px;flex-wrap:wrap'>"
                "<input type='hidden' name='id' value='" + nid + "'>"
                "<select name='stat' class='btn ol sm' style='padding:5px;font-size:11px;width:auto'>";
        if(!hasB)  html += "<option value='1'>Brute Force</option>";
        if(!hasS)  html += "<option value='2'>Stealth</option>";
        if(!hasFW) html += "<option value='3'>Firewall</option>";
        html += "</select>"
                "<button type='submit' class='btn ol sm'>Recon (" + String(2-nd.recon_count) + " left)</button>"
                "</form>";
      } else {
        html += "<button class='btn ol sm' disabled style='opacity:.4'>Recon (done)</button>";
      }

      bool canHack = !hackDone || hackCanRetry;
      if (canHack) {
        html += "<form action='/hack' method='get'>"
                "<input type='hidden' name='id' value='" + nid + "'>"
                "<button type='submit' class='btn sm' "
                "onclick='return confirm(\"One attempt only. Sure?\")'>"
                "Hack (1 attempt)</button></form>";
      } else {
        String lockLbl = nd.hack_won ? "Owned" : "Cooldown";
        html += "<button class='btn sm' disabled style='opacity:.4'>" + lockLbl + "</button>";
      }
      html += "</div>";
    }
    html += "</div>"; // end node card
  }
  html += "</div>"; // end nodes page

  // ── SKILLS PAGE ───────────────────────────────────────────────────────────
  html += "<div class='pg' id='skills'>";

  if (skillPoints > 0) {
    html += "<div class='card' style='text-align:center;padding:16px'>"
            "<div class='lbl'>Skill points available</div>"
            "<div style='font-size:32px;font-weight:bold;color:#ff0;margin:8px 0'>" + String(skillPoints) + "</div>"
            "<div class='lbl'>Spend wisely &mdash; max level is 32</div>"
            "</div>";

    struct { const char* name; const char* key; const char* desc; int val; } skills[3] = {
      {"Brute Force", "br", "Shrinks hack pool — easier to crack codes", skillBrute},
      {"Stealth",     "st", "+1 attempt per point — more guesses per hack", skillStealth},
      {"Firewall",    "fi", "Reduces XP loss when counter-hacked",   skillFirewall},
    };
    for (int i = 0; i < 3; i++) {
      int pct = skills[i].val * 100 / 35;
      html += "<div class='card'>"
              "<div style='display:flex;justify-content:space-between;align-items:baseline;margin-bottom:6px'>"
              "<span style='font-size:13px;font-weight:bold'>" + String(skills[i].name) + "</span>"
              "<span class='lbl'>" + String(skills[i].val) + " / 35</span></div>"
              "<div class='bar' style='margin-bottom:8px'><div class='bf' style='width:" + String(pct) + "%'></div></div>"
              "<div class='hint' style='margin-bottom:8px'>" + String(skills[i].desc) + "</div>"
              "<form action='/add' method='get'>"
              "<input type='hidden' name='s' value='" + String(skills[i].key) + "'>"
              "<button type='submit' class='btn" + String(i==0?" ":" ol") + "'>+ Upgrade " + String(skills[i].name) + "</button>"
              "</form></div>";
    }
  } else if (myLevel >= MAX_LEVEL) {
    html += "<div class='card' style='text-align:center'><div class='big' style='font-size:16px'>MAX LEVEL</div>"
            "<div class='hint' style='margin-top:6px'>LVL 32 reached. Respect.</div></div>";
  } else {
    // Show skills read-only when no SP available
    struct { const char* n; int v; const char* d; } sk[3] = {
      {"Brute Force", skillBrute,    "Shrinks hack pool"},
      {"Stealth",     skillStealth,  "+1 attempt per point"},
      {"Firewall",    skillFirewall, "Reduces XP loss"},
    };
    for (int i=0;i<3;i++) {
      int pct = sk[i].v * 100 / 35;
      html += "<div class='card'>"
              "<div style='display:flex;justify-content:space-between;margin-bottom:4px'>"
              "<span style='font-size:13px;font-weight:bold'>" + String(sk[i].n) + "</span>"
              "<span class='lbl'>" + String(sk[i].v) + " / 35</span></div>"
              "<div class='bar' style='margin-bottom:5px'><div class='bf' style='width:" + String(pct) + "%'></div></div>"
              "<div class='hint'>" + String(sk[i].d) + "</div>"
              "<button class='btn ol' disabled style='margin-top:8px;opacity:.35'>No SP available</button>"
              "</div>";
    }
  }
  html += "</div>"; // end skills

  // ── MESSAGES PAGE ─────────────────────────────────────────────────────────
  html += "<div class='pg' id='msgs'>";

  // Send form
  html += "<div class='card'><div class='ct'>SEND MESSAGE</div>"
          "<form action='/msg' method='get'>"
          "To:<br><select name='id'>";
  if (knownCount == 0) html += "<option value=''>No nodes known yet</option>";
  for (int i = 0; i < knownCount; i++) {
    String nn = nodeNameFromId(knownNodes[i].chip_id);
    String ni = chipIdStr(knownNodes[i].chip_id);
    html += "<option value='" + ni + "'>" + nn + " — LVL " + String(knownNodes[i].level) + "</option>";
  }
  html += "</select>"
          "Message <span class='hint'>(max 32 chars)</span>:<br>"
          "<textarea name='txt' maxlength='32' id='mt' onkeyup='document.getElementById(&quot;mc&quot;).textContent=this.value.length+&quot;/32&quot;'></textarea>"
          "<div style='display:flex;justify-content:space-between;align-items:center;margin-top:4px'>"
          "<span class='lbl' id='mc'>0/32</span>"
          "<button type='submit' class='btn ol sm'" + String(knownCount==0?" disabled":"") + ">Send via LoRa</button>"
          "</div></form></div>";

  // Sent messages
  html += "<div class='card'><div class='ct'>SENT</div>";
  bool hasSent = false;
  for (int i = 0; i < knownCount; i++) if (knownNodes[i].msg_sent[0]) hasSent = true;
  if (!hasSent) {
    html += "<div class='lbl'>No messages sent yet.</div>";
  } else {
    for (int i = 0; i < knownCount; i++) {
      if (!knownNodes[i].msg_sent[0]) continue;
      String rName = nodeNameFromId(knownNodes[i].chip_id);
      html += "<div class='mr'>"
              "<div class='mf' style='color:#0a8c0a'>To: " + rName + "</div>"
              "<div class='mt'>" + String(knownNodes[i].msg_sent) + "</div>"
              "</div>";
    }
  }
  html += "</div>";

  // Inbox
  bool hasMessages = false;
  for (int i = 0; i < knownCount; i++) if (knownNodes[i].msg_inbox[0]) hasMessages = true;

  html += "<div class='card'><div class='ct'>INBOX</div>";
  if (!hasMessages) {
    html += "<div class='lbl'>No messages received yet.</div>";
  } else {
    for (int i = 0; i < knownCount; i++) {
      if (!knownNodes[i].msg_inbox[0]) continue;
      String nid = chipIdStr(knownNodes[i].chip_id);
      String senderName = nodeNameFromId(knownNodes[i].chip_id);
      html += "<div class='mr'>"
              "<div class='mf'>" + senderName + " (" + nid + ")</div>"
              "<div class='mt'>" + String(knownNodes[i].msg_inbox) + "</div>"
              "</div>";
    }
  }
  html += "</div></div>"; // end msgs

  // ── SETTINGS PAGE ─────────────────────────────────────────────────────────
  html += "<div class='pg' id='cfg'>";
  html += "<div class='card'><div class='ct'>DEVICE</div>"
          "<div class='row'><span class='lbl'>Codename</span><span class='val'>" + myName + "</span></div>"
          "<div class='row'><span class='lbl'>Chip ID</span><span class='val'>" + myUniqueID + "</span></div>"
          "<div class='row'><span class='lbl'>Faction</span><span class='val'><span class='badge " + fcls + "'>" + myFaction + "</span></span></div>"
          "</div>"
          "<div class='card'><div class='ct'>CHANGE PASSWORD</div>"
          "<form action='/setpw' method='get'>"
          "<input type='password' name='p' id='np1' minlength='6' placeholder='New password (min 6)' style='margin-bottom:6px'>"
          "<br><input type='password' id='np2' minlength='6' placeholder='Confirm password' style='margin-bottom:6px'>"
          "<div class='hint' id='pwe2' style='color:#f66'></div>"
          "<button type='submit' class='btn ol' onclick=\"var a=document.getElementById(\'np1\').value,"
          "b=document.getElementById(\'np2\').value;"
          "if(a.length<6){document.getElementById(\'pwe2\').textContent=\'Min 6 chars!\';return false;}"
          "if(a!==b){document.getElementById(\'pwe2\').textContent=\'Passwords do not match!\';return false;}"
          "return true;\">"
          "Update password</button></form></div>";

  html += "<div class='card'><div class='ct'>CHANGE PASSWORD</div>"
          "<form action='/setpw' method='get'>"
          "New password:<br><input type='password' id='p1' name='p' minlength='6'>"
          "<br>Confirm:<br><input type='password' id='p2' minlength='6'>"
          "<div class='hint' id='pwm' style='color:#f66'></div>"
          "<button type='submit' class='btn ol' onclick=\""
          "var a=document.getElementById(\'p1\').value,b=document.getElementById(\'p2\').value;"
          "if(a!==b){document.getElementById(\'pwm\').textContent=\'Passwords do not match!\';event.preventDefault();}"
          "else if(a.length<8){document.getElementById(\'pwm\').textContent=\'Min 6 chars!\';event.preventDefault();}"
          "\">Update password</button></form></div>";

  html += "<div class='card'><div class='ct'>LORA</div>"
          "<div class='row'><span class='lbl'>Status</span><span class='val'><span class='" + dotCls + "'></span> " + loraStatus + "</span></div>"
          "<div class='row'><span class='lbl'>Signal</span><span class='val'>" + loraGetSignal() + "</span></div>"
          "<div class='row'><span class='lbl'>Frequency</span><span class='val'>868 MHz</span></div>"
          "<div class='row'><span class='lbl'>Spreading</span><span class='val'>SF9</span></div>"
          "</div>";

  html += "<div class='card'>"
          "<div class='ct'>DANGER ZONE</div>"
          "<form action='/reset' method='get' onsubmit='return confirm(\"Wipe all progress?\")'>"
          "<button type='submit' class='btn dn'>Factory reset — wipe all data</button>"
          "</form></div>";
  html += "</div>"; // end settings

  // ── Tab switching JS ──────────────────────────────────────────────────────
  html += "<script>"
          "function show(id,el){"
          "document.querySelectorAll('.pg').forEach(function(p){p.classList.remove('a')});"
          "document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('a')});"
          "document.getElementById(id).classList.add('a');"
          "if(el)el.classList.add('a');"
          "return false;}"
          "</script>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}


void handleSetup() {
  // Guard: ignore if already configured (prevents rename via direct URL)
  if (myName != "" && myFaction != "NONE") {
    server.sendHeader("Location", "/"); server.send(303); return;
  }
  if (server.hasArg("f") && server.hasArg("p") && server.arg("p").length() >= 6) {
    myName     = generateName();
    myFaction  = server.arg("f");
    myPassword = server.arg("p");
    if      (myFaction == "BLACK") { skillBrute    = 3; }
    else if (myFaction == "WHITE") { skillFirewall = 3; }
    else if (myFaction == "RED")   { skillStealth  = 3; }
    else if (myFaction == "GREEN") { skillBrute = 1; skillStealth = 1; skillFirewall = 1; }
    saveProgress(); ESP.restart();
  }
  server.sendHeader("Location", "/"); server.send(303);
}

void handleAddSkill() {
  if (skillPoints > 0) {
    String s = server.arg("s");
    if      (s == "st") skillStealth++;
    else if (s == "br") skillBrute++;
    else if (s == "fi") skillFirewall++;
    skillPoints--; saveProgress();
  }
  server.sendHeader("Location", "/#skills"); server.send(303);
}

void handleReset() {
  preferences.begin("cypher-v8", false); preferences.clear(); preferences.end(); ESP.restart();
}

// Manual beacon — fires immediately from web UI
void handleBeacon() {
  loraSendBeacon();
  server.sendHeader("Location", "/#hud"); server.send(303);
}

// T0.3 — machine-readable diagnostics.  curl 192.168.4.1/api/diag
void handleApiDiag() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", loraDiagJson());
}

// T3.7 groundwork — reliable no-op probe against one node.
void handleApiPing() {
  if (!server.hasArg("id")) { server.send(400, "application/json", "{\"error\":\"id required\"}"); return; }
  uint32_t target = (uint32_t)strtoul(server.arg("id").c_str(), nullptr, 16);
  unsigned long t0 = millis();
  loraSendPing(target);
  // Pump until ACK or the reliable layer gives up (4 tries ≈ 2.8 s worst case).
  while (loraActionPending() && (uint32_t)(millis() - t0) < 3500) {
    loraTick(); delay(5); yield();
  }
  bool ok = (loraActionState == LA_SUCCESS);
  String j = "{\"ok\":" + String(ok ? "true" : "false") +
             ",\"rttMs\":" + String((uint32_t)(millis() - t0)) +
             ",\"tries\":" + String(loraActionTries) +
             ",\"rssi\":"  + String(loraLastRSSI) + "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

// Clear all known nodes from RAM (no flash wipe — they repopulate from beacons)
void handleClearNodes() {
  knownCount = 0;
  memset(knownNodes, 0, sizeof(knownNodes));
  server.sendHeader("Location", "/#nodes"); server.send(303);
}

// Change password handler
void handleSetPw() {
  if (server.hasArg("p")) {
    String pw = server.arg("p");
    if (pw.length() >= 6) {
      myPassword = pw;
      saveProgress();
    }
  }
  server.sendHeader("Location", "/#cfg"); server.send(303);
}

// Recon — send LoRa recon request; cap at 3 per node
void handleRecon() {
  if (!server.hasArg("id")) { server.sendHeader("Location","/#nodes"); server.send(303); return; }
  String hexId = server.arg("id");
  uint32_t target = (uint32_t)strtoul(hexId.c_str(), nullptr, 16);
  KnownNode* n = findOrAddNode(target);
  if (n && n->recon_count < 3 && loraReady) {
    int prevCount = n->recon_count;
    loraSendRecon(target);

    // Bounded pump, not a blocking wait. With deferred replies (60–120 ms) and
    // the ACK layer, a round trip lands in ~250 ms; 1.5 s covers the first
    // retry too. The old code waited 5 s with no retry underneath it, so it
    // usually just timed out slowly.
    // Goes away entirely at T3.2/T3.5 when the portal polls /api/state.
    unsigned long t0 = millis();
    while ((uint32_t)(millis() - t0) < 1500 && n->recon_count == prevCount) {
      loraTick();
      delay(5);
      yield();
    }
  }
  server.sendHeader("Location", "/#nodes"); server.send(303);
}

// Hack — one attempt, resolves locally using known recon data
void handleHack() {
  if (!server.hasArg("id")) { server.sendHeader("Location","/#nodes"); server.send(303); return; }
  String hexId = server.arg("id");
  String nid   = hexId;   // already the 4-char hex string from web
  uint32_t target = (uint32_t)strtoul(hexId.c_str(), nullptr, 16);

  KnownNode* n = findOrAddNode(target);
  if (!n || n->hack_attempted || recentlyHacked(nid)) {
    server.sendHeader("Location", "/#nodes"); server.send(303); return;
  }

  // Determine enemy faction from node record
  String enemyFaction = "NONE";
  if      (n->faction=='B') enemyFaction="BLACK";
  else if (n->faction=='W') enemyFaction="WHITE";
  else if (n->faction=='R') enemyFaction="RED";
  else if (n->faction=='G') enemyFaction="GREEN";

  // Get enemy firewall — use recon data if available, else random
  int enemyFW = 4;  // default estimate
  for (int i=0; i<n->recon_count; i++) {
    if (n->recon_types[i]==STAT_FIREWALL) { enemyFW=n->recon_values[i]; break; }
  }
  if (n->recon_count == 0) enemyFW = random(1,9);  // unknown — gamble

  // Hack success chance:
  //   Base: 60%
  //   +5% per recon completed (max +15% for 3 recons)
  //   Brute vs enemy FW: each point difference = +/-2%
  //   Clamped 25%-90%
  int pct = 60;
  pct += n->recon_count * 5;
  pct += (skillBrute - enemyFW) * 2;
  pct = constrain(pct, 25, 90);
  bool won = (random(0, 100) < pct);

  // Resolve XP
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

  // Record
  n->hack_attempted = true;
  n->hack_won       = won;
  n->hack_time_ms   = millis();
  bool lvlUp = false;

  // Tell the defender. Until v54 this packet was never transmitted by anyone —
  // loraSendHackResult() existed but had no callers, so the target of a hack
  // had no idea it had happened. Reliable send: retried and ACKed.
  if (loraReady) loraSendHackResult(target, won, (int8_t)constrain(result.xpDelta, -128, 127));

  if (won) {
    recordHack(nid);
    displayHackSuccess(nid, result.xpDelta, result.note);
    lvlUp = applyXP(result.xpDelta);
  } else {
    displayHackFailed(nid, abs(result.xpDelta), result.note);
    applyXP(result.xpDelta);
  }

  saveProgress();

  // Non-blocking hold. The old delay(4000)/delay(5000) pair kept the radio
  // deaf for up to nine seconds right after a hack — exactly when the
  // defender's ACK and any retry were in flight.
  if (lvlUp) { displayLevelUp(); revertIdleAtMs = millis() + 6000; }
  else                            revertIdleAtMs = millis() + 4000;

  server.sendHeader("Location", "/#nodes"); server.send(303);
}

// Send message handler
void handleMsg() {
  if (!server.hasArg("id") || !server.hasArg("txt")) {
    server.sendHeader("Location", "/#msgs"); server.send(303); return;
  }
  String hexId = server.arg("id");
  String txt   = server.arg("txt");
  if (txt.length() == 0) {
    server.sendHeader("Location", "/#msgs"); server.send(303); return;
  }
  uint32_t target = (uint32_t)strtoul(hexId.c_str(), nullptr, 16);
  // Store in outbox regardless of LoRa status
  KnownNode* n = findOrAddNode(target);
  if (n) {
    strncpy(n->msg_sent, txt.c_str(), 32);
    n->msg_sent[32] = '\0';
  }
  // Send over LoRa if available
  if (loraReady) loraSendMsg(target, txt.c_str());
  server.sendHeader("Location", "/#msgs"); server.send(303);
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

void checkFactoryReset() {
  // GPIO0 = PRG/BTN-0 on Wireless Paper schematic.
  // Active LOW (pulled to GND when pressed). Internal pull-up enabled.
  // We deliberately avoid ALL display calls here — each display.update()
  // blocks for ~2s and would eat the entire 5s window invisibly.
  // Instead: hold 5s → silent wipe → reboot. No display feedback needed.

  pinMode(PRG_PIN, INPUT_PULLUP);
  delay(50);  // let pin settle after boot

  if (digitalRead(PRG_PIN) == HIGH) return;  // not pressed

  // Sample button continuously for RESET_HOLD_MS.
  // If it goes HIGH at any point before the deadline → cancel.
  unsigned long pressStart = millis();
  while (millis() - pressStart < RESET_HOLD_MS) {
    if (digitalRead(PRG_PIN) == HIGH) return;  // released — abort
    delay(20);
  }

  // Still held after 5s — perform factory reset
  preferences.begin("cypher-v8", false);
  preferences.clear();
  preferences.end();
  delay(100);
  ESP.restart();
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

  checkFactoryReset();
  loadProgress();

  // Seed the PRNG before the radio comes up. Beacon jitter, CAD backoff and
  // retry jitter all draw from it — if every board booted with the same seed
  // they would back off in lockstep, which is the collision problem (D2) all
  // over again. Chip ID differs per device, so the sequences diverge.
  randomSeed(myChipID32 ^ micros());

  loraSetup();

  WiFi.mode(WIFI_AP_STA);
  updateBeacon(false);

  server.on("/",         handleRoot);
  server.on("/setup",    handleSetup);
  server.on("/add",      handleAddSkill);
  server.on("/reset",    handleReset);
  server.on("/recon",    handleRecon);
  server.on("/hack",     handleHack);
  server.on("/msg",      handleMsg);
  server.on("/setpw",    handleSetPw);
  server.on("/beacon",   handleBeacon);
  server.on("/clearnodes",handleClearNodes);
  server.on("/api/diag", handleApiDiag);
  server.on("/api/ping", handleApiPing);
  server.begin();

  // First beacon immediately; loraTick() follows up at ~3 s and ~8 s, then
  // settles into the adaptive cadence (T1.7 / T2.4).
  if (myName != "" && myFaction != "NONE") {
    loraBeaconEnabled = true;
    loraSendBeacon();
  }

  if (myName == "" || myFaction == "NONE") displaySetup();
  else                                     displayIdle();

}

// ─────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────

void loop() {
  server.handleClient();
  loraTick();

  if (myName == "" || myFaction == "NONE") return;

  // Deferred return-to-idle. Replaces the old delay(5000), which held the
  // radio deaf for five seconds every time a message arrived.
  if (revertIdleAtMs && (int32_t)(millis() - revertIdleAtMs) >= 0) {
    revertIdleAtMs = 0;
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

  // Someone hacked US. Before v54 the defender was never told — HACK_RESULT
  // existed in the protocol but nothing ever transmitted it.
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

  // Mood drift + display refresh every 10 minutes
  // Long interval so display.update() (~2s) rarely blocks the loop
  static unsigned long lastMood = 0;
  if (millis() - lastMood > 600000UL) {
    lastMood = millis();
    driftMood();
    displayIdle();
  }

  // Prune stale hack records
  static unsigned long lastPrune = 0;
  if (millis() - lastPrune > 300000UL) {
    lastPrune = millis();
    pruneHackedList();
  }
}
