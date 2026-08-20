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
  memset(&pendingUser, 0, sizeof(pendingUser));
  memset(&pendingReply, 0, sizeof(pendingReply));
  memset(knownNodes, 0, sizeof(knownNodes)); knownCount = 0;
  txSeq = 0; loraDioFlag = false; radioState = RS_RX;
  loraReady = true;
  loraPktSent = loraPktRecv = loraBeaconsSent = 0;
  loraAcksSent = loraAcksRecv = loraRetries = loraTimeouts = 0;
  loraDupsDropped = loraCrcErrors = loraTxErrors = loraCadBusy = 0;
  loraDroppedNotForMe = loraTxQueueDrops = loraWatchdogFires = loraReinits = 0;
  loraActionState = LA_IDLE; loraActionTries = 0;
  pendingHackAlert = false; pendingMsg = "";
  loraNodesEvicted = 0; loraDutyDeferred = 0;
  loraBadSig = 0; loraReplaysDropped = 0;
  hackInFlight = false; hackVerdictReady = false; hackTimedOut = false;
  memset(dutyBucketMs, 0, sizeof(dutyBucketMs));
  dutyBucketIdx = 0; dutyBucketStart = 0;
  loraBeaconEnabled = false; loraNextBeaconMs = 0;
  loraFastUntilMs = 0; loraBootBurst = 0;
  radio.sent.clear(); radio.rxBuf.clear();
  radio.cadBusy = false; radio.failTransmit = false; radio.failReceive = false;
  radio.startRxCalls = 0; radio.beginCalls = 0;
}

// Hand a frame to the radio as if a real peer had just transmitted it —
// including the T4.1 signature, which the RX path now requires.
void deliver(const void* pkt, int len) {
  uint8_t frame[64];
  memcpy(frame, pkt, len);
  int total = len;
#if LORA_SIGN
  uint8_t tag[32];
  hmacSha256(LORA_KEY, sizeof(LORA_KEY), frame, (size_t)len, tag);
  memcpy(frame + len, tag, SIG_LEN);
  total = len + SIG_LEN;
#endif
  radio.rxBuf.assign(frame, frame + total);
  radioState  = RS_RX;
  loraDioFlag = true;
  loraTick();
}

// Deliver a frame with a deliberately invalid signature.
void deliverForged(const void* pkt, int len) {
  uint8_t frame[64];
  memcpy(frame, pkt, len);
  memset(frame + len, 0xAB, SIG_LEN);          // junk tag
  radio.rxBuf.assign(frame, frame + len + SIG_LEN);
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
    CHECK(pendingUser.active,                   "reliable slot occupied");
    CHECK(loraActionState == LA_SENDING,      "action state = SENDING");
    uint8_t seq = pendingUser.seq;
    run(30);
    CHECK(sentCountOfType(PKT_RECON_REQ) == 1, "request went out once");
    CHECK(loraActionState == LA_WAITING,       "action state = WAITING after TX");

    PktAck ack;
    mkHdr(&ack.hdr, PKT_ACK, seq, PKTFLAG_IS_ACK, PEER, myChipID32);
    ack.ack_type = PKT_RECON_REQ;
    deliver(&ack, sizeof(ack));
    CHECK(!pendingUser.active,                  "matching ACK clears the slot");
    CHECK(loraActionState == LA_SUCCESS,      "action state = SUCCESS");
    CHECK(loraAcksRecv == 1,                  "ACK counted");
  }
  {
    resetAll();
    loraSendRecon(PEER);
    uint8_t seq = pendingUser.seq;
    PktAck ack;
    mkHdr(&ack.hdr, PKT_ACK, (uint8_t)(seq + 9), PKTFLAG_IS_ACK, PEER, myChipID32);
    ack.ack_type = PKT_RECON_REQ;
    deliver(&ack, sizeof(ack));
    CHECK(pendingUser.active, "ACK with wrong seq does NOT clear the slot");
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
    CHECK(!pendingUser.active,             "slot released");
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

  // ── reply slot must survive a concurrent local action ──
  printf("slot separation\n");
  {
    resetAll();
    PktReconReq req;
    mkHdr(&req.hdr, PKT_RECON_REQ, 5, PKTFLAG_ACK_REQ, PEER, myChipID32);
    deliver(&req, sizeof(req));                    // we owe PEER an answer
    CHECK(pendingReply.active,                     "reply occupies the reply slot");
    CHECK(pendingReply.type == PKT_RECON_REPLY,    "correct frame held for retry");
    CHECK(!pendingUser.active,                     "player's slot untouched");

    loraSendRecon(0xCCCC3333);                     // player acts a moment later
    CHECK(pendingReply.active,                     "in-flight reply NOT evicted");
    CHECK(pendingReply.type == PKT_RECON_REPLY,    "peer still gets its answer retried");
    CHECK(pendingUser.active,                      "player's action also in flight");
    CHECK(pendingUser.type == PKT_RECON_REQ,       "player's frame in the user slot");
    CHECK(loraActionState == LA_SENDING,           "portal tracks the player's action");
  }
  {
    // A background reply completing must not overwrite the portal's status.
    resetAll();
    loraSendRecon(0xCCCC3333);
    run(30);
    CHECK(loraActionState == LA_WAITING, "player's action is WAITING");
    PktReconReq req;
    mkHdr(&req.hdr, PKT_RECON_REQ, 5, PKTFLAG_ACK_REQ, PEER, myChipID32);
    deliver(&req, sizeof(req));
    PktAck ack;                                    // PEER acks our reply
    mkHdr(&ack.hdr, PKT_ACK, pendingReply.seq, PKTFLAG_IS_ACK, PEER, myChipID32);
    ack.ack_type = PKT_RECON_REPLY;
    deliver(&ack, sizeof(ack));
    CHECK(!pendingReply.active,          "reply slot cleared by its ACK");
    CHECK(loraActionState == LA_WAITING, "portal status NOT clobbered by background traffic");
    CHECK(pendingUser.active,            "player's action still pending");
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

  // ── CAD must not corrupt the TX state machine ──
  printf("CAD / TxDone interaction\n");
  {
    resetAll();
    loraSendBeacon();
    loraTick();                 // CAD runs, DIO1 pulses, then startTransmit
    CHECK(radioState == RS_TX, "transmission started after CAD");
    CHECK(!loraDioFlag,        "CAD's DIO1 pulse cleared, not left pending");
    loraTick();                 // must not mistake it for TxDone
    CHECK(radioState == RS_TX, "still transmitting — frame not truncated");
    CHECK(radio.sent.size() == 1, "exactly one frame on the air");
    loraDioFlag = true; loraTick();          // the real TxDone
    CHECK(radioState == RS_RX, "returns to RX on genuine TxDone");
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
    CHECK(!pendingUser.active,                    "beacon does not occupy the reliable slot");
    CHECK(radio.sent[0].data.size() == sizeof(PktBeacon) + SIG_LEN, "beacon carries struct + signature");
  }
  {
    resetAll();
    loraSendHackResult(PEER, true, 42);
    CHECK(pendingUser.active,               "HACK_RESULT is sent reliably");
    CHECK(pendingUser.type == PKT_HACK_RESULT, "correct type in flight");
  }
  {
    // The defender learns it was hit when the REQUEST arrives, not when the
    // attacker deigns to report a result (T4.3).
    resetAll();
    PktHackReq hq;
    mkHdr(&hq.hdr, PKT_HACK_REQ, 3, PKTFLAG_ACK_REQ, PEER, myChipID32);
    hq.brute = 12; hq.recon_count = 2; hq.stealth = 4;
    deliver(&hq, sizeof(hq));
    CHECK(pendingHackAlert,             "defender is notified of an inbound hack");
    CHECK(txqCountOfType(PKT_HACK_REPLY) == 1, "defender returns a verdict");
    CHECK(txqCountOfType(PKT_ACK) == 1, "HACK_REQ is ACKed");
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

  // ── Phase 2 ──
  printf("T2.1 per-node signal\n");
  {
    resetAll();
    PktBeacon b;
    for (int i = 0; i < 3; i++) {
      mkHdr(&b.hdr, PKT_BEACON, (uint8_t)i, 0, PEER, 0);
      b.level = 6; b.faction = 'R';
      deliver(&b, sizeof(b));
    }
    KnownNode* n = findNode(PEER);
    CHECK(n && n->rssi == -70,   "per-node RSSI recorded");
    CHECK(n && n->rssi_n == 3,   "rolling window fills");
    CHECK(n && nodeAvgRssi(n) == -70, "average over the window");
    CHECK(n && n->snr10 == 95,   "per-node SNR recorded (x10)");
    CHECK(nodeSignalBars(n) == 3,       "-70 dBm maps to 3 bars");
    CHECK(String(nodeProximity(n)) == "CLOSE", "-70 dBm reads as CLOSE");
    CHECK(n && n->first_seen_ms != 0,   "first_seen recorded");
  }
  {
    // Every packet type must refresh presence, not just beacons.
    resetAll();
    PktPing p;
    mkHdr(&p.hdr, PKT_PING, 1, PKTFLAG_ACK_REQ, PEER, myChipID32);
    deliver(&p, sizeof(p));
    CHECK(findNode(PEER) != nullptr, "a ping registers the sender as present");
  }

  printf("T2.2 rollover-safe ageing\n");
  {
    resetAll();
    PktBeacon b; mkHdr(&b.hdr, PKT_BEACON, 1, 0, PEER, 0);
    b.level = 2; b.faction = 'W';
    g_millis = 0xFFFFF000;                 // just before the 49-day wrap
    deliver(&b, sizeof(b));
    g_millis = 0x00002000;                 // wrapped: ~12 s later in real time
    KnownNode* n = findNode(PEER);
    CHECK(n && ageMs(n->last_seen_ms) < 30000, "age stays correct across wrap");
    CHECK(nodeIsActive(n),                     "node still ACTIVE across wrap");
  }

  printf("T2.3 node expiry\n");
  {
    resetAll();
    PktBeacon b; mkHdr(&b.hdr, PKT_BEACON, 1, 0, PEER, 0);
    b.level = 2; b.faction = 'W';
    deliver(&b, sizeof(b));
    CHECK(knownCount == 1, "node present");
    CHECK(String(nodeStatusText(findNode(PEER))) == "ACTIVE", "fresh node is ACTIVE");

    advance(NODE_ACTIVE_MS + 1000);
    CHECK(String(nodeStatusText(findNode(PEER))) == "FADING", "goes FADING after 90 s");
    pruneNodes();
    CHECK(knownCount == 1, "FADING nodes are kept");

    advance(NODE_FADING_MS);
    pruneNodes();
    CHECK(knownCount == 0,        "evicted after 5 min unheard");
    CHECK(loraNodesEvicted == 1,  "eviction counted");
  }

  printf("T2.4 adaptive beacon\n");
  {
    resetAll();
    loraFastUntilMs = millis() + BEACON_FAST_WINDOW_MS;
    uint32_t fast = loraBeaconInterval();
    CHECK(fast >= LORA_BEACON_FAST_MIN_MS && fast < LORA_BEACON_FAST_MAX_MS,
          "fast mode uses the 12-18 s window");
    loraFastUntilMs = 0;
    uint32_t slow = loraBeaconInterval();
    CHECK(slow >= LORA_BEACON_MIN_MS && slow < LORA_BEACON_MAX_MS,
          "steady state uses the 25-35 s window");
  }
  {
    // Meeting a new node should re-trigger fast discovery.
    resetAll();
    loraFastUntilMs = 0;
    PktBeacon b; mkHdr(&b.hdr, PKT_BEACON, 1, 0, PEER, 0);
    b.level = 4; b.faction = 'G';
    deliver(&b, sizeof(b));
    CHECK((int32_t)(millis() - loraFastUntilMs) < 0, "new node triggers fast beaconing");
  }
  {
    resetAll();
    loraBeaconEnabled = true;
    loraNextBeaconMs = 0; loraBootBurst = 0;
    run(200);
    CHECK(sentCountOfType(PKT_BEACON) == 0, "no beacon before the 3 s mark");
    run(4000);
    CHECK(sentCountOfType(PKT_BEACON) == 1, "boot burst beacon at ~3 s");
    run(6000);
    CHECK(sentCountOfType(PKT_BEACON) == 2, "second boot burst beacon at ~8 s");
    loraBeaconEnabled = false;
  }

  printf("T2.5 duty cycle budget\n");
  {
    resetAll();
    uint32_t toa = loraTimeOnAirMs(sizeof(PktBeacon));
    // SF7/125 kHz, 13-byte payload: tens of milliseconds.
    CHECK(toa > 20 && toa < 100, "time-on-air is plausible for SF7/125 kHz");
    printf("       (beacon airtime %u ms, msg airtime %u ms)\n",
           toa, loraTimeOnAirMs(sizeof(PktMsg)));
    CHECK(loraTimeOnAirMs(sizeof(PktMsg)) > toa, "longer payload takes longer");
  }
  {
    resetAll();
    CHECK(dutyCyclePct() == 0.0f, "starts at zero");
    dutyRecord(36000);                       // 36 s in the hour = 1%
    CHECK(dutyCyclePct() > 0.99f && dutyCyclePct() < 1.01f, "1% computed correctly");
    CHECK(dutyBudgetExceeded(), "1% is over the 0.8% soft cap");
  }
  {
    // Over budget: beacons wait, player actions do not.
    resetAll();
    dutyRecord(36000);
    loraSendBeacon();
    run(300);
    CHECK(sentCountOfType(PKT_BEACON) == 0, "beacon deferred while over budget");
    CHECK(loraDutyDeferred > 0,             "deferral counted");
    loraSendRecon(PEER);
    run(300);
    CHECK(sentCountOfType(PKT_RECON_REQ) == 1, "player's action still goes out");
  }

  // ── Phase 4 ──
  printf("T4.1 packet signing\n");
  {
    resetAll();
    PktBeacon b; mkHdr(&b.hdr, PKT_BEACON, 1, 0, PEER, 0);
    b.level = 5; b.faction = 'B';
    deliverForged(&b, sizeof(b));
    CHECK(loraBadSig == 1, "forged frame rejected");
    CHECK(knownCount == 0, "forged frame never reaches the game logic");

    deliver(&b, sizeof(b));
    CHECK(knownCount == 1, "correctly signed frame is accepted");
    CHECK(loraBadSig == 1, "and does not raise the counter");
  }
  {
    // The specific exploit: a third party handing itself a win.
    resetAll();
    PktHackResult hr;
    mkHdr(&hr.hdr, PKT_HACK_RESULT, 9, PKTFLAG_ACK_REQ, 0xDEADBEEF, myChipID32);
    hr.outcome = HACK_WIN; hr.xp_delta = 120;
    deliverForged(&hr, sizeof(hr));
    CHECK(loraBadSig == 1,  "unsigned HACK_RESULT injection rejected");
    CHECK(knownCount == 0,  "attacker never enters the node table");
  }
  {
    resetAll();
    loraSendBeacon(); run(30);
    CHECK(radio.sent.size() == 1, "beacon sent");
    // A single flipped byte anywhere must invalidate the tag.
    auto f = radio.sent[0].data;
    CHECK(f.size() == sizeof(PktBeacon) + SIG_LEN, "tag appended on transmit");
    f[5] ^= 0x01;
    uint8_t tag[32];
    hmacSha256(LORA_KEY, sizeof(LORA_KEY), f.data(), f.size() - SIG_LEN, tag);
    CHECK(memcmp(tag, f.data() + f.size() - SIG_LEN, SIG_LEN) != 0,
          "tampering with the payload breaks the tag");
  }

  printf("T4.2 replay protection\n");
  {
    resetAll();
    PktHackResult hr;
    mkHdr(&hr.hdr, PKT_HACK_RESULT, 10, PKTFLAG_ACK_REQ, PEER, myChipID32);
    hr.outcome = HACK_WIN; hr.xp_delta = 40;
    deliver(&hr, sizeof(hr));
    KnownNode* n = findNode(PEER);
    CHECK(n && n->have_result_seq && n->last_result_seq == 10, "result seq recorded");

    // Same seq replayed after the dup ring has rolled past it.
    memset(seenRing, 0, sizeof(seenRing)); seenIdx = 0;
    deliver(&hr, sizeof(hr));
    CHECK(loraReplaysDropped == 1, "replayed HACK_RESULT rejected");

    // An older seq is equally invalid.
    memset(seenRing, 0, sizeof(seenRing)); seenIdx = 0;
    mkHdr(&hr.hdr, PKT_HACK_RESULT, 4, PKTFLAG_ACK_REQ, PEER, myChipID32);
    deliver(&hr, sizeof(hr));
    CHECK(loraReplaysDropped == 2, "stale seq rejected");

    // A genuinely newer one still works, including across the byte wrap.
    memset(seenRing, 0, sizeof(seenRing)); seenIdx = 0;
    mkHdr(&hr.hdr, PKT_HACK_RESULT, 11, PKTFLAG_ACK_REQ, PEER, myChipID32);
    deliver(&hr, sizeof(hr));
    CHECK(loraReplaysDropped == 2, "newer seq accepted");
    n = findNode(PEER);
    CHECK(n && n->last_result_seq == 11, "high-water mark advanced");
  }
  {
    resetAll();
    PktHackResult hr;
    mkHdr(&hr.hdr, PKT_HACK_RESULT, 250, PKTFLAG_ACK_REQ, PEER, myChipID32);
    hr.outcome = HACK_WIN; hr.xp_delta = 40;
    deliver(&hr, sizeof(hr));
    memset(seenRing, 0, sizeof(seenRing)); seenIdx = 0;
    mkHdr(&hr.hdr, PKT_HACK_RESULT, 3, PKTFLAG_ACK_REQ, PEER, myChipID32);  // wrapped
    deliver(&hr, sizeof(hr));
    CHECK(loraReplaysDropped == 0, "seq wraparound is not mistaken for a replay");
  }

  printf("T4.3 defender-authoritative hack\n");
  {
    // Odds must be identical on both sides, and bounded.
    //                       brute recon stealth firewall
    CHECK(loraHackChancePct(10,   0,    0,      10) == 60, "base 60%");
    CHECK(loraHackChancePct(10,   3,    0,      10) == 75, "+5% per recon");
    CHECK(loraHackChancePct(15,   0,    0,      10) == 70, "+2% per brute over firewall");
    CHECK(loraHackChancePct(10,   0,    8,      10) == 68, "+1% per point of stealth");
    CHECK(loraHackChancePct(0,    0,    0,      35) == 25, "floor 25%");
    CHECK(loraHackChancePct(35,   3,   35,       0) == 90, "ceiling 90%");

    // Stealth must actually move the defender's roll — it used to feed only
    // the attacker's own second roll, which nobody else could see.
    CHECK(loraHackChancePct(5, 0, 0, 5) < loraHackChancePct(5, 0, 10, 5),
          "stealth raises the odds");
    // Firewall is subtracted from brute only, so stealth survives a firewall
    // that has already wiped out the brute advantage.
    CHECK(loraHackChancePct(5, 0, 10, 15) > loraHackChancePct(5, 0, 0, 15),
          "stealth still counts once firewall has cancelled brute");
    // ...but nothing lifts you off the 25% floor. Against a firewall that far
    // ahead the floor is already charity, and stealth cannot buy more.
    CHECK(loraHackChancePct(5, 0, 10, 30) == 25 &&
          loraHackChancePct(5, 0,  0, 30) == 25,
          "hopeless odds sit on the floor regardless of stealth");
  }
  {
    // Defender rolls and reports; attacker consumes the verdict.
    resetAll();
    loraHackStart(PEER, 2);
    CHECK(hackInFlight,                     "hack marked in flight");
    CHECK(pendingUser.type == PKT_HACK_REQ, "HACK_REQ sent reliably");
    run(30);
    PktHeader* h = (PktHeader*)radio.sent[0].data.data();
    PktHackReq* sentReq = (PktHackReq*)radio.sent[0].data.data();
    CHECK(h->type == PKT_HACK_REQ,        "HACK_REQ on the wire");
    CHECK(sentReq->brute == skillBrute,   "carries attacker brute");
    CHECK(sentReq->recon_count == 2,      "carries attacker recon count");
    CHECK(sentReq->stealth == skillStealth, "carries attacker stealth");

    PktHackReply rep;
    mkHdr(&rep.hdr, PKT_HACK_REPLY, 77, PKTFLAG_ACK_REQ, PEER, myChipID32);
    rep.outcome = HACK_WIN; rep.firewall = 6; rep.faction = 'W';
    deliver(&rep, sizeof(rep));
    CHECK(!hackInFlight,                  "hack no longer in flight");
    CHECK(hackVerdictReady,               "verdict ready for the sketch");
    CHECK(hackVerdictWon,                 "defender's WIN verdict applied");
    CHECK(hackVerdictFirewall == 6,       "defender firewall learned");
    CHECK(hackVerdictFaction == 'W',      "defender faction learned");
  }
  {
    resetAll();
    loraHackStart(PEER, 0);
    PktHackReply rep;
    mkHdr(&rep.hdr, PKT_HACK_REPLY, 78, PKTFLAG_ACK_REQ, PEER, myChipID32);
    rep.outcome = HACK_LOSE; rep.firewall = 20; rep.faction = 'B';
    deliver(&rep, sizeof(rep));
    CHECK(hackVerdictReady && !hackVerdictWon, "defender's LOSE verdict applied");
  }
  {
    // Target out of range: must not leave the sketch waiting forever.
    resetAll();
    loraHackStart(PEER, 1);
    run(6000);
    CHECK(!hackInFlight,     "hack cleared on timeout");
    CHECK(hackTimedOut,      "timeout surfaced to the sketch");
    CHECK(!hackVerdictReady, "no verdict invented locally");
    CHECK(loraActionState == LA_TIMEOUT, "portal shows NO RESPONSE");
  }
  {
    // A reply from someone we are not attacking must be ignored.
    resetAll();
    loraHackStart(PEER, 1);
    PktHackReply rep;
    mkHdr(&rep.hdr, PKT_HACK_REPLY, 5, PKTFLAG_ACK_REQ, 0xCCCC3333, myChipID32);
    rep.outcome = HACK_WIN; rep.firewall = 1; rep.faction = 'G';
    deliver(&rep, sizeof(rep));
    CHECK(hackInFlight,       "unrelated HACK_REPLY does not resolve our hack");
    CHECK(!hackVerdictReady,  "and grants no verdict");
  }

  // ── naming: one derivation, agreed by everyone ──
  printf("node naming\n");
  {
    // A device's own name and the name its peers compute must come from the
    // same function. They used to be two implementations — one random, one
    // derived — so a message always arrived under the wrong name.
    CHECK(nodeNameFromId(0xAAAA1111) == nodeNameFromId(0xAAAA1111),
          "same chip id always yields the same name");
    CHECK(!(nodeNameFromId(0xAAAA1111) == nodeNameFromId(0xBBBB2222)),
          "different chip ids yield different names");

    // Must be sane and display-safe across the whole id space.
    int empty = 0, tooLong = 0;
    uint32_t id = 1;
    for (int i = 0; i < 4000; i++) {
      id = id * 1664525u + 1013904223u;
      String n = nodeNameFromId(id);
      if (n.length() == 0)  empty++;
      if (n.length() > 10)  tooLong++;
    }
    CHECK(empty == 0,   "never produces an empty name");
    CHECK(tooLong == 0, "never exceeds the 10-char display budget");
    printf("       (e.g. %08x -> %s, %08x -> %s)\n",
           0xAAAA1111, nodeNameFromId(0xAAAA1111).c_str(),
           0xBBBB2222, nodeNameFromId(0xBBBB2222).c_str());
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
