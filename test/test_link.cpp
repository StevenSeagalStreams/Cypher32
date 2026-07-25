// Host-side tests for the Cypher32 v54 link layer (roadmap Phase 1).
// Compiles the real cypher32_lora.h against Arduino/RadioLib stubs.
#include <Arduino.h>
#include <cstdio>

uint32_t   g_millis   = 1000;
uint32_t   g_rngState = 12345;
SerialStub Serial;
ESPStub    ESP;

// Globals the sketch normally owns
uint32_t myChipID32 = 0xAAAA1111;
String   myFaction  = "BLACK";
int      myLevel    = 7;
int      skillBrute = 11, skillStealth = 5, skillFirewall = 8;

#include "../cypher32_lora.h"

static const uint32_t PEER = 0xBBBB2222;

// ── harness ──
int  failures = 0, checks = 0;
void CHECK(bool cond, const char* what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

void resetAll() {
  g_millis = 1000; g_rngState = 12345;
  memset(txq, 0, sizeof(txq));
  memset(seenRing, 0, sizeof(seenRing)); seenIdx = 0;
  memset(&pendingTx, 0, sizeof(pendingTx));
  memset(knownNodes, 0, sizeof(knownNodes)); knownCount = 0;
  txSeq = 0; loraDioFlag = false; radioState = RS_RX;
  loraReady = true;
  loraPktSent = loraPktRecv = loraBeaconsSent = 0;
  loraAcksSent = loraAcksRecv = loraRetries = loraTimeouts = 0;
  loraDupsDropped = loraCrcErrors = loraTxErrors = loraCadBusy = 0;
  loraDroppedNotForMe = loraTxQueueDrops = loraWatchdogFires = loraReinits = 0;
  loraActionState = LA_IDLE; loraActionTries = 0;
  pendingHackAlert = false; pendingMsg = "";
  radio.sent.clear(); radio.rxBuf.clear();
  radio.cadBusy = false; radio.failTransmit = false; radio.failReceive = false;
  radio.startRxCalls = 0; radio.beginCalls = 0;
}

// Hand a frame to the radio as if it had just been received.
void deliver(const void* pkt, int len) {
  const uint8_t* p = (const uint8_t*)pkt;
  radio.rxBuf.assign(p, p + len);
  radioState  = RS_RX;
  loraDioFlag = true;
  loraTick();
}

int txqCountOfType(uint8_t type) {
  int n = 0;
  for (int i = 0; i < TXQ_SIZE; i++)
    if (txq[i].active && ((PktHeader*)txq[i].buf)->type == type) n++;
  return n;
}
int sentCountOfType(uint8_t type) {
  int n = 0;
  for (auto& f : radio.sent)
    if (f.data.size() >= sizeof(PktHeader) && ((PktHeader*)f.data.data())->type == type) n++;
  return n;
}
// Advance the clock in slices, ticking as we go. Models the DIO1 TxDone
// interrupt firing ~50 ms after startTransmit (roughly SF7 airtime for these
// payloads) so the TX queue drains the way it does on real hardware.
void run(uint32_t ms, uint32_t step = 10) {
  for (uint32_t t = 0; t < ms; t += step) {
    advance(step);
    if (radioState == RS_TX && (uint32_t)(millis() - txStartMs) >= 50) loraDioFlag = true;
    loraTick();
  }
}

void mkHdr(PktHeader* h, uint8_t type, uint8_t seq, uint8_t flags, uint32_t from, uint32_t to) {
  h->type = type; h->seq = seq; h->flags = flags; h->from_id = from; h->to_id = to;
}

// ─────────────────────────────────────────────
int main() {
  printf("Cypher32 v54 link-layer tests\n\n");

  // ── T1.1 duplicate suppression ──
  printf("T1.1 duplicate suppression\n");
  {
    resetAll();
    PktReconReq req;
    mkHdr(&req.hdr, PKT_RECON_REQ, 42, PKTFLAG_ACK_REQ, PEER, myChipID32);
    deliver(&req, sizeof(req));
    CHECK(txqCountOfType(PKT_RECON_REPLY) == 1, "first req queues one reply");
    CHECK(txqCountOfType(PKT_ACK) == 1,         "first req is ACKed");
    CHECK(loraDupsDropped == 0,                 "no dup yet");

    deliver(&req, sizeof(req));                 // byte-identical retransmit
    CHECK(loraDupsDropped == 1,                 "duplicate detected");
    CHECK(txqCountOfType(PKT_RECON_REPLY) == 1, "payload processed exactly once");
    CHECK(txqCountOfType(PKT_ACK) == 2,         "duplicate is re-ACKed (our ACK was lost)");
  }

  // ── T1.2 ACK clears the reliable slot ──
  printf("T1.2 ACK / retry / timeout\n");
  {
    resetAll();
    loraSendRecon(PEER);
    CHECK(pendingTx.active,                   "reliable slot occupied");
    CHECK(loraActionState == LA_SENDING,      "action state = SENDING");
    uint8_t seq = pendingTx.seq;
    run(30);
    CHECK(sentCountOfType(PKT_RECON_REQ) == 1, "request went out once");
    CHECK(loraActionState == LA_WAITING,       "action state = WAITING after TX");

    PktAck ack;
    mkHdr(&ack.hdr, PKT_ACK, seq, PKTFLAG_IS_ACK, PEER, myChipID32);
    ack.ack_type = PKT_RECON_REQ;
    deliver(&ack, sizeof(ack));
    CHECK(!pendingTx.active,                  "matching ACK clears the slot");
    CHECK(loraActionState == LA_SUCCESS,      "action state = SUCCESS");
    CHECK(loraAcksRecv == 1,                  "ACK counted");
  }
  {
    resetAll();
    loraSendRecon(PEER);
    uint8_t seq = pendingTx.seq;
    PktAck ack;
    mkHdr(&ack.hdr, PKT_ACK, (uint8_t)(seq + 9), PKTFLAG_IS_ACK, PEER, myChipID32);
    ack.ack_type = PKT_RECON_REQ;
    deliver(&ack, sizeof(ack));
    CHECK(pendingTx.active, "ACK with wrong seq does NOT clear the slot");
  }
  {
    // Peer is dead: expect retries then a clean timeout, not a silent hang.
    resetAll();
    loraSendRecon(PEER);
    run(5000);
    CHECK(loraActionState == LA_TIMEOUT, "gives up as TIMEOUT");
    CHECK(loraTimeouts == 1,             "timeout counted");
    CHECK(loraRetries == TX_MAX_TRIES-1, "retried TX_MAX_TRIES-1 times");
    CHECK(sentCountOfType(PKT_RECON_REQ) == TX_MAX_TRIES, "4 transmissions total");
    CHECK(!pendingTx.active,             "slot released");
  }
  {
    // Roadmap acceptance: portal reports TIMEOUT within ~3 s.
    resetAll();
    uint32_t t0 = millis();
    loraSendRecon(PEER);
    while (loraActionPending() && millis() - t0 < 10000) { advance(10); loraTick(); }
    uint32_t elapsed = millis() - t0;
    CHECK(elapsed < 3500, "timeout surfaces in under 3.5 s");
    printf("       (timeout after %u ms)\n", elapsed);
  }

  // ── T1.3 deferred replies (fixes D3) ──
  printf("T1.3 deferred replies\n");
  {
    resetAll();
    PktReconReq req;
    mkHdr(&req.hdr, PKT_RECON_REQ, 7, PKTFLAG_ACK_REQ, PEER, myChipID32);
    size_t before = radio.sent.size();
    deliver(&req, sizeof(req));
    CHECK(radio.sent.size() == before, "reply NOT transmitted inline from the RX handler");
    CHECK(txqCountOfType(PKT_RECON_REPLY) == 1, "reply is queued instead");
    run(300);
    CHECK(sentCountOfType(PKT_RECON_REPLY) == 1, "reply goes out after the defer window");
    CHECK(radio.sent[0].atMs > 1000, "reply delayed past the requester's RX re-arm");
  }

  // ── T1.4 non-blocking TX ──
  printf("T1.4 non-blocking TX state machine\n");
  {
    resetAll();
    loraSendBeacon();
    loraTick();
    CHECK(radioState == RS_TX, "radio enters TX state");
    loraDioFlag = true; loraTick();
    CHECK(radioState == RS_RX, "TxDone returns radio to RX");
  }
  {
    resetAll();
    loraSendBeacon(); loraTick();
    CHECK(radioState == RS_TX, "in TX");
    advance(TX_HARD_TIMEOUT_MS + 50); loraTick();   // TxDone never fires
    CHECK(radioState == RS_RX,  "hard timeout recovers to RX");
    CHECK(loraTxErrors == 1,    "TX error counted");
  }

  // ── T1.5 CAD ──
  printf("T1.5 channel activity detection\n");
  {
    resetAll();
    radio.cadBusy = true;
    loraSendBeacon();
    loraTick();
    CHECK(radio.sent.empty(), "busy channel defers the transmission");
    CHECK(loraCadBusy == 1,   "CAD busy counted");
    CHECK(radioState == RS_RX, "stays in RX while backing off, not deaf");
    radio.cadBusy = false;
    run(200);
    CHECK(sentCountOfType(PKT_BEACON) == 1, "transmits once channel clears");
  }
  {
    resetAll();
    radio.cadBusy = true;                    // never clears
    loraSendBeacon();
    run(2000);
    CHECK(sentCountOfType(PKT_BEACON) == 1, "sends anyway after CAD_MAX_TRIES rather than starving");
  }

  // ── T1.6 RX watchdog ──
  printf("T1.6 RX watchdog\n");
  {
    resetAll();
    int before = radio.startRxCalls;
    run(RX_WATCHDOG_MS + 500, 100);
    CHECK(radio.startRxCalls > before, "watchdog re-arms RX");
    CHECK(loraWatchdogFires > 0,       "watchdog fired");
  }
  {
    resetAll();
    radio.failReceive = true;                // radio wedged
    run(RX_WATCHDOG_MS * 4, 100);
    CHECK(loraReinits > 0, "repeated failure triggers full re-init");
    CHECK(radio.beginCalls > 0, "loraSetup() re-ran");
  }

  // ── protocol invariants ──
  printf("protocol invariants\n");
  {
    resetAll();
    loraSendBeacon();
    loraTick();
    CHECK(radio.sent.size() == 1, "beacon transmitted");
    PktHeader* h = (PktHeader*)radio.sent[0].data.data();
    CHECK(h->to_id == 0,                        "beacon is broadcast");
    CHECK(!(h->flags & PKTFLAG_ACK_REQ),        "beacon does not request an ACK");
    CHECK(!pendingTx.active,                    "beacon does not occupy the reliable slot");
    CHECK(radio.sent[0].data.size() == sizeof(PktBeacon), "beacon size matches struct");
  }
  {
    resetAll();
    loraSendHackResult(PEER, true, 42);
    CHECK(pendingTx.active,               "HACK_RESULT is sent reliably");
    CHECK(pendingTx.type == PKT_HACK_RESULT, "correct type in flight");
  }
  {
    // The defender must actually learn they were hit.
    resetAll();
    PktHackResult hr;
    mkHdr(&hr.hdr, PKT_HACK_RESULT, 3, PKTFLAG_ACK_REQ, PEER, myChipID32);
    hr.outcome = HACK_WIN; hr.xp_delta = 30;
    deliver(&hr, sizeof(hr));
    CHECK(pendingHackAlert,          "defender is notified of an inbound hack");
    CHECK(pendingHackAttackerWon,    "outcome decoded correctly");
    CHECK(txqCountOfType(PKT_ACK) == 1, "HACK_RESULT is ACKed");
  }
  {
    resetAll();
    PktHeader h;
    mkHdr(&h, PKT_BEACON, 1, 0, PEER, 0x99999999);   // addressed elsewhere
    PktBeacon b; b.hdr = h; b.level = 4; b.faction = 'W';
    deliver(&b, sizeof(b));
    CHECK(loraDroppedNotForMe == 1, "unicast for another node is dropped");
    CHECK(knownCount == 0,          "and not added to the node table");
  }
  {
    resetAll();
    radio.crcNext = 1;
    PktBeacon b;
    mkHdr(&b.hdr, PKT_BEACON, 1, 0, PEER, 0);
    b.level = 3; b.faction = 'R';
    deliver(&b, sizeof(b));
    CHECK(loraCrcErrors == 1, "CRC failure counted, not silently swallowed");
  }
  {
    resetAll();
    PktBeacon b;
    for (int i = 0; i < 3; i++) {
      mkHdr(&b.hdr, PKT_BEACON, (uint8_t)i, 0, PEER, 0);
      b.level = 9; b.faction = 'G';
      deliver(&b, sizeof(b));
    }
    CHECK(knownCount == 1, "repeated beacons from one peer make one node");
    KnownNode* n = findNode(PEER);
    CHECK(n && n->level == 9 && n->faction == 'G', "beacon fields stored");
  }
  {
    resetAll();
    CHECK(sizeof(PktHeader) == 11, "header is 11 bytes as documented");
    CHECK(sizeof(PktMsg) <= 64,    "largest packet fits the 64-byte cap");
  }
  {
    // seq must roll over cleanly and the ring must not false-positive.
    resetAll();
    for (int i = 0; i < SEEN_RING_SIZE + 4; i++) markSeen(PEER, (uint8_t)i);
    CHECK(!seenBefore(PEER, 0),                    "oldest entry aged out of the ring");
    CHECK(seenBefore(PEER, SEEN_RING_SIZE + 3),    "newest entry retained");
    CHECK(!seenBefore(0xDEADBEEF, 5),              "different sender is not a dup");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
