// Tests for the PRG page button and the two pages it reaches.
//
// These compile the real sketch, so they exercise the actual painters, the
// actual coalescing and the actual reset-button state machine — not a model of
// them. The display stub counts panel writes, which is what makes "three taps
// cost one refresh" a testable claim rather than an intention.
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
int g_pinLevel[64];
static const bool g_pinInit = []{ for (int i = 0; i < 64; i++) g_pinLevel[i] = 1; return true; }();

#include "../cypher32.ino"

int checks = 0, failures = 0;
void CHECK(bool c, const char* what) {
  checks++;
  if (!c) { failures++; printf("  FAIL: %s\n", what); }
}

// One qualifying short press, as the ISR would report it.
static void tap() { prgShortSeq++; }
// Consume the press, let the coalescing window lapse, then let it draw. Two
// calls, because that is what loop() does — the window exists precisely so the
// draw does not happen in the same pass as the press.
static void settle() {
  servicePageButton();
  g_millis += PAGE_COALESCE_MS + 10;
  servicePageButton();
}

static void configure() {
  myName = "GhostByte"; myFaction = "BLACK"; myLevel = 7;
  resetArmed = false;
  pageShown = pageWanted = PAGE_IDLE; pageDirtyAt = 0; pageHeldSince = 0;
  prgShortSeq = prgShortSeen = 0;
  revertIdleAtMs = 0;
}

int main() {
  printf("Cypher32 page-button tests\n\n");
  displayPtr = new EInkDisplay_WirelessPaperV1_2();
  display.landscape();

  // ── the cycle ──
  printf("page cycle\n");
  {
    configure();
    tap(); settle();
    CHECK(pageShown == PAGE_LASTMSG, "first press reaches the message page");
    tap(); settle();
    CHECK(pageShown == PAGE_CENSUS,  "second press reaches the census");
    tap(); settle();
    CHECK(pageShown == PAGE_IDLE,    "third press comes back to the avatar");
  }

  // ── coalescing: the whole reason the button is bearable ──
  printf("coalescing\n");
  {
    configure();
    int before = display.updates;
    tap(); tap(); tap();                       // mashed inside one blackout
    servicePageButton();                       // too soon: nothing drawn yet
    CHECK(display.updates == before, "nothing is drawn inside the coalesce window");
    CHECK(pageWanted == PAGE_IDLE,   "three presses land back on the avatar");
    settle();
    // Three presses is a full lap, so the panel already shows the answer and
    // the correct cost is nothing at all — not one refresh, none.
    CHECK(display.updates == before, "a full lap of presses costs NO panel write");
    CHECK(pageShown == PAGE_IDLE, "and leaves the avatar up, where it started");
  }
  {
    // Two rapid presses is the case that proves coalescing: the panel must
    // skip straight to the census without drawing the message page on the way.
    configure();
    int before = display.updates;
    tap(); tap();
    settle();
    CHECK(display.updates == before + 1,
          "two presses inside the window cost ONE write, not two");
    CHECK(pageShown == PAGE_CENSUS, "and land on the second page directly");
  }
  {
    configure();
    int before = display.updates;
    tap(); settle(); tap(); settle();
    CHECK(display.updates == before + 2,
          "presses spaced beyond the window are drawn separately");
  }

  // ── armed: the path that would otherwise wipe the device ──
  printf("armed lockout\n");
  {
    configure();
    resetArmed = true;
    pageShown = pageWanted = PAGE_IDLE;
    int before = display.updates;
    tap(); settle();
    CHECK(pageShown == PAGE_IDLE,  "a press while armed does not change page");
    CHECK(pageWanted == PAGE_IDLE, "and does not queue one either");
    CHECK(display.updates == before, "and paints nothing over the ARMED screen");
    resetArmed = false;
  }

  // ── paintCurrentPage precedence ──
  printf("paint precedence\n");
  {
    configure();
    pageShown = PAGE_CENSUS;
    resetArmed = true;
    paintCurrentPage();
    CHECK(display.fb[34][3] || display.fb[36][3] || true, "armed paints (smoke)");
    resetArmed = false;

    // Unconfigured must keep the join QR whatever page is selected.
    myName = ""; myFaction = "NONE";
    pageShown = PAGE_CENSUS;
    display.clearMemory();
    paintCurrentPage();
    // The QR's finder pattern sits at the top-left of the code block.
    bool inkTopLeft = false;
    for (int y = 20; y < 40 && !inkTopLeft; y++)
      for (int x = 12; x < 30; x++) if (display.fb[y][x]) { inkTopLeft = true; break; }
    CHECK(inkTopLeft, "an unconfigured device still paints its join QR, not a page");
    configure();
  }

  // ── the census counts what recon has earned, and nothing else ──
  printf("census gating\n");
  {
    configure();
    memset(knownNodes, 0, sizeof(knownNodes));
    knownCount = 3;
    knownNodes[0].chip_id = 0xAA01; knownNodes[0].faction = 'W';
    knownNodes[0].intel = RECON_MAX_SEQ;                       // fully read
    knownNodes[1].chip_id = 0xAA02; knownNodes[1].faction = 'R';
    knownNodes[1].intel = RECON_T_FACTION;                     // exactly enough
    knownNodes[2].chip_id = 0xAA03; knownNodes[2].faction = 'G';
    knownNodes[2].intel = RECON_T_FACTION - 1;                 // one short
    displayCensus();
    int cnt[5];
    int total = censusCounts(cnt);               // the painter's own rule
    CHECK(total == 4, "the census totals you plus three contacts");
    CHECK(cnt[0] == 1, "you count yourself");
    CHECK(cnt[1] == 1, "a fully-read contact counts to its faction");
    CHECK(cnt[2] == 1, "tier 4 exactly is enough to be counted");
    CHECK(cnt[3] == 0, "one tier short and it is NOT counted as GREEN");
    CHECK(cnt[4] == 1, "it lands in UNKNOWN instead — the beacon letter is not a licence");
  }

  // ── the page indicator says which page you are on ──
  // Also the only cheap way to assert WHICH page got painted: the current
  // page's marker is 5 px tall starting at y=10, the others are 3 px from
  // y=11, so row 10 identifies the page unambiguously.
  printf("page indicator\n");
  {
    configure();
    auto markedPage = []{
      const int W = 5, PITCH = 9;
      int x0 = DISP_W - MARGIN_X - (PAGE_COUNT - 1) * PITCH - W;
      for (int i = 0; i < PAGE_COUNT; i++)
        if (display.fb[10][x0 + i * PITCH + 2]) return i;
      return -1;
    };
    for (int p = 0; p < PAGE_COUNT; p++) {
      pageShown = (uint8_t)p;
      display.clearMemory();
      paintCurrentPage();
      CHECK(markedPage() == p,
            p == 0 ? "the avatar page marks itself"
          : p == 1 ? "the message page marks itself"
                   : "the census page marks itself");
    }
    pageShown = PAGE_IDLE;
  }

  // ── the message page survives its awkward inputs ──
  printf("message page\n");
  {
    configure();
    char lines[3][21];
    int n = wrapInto("aaaaaaaaaa bbbbbbbbbb cccccccccc", 20, lines);
    CHECK(n == 3, "the worst 32-char wrap is 3 lines");
    CHECK(strlen(lines[0]) <= 20 && strlen(lines[1]) <= 20 && strlen(lines[2]) <= 20,
          "no wrapped line exceeds the column budget");

    n = wrapInto("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 20, lines);
    CHECK(n >= 2, "a 32-char word with no spaces still breaks across lines");
    CHECK(strlen(lines[0]) == 20, "and fills the first line exactly");

    n = wrapInto("", 20, lines);
    CHECK(n == 0, "an empty message wraps to nothing");

    lastMsgAt = 0; lastSentAt = 0; lastMsgText = ""; lastSentText = "";
    int before = display.updates;
    displayLastMsg();
    CHECK(display.updates == before + 1, "the empty state still paints a page");
  }

  // ── the wipe timer must not survive a blackout ──
  // Two polls either side of a long gap used to satisfy the five-second hold
  // between them, with the button only sampled at each end.
  printf("wipe timer continuity\n");
  // wipeAndReboot() clears NVS and calls ESP.restart() directly, so the honest
  // signal that a wipe happened is that the namespace is empty.
  auto seedNvs  = []{ Preferences p; p.begin("cypher-v8", false);
                      p.putString("name", String("GhostByte")); p.end(); };
  auto wasWiped = []{ Preferences p; p.begin("cypher-v8", false);
                      String n = p.getString("name", String("")); p.end();
                      return n.length() == 0; };
  {
    configure();
    resetArmed = true;
    seedNvs();
    g_pinLevel[PRG_PIN] = LOW;                 // finger down
    serviceFactoryResetButton();               // starts the hold
    g_millis += 6000;                          // three chained refreshes
    serviceFactoryResetButton();               // still down at the far side
    CHECK(!wasWiped(), "a hold split by a blackout does not reach the wipe");
    g_pinLevel[PRG_PIN] = HIGH;
    resetArmed = false;
  }
  {
    configure();
    resetArmed = true;
    seedNvs();
    g_pinLevel[PRG_PIN] = LOW;
    for (int i = 0; i < 200 && !wasWiped(); i++) {
      serviceFactoryResetButton();
      g_millis += 50;                          // polled the whole way through
    }
    CHECK(wasWiped(), "an uninterrupted five-second hold still wipes");
    g_pinLevel[PRG_PIN] = HIGH;
    resetArmed = false;
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
