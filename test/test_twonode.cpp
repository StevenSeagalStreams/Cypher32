// Two-node end-to-end simulation of the v54 link layer.
// Runs two independent instances of the firmware state over a shared lossy
// half-duplex channel and measures whether game actions actually complete.
//
// This does not replace the T0.4 hardware bench test — it validates protocol
// interop and the retry logic under loss, which is what D1/D3 broke.
#include <Arduino.h>
#include <cstdio>
#include <vector>

uint32_t   g_millis   = 1000;
uint32_t   g_rngState = 999;
SerialStub Serial;
ESPStub    ESP;

uint32_t myChipID32 = 0;
String   myFaction  = "BLACK";
int      myLevel    = 1;
int      skillBrute = 10, skillStealth = 4, skillFirewall = 6;

#include "../cypher32_lora.h"

// ── snapshot of one device's entire link state ──
struct NodeCtx {
  TxFrame    txq_[TXQ_SIZE];
  SeenEntry  seen_[SEEN_RING_SIZE];
  int        seenIdx_;
  PendingTx  pendingU_, pendingR_;
  KnownNode  nodes_[MAX_KNOWN_NODES];
  int        knownCount_;
  uint8_t    txSeq_;
  RadioState rs_;
  uint32_t   txStart_;
  bool       dio_;
  uint32_t   chipId_;
  char       fac_[8];          // plain POD — this struct gets bulk-zeroed
  int        lvl_, brute_, stealth_, fw_;
  int        acksRecv_, acksSent_, timeouts_, retries_, dups_, pktSent_, pktRecv_, cad_;
  LoraActionState action_;
  // hack state (T4.3) — per device, must not bleed between the two instances
  bool       hackAlert_, hackAlertWon_, hackInFlight_, hackVerdictReady_;
  bool       hackVerdictWon_, hackTimedOut_, hackVerdictLate_;
  uint32_t   hackTarget_, hackGrace_;
  ReconLedgerEntry ledger_[RECON_LEDGER_SIZE];
  uint8_t    ledgerIdx_;
  uint8_t    hackVerdictFw_;
  char        hackFrom_[16];
  std::vector<uint8_t> rxbuf_;
  size_t     sentSeen_;      // how many of radio.sent we've already drained
};

void save(NodeCtx& c) {
  memcpy(c.txq_, txq, sizeof(txq));
  memcpy(c.seen_, seenRing, sizeof(seenRing)); c.seenIdx_ = seenIdx;
  c.pendingU_ = pendingUser; c.pendingR_ = pendingReply;
  memcpy(c.nodes_, knownNodes, sizeof(knownNodes)); c.knownCount_ = knownCount;
  c.txSeq_ = txSeq; c.rs_ = radioState; c.txStart_ = txStartMs; c.dio_ = loraDioFlag;
  c.chipId_ = myChipID32;
  strncpy(c.fac_, myFaction.c_str(), sizeof(c.fac_) - 1); c.fac_[sizeof(c.fac_) - 1] = '\0';
  c.lvl_ = myLevel; c.brute_ = skillBrute; c.stealth_ = skillStealth; c.fw_ = skillFirewall;
  c.acksRecv_ = loraAcksRecv; c.acksSent_ = loraAcksSent; c.timeouts_ = loraTimeouts;
  c.retries_ = loraRetries;   c.dups_ = loraDupsDropped;
  c.pktSent_ = loraPktSent;   c.pktRecv_ = loraPktRecv; c.cad_ = loraCadBusy;
  c.action_ = loraActionState;
  c.hackAlert_ = pendingHackAlert; c.hackAlertWon_ = pendingHackAttackerWon;
  c.hackInFlight_ = hackInFlight;  c.hackVerdictReady_ = hackVerdictReady;
  c.hackVerdictWon_ = hackVerdictWon; c.hackTimedOut_ = hackTimedOut;
  c.hackVerdictLate_ = hackVerdictLate; c.hackGrace_ = hackGraceUntil;
  memcpy(c.ledger_, reconLedger, sizeof(reconLedger)); c.ledgerIdx_ = reconLedgerIdx;
  c.hackTarget_ = hackTargetId;    c.hackVerdictFw_ = hackVerdictFirewall;
  strncpy(c.hackFrom_, pendingHackFrom.c_str(), sizeof(c.hackFrom_) - 1);
  c.hackFrom_[sizeof(c.hackFrom_) - 1] = '\0';
  c.rxbuf_ = radio.rxBuf;
}

void load(NodeCtx& c) {
  memcpy(txq, c.txq_, sizeof(txq));
  memcpy(seenRing, c.seen_, sizeof(seenRing)); seenIdx = c.seenIdx_;
  pendingUser = c.pendingU_; pendingReply = c.pendingR_;
  memcpy(knownNodes, c.nodes_, sizeof(knownNodes)); knownCount = c.knownCount_;
  txSeq = c.txSeq_; radioState = c.rs_; txStartMs = c.txStart_; loraDioFlag = c.dio_;
  myChipID32 = c.chipId_; myFaction = String(c.fac_);
  myLevel = c.lvl_; skillBrute = c.brute_; skillStealth = c.stealth_; skillFirewall = c.fw_;
  loraAcksRecv = c.acksRecv_; loraAcksSent = c.acksSent_; loraTimeouts = c.timeouts_;
  loraRetries = c.retries_;   loraDupsDropped = c.dups_;
  loraPktSent = c.pktSent_;   loraPktRecv = c.pktRecv_; loraCadBusy = c.cad_;
  loraActionState = c.action_;
  pendingHackAlert = c.hackAlert_; pendingHackAttackerWon = c.hackAlertWon_;
  hackInFlight = c.hackInFlight_;  hackVerdictReady = c.hackVerdictReady_;
  hackVerdictWon = c.hackVerdictWon_; hackTimedOut = c.hackTimedOut_;
  hackVerdictLate = c.hackVerdictLate_; hackGraceUntil = c.hackGrace_;
  memcpy(reconLedger, c.ledger_, sizeof(reconLedger)); reconLedgerIdx = c.ledgerIdx_;
  hackTargetId = c.hackTarget_;    hackVerdictFirewall = c.hackVerdictFw_;
  pendingHackFrom = String(c.hackFrom_);
  radio.rxBuf = c.rxbuf_;
  radio.sent.clear();
  loraReady = true;
}

void initCtx(NodeCtx& c, uint32_t id, const char* fac, int brute, int fw) {
  // Zero the POD members individually — NodeCtx holds a std::vector, so a
  // blanket memset over the whole struct would corrupt it.
  memset(c.txq_,   0, sizeof(c.txq_));
  memset(c.seen_,  0, sizeof(c.seen_));   c.seenIdx_    = 0;
  memset(&c.pendingU_, 0, sizeof(c.pendingU_));
  memset(&c.pendingR_, 0, sizeof(c.pendingR_));
  memset(c.nodes_, 0, sizeof(c.nodes_));  c.knownCount_ = 0;
  c.txSeq_ = 0; c.rs_ = RS_RX; c.txStart_ = 0; c.dio_ = false;
  c.chipId_ = id;
  strncpy(c.fac_, fac, sizeof(c.fac_) - 1); c.fac_[sizeof(c.fac_) - 1] = '\0';
  c.lvl_ = 5; c.brute_ = brute; c.stealth_ = 4; c.fw_ = fw;
  c.acksRecv_ = c.acksSent_ = c.timeouts_ = c.retries_ = 0;
  c.dups_ = c.pktSent_ = c.pktRecv_ = c.cad_ = 0;
  c.action_ = LA_IDLE;
  c.hackAlert_ = c.hackAlertWon_ = c.hackInFlight_ = false;
  c.hackVerdictReady_ = c.hackVerdictWon_ = c.hackTimedOut_ = false;
  c.hackVerdictLate_ = false; c.hackGrace_ = 0;
  memset(c.ledger_, 0, sizeof(c.ledger_)); c.ledgerIdx_ = 0;
  c.hackTarget_ = 0; c.hackVerdictFw_ = 0; c.hackFrom_[0] = '\0';
  c.rxbuf_.clear();
  c.sentSeen_ = 0;
}

// ── the air ──
struct InFlight { std::vector<uint8_t> data; uint32_t arriveMs; int dest; };
std::vector<InFlight> air;

int      lossPercent = 0;
uint32_t lossRng     = 4242;
bool     dropped() {
  lossRng = lossRng * 1103515245u + 12345u;
  return (int)((lossRng >> 16) % 100) < lossPercent;
}

int framesOffered = 0, framesDropped = 0;

// Step one device forward by `step` ms.
void stepNode(NodeCtx& me, int myIndex, int peerIndex, uint32_t step) {
  load(me);

  // Half duplex: a device transmitting cannot hear anything.
  if (radioState == RS_TX) {
    if ((uint32_t)(millis() - txStartMs) >= 50) loraDioFlag = true;   // TxDone
  } else {
    for (size_t i = 0; i < air.size(); i++) {
      if (air[i].dest != myIndex) continue;
      if ((int32_t)(millis() - air[i].arriveMs) < 0) continue;
      radio.rxBuf = air[i].data;
      loraDioFlag = true;
      air.erase(air.begin() + i);
      break;                        // one packet per tick
    }
  }

  loraTick();

  // Anything this device transmitted goes into the air toward the peer.
  for (auto& f : radio.sent) {
    framesOffered++;
    if (dropped()) { framesDropped++; continue; }
    air.push_back({f.data, millis() + 50, peerIndex});
  }
  radio.sent.clear();

  save(me);
}

// Advance the world while `blind` never runs — its loop() is inside
// display.update(), busy-waiting on the panel's BUSY line. Frames aimed at it
// stay in the air until it comes back, which is the generous reading; a real
// SX1262 holds one and loses the rest.
void runBlackout(NodeCtx& awake, int awakeIdx, int blindIdx, uint32_t ms) {
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    advance(10);
    stepNode(awake, awakeIdx, blindIdx, 10);
  }
}

int failures = 0, checks = 0;
void CHECK(bool c, const char* what) {
  checks++;
  if (!c) { failures++; printf("  FAIL: %s\n", what); }
}

// Run both nodes until `pred` on node A is true, or timeout.
template <typename F>
bool runUntil(NodeCtx& A, NodeCtx& B, uint32_t maxMs, F pred) {
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < maxMs) {
    advance(10);
    stepNode(A, 0, 1, 10);
    stepNode(B, 1, 0, 10);
    load(A);
    if (pred()) { save(A); return true; }
    save(A);
  }
  return false;
}

int main() {
  printf("Cypher32 v54 two-node simulation\n\n");
  const uint32_t ID_A = 0xAAAA1111, ID_B = 0xBBBB2222;

  // ── clean channel: discovery ──
  {
    printf("clean channel\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 0; g_millis = 1000;

    load(A); loraSendBeacon(); save(A);
    bool found = runUntil(A, B, 3000, []{ return false; });
    (void)found;
    load(B);
    CHECK(findNode(ID_A) != nullptr, "B discovers A from a beacon");
    KnownNode* n = findNode(ID_A);
    CHECK(n && n->faction == 'B', "B learns A's faction");
    CHECK(n && n->level == 5,     "B learns A's level");
    save(B);
  }

  // ── recon round trip ──
  {
    printf("recon round trip\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 0; g_millis = 1000;

    load(A);
    loraReconProbeStart(ID_B);
    loraSendRecon(ID_B);
    save(A);
    bool ok = runUntil(A, B, 5000, []{
      return reconProbe.state == RECON_PROBE_READY;
    });
    CHECK(ok, "A receives a recon reply from B");
    load(A);
    // The whole dossier comes back in one reply and is staged, not stored: the
    // mini-game draws it down a tier per round, so nothing is written into the
    // node record until a round has been paid for.
    CHECK(reconProbe.target   == ID_B, "the dossier is filed against B");
    CHECK(reconProbe.brute    == 6,    "brute matches B's real value");
    CHECK(reconProbe.stealth  == 4,    "stealth matches B's real value");
    CHECK(reconProbe.firewall == 9,    "firewall matches B's real value");
    CHECK(reconProbe.faction  == 'W',  "faction matches B");
    KnownNode* n = findNode(ID_B);
    CHECK(n && n->intel == 0,       "no intel is granted by the probe alone");
    CHECK(n && n->seen_firewall == 0, "no stat is stored before a round buys it");
    CHECK(loraActionState == LA_SUCCESS, "A's action reports SUCCESS");
    CHECK(loraTimeouts == 0,             "no spurious timeout");

    // The probe has to fail on its own clock, or the portal waits forever on a
    // target that walked out of range.
    reconProbe.state = RECON_PROBE_WAIT;
    reconProbe.deadline = g_millis + RECON_PROBE_MS;
    loraServiceReconProbe();
    CHECK(reconProbe.state == RECON_PROBE_WAIT, "a fresh probe is still waiting");
    g_millis += RECON_PROBE_MS + 1;
    loraServiceReconProbe();
    CHECK(reconProbe.state == RECON_PROBE_FAILED, "a silent target times the probe out");
    reconProbe.state = RECON_PROBE_IDLE;
    save(A);
  }

  // ── message delivery ──
  {
    printf("message delivery\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 0; g_millis = 1000;

    load(A); loraSendMsg(ID_B, "meet at the north gate"); save(A);
    runUntil(A, B, 5000, []{ return loraActionState == LA_SUCCESS; });
    load(B);
    KnownNode* n = findNode(ID_A);
    CHECK(n && n->msg_unread, "B has an unread message");
    CHECK(n && String(n->msg_inbox) == "meet at the north gate", "message text intact");
    save(B);
    load(A);
    CHECK(loraActionState == LA_SUCCESS, "A sees the message ACKed");
    save(A);
  }

  // ── full defender-authoritative hack exchange (T4.3) ──
  {
    printf("hack exchange\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 0; g_millis = 1000;

    load(A); loraHackStart(ID_B, 2); save(A);
    bool got = runUntil(A, B, 6000, []{ return hackVerdictReady; });
    CHECK(got, "A receives a verdict from B");
    load(A);
    CHECK(!hackInFlight,               "A's hack resolved");
    CHECK(hackVerdictFirewall == 9,    "A learns B's real firewall");
    CHECK(hackVerdictFaction == 'W',   "A learns B's faction");
    bool aThinksWon = hackVerdictWon;
    save(A);
    load(B);
    CHECK(pendingHackAlert,            "B knows it was attacked");
    CHECK(pendingHackFrom == chipIdStr(ID_A), "B knows who did it");
    CHECK(pendingHackAttackerWon == aThinksWon,
          "both sides agree on the outcome the DEFENDER decided");
    save(B);
  }
  {
    // Attacking a device that is not there must fail cleanly.
    printf("hack against absent target\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 100; g_millis = 1000;

    load(A); loraHackStart(ID_B, 1); save(A);
    runUntil(A, B, 8000, []{ return hackTimedOut; });
    load(A);
    CHECK(hackTimedOut,      "reports the target never answered");
    CHECK(!hackVerdictReady, "and never invents a local verdict");
    save(A);
  }

  // ── lossy channel: does retry actually rescue the exchange? ──
  printf("\nrecon success rate vs channel loss (30 trials each)\n");
  for (int loss : {0, 10, 25, 40, 55, 70, 85}) {
    int wins = 0; const int TRIALS = 30;
    framesOffered = framesDropped = 0;
    for (int trial = 0; trial < TRIALS; trial++) {
      NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
      air.clear(); lossPercent = loss; g_millis = 1000;
      lossRng = 4242 + trial * 7919; g_rngState = 999 + trial * 31;

      load(A); reconProbe.state = RECON_PROBE_IDLE;
      loraReconProbeStart(ID_B); loraSendRecon(ID_B); save(A);
      bool ok = runUntil(A, B, 6000, []{
        return reconProbe.state == RECON_PROBE_READY;
      });
      if (ok) wins++;
    }
    printf("  %2d%% loss -> %2d/%d recons completed (%3.0f%%)\n",
           loss, wins, TRIALS, wins * 100.0 / TRIALS);
    if (loss == 0)  CHECK(wins == TRIALS, "clean channel: every recon completes");
    if (loss == 25) CHECK(wins >= TRIALS * 2 / 3, "25% loss: retries rescue most recons");
  }

  // ── no-peer case must fail fast and visibly, not hang ──
  {
    printf("\npeer offline\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 100; g_millis = 1000;   // nothing gets through

    load(A); uint32_t t0 = millis(); loraSendRecon(ID_B); save(A);
    runUntil(A, B, 8000, []{ return loraActionState == LA_TIMEOUT; });
    load(A);
    CHECK(loraActionState == LA_TIMEOUT, "reports NO RESPONSE");
    CHECK(millis() - t0 < 4000,          "and does so within 4 s");
    printf("  gave up after %u ms\n", millis() - t0);
    save(A);
  }

  // ── the defender's own screen must not cost the attacker the verdict ──
  //
  // The defender rolls the outcome, queues HACK_REPLY behind a 60-120 ms
  // defer, and then blocks ~2 s painting its alert. The attacker's four tries
  // expire at 1.6-2.8 s. Whoever loses that race, the defender has already
  // counted the fight and written NVS — so a dropped verdict is not a hack
  // that did not happen, it is a hack that happened on one device only. The
  // attacker got no cooldown out of it and could immediately go again.
  {
    printf("\nverdict arriving late\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 0; g_millis = 1000;

    load(A); loraHackStart(ID_B, 5); save(A);

    // B is inside display.update(). It hears nothing and answers nothing.
    // Outlast the worst case the retry timers can produce, rather than a
    // number that happens to work: 2600 ms passed or failed on the jitter draw.
    runBlackout(A, 0, 1, TX_MAX_TRIES * (TX_RETRY_BASE_MS + TX_RETRY_JITTER_MS) + 400);

    load(A);
    CHECK(hackTimedOut,  "the attacker's retries give up during the blackout");
    CHECK(!hackInFlight, "and the hack is no longer in flight");
    CHECK(hackTargetId == ID_B,
          "but the target is still named, which is what makes a late verdict placeable");
    save(A);

    // B's loop comes back. It now sees the request and answers.
    runUntil(A, B, 4000, []{ return hackVerdictReady; });

    load(A);
    CHECK(hackVerdictReady,
          "the verdict is accepted after the retries gave up");
    CHECK(hackVerdictLate,
          "and is marked as rescued from the grace window");
    CHECK(!hackTimedOut,
          "so the player is NOT also told the target never answered");
    save(A);
  }

  // The other half of the same rule: silence really is silence. A grace window
  // that never closes would just move the bug.
  {
    printf("grace window closes\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 100; g_millis = 1000;

    load(A); loraHackStart(ID_B, 5); save(A);
    runUntil(A, B, 12000, []{ return false; });

    load(A);
    CHECK(hackTimedOut, "a target that truly never answers still reports a timeout");
    CHECK(!hackVerdictReady, "and no verdict is invented for it");
    CHECK((int32_t)(millis() - hackGraceUntil) >= 0, "the grace window has closed");
    save(A);
  }

  // ── the recon budget outlives the node record that carries it ──
  {
    printf("recon ledger\n");
    NodeCtx A; initCtx(A, ID_A, "BLACK", 12, 5);
    load(A);

    KnownNode* n = findOrAddNode(ID_B);
    n->recon_count = 3;
    reconLedgerSet(ID_B, 3);            // what /api/reveal does when it charges

    // The portal's "clear nodes" button, verbatim.
    knownCount = 0; memset(knownNodes, 0, sizeof(knownNodes));

    KnownNode* again = findOrAddNode(ID_B);
    CHECK(again->recon_count == 3,
          "clearing the node table does NOT refund spent recon attempts");

    // And eviction, which happens by itself in a room bigger than the table.
    knownCount = 0; memset(knownNodes, 0, sizeof(knownNodes));
    for (int i = 0; i < MAX_KNOWN_NODES; i++) findOrAddNode(0xC0DE0000u + i);
    findOrAddNode(0xFFFF0001u);          // forces an eviction
    CHECK(findOrAddNode(ID_B)->recon_count == 3,
          "nor does being evicted to make room for somebody else");

    // Only the lock expiring hands them back.
    reconLedgerSet(ID_B, 0);
    knownCount = 0; memset(knownNodes, 0, sizeof(knownNodes));
    CHECK(findOrAddNode(ID_B)->recon_count == 0,
          "and when the lock expires the budget really does come back");
    save(A);
  }

  // ── the queue is drained before the caller goes deaf ──
  {
    printf("flush before blocking\n");
    NodeCtx A, B; initCtx(A, ID_A, "BLACK", 12, 5); initCtx(B, ID_B, "WHITE", 6, 9);
    air.clear(); lossPercent = 0; g_millis = 1000;

    load(A);
    PktBeacon pkt;
    fillHdr(&pkt.hdr, PKT_BEACON, 0);
    pkt.level = 5; pkt.faction = 'B';
    loraSendUnreliable(&pkt, sizeof(pkt));
    CHECK(txQueueDepth() == 1, "a frame is queued");
    CHECK(txQueueDueWithin(200), "and is due to go out shortly");

    int sentBefore = loraPktSent;
    loraFlushTx(300);
    CHECK(loraPktSent == sentBefore + 1,
          "loraFlushTx puts it on the air before the caller blocks");

    // It is still holding the transmission it just started, which is correct —
    // going deaf mid-frame would truncate it.
    CHECK(radioState == RS_TX, "and waits out the transmission it started");

    // But with an idle radio and an empty queue it must cost nothing, or every
    // screen in the game pays 300 ms for the privilege of drawing itself.
    radioState = RS_RX;
    uint32_t t0 = millis();
    loraFlushTx(300);
    CHECK((uint32_t)(millis() - t0) < 50,
          "and returns at once when there is nothing to send");
    save(A);
  }

  // ── ACKs must not all be scheduled for the same instant ──
  // Two devices answering different senders in the same pass would otherwise
  // both wake at exactly REPLY_DELAY_MIN_MS, CAD together and collide. Only
  // reachable with three or more devices, which is why it went unnoticed.
  {
    printf("ack jitter\n");
    NodeCtx A; initCtx(A, ID_A, "BLACK", 12, 5);
    load(A);
    for (int i = 0; i < TXQ_SIZE; i++) txq[i].active = false;

    uint32_t when[6]; bool spread = false;
    for (int i = 0; i < 6; i++) {
      for (int k = 0; k < TXQ_SIZE; k++) txq[k].active = false;
      sendAck(0xB0000000u + i, (uint8_t)i, PKT_PING);
      when[i] = 0;
      for (int k = 0; k < TXQ_SIZE; k++) if (txq[k].active) { when[i] = txq[k].sendAfterMs; break; }
      if (i && when[i] != when[0]) spread = true;
    }
    CHECK(spread, "ACK send times are jittered, not all on the same millisecond");

    uint32_t lo = when[0], hi = when[0];
    for (int i = 1; i < 6; i++) { if (when[i] < lo) lo = when[i]; if (when[i] > hi) hi = when[i]; }
    CHECK(hi - lo <= REPLY_DELAY_JIT_MS,
          "and stay inside the reply window, so an ACK is still prompt");
    save(A);
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
