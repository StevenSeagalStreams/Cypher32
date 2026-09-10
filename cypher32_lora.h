#pragma once
#include <math.h>
#include <RadioLib.h>
#include "cypher32_packets.h"
#include "cypher32_crypto.h"

// ─────────────────────────────────────────────
//  CYPHER32 LORA — v60
//
//  Roadmap Phases 0-2 and the protocol half of Phase 4.
//
//  What changed from v29, and why:
//
//   D1  no reliability   → seq numbers, ACKs, retry slot     (T1.1 / T1.2)
//   D3  reply race       → replies deferred via TX queue      (T1.3)
//   D4  blocking TX      → startTransmit() state machine      (T1.4)
//   D6  pure ALOHA       → CAD before every TX                (T1.5)
//   D5  no RX watchdog   → 10 s re-arm + 3-strike re-init     (T1.6)
//   D9  no TX diags      → error codes captured, counters     (T0.2)
//   D10 lying API        → loraSendReconStat() deleted        (T4.5)
//   D7  global RSSI      → per-node signal + rolling average   (T2.1)
//   D8  ghost nodes      → TTL, eviction, rollover-safe ages   (T2.2/T2.3)
//   D11 forgeable        → HMAC-signed frames, replay guard    (T4.1/T4.2)
//   D13 hack never sent  → full HACK_REQ/REPLY exchange        (T4.3)
//
//  NOTE ON THE DIO1 HANDLER
//  v29 used setPacketReceivedAction(). That is an RX-only wrapper around
//  setDio1Action(). Now that TX is non-blocking we need one handler serving
//  both TxDone and RxDone, so we use setDio1Action() directly and track which
//  operation is in flight via radioState. The IRQ mask itself is configured by
//  startReceive() / startTransmit(), so a single handler is correct here.
// ─────────────────────────────────────────────

#define LORA_MOSI  10
#define LORA_MISO  11
#define LORA_SCK    9

// ── Tunables ─────────────────────────────────
#define LORA_DEBUG          1      // 1 = serial trace of every TX/RX (T0.1)
#define TXQ_SIZE            8      // outbound frame queue depth
#define SEEN_RING_SIZE      16     // duplicate-suppression history (T1.1)
#define TX_MAX_TRIES        4      // total attempts per reliable frame (T1.2)
#define TX_RETRY_BASE_MS    400    // + random(0,300) jitter
#define TX_RETRY_JITTER_MS  300
#define REPLY_DELAY_MIN_MS  60     // deferred reply window (T1.3)
#define REPLY_DELAY_JIT_MS  60
#define TX_HARD_TIMEOUT_MS  500    // force recovery if TxDone never fires (T1.4)
#define CAD_MAX_TRIES       5      // then transmit anyway (T1.5)
#define CAD_BACKOFF_MIN_MS  20
#define CAD_BACKOFF_JIT_MS  100
#define RX_WATCHDOG_MS      10000  // re-arm cadence (T1.6)
#define RX_WATCHDOG_STRIKES 3      // failures before full re-init

// Beacon cadence (T1.7 / T2.4). Steady state is 25–35 s; a device speeds up to
// 12–18 s for the first few minutes after boot and whenever a new neighbour
// appears, so joining a group is fast without raising average airtime.
// For the T0.4 bench test, drop the steady pair to 5000 / 5000.
#define LORA_BEACON_MIN_MS       25000
#define LORA_BEACON_MAX_MS       35000
#define LORA_BEACON_FAST_MIN_MS  12000
#define LORA_BEACON_FAST_MAX_MS  18000
#define BEACON_FAST_WINDOW_MS   180000   // how long "fast" lasts (3 min)

// EU 868 g1 duty cycle (T2.5). Legal limit is 1%; we soft-cap below it and
// defer deferrable traffic above the cap so a retry storm cannot push us over.
#define DUTY_BUCKETS         60          // one bucket per minute, rolling hour
#define DUTY_LIMIT_PCT       0.8f
#define NODE_PRUNE_MS        10000UL     // how often to age out nodes (T2.3)

// Packet signing (T4.1). A 4-byte truncated HMAC-SHA256 tag is appended to
// every frame. Set to 0 only for interop debugging — devices with different
// settings cannot talk to each other.
#define LORA_SIGN            1
#define SIG_LEN              4

SPIClass loraSPI(FSPI);
// The Module constructor only stores pin numbers — no SPI access happens until
// radio.begin() inside loraSetup(), so file-scope construction is safe.
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, loraSPI);

// ── Radio state machine (T1.4) ───────────────
enum RadioState { RS_DOWN, RS_RX, RS_TX };
volatile bool loraDioFlag = false;
void IRAM_ATTR loraDioISR() { loraDioFlag = true; }

RadioState radioState = RS_DOWN;
uint32_t   txStartMs  = 0;

// ── Node table ───────────────────────────────
KnownNode knownNodes[MAX_KNOWN_NODES];
int       knownCount = 0;

// ── Status / counters (Phase 0) ──────────────
bool      loraReady           = false;
int       loraLastRSSI        = 0;
float     loraLastSNR         = 0;
String    loraStatus          = "Offline";
int       loraInitError       = 0;
int       loraPktSent         = 0;
int       loraPktRecv         = 0;
int       loraBeaconsSent     = 0;
int       loraLastPktLen      = 0;
uint8_t   loraLastPktType     = 0;
int       loraTxErrors        = 0;   // T0.2
int       loraLastTxError     = 0;
int       loraCrcErrors       = 0;
int       loraDroppedNotForMe = 0;
int       loraDupsDropped     = 0;   // T1.1
int       loraAcksSent        = 0;   // T1.2
int       loraAcksRecv        = 0;
int       loraRetries         = 0;
int       loraTimeouts        = 0;
int       loraCadBusy         = 0;   // T1.5
int       loraTxQueueDrops    = 0;
int       loraWatchdogFires   = 0;   // T1.6
int       loraReinits         = 0;
int       loraNodesEvicted    = 0;   // T2.3
int       loraDutyDeferred    = 0;   // T2.5
int       loraBadSig          = 0;   // T4.1
int       loraReplaysDropped  = 0;   // T4.2

// Beacon scheduling lives here rather than in the sketch's loop() so the
// adaptive logic (T2.4) sits next to the radio it governs.
bool      loraBeaconEnabled   = false;   // sketch sets this once configured
uint32_t  loraNextBeaconMs    = 0;
uint32_t  loraFastUntilMs     = 0;
uint8_t   loraBootBurst       = 0;

String    pendingMsg     = "";
String    pendingMsgFrom = "";
bool      pendingMsgRelayed = false;   // arrived by courier, not in person
uint32_t  pendingCourierFor = 0;       // we just accepted mail for this node

// ── the last thing anyone said ───────────────
// pendingMsg is a doorbell: loop() consumes it, clears it, and the text then
// exists nowhere. KnownNode.msg_inbox keeps a copy per sender but carries no
// timestamp, so it cannot be ordered — and pruneNodes() deletes the row five
// minutes after that sender walks away, which is exactly when you would want
// to read it again. These four survive both.
//
// Set at RX rather than at the consume point, so two messages arriving inside
// one blocked loop() do not lose the first. RAM only, deliberately, for the
// same reason the event log is: NVS writes are what wears out on this board,
// and persisting on inbound message would let anyone in radio range drive
// flash wear by sending you text.
uint32_t  lastMsgFrom  = 0;
String    lastMsgText  = "";
uint32_t  lastMsgAt    = 0;
bool      lastMsgCarried = false;   // arrived as mail, in somebody's pocket
uint32_t  lastSentTo   = 0;
String    lastSentText = "";
uint32_t  lastSentAt   = 0;

// ── Things the sketch wants to know about, queued rather than flagged ──
// A single flag loses the second event when two land close together, and
// meeting two people at once is exactly when you most want to be told.
#define PEER_EVT_QUEUE 6
static uint32_t newNodeQ[PEER_EVT_QUEUE]; static uint8_t newNodeHead = 0, newNodeTail = 0;
static uint32_t scoutedQ[PEER_EVT_QUEUE]; static uint8_t scoutedHead = 0, scoutedTail = 0;

static void qPush(uint32_t* q, uint8_t* head, uint8_t tail, uint32_t id) {
  uint8_t next = (uint8_t)((*head + 1) % PEER_EVT_QUEUE);
  if (next == tail) return;                 // full: drop the newest, keep order
  q[*head] = id; *head = next;
}
static bool qPop(uint32_t* q, uint8_t head, uint8_t* tail, uint32_t* out) {
  if (head == *tail) return false;
  *out = q[*tail]; *tail = (uint8_t)((*tail + 1) % PEER_EVT_QUEUE);
  return true;
}
// A node we had never heard from before just appeared.
bool loraPopNewNode(uint32_t* out)  { return qPop(newNodeQ, newNodeHead, &newNodeTail, out); }
// Someone ran recon against us.
bool loraPopScoutedBy(uint32_t* out){ return qPop(scoutedQ, scoutedHead, &scoutedTail, out); }

// Inbound hack notification for the defender's display. Previously impossible:
// HACK_RESULT was never transmitted by anyone.
bool      pendingHackAlert       = false;
String    pendingHackFrom        = "";
bool      pendingHackAttackerWon = false;

// ── Outbound hack, defender-authoritative (T4.3) ──
// The attacker asks; the DEFENDER rolls and returns the verdict. A modified
// attacker can still lie about its own brute stat, but it cannot simply
// declare itself the winner, which was the actual exploit.
bool      hackInFlight        = false;
uint32_t  hackTargetId        = 0;
bool      hackVerdictReady    = false;   // a verdict arrived, sketch to apply it
bool      hackVerdictWon      = false;
uint8_t   hackVerdictFirewall = 0;
char      hackVerdictFaction  = '?';
bool      hackTimedOut        = false;   // target never answered
// ...but "never" is a claim with a deadline on it, and ours was too short.
//
// The reliable slot gives up after TX_MAX_TRIES tries, 1.6-2.8 s. A defender
// that is mid e-ink refresh answers later than that through no fault of
// anyone's, and the verdict used to land on `if (hackInFlight ...)` — already
// false — and be dropped without a word. resolveHackVerdict() never ran, so
// recordFail() never ran, so no cooldown was recorded: the attacker could
// re-hack at once, forever, and every attempt cost the defender an NVS write.
// Free re-rolls plus attacker-driven flash wear on somebody else's device.
//
// So the slot timing out now only stops the *retries*. We keep listening for
// the verdict, and only call it a miss once this window closes too.
#define HACK_GRACE_MS  6000UL
uint32_t  hackGraceUntil      = 0;
bool      hackVerdictLate     = false;   // arrived after the slot gave up

// ── Staged recon dossier ─────────────────────
// The mini-game needs the target's numbers in hand before the player has
// finished earning them, so a recon probe parks the whole reply here and the
// portal draws down one tier per round it clears. Nothing reaches the browser
// until a round pays for it.
#define RECON_PROBE_IDLE   0
#define RECON_PROBE_WAIT   1
#define RECON_PROBE_READY  2
#define RECON_PROBE_FAILED 3
#define RECON_PROBE_MS     9000UL    // give up on a silent target

struct ReconProbe {
  uint32_t      target;
  uint8_t       state;
  unsigned long deadline;
  bool          charged;    // has this run cost the player an attempt yet
  uint8_t       level, brute, stealth, firewall;
  char          faction;
};
ReconProbe reconProbe = {0, RECON_PROBE_IDLE, 0, false, 0, 0, 0, 0, '?'};

void loraReconProbeStart(uint32_t target) {
  reconProbe.target   = target;
  reconProbe.state    = RECON_PROBE_WAIT;
  reconProbe.deadline = millis() + RECON_PROBE_MS;
  reconProbe.charged  = false;
}
// Answer a probe from local data — the training dummy, which has to teach the
// whole loop with nobody else in the room.
void loraReconProbeLocal(uint32_t target, uint8_t lvl, char fac,
                         uint8_t br, uint8_t st, uint8_t fw) {
  loraReconProbeStart(target);
  reconProbe.level = lvl; reconProbe.faction = fac;
  reconProbe.brute = br;  reconProbe.stealth = st; reconProbe.firewall = fw;
  reconProbe.state = RECON_PROBE_READY;
}
const char* loraReconProbeState() {
  switch (reconProbe.state) {
    case RECON_PROBE_WAIT:   return "wait";
    case RECON_PROBE_READY:  return "ready";
    case RECON_PROBE_FAILED: return "failed";
    default:                 return "idle";
  }
}
// Called from loop(). A probe that never gets an answer has to fail on its own
// clock: the link layer's retries are about frames, not about whether a player
// is sitting in front of a modal waiting to be told the target is out of range.
void loraServiceReconProbe() {
  if (reconProbe.state == RECON_PROBE_WAIT &&
      (long)(millis() - reconProbe.deadline) >= 0)
    reconProbe.state = RECON_PROBE_FAILED;
}

extern uint32_t myChipID32;
extern String   myFaction;
extern int      myLevel;
extern int      skillBrute, skillStealth, skillFirewall;

// ── Action feedback for the portal (T1.2, groundwork for T3.5) ──
enum LoraActionState { LA_IDLE, LA_SENDING, LA_WAITING, LA_SUCCESS, LA_TIMEOUT };
LoraActionState loraActionState = LA_IDLE;
String          loraActionLabel = "";
uint8_t         loraActionTries = 0;

const char* loraActionText() {
  switch (loraActionState) {
    case LA_SENDING: return "SENDING";
    case LA_WAITING: return "WAITING FOR REPLY";
    case LA_SUCCESS: return "SUCCESS";
    case LA_TIMEOUT: return "NO RESPONSE";
    default:         return "IDLE";
  }
}

// ── Debug tracing (T0.1) ─────────────────────
const char* pktTypeName(uint8_t t) {
  switch (t) {
    case PKT_BEACON:      return "BEACON";
    case PKT_RECON_REQ:   return "RECON_REQ";
    case PKT_RECON_REPLY: return "RECON_REPLY";
    case PKT_HACK_REQ:    return "HACK_REQ";
    case PKT_HACK_REPLY:  return "HACK_REPLY";
    case PKT_HACK_RESULT: return "HACK_RESULT";
    case PKT_MSG:         return "MSG";
    case PKT_ACK:         return "ACK";
    case PKT_PING:        return "PING";
    case PKT_ECHO:        return "ECHO";
    case PKT_MAIL:        return "MAIL";
    default:              return "UNKNOWN";
  }
}

#if LORA_DEBUG
  #define LORA_LOG(fmt, ...) Serial.printf("[%8lu] LORA " fmt "\n", millis(), ##__VA_ARGS__)
#else
  #define LORA_LOG(fmt, ...) do {} while (0)
#endif

// ── Recon attempt ledger ─────────────────────
// recon_count lives in the node record, and the node record does not last as
// long as the rule it enforces. findOrAddNode() memsets a slot when it evicts,
// and /api/action?a=clearnodes wipes the whole table — either one silently
// hands back three fresh recon attempts on a target they were already spent
// on. In a room of more than MAX_KNOWN_NODES players that happens by itself,
// without anyone trying to cheat.
//
// The ledger outlives the table, so the budget survives both. RAM only, and
// deliberately: the locks that matter are already NVS-backed in the sketch's
// hackedList/failList, and writing flash on every recon would let anyone in
// radio range drive wear on your device.
#define RECON_LEDGER_SIZE 24
struct ReconLedgerEntry { uint32_t chip_id; uint8_t spent; };
ReconLedgerEntry reconLedger[RECON_LEDGER_SIZE];
uint8_t          reconLedgerIdx = 0;

uint8_t reconLedgerGet(uint32_t chip_id) {
  for (int i = 0; i < RECON_LEDGER_SIZE; i++)
    if (reconLedger[i].chip_id == chip_id) return reconLedger[i].spent;
  return 0;
}
void reconLedgerSet(uint32_t chip_id, uint8_t spent) {
  for (int i = 0; i < RECON_LEDGER_SIZE; i++)
    if (reconLedger[i].chip_id == chip_id) { reconLedger[i].spent = spent; return; }
  reconLedger[reconLedgerIdx].chip_id = chip_id;
  reconLedger[reconLedgerIdx].spent   = spent;
  reconLedgerIdx = (uint8_t)((reconLedgerIdx + 1) % RECON_LEDGER_SIZE);
}

// ── Node helpers ─────────────────────────────
KnownNode* findOrAddNode(uint32_t chip_id) {
  for (int i = 0; i < knownCount; i++)
    if (knownNodes[i].chip_id == chip_id) return &knownNodes[i];
  if (knownCount >= MAX_KNOWN_NODES) {
    // Evict least-recently-seen. Rollover-safe age comparison.
    int oldest = 0;
    uint32_t now = millis();
    for (int i = 1; i < knownCount; i++)
      if ((uint32_t)(now - knownNodes[i].last_seen_ms) >
          (uint32_t)(now - knownNodes[oldest].last_seen_ms)) oldest = i;
    memset(&knownNodes[oldest], 0, sizeof(KnownNode));
    knownNodes[oldest].chip_id     = chip_id;
    knownNodes[oldest].faction     = '?';
    knownNodes[oldest].recon_count = reconLedgerGet(chip_id);
    return &knownNodes[oldest];
  }
  KnownNode* n = &knownNodes[knownCount++];
  memset(n, 0, sizeof(KnownNode));
  n->chip_id = chip_id; n->faction = '?';
  n->recon_count = reconLedgerGet(chip_id);   // a fresh row is not a fresh budget
  return n;
}

KnownNode* findNode(uint32_t chip_id) {
  for (int i = 0; i < knownCount; i++)
    if (knownNodes[i].chip_id == chip_id) return &knownNodes[i];
  return nullptr;
}

String chipIdStr(uint32_t id) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08lx", (unsigned long)id);
  return String(buf);
}

// Record presence and per-node signal for a peer we just heard from
// (T2.1 — fixes D7). Every RX path funnels through here so no packet type can
// forget to update the node's freshness.
KnownNode* touchNode(uint32_t chip_id) {
  bool isNew = (findNode(chip_id) == nullptr);
  KnownNode* n = findOrAddNode(chip_id);
  if (!n) return nullptr;
  if (isNew || n->first_seen_ms == 0) {
    n->first_seen_ms = millis();
    // A new neighbour appearing is exactly when discovery should speed up.
    loraFastUntilMs  = millis() + BEACON_FAST_WINDOW_MS;
    qPush(newNodeQ, &newNodeHead, newNodeTail, chip_id);
  }
  n->last_seen_ms = millis();
  n->rssi  = (int16_t)loraLastRSSI;
  n->snr10 = (int16_t)(loraLastSNR * 10.0f);
  n->rssi_hist[n->rssi_idx] = n->rssi;
  n->rssi_idx = (uint8_t)((n->rssi_idx + 1) % RSSI_HIST);
  if (n->rssi_n < RSSI_HIST) n->rssi_n++;
  return n;
}

// Age out nodes nobody has heard from (T2.3 — fixes D8).
//
// NOTE: eviction drops the node's 12 h hack retry cooldown along with it. The
// 12-hour win lock is safe because it lives in the sketch's persisted hackedList,
// but the retry cooldown is per-node RAM. T4.4 persists it to NVS keyed by chip
// ID so walking out of range cannot be used to clear a failed-hack cooldown.
void pruneNodes() {
  for (int i = 0; i < knownCount; ) {
    if (nodeIsExpired(&knownNodes[i])) {
      LORA_LOG("evicting stale node %08lx (age %lus)",
               (unsigned long)knownNodes[i].chip_id,
               (unsigned long)(ageMs(knownNodes[i].last_seen_ms) / 1000));
      for (int j = i; j < knownCount - 1; j++) knownNodes[j] = knownNodes[j + 1];
      memset(&knownNodes[knownCount - 1], 0, sizeof(KnownNode));
      knownCount--;
      loraNodesEvicted++;
    } else i++;
  }
}

// ─────────────────────────────────────────────
//  T2.5 — EU duty cycle budget
// ─────────────────────────────────────────────
uint16_t dutyBucketMs[DUTY_BUCKETS];
uint8_t  dutyBucketIdx    = 0;
uint32_t dutyBucketStart  = 0;

// LoRa time-on-air, Semtech AN1200.13. BW is in kHz so tSym comes out in ms.
uint32_t loraTimeOnAirMs(int payloadLen) {
  const float tSym = (float)(1UL << LORA_SF) / (float)LORA_BW;
  const int   cr   = LORA_CR - 4;   // 5..8 encodes 4/5..4/8
  const int   de   = 0;             // low-data-rate optimise off at SF7/125 kHz
  const int   ih   = 0;             // explicit header
  const int   crc  = 1;
  float tPreamble = ((float)LORA_PREAMBLE + 4.25f) * tSym;
  int   num = 8 * payloadLen - 4 * LORA_SF + 28 + 16 * crc - 20 * ih;
  int   den = 4 * (LORA_SF - 2 * de);
  int   nPayload = 8;
  if (num > 0) nPayload += (int)ceilf((float)num / (float)den) * (cr + 4);
  return (uint32_t)(tPreamble + (float)nPayload * tSym + 0.5f);
}

static void dutyRotate() {
  if (dutyBucketStart == 0) { dutyBucketStart = millis(); return; }
  while (elapsed(dutyBucketStart, 60000UL)) {
    dutyBucketIdx = (uint8_t)((dutyBucketIdx + 1) % DUTY_BUCKETS);
    dutyBucketMs[dutyBucketIdx] = 0;
    dutyBucketStart += 60000UL;
  }
}

void dutyRecord(uint32_t airMs) {
  dutyRotate();
  uint32_t v = dutyBucketMs[dutyBucketIdx] + airMs;
  dutyBucketMs[dutyBucketIdx] = (uint16_t)(v > 65535 ? 65535 : v);
}

float dutyCyclePct() {
  dutyRotate();
  uint32_t total = 0;
  for (int i = 0; i < DUTY_BUCKETS; i++) total += dutyBucketMs[i];
  return (float)total * 100.0f / 3600000.0f;
}

bool dutyBudgetExceeded() { return dutyCyclePct() >= DUTY_LIMIT_PCT; }

// ─────────────────────────────────────────────
//  T1.1 — duplicate suppression
// ─────────────────────────────────────────────
struct SeenEntry { uint32_t from; uint8_t seq; bool used; };
SeenEntry seenRing[SEEN_RING_SIZE];
int       seenIdx = 0;

bool seenBefore(uint32_t from, uint8_t seq) {
  for (int i = 0; i < SEEN_RING_SIZE; i++)
    if (seenRing[i].used && seenRing[i].from == from && seenRing[i].seq == seq)
      return true;
  return false;
}

void markSeen(uint32_t from, uint8_t seq) {
  seenRing[seenIdx].from = from;
  seenRing[seenIdx].seq  = seq;
  seenRing[seenIdx].used = true;
  seenIdx = (seenIdx + 1) % SEEN_RING_SIZE;
}

// ─────────────────────────────────────────────
//  TX queue (T1.3 / T1.4 / T1.5)
// ─────────────────────────────────────────────
struct TxFrame {
  uint8_t  buf[64];
  uint8_t  len;
  uint32_t sendAfterMs;
  uint8_t  cadTries;
  bool     active;
  bool     urgent;     // false = deferrable when over the duty budget (T2.5)
};
TxFrame txq[TXQ_SIZE];

uint8_t txSeq = 0;   // rolling per-sender counter

// urgent: anything a player is waiting on, plus ACKs and replies. Beacons are
// not urgent — they are the one thing worth delaying to stay inside the budget.
bool enqueueTx(const void* pkt, int len, uint32_t delayMs = 0, bool urgent = true) {
  if (len <= 0 || len > (int)(64 - SIG_LEN)) return false;
  for (int i = 0; i < TXQ_SIZE; i++) {
    if (txq[i].active) continue;
    memcpy(txq[i].buf, pkt, len);
    int total = len;
#if LORA_SIGN
    // Append the truncated tag over the whole packet (T4.1). Signing happens
    // here rather than at transmit time so retries re-sign identically.
    uint8_t tag[32];
    hmacSha256(LORA_KEY, sizeof(LORA_KEY), txq[i].buf, (size_t)len, tag);
    memcpy(txq[i].buf + len, tag, SIG_LEN);
    total = len + SIG_LEN;
#endif
    txq[i].len         = (uint8_t)total;
    txq[i].sendAfterMs = millis() + delayMs;
    txq[i].cadTries    = 0;
    txq[i].active      = true;
    txq[i].urgent      = urgent;
    return true;
  }
  loraTxQueueDrops++;
  LORA_LOG("TXQ FULL — frame dropped");
  return false;
}

int txQueueDepth() {
  int n = 0;
  for (int i = 0; i < TXQ_SIZE; i++) if (txq[i].active) n++;
  return n;
}

// Is any queued frame due to go out within the next `ms`? Distinct from
// txQueueDepth(), which counts frames parked for a minute by the duty-cycle
// deferral just the same as one that is ready now.
static bool txQueueDueWithin(uint32_t ms) {
  uint32_t now = millis();
  for (int i = 0; i < TXQ_SIZE; i++)
    if (txq[i].active && (int32_t)(now + ms - txq[i].sendAfterMs) >= 0) return true;
  return false;
}

// Push what is queued onto the air before the caller goes deaf.
//
// display.update() busy-waits on the panel's BUSY line for about two seconds,
// and serviceTxQueue() only runs from loraTick() <- loop(). So a reply queued
// during the same pass that raises an event screen sits in the queue for the
// whole refresh — and that is long enough to lose a race the *other* device is
// timing. A defender's HACK_REPLY is deferred 60-120 ms by deferReply() and
// then held behind the alert screen, while the attacker's four tries expire at
// TX_MAX_TRIES * (TX_RETRY_BASE_MS + jitter) = 1.6-2.8 s. The attacker is told
// NO RESPONSE by a defender that answered immediately.
//
// Called before every blocking panel write. Cheap when there is nothing to do.
void loraTick();          // defined below; loraFlushTx is the only early caller
void loraFlushTx(uint32_t budgetMs) {
  if (!loraReady) return;
  uint32_t t0 = millis();
  for (;;) {
    uint32_t spent = (uint32_t)(millis() - t0);
    if (spent >= budgetMs) return;
    if (radioState != RS_TX && !txQueueDueWithin(budgetMs - spent)) return;
    loraTick();
    delayMicroseconds(300);
  }
}

// ─────────────────────────────────────────────
//  T1.2 — reliable send slot (one in flight)
// ─────────────────────────────────────────────
struct PendingTx {
  uint8_t  buf[64];
  uint8_t  len;
  uint32_t to_id;
  uint8_t  seq;
  uint8_t  type;
  uint8_t  triesLeft;
  uint32_t nextAttemptMs;
  bool     active;
};

// Two slots, deliberately — the roadmap called for one.
//
// One is right for the local player: you never have two hacks in flight. But
// replies carry the game payload, so they are retried too (see deferReply),
// and a device answering someone else's recon can have its own player press a
// button a moment later. With a single slot that action would evict the reply
// mid-retry. The requester has already had its request ACKed, so it never asks
// again — and sits on SUCCESS with no stat to show. That is the exact failure
// this phase exists to remove, so the two kinds of traffic get separate slots.
PendingTx pendingUser  = {};   // local player's action — drives portal status
PendingTx pendingReply = {};   // answering a peer — silent, never touches the UI

// Fire-and-forget. Used for broadcasts (beacons) and ACKs.
bool loraSendUnreliable(void* pkt, int len, bool urgent = true) {
  if (!loraReady) return false;
  PktHeader* h = (PktHeader*)pkt;
  h->seq = txSeq++;
  return enqueueTx(pkt, len, 0, urgent);
}

// Arm one slot with a frame and hand it to the TX queue.
static bool armSlot(PendingTx& slot, void* pkt, int len, uint32_t delayMs) {
  PktHeader* h = (PktHeader*)pkt;
  h->flags |= PKTFLAG_ACK_REQ;
  memcpy(slot.buf, pkt, len);
  slot.len           = (uint8_t)len;
  slot.to_id         = h->to_id;
  slot.seq           = h->seq;
  slot.type          = h->type;
  slot.triesLeft     = TX_MAX_TRIES - 1;
  slot.nextAttemptMs = millis() + delayMs + TX_RETRY_BASE_MS + random(0, TX_RETRY_JITTER_MS);
  slot.active        = true;
  return enqueueTx(pkt, len, delayMs);
}

// Reliable unicast for the local player's action: ACK requested, retried up to
// TX_MAX_TRIES, outcome shown in the portal.
bool loraSendReliable(void* pkt, int len, const char* label) {
  // Must match enqueueTx()'s cap exactly. It used to read 64, so a 61-64 byte
  // packet passed here, set the action to SENDING, and was then silently
  // refused by the queue — the player would watch it time out as NO RESPONSE
  // with nothing ever having been transmitted. Unreachable today at 44 bytes
  // max, but any packet designed against "64" would land in it.
  if (!loraReady || len <= 0 || len > (int)(64 - SIG_LEN)) return false;
  PktHeader* h = (PktHeader*)pkt;
  h->seq = txSeq++;

  loraActionState = LA_SENDING;
  loraActionLabel = String(label);
  loraActionTries = 1;

  LORA_LOG("TX-REL %s seq=%u to=%08lx len=%d", pktTypeName(h->type),
           h->seq, (unsigned long)h->to_id, len);
  return armSlot(pendingUser, pkt, len, 0);
}

static void clearSlot(PendingTx& slot, bool success) {
  if (!slot.active) return;
  slot.active = false;
  if (&slot == &pendingUser) loraActionState = success ? LA_SUCCESS : LA_TIMEOUT;
  // A hack whose target never answered must not leave the sketch waiting.
  if (!success && slot.type == PKT_HACK_REQ && hackInFlight) {
    hackInFlight   = false;
    hackTimedOut   = true;
    hackGraceUntil = millis() + HACK_GRACE_MS;   // keep listening a little longer
  }
  if (!success) {
    loraTimeouts++;
    LORA_LOG("TIMEOUT %s seq=%u — no ACK after %d tries",
             pktTypeName(slot.type), slot.seq, TX_MAX_TRIES);
  }
}

// True while the player's own action is awaiting its ACK — the portal polls this.
bool loraActionPending() { return pendingUser.active; }

// ─────────────────────────────────────────────
//  Packet constructors
// ─────────────────────────────────────────────
static void fillHdr(PktHeader* h, uint8_t type, uint32_t to) {
  h->type    = type;
  h->seq     = 0;      // assigned at send time
  h->flags   = 0;
  h->from_id = myChipID32;
  h->to_id   = to;
}

void loraSendBeacon() {
  if (!loraReady) return;
  PktBeacon pkt;
  fillHdr(&pkt.hdr, PKT_BEACON, 0);
  pkt.level   = (uint8_t)myLevel;
  pkt.faction = myFaction.length() > 0 ? myFaction.charAt(0) : '?';
  if (loraSendUnreliable(&pkt, sizeof(pkt), /*urgent=*/false)) loraBeaconsSent++;
}

void loraSendRecon(uint32_t target_id) {
  PktReconReq pkt;
  fillHdr(&pkt.hdr, PKT_RECON_REQ, target_id);
  loraSendReliable(&pkt, sizeof(pkt), "RECON");
}

void loraSendMsg(uint32_t target_id, const char* text) {
  PktMsg pkt;
  fillHdr(&pkt.hdr, PKT_MSG, target_id);
  strncpy(pkt.text, text, 32); pkt.text[32] = '\0';
  loraSendReliable(&pkt, sizeof(pkt), "MESSAGE");
}

// One implementation of the odds, used by both sides so the defender's roll
// matches what the attacker's UI predicted.
//
// Brute is contested — it is measured against the defender's firewall, so it
// swings hard both ways and is the stat for cracking hard targets. Stealth is
// uncontested and worth half as much per point, but firewall can never cancel
// it, which makes it the reliable investment. Recon is the tactical lever you
// pull per target.
//
// Stealth used to feed only the attacker's own second roll, which the defender
// never saw, so it had no bearing on whether a hack landed at all.
// attackerRecon is the sequence-memory score for this target, 0..RECON_MAX_SEQ.
// A perfect run is worth RECON_MAX_BONUS, which is what three button-press
// recons used to be worth, so the curve is unchanged — it is just earned now.
int loraHackChancePct(int attackerBrute, int attackerRecon,
                      int attackerStealth, int defenderFirewall) {
  if (attackerRecon < 0)              attackerRecon = 0;
  if (attackerRecon > RECON_MAX_SEQ)  attackerRecon = RECON_MAX_SEQ;
  int pct = 60
          + (attackerRecon * RECON_MAX_BONUS) / RECON_MAX_SEQ
          + (attackerBrute - defenderFirewall) * 2
          + attackerStealth;
  if (pct < 25) pct = 25;
  if (pct > 90) pct = 90;
  return pct;
}

// Begin a hack. The verdict arrives later in PKT_HACK_REPLY (T4.3).
void loraHackStart(uint32_t target_id, int reconScore) {
  PktHackReq pkt;
  fillHdr(&pkt.hdr, PKT_HACK_REQ, target_id);
  pkt.brute       = (uint8_t)skillBrute;
  pkt.recon_score = (uint8_t)(reconScore < 0 ? 0 :
                              reconScore > RECON_MAX_SEQ ? RECON_MAX_SEQ : reconScore);
  pkt.stealth     = (uint8_t)skillStealth;
  hackInFlight     = true;
  hackTargetId     = target_id;
  hackVerdictReady = false;
  hackTimedOut     = false;
  loraSendReliable(&pkt, sizeof(pkt), "HACK");
}

void loraSendHackResult(uint32_t defender_id, bool won, int8_t xp) {
  PktHackResult pkt;
  fillHdr(&pkt.hdr, PKT_HACK_RESULT, defender_id);
  pkt.outcome  = won ? HACK_WIN : HACK_LOSE;
  pkt.xp_delta = xp;
  loraSendReliable(&pkt, sizeof(pkt), "HACK RESULT");
}

void loraSendPing(uint32_t target_id) {
  PktPing pkt;
  fillHdr(&pkt.hdr, PKT_PING, target_id);
  loraSendReliable(&pkt, sizeof(pkt), "PING");
}

// ─────────────────────────────────────────────
//  ECHO — presence one hop past the edge of hearing
// ─────────────────────────────────────────────
//  What an echo is: a neighbour told us it can hear this device. That is the
//  entire content. No level, no faction, no stats, no signal — a relayed RSSI
//  would be the relay's distance, not ours, and reporting it as the origin's
//  would put "VERY CLOSE" on somebody two kilometres away and sort them above
//  the person standing in front of you.
//
//  Echoes live here and not in knownNodes[] on purpose. Every rule in the game
//  that means "near me" is written as findNode() returning non-null — the hack
//  gate, recon, the census, the leaderboard, statMet, the discovery chime,
//  backdoor refresh. Adding a hops field to KnownNode would leave one boolean
//  between all of that and a player farming XP off someone they have never
//  met, and every one of those call sites would need its own check. A separate
//  table makes it unconstructable instead of merely forbidden.
#define ECHO_MAX_NODES     12
#define ECHO_TTL_MS        360000UL   // second-hand news goes stale faster
#define ECHO_INTERVAL_MS   120000UL   // one frame per node per two minutes
#define ECHO_JITTER_MS     20000UL

struct EchoNode {
  uint32_t chip_id;      // who
  uint32_t via;          // which of OUR direct neighbours can hear them
  uint32_t heard_ms;     // when that neighbour last told us so
};
EchoNode echoNodes[ECHO_MAX_NODES];
int      echoCount = 0;
uint32_t loraNextEchoMs = 0;
int      loraEchoesSent = 0;
int      loraEchoesRecv = 0;

EchoNode* findEcho(uint32_t chip_id) {
  for (int i = 0; i < echoCount; i++)
    if (echoNodes[i].chip_id == chip_id) return &echoNodes[i];
  return nullptr;
}

// Record "via can hear chip_id". Silently ignores anyone we can hear ourselves
// — a direct contact is never downgraded to a rumour — and ourselves.
void echoRecord(uint32_t chip_id, uint32_t via) {
  if (chip_id == 0 || chip_id == myChipID32 || chip_id == via) return;
  if (findNode(chip_id)) return;              // we hear them directly; not news
  EchoNode* e = findEcho(chip_id);
  if (!e) {
    if (echoCount >= ECHO_MAX_NODES) {
      int oldest = 0;
      uint32_t now = millis();
      for (int i = 1; i < echoCount; i++)
        if ((uint32_t)(now - echoNodes[i].heard_ms) >
            (uint32_t)(now - echoNodes[oldest].heard_ms)) oldest = i;
      e = &echoNodes[oldest];
    } else {
      e = &echoNodes[echoCount++];
    }
    e->chip_id = chip_id;
  }
  e->via = via;
  e->heard_ms = millis();
}

void pruneEchoes() {
  for (int i = 0; i < echoCount; ) {
    // Drop it once it is stale, and also the moment we can hear them for
    // ourselves — at that point the rumour is not just redundant, it is a
    // worse copy of something true.
    if ((uint32_t)(millis() - echoNodes[i].heard_ms) >= ECHO_TTL_MS ||
        findNode(echoNodes[i].chip_id)) {
      for (int j = i; j < echoCount - 1; j++) echoNodes[j] = echoNodes[j + 1];
      echoCount--;
    } else i++;
  }
}

// Which of our direct neighbours claims it can reach `target`, if any? This is
// the entire routing table: one hop, chosen, never broadcast.
uint32_t echoCarrierFor(uint32_t target) {
  EchoNode* e = findEcho(target);
  if (!e) return 0;
  KnownNode* via = findNode(e->via);
  if (!via || !nodeIsActive(via)) return 0;   // the bridge walked away
  return e->via;
}

void loraSendEcho() {
  if (!loraReady || knownCount == 0) return;
  PktEcho pkt;
  fillHdr(&pkt.hdr, PKT_ECHO, 0);
  memset(pkt.peers, 0, sizeof(pkt.peers));
  uint8_t n = 0;
  // Only nodes we can hear right now. A fading contact is not a bridge, and
  // announcing one would send mail to a carrier that cannot deliver it.
  for (int i = 0; i < knownCount && n < ECHO_MAX_PEERS; i++)
    if (nodeIsActive(&knownNodes[i])) pkt.peers[n++] = knownNodes[i].chip_id;
  if (n == 0) return;
  pkt.count = n;
  if (loraSendUnreliable(&pkt, sizeof(pkt), /*urgent=*/false)) loraEchoesSent++;
}

static void serviceEcho() {
  if (!loraBeaconEnabled || !loraReady) return;
  if (loraNextEchoMs == 0) { loraNextEchoMs = millis() + ECHO_INTERVAL_MS; return; }
  if ((int32_t)(millis() - loraNextEchoMs) < 0) return;
  loraSendEcho();
  loraNextEchoMs = millis() + ECHO_INTERVAL_MS + random(0, ECHO_JITTER_MS);
}

// ─────────────────────────────────────────────
//  MAIL — a message that travels in somebody's pocket
// ─────────────────────────────────────────────
//  Not a relay. Nothing is forwarded in real time and nothing is flooded: a
//  message waits in the outbox until either the recipient is in direct range,
//  or a neighbour that an echo says CAN reach them is in direct range. Then it
//  goes to exactly one device.
//
//  So the transport is the player walking somewhere, which is the one routing
//  protocol this game can afford — and the only one that makes the mesh a
//  reason to move rather than a reason to sit still.
#define MAIL_SLOTS        8
#define MAIL_TTL_MS       1800000UL   // half an hour, then it never arrives
#define MAIL_RETRY_MS     15000UL     // how often one item may try again
#define MAIL_MAX_TRIES    12

struct MailItem {
  uint32_t final_id;     // who it is for
  uint32_t origin_id;    // who wrote it
  char     text[MAIL_TEXT_MAX + 1];
  uint32_t createdAt;
  uint32_t lastTryAt;
  uint8_t  tries;
  uint8_t  seq;          // seq of the frame in flight, for ACK matching
  bool     awaitingAck;
  bool     carried;      // we are couriering this for somebody else
  bool     active;
};
MailItem mailbox[MAIL_SLOTS];
int      loraMailQueued    = 0;
int      loraMailDelivered = 0;
int      loraMailHandedOff = 0;
int      loraMailExpired   = 0;
int      loraMailDropped   = 0;

int mailPending() {
  int n = 0;
  for (int i = 0; i < MAIL_SLOTS; i++) if (mailbox[i].active) n++;
  return n;
}
int mailCarriedCount() {
  int n = 0;
  for (int i = 0; i < MAIL_SLOTS; i++)
    if (mailbox[i].active && mailbox[i].carried) n++;
  return n;
}

// Returns false when the outbox is full, so the caller can say so rather than
// accepting a message that silently goes nowhere.
bool mailQueue(uint32_t final_id, uint32_t origin_id, const char* text, bool carried) {
  if (final_id == 0 || final_id == myChipID32) return false;
  for (int i = 0; i < MAIL_SLOTS; i++) {
    MailItem& m = mailbox[i];
    // Same author, same recipient, same words: a duplicate handed to us twice
    // by two carriers is one message, not two.
    if (m.active && m.final_id == final_id && m.origin_id == origin_id &&
        strncmp(m.text, text, MAIL_TEXT_MAX) == 0) return true;
  }
  for (int i = 0; i < MAIL_SLOTS; i++) {
    MailItem& m = mailbox[i];
    if (m.active) continue;
    memset(&m, 0, sizeof(m));
    m.final_id = final_id; m.origin_id = origin_id;
    strncpy(m.text, text, MAIL_TEXT_MAX); m.text[MAIL_TEXT_MAX] = '\0';
    m.createdAt = millis();
    m.lastTryAt = millis() - MAIL_RETRY_MS;   // eligible immediately
    m.carried = carried; m.active = true;
    loraMailQueued++;
    return true;
  }
  loraMailDropped++;
  return false;
}

// Someone's mail reached its addressee, or we handed it on. Either way it
// stops being our problem.
static void mailClear(MailItem& m, bool handedOff) {
  if (!m.active) return;
  if (handedOff) loraMailHandedOff++; else loraMailDelivered++;
  m.active = false; m.awaitingAck = false;
}

// A mail frame we sent has been acknowledged by whoever we sent it to.
static bool mailAckMatch(uint32_t from, uint8_t seq) {
  for (int i = 0; i < MAIL_SLOTS; i++) {
    MailItem& m = mailbox[i];
    if (!m.active || !m.awaitingAck || m.seq != seq) continue;
    bool direct = (from == m.final_id);
    if (!direct && from != echoCarrierFor(m.final_id)) continue;
    LORA_LOG("MAIL %s by %08lx: \"%s\"", direct ? "delivered" : "handed to",
             (unsigned long)from, m.text);
    mailClear(m, !direct);
    return true;
  }
  return false;
}

static void serviceMail() {
  if (!loraReady) return;
  for (int i = 0; i < MAIL_SLOTS; i++) {
    MailItem& m = mailbox[i];
    if (!m.active) continue;
    if ((uint32_t)(millis() - m.createdAt) >= MAIL_TTL_MS ||
        m.tries >= MAIL_MAX_TRIES) {
      LORA_LOG("MAIL expired for %08lx", (unsigned long)m.final_id);
      m.active = false; loraMailExpired++;
      continue;
    }
    if ((uint32_t)(millis() - m.lastTryAt) < MAIL_RETRY_MS) continue;

    // Prefer the recipient in person; fall back to a neighbour who says they
    // can reach them. A message we are already carrying is never handed on a
    // second time — two hops is the whole budget, and without that rule a
    // message could circulate between two carriers indefinitely.
    uint32_t hop = 0; bool direct = false;
    KnownNode* dest = findNode(m.final_id);
    if (dest && nodeIsActive(dest)) { hop = m.final_id; direct = true; }
    else if (!m.carried)            { hop = echoCarrierFor(m.final_id); }
    if (hop == 0) continue;                       // nobody to give it to yet

    if (dutyBudgetExceeded()) continue;           // it can wait; it always could

    PktMail pkt;
    fillHdr(&pkt.hdr, PKT_MAIL, hop);
    pkt.final_id  = m.final_id;
    pkt.origin_id = m.origin_id;
    pkt.carried   = m.carried ? 1 : 0;
    strncpy(pkt.text, m.text, MAIL_TEXT_MAX); pkt.text[MAIL_TEXT_MAX] = '\0';
    pkt.hdr.seq   = txSeq++;
    pkt.hdr.flags = PKTFLAG_ACK_REQ;   // the ACK is the delivery receipt

    m.seq = pkt.hdr.seq;
    m.lastTryAt = millis();
    m.tries++;
    m.awaitingAck = enqueueTx(&pkt, sizeof(pkt), 0, /*urgent=*/false);
    LORA_LOG("MAIL %s to %08lx for %08lx (try %u)",
             direct ? "direct" : "via carrier", (unsigned long)hop,
             (unsigned long)m.final_id, m.tries);
    return;                            // one mail frame per pass, deliberately
  }
}


// Replies are queued with a short delay so the requester has finished
// re-arming RX before the answer lands (T1.3 — fixes D3).
//
// Replies carry the game payload — the stat, the firewall value — so losing
// one silently is worse than losing the request. The request is retried and
// ACKed, which would leave the requester showing SUCCESS with nothing to show
// for it. So a reply goes out reliably too, using this device's own slot.
// If that slot is already busy we fall back to unreliable rather than
// discarding somebody else's in-flight action.
static void deferReply(void* pkt, int len) {
  PktHeader* h = (PktHeader*)pkt;
  uint32_t delayMs = REPLY_DELAY_MIN_MS + random(0, REPLY_DELAY_JIT_MS);
  h->seq = txSeq++;

  // Reply slot free: send it reliably so a dropped answer gets retried.
  // Already answering someone else: fall back to a single unreliable send
  // rather than abandoning the earlier peer mid-retry.
  if (!pendingReply.active) { armSlot(pendingReply, pkt, len, delayMs); return; }
  enqueueTx(pkt, len, delayMs);
}

static void sendAck(uint32_t to_id, uint8_t seq, uint8_t ackedType) {
  PktAck ack;
  fillHdr(&ack.hdr, PKT_ACK, to_id);
  ack.hdr.seq   = seq;                 // echo the seq being acknowledged
  ack.hdr.flags = PKTFLAG_IS_ACK;
  ack.ack_type  = ackedType;
  // Same jitter deferReply() uses. Two devices answering different senders in
  // the same instant would otherwise both schedule for exactly REPLY_DELAY_MIN_MS,
  // CAD together, and collide — then back off into the same window and do it
  // again. Rare today; guaranteed the moment anything raises the frame rate.
  enqueueTx(&ack, sizeof(ack), REPLY_DELAY_MIN_MS + random(0, REPLY_DELAY_JIT_MS));
  loraAcksSent++;
}

// Verify and strip the 4-byte tag (T4.1). Returns false for a forged or
// truncated frame; on success *lenInOut drops to the real packet length.
bool loraVerifyFrame(const uint8_t* buf, int* lenInOut) {
#if LORA_SIGN
  int len = *lenInOut;
  if (len <= (int)SIG_LEN) { loraBadSig++; return false; }
  uint8_t tag[32];
  hmacSha256(LORA_KEY, sizeof(LORA_KEY), buf, (size_t)(len - SIG_LEN), tag);
  if (memcmp(tag, buf + len - SIG_LEN, SIG_LEN) != 0) {
    loraBadSig++;
    LORA_LOG("BAD SIGNATURE — frame rejected (len=%d)", len);
    return false;
  }
  *lenInOut = len - SIG_LEN;
#endif
  return true;
}

// ─────────────────────────────────────────────
//  RX dispatch
// ─────────────────────────────────────────────
void loraHandlePacket(uint8_t* buf, int len) {
  loraLastPktLen = len;
  if (len < (int)sizeof(PktHeader)) return;
  PktHeader* hdr = (PktHeader*)buf;
  loraLastPktType = hdr->type;

  if (hdr->from_id == myChipID32) return;          // our own echo
  if (hdr->to_id != 0 && hdr->to_id != myChipID32) {
    loraDroppedNotForMe++;
    return;
  }
  loraPktRecv++;

  LORA_LOG("RX %s seq=%u from=%08lx len=%d rssi=%d snr=%.1f",
           pktTypeName(hdr->type), hdr->seq, (unsigned long)hdr->from_id,
           len, loraLastRSSI, (double)loraLastSNR);

  // ── ACK handling (T1.2) ──
  if (hdr->flags & PKTFLAG_IS_ACK) {
    loraAcksRecv++;
    PktHeader* a = hdr;
    // The reliable slots get first refusal. txSeq is one counter shared by
    // every frame we send, so a mail frame and a player action to the same peer
    // can collide on a seq once it wraps — and if mail claimed the ACK first,
    // the action the player is actually watching would sit there and time out.
    // Mail can afford to be wrong about a receipt; it retries from the outbox
    // fifteen seconds later. The hack in front of them cannot.
    bool claimed = false;
    for (PendingTx* s : { &pendingUser, &pendingReply }) {
      if (s->active && s->to_id == a->from_id && s->seq == a->seq) {
        LORA_LOG("ACK matched seq=%u — %s delivered", a->seq, pktTypeName(s->type));
        clearSlot(*s, true);
        claimed = true;
        break;
      }
    }
    // Mail carries no retry slot — the outbox is its retry mechanism, on a
    // timescale that suits something waiting for a person to walk somewhere.
    // So the ACK is the delivery receipt, and matching it is what takes the
    // message out of the bag.
    if (!claimed) mailAckMatch(a->from_id, a->seq);
    return;
  }

  // ── Duplicate suppression (T1.1) ──
  // A duplicate means our previous ACK was lost, so re-ACK it — but do not
  // process the payload twice.
  if (seenBefore(hdr->from_id, hdr->seq)) {
    loraDupsDropped++;
    if (hdr->flags & PKTFLAG_ACK_REQ) sendAck(hdr->from_id, hdr->seq, hdr->type);
    LORA_LOG("DUP seq=%u from=%08lx — dropped, re-ACKed", hdr->seq,
             (unsigned long)hdr->from_id);
    return;
  }
  markSeen(hdr->from_id, hdr->seq);

  // Any unicast asking for an ACK gets one, whatever the type. The to_id test
  // is the load-bearing half: broadcasts reach every device in range, so a
  // broadcast with this flag set would draw a simultaneous ACK from all of
  // them. The comment claimed "unicast" long before the code checked for it.
  if ((hdr->flags & PKTFLAG_ACK_REQ) && hdr->to_id != 0)
    sendAck(hdr->from_id, hdr->seq, hdr->type);

  switch (hdr->type) {
    case PKT_BEACON: {
      if (len < (int)sizeof(PktBeacon)) return;
      PktBeacon* p = (PktBeacon*)buf;
      KnownNode* n = touchNode(p->hdr.from_id);
      if (!n) return;
      n->level = p->level; n->faction = (char)p->faction;
      break;
    }
    case PKT_RECON_REQ: {
      if (len < (int)sizeof(PktReconReq)) return;
      PktReconReply reply;
      fillHdr(&reply.hdr, PKT_RECON_REPLY, hdr->from_id);
      reply.level    = (uint8_t)myLevel;
      reply.faction  = myFaction.length() > 0 ? myFaction.charAt(0) : '?';
      reply.brute    = (uint8_t)skillBrute;
      reply.stealth  = (uint8_t)skillStealth;
      reply.firewall = (uint8_t)skillFirewall;
      deferReply(&reply, sizeof(reply));
      touchNode(hdr->from_id);
      qPush(scoutedQ, &scoutedHead, scoutedTail, hdr->from_id);   // they scouted us
      break;
    }
    case PKT_RECON_REPLY: {
      if (len < (int)sizeof(PktReconReply)) return;
      PktReconReply* p = (PktReconReply*)buf;
      KnownNode* n = touchNode(p->hdr.from_id);
      if (!n) return;
      n->level   = p->level;
      n->faction = (char)p->faction;
      // Stage it. Nothing is written into the node record here — the tiers the
      // player earns do that, one round at a time.
      if (reconProbe.state == RECON_PROBE_WAIT &&
          reconProbe.target == p->hdr.from_id) {
        reconProbe.level    = p->level;
        reconProbe.faction  = (char)p->faction;
        reconProbe.brute    = p->brute;
        reconProbe.stealth  = p->stealth;
        reconProbe.firewall = p->firewall;
        reconProbe.state    = RECON_PROBE_READY;
        LORA_LOG("recon dossier from %08lx: L%u %c b%u s%u f%u",
                 (unsigned long)p->hdr.from_id, p->level, (char)p->faction,
                 p->brute, p->stealth, p->firewall);
      }
      break;
    }
    case PKT_HACK_REQ: {
      if (len < (int)sizeof(PktHackReq)) return;
      PktHackReq* p = (PktHackReq*)buf;

      // T4.3 — the defender rolls. The attacker only supplies its own stats.
      // The score arrives from the attacker, so clamp it here. Everything
      // else in this packet is self-reported too — the HMAC keeps strangers
      // out, it does not make a device honest about its own stats.
      int  claimed      = p->recon_score > RECON_MAX_SEQ ? RECON_MAX_SEQ
                                                         : p->recon_score;
      int  pct          = loraHackChancePct(p->brute, claimed,
                                            p->stealth, skillFirewall);
      bool attackerWins = (random(0, 100) < pct);

      PktHackReply reply;
      fillHdr(&reply.hdr, PKT_HACK_REPLY, hdr->from_id);
      reply.outcome  = attackerWins ? HACK_WIN : HACK_LOSE;
      reply.firewall = (uint8_t)skillFirewall;
      reply.faction  = myFaction.length() > 0 ? myFaction.charAt(0) : '?';
      deferReply(&reply, sizeof(reply));
      touchNode(hdr->from_id);

      LORA_LOG("HACK from %08lx brute=%u stealth=%u reconSeq=%d vs fw=%d -> %d%% -> %s",
               (unsigned long)hdr->from_id, p->brute, p->stealth, claimed,
               skillFirewall, pct, attackerWins ? "THEY WIN" : "HELD");

      // Alert here rather than on HACK_RESULT: a modified attacker can decline
      // to send HACK_RESULT, but cannot stop us knowing we were attacked.
      // Someone who kicks your door in has introduced themselves. Free name,
      // no odds bonus — reconAtLeast() exists to keep those two apart.
      reconAtLeast(findNode(hdr->from_id), RECON_T_NAME);

      pendingHackAlert       = true;
      pendingHackFrom        = chipIdStr(hdr->from_id);
      pendingHackAttackerWon = attackerWins;
      break;
    }
    case PKT_HACK_REPLY: {
      if (len < (int)sizeof(PktHackReply)) return;
      PktHackReply* p = (PktHackReply*)buf;
      KnownNode* n = touchNode(p->hdr.from_id);
      if (!n) return;
      n->faction      = (char)p->faction;
      n->seen_firewall = p->firewall;   // straight from the fight; keeps a
                                        // backdoored node's number current
      // You engaged them, so you know who and what they are. The rest of the
      // dossier still has to be played for.
      reconAtLeast(n, RECON_T_FACTION);
      // Accept it while the hack is live, and also during the grace window
      // after the retries gave up — the defender rolled either way, and a
      // verdict we drop is a hack that resolves on their device and not on
      // ours. hackTargetId is not cleared on timeout, so it still names them.
      bool late = (!hackInFlight && hackTimedOut &&
                   (int32_t)(millis() - hackGraceUntil) < 0);
      if ((hackInFlight || late) && hackTargetId == p->hdr.from_id) {
        hackInFlight        = false;
        hackTimedOut        = false;      // it answered after all
        hackVerdictLate     = late;
        hackVerdictReady    = true;
        hackVerdictWon      = (p->outcome == HACK_WIN);
        hackVerdictFirewall = p->firewall;
        hackVerdictFaction  = (char)p->faction;
        LORA_LOG("hack verdict from %08lx: %s%s",
                 (unsigned long)p->hdr.from_id, hackVerdictWon ? "WON" : "LOST",
                 late ? " (late — rescued from the grace window)" : "");
      }
      break;
    }
    case PKT_HACK_RESULT: {
      if (len < (int)sizeof(PktHackResult)) return;
      PktHackResult* p = (PktHackResult*)buf;
      (void)p;
      KnownNode* n = touchNode(hdr->from_id);
      // T4.2 — a captured winning packet replayed later must grant nothing.
      // seq is a rolling byte, so compare with signed wraparound arithmetic.
      if (n && n->have_result_seq && (int8_t)(hdr->seq - n->last_result_seq) <= 0) {
        loraReplaysDropped++;
        LORA_LOG("REPLAY HACK_RESULT seq=%u from %08lx — rejected",
                 hdr->seq, (unsigned long)hdr->from_id);
        break;
      }
      if (n) { n->last_result_seq = hdr->seq; n->have_result_seq = true; }
      // The alert already fired when we handled HACK_REQ and rolled the
      // outcome ourselves; this packet only carries the attacker's XP delta.
      break;
    }
    case PKT_MSG: {
      if (len < (int)sizeof(PktMsg)) return;
      PktMsg* p = (PktMsg*)buf;
      KnownNode* n = touchNode(p->hdr.from_id);
      if (n) {
        strncpy(n->msg_inbox, p->text, 32); n->msg_inbox[32] = '\0';
        n->msg_unread = true;
        lastMsgFrom = p->hdr.from_id;
        lastMsgText = String(p->text);
        lastMsgAt   = millis();
        lastMsgCarried = false;
        reconAtLeast(n, RECON_T_NAME);   // they signed the message by sending it
        pendingMsg = String(p->text); pendingMsgFrom = chipIdStr(p->hdr.from_id);
      }
      break;
    }
    case PKT_PING:
      touchNode(hdr->from_id);
      break;   // the ACK above is the entire point of a ping

    case PKT_ECHO: {
      if (len < (int)sizeof(PktEcho)) return;
      PktEcho* p = (PktEcho*)buf;
      // The SENDER is a direct contact — we just heard their radio — so they
      // get the full touchNode treatment including RSSI. The devices they name
      // get none of it. That asymmetry is the whole point of the packet.
      touchNode(p->hdr.from_id);
      loraEchoesRecv++;
      uint8_t n = p->count > ECHO_MAX_PEERS ? ECHO_MAX_PEERS : p->count;
      for (uint8_t i = 0; i < n; i++)
        echoRecord(p->peers[i], p->hdr.from_id);
      LORA_LOG("ECHO from %08lx naming %u peer(s)",
               (unsigned long)p->hdr.from_id, n);
      break;
    }

    case PKT_MAIL: {
      if (len < (int)sizeof(PktMail)) return;
      PktMail* p = (PktMail*)buf;
      touchNode(p->hdr.from_id);
      char body[MAIL_TEXT_MAX + 1];
      strncpy(body, p->text, MAIL_TEXT_MAX); body[MAIL_TEXT_MAX] = '\0';

      if (p->final_id == myChipID32) {
        // It arrived. Credit the author, not whoever happened to carry it —
        // the courier did not write it and must not be answered as if they had.
        KnownNode* n = findNode(p->origin_id);
        if (n) {
          strncpy(n->msg_inbox, body, MAIL_TEXT_MAX); n->msg_inbox[MAIL_TEXT_MAX] = '\0';
          n->msg_unread = true;
          reconAtLeast(n, RECON_T_NAME);   // they signed it by writing it
        }
        lastMsgFrom = p->origin_id;
        lastMsgText = String(body);
        lastMsgAt   = millis();
        lastMsgCarried = (p->hdr.from_id != p->origin_id);
        pendingMsg     = String(body);
        pendingMsgFrom = chipIdStr(p->origin_id);
        pendingMsgRelayed = (p->hdr.from_id != p->origin_id);
        LORA_LOG("MAIL arrived from %08lx%s",
                 (unsigned long)p->origin_id,
                 pendingMsgRelayed ? " (carried)" : "");
        break;
      }

      // It is for somebody else, so we are being asked to carry it. Only ever
      // one hop: a message already carried once is refused, which is what stops
      // two couriers passing the same message back and forth forever.
      if (p->carried) { LORA_LOG("MAIL refused — already carried once"); break; }
      if (mailCarriedCount() >= MAIL_SLOTS / 2) {
        LORA_LOG("MAIL refused — courier bag full");
        break;                        // never let other people's post crowd out our own
      }
      if (mailQueue(p->final_id, p->origin_id, body, /*carried=*/true)) {
        pendingCourierFor = p->final_id;
        LORA_LOG("MAIL accepted for carriage to %08lx",
                 (unsigned long)p->final_id);
      }
      break;
    }

    default: break;
  }
}

// ─────────────────────────────────────────────
//  Radio plumbing
// ─────────────────────────────────────────────
static bool armRx() {
  radio.setDio1Action(loraDioISR);
  int st = radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) {
    loraLastTxError = st;
    LORA_LOG("startReceive FAILED %d", st);
    radioState = RS_DOWN;
    return false;
  }
  radioState = RS_RX;
  return true;
}

bool loraSetup() {
  loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  pinMode(LORA_DIO1, INPUT);
  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_PWR, LORA_PREAMBLE);
  if (state != RADIOLIB_ERR_NONE) {
    loraInitError = state; loraStatus = "Err:" + String(state);
    loraReady = false; radioState = RS_DOWN;
    LORA_LOG("begin() FAILED %d", state);
    return false;
  }
  if (!armRx()) {
    loraInitError = loraLastTxError;
    loraStatus = "RX err:" + String(loraLastTxError);
    loraReady = false;
    return false;
  }
  loraReady = true; loraStatus = "Online";
  LORA_LOG("online — %.1f MHz SF%d BW %.0f kHz sync 0x%02X id=%08lx",
           (double)LORA_FREQ, LORA_SF, (double)LORA_BW, LORA_SYNC,
           (unsigned long)myChipID32);
  return true;
}

// Transmit the head of the queue, CAD-gated (T1.5) and non-blocking (T1.4).
static void serviceTxQueue() {
  if (radioState == RS_TX) return;          // one frame in flight at a time

  bool overBudget = dutyBudgetExceeded();

  int idx = -1;
  uint32_t now = millis();
  for (int i = 0; i < TXQ_SIZE; i++) {
    if (!txq[i].active) continue;
    if ((int32_t)(now - txq[i].sendAfterMs) < 0) continue;
    // Over the duty budget, hold back anything nobody is waiting on (T2.5).
    // Urgent frames still go: refusing to answer a player to protect an
    // airtime budget would be worse than the budget being tight.
    if (overBudget && !txq[i].urgent) {
      txq[i].sendAfterMs = millis() + 60000UL;
      loraDutyDeferred++;
      continue;
    }
    idx = i; break;
  }
  if (idx < 0) return;

  // Listen before transmit.
  radio.standby();
  int cad = radio.scanChannel();

  // CAD asserts DIO1 when it finishes, and our shared handler is still armed,
  // so the scan leaves loraDioFlag set. If we do not clear it here the very
  // next loraTick() reads it as TxDone, calls finishTransmit() and re-arms RX
  // while the packet is still going out — every frame gets truncated and
  // nobody ever hears anybody. Clear it before arming the real transmission.
  loraDioFlag = false;

  if (cad != RADIOLIB_CHANNEL_FREE) {
    loraCadBusy++;
    txq[idx].cadTries++;
    if (txq[idx].cadTries < CAD_MAX_TRIES) {
      txq[idx].sendAfterMs = millis() + CAD_BACKOFF_MIN_MS + random(0, CAD_BACKOFF_JIT_MS);
      armRx();
      return;
    }
    // Channel never cleared — send anyway rather than starve.
    LORA_LOG("CAD busy x%u — transmitting regardless", txq[idx].cadTries);
  }

  // scanChannel() reconfigures DIO1 for CAD; put our handler back before TX.
  radio.setDio1Action(loraDioISR);
  int st = radio.startTransmit(txq[idx].buf, txq[idx].len);
  if (st != RADIOLIB_ERR_NONE) {
    loraTxErrors++; loraLastTxError = st;
    LORA_LOG("startTransmit FAILED %d", st);
    txq[idx].active = false;
    armRx();
    return;
  }

  PktHeader* h = (PktHeader*)txq[idx].buf;
  LORA_LOG("TX %s seq=%u to=%08lx len=%u", pktTypeName(h->type), h->seq,
           (unsigned long)h->to_id, txq[idx].len);

  if (pendingUser.active && pendingUser.seq == h->seq && loraActionState == LA_SENDING)
    loraActionState = LA_WAITING;

  dutyRecord(loraTimeOnAirMs(txq[idx].len));   // T2.5

  txq[idx].active = false;
  radioState      = RS_TX;
  txStartMs       = millis();
  loraPktSent++;
}

// ─────────────────────────────────────────────
//  T2.4 — adaptive beacon scheduling
// ─────────────────────────────────────────────
uint32_t loraBeaconInterval() {
  bool fast = (int32_t)(millis() - loraFastUntilMs) < 0;
  return fast ? (uint32_t)random(LORA_BEACON_FAST_MIN_MS, LORA_BEACON_FAST_MAX_MS)
              : (uint32_t)random(LORA_BEACON_MIN_MS,      LORA_BEACON_MAX_MS);
}

static void serviceBeacon() {
  if (!loraBeaconEnabled || !loraReady) return;

  // Boot burst: the sketch sends one beacon as soon as it is configured, then
  // we follow up at ~3 s and ~8 s so a device joining a group is discovered in
  // seconds rather than up to half a minute (T1.7).
  if (loraNextBeaconMs == 0) { loraNextBeaconMs = millis() + 3000; return; }
  if ((int32_t)(millis() - loraNextBeaconMs) < 0) return;

  loraSendBeacon();
  if (loraBootBurst < 2) {
    loraBootBurst++;
    loraNextBeaconMs = millis() + 5000;
  } else {
    loraNextBeaconMs = millis() + loraBeaconInterval();
  }
}

static void serviceNodePrune() {
  static uint32_t lastPrune = 0;
  if (!elapsed(lastPrune, NODE_PRUNE_MS)) return;
  lastPrune = millis();
  pruneNodes();
  pruneEchoes();     // must follow pruneNodes: an echo dies when we hear them
}

// Retry / expire one reliable slot (T1.2).
static void serviceSlot(PendingTx& slot) {
  if (!slot.active) return;
  if ((int32_t)(millis() - slot.nextAttemptMs) < 0) return;

  if (slot.triesLeft == 0) { clearSlot(slot, false); return; }

  slot.triesLeft--;
  slot.nextAttemptMs = millis() + TX_RETRY_BASE_MS + random(0, TX_RETRY_JITTER_MS);
  loraRetries++;
  if (&slot == &pendingUser) loraActionTries++;
  LORA_LOG("RETRY %s seq=%u (%u left)", pktTypeName(slot.type),
           slot.seq, slot.triesLeft);
  enqueueTx(slot.buf, slot.len);
}

static void servicePendingTx() {
  serviceSlot(pendingUser);
  serviceSlot(pendingReply);
}

// Re-arm RX periodically; full re-init after repeated failure (T1.6).
static void serviceRxWatchdog() {
  static uint32_t lastCheck = 0;
  static int      strikes   = 0;
  if ((uint32_t)(millis() - lastCheck) < RX_WATCHDOG_MS) return;
  lastCheck = millis();

  if (radioState == RS_TX) return;          // TX in flight, leave it alone
  if (txQueueDepth() > 0)  return;          // about to transmit anyway

  loraWatchdogFires++;
  if (armRx()) { strikes = 0; return; }

  if (++strikes >= RX_WATCHDOG_STRIKES) {
    strikes = 0;
    loraReinits++;
    LORA_LOG("watchdog: %d strikes — full re-init", RX_WATCHDOG_STRIKES);
    loraSetup();
    if (loraReady) loraStatus = "Recovered";
  }
}

void loraTick() {
  if (!loraReady) return;

  // ── DIO1 event ──
  if (loraDioFlag) {
    loraDioFlag = false;

    if (radioState == RS_TX) {
      radio.finishTransmit();
      armRx();
    } else {
      int pktLen = radio.getPacketLength();
      if (pktLen > 0 && pktLen <= 64) {
        uint8_t buf[64];
        int st = radio.readData(buf, (size_t)pktLen);
        if (st == RADIOLIB_ERR_NONE) {
          loraLastRSSI = (int)radio.getRSSI();
          loraLastSNR  = radio.getSNR();
          if (loraVerifyFrame(buf, &pktLen)) loraHandlePacket(buf, pktLen);
        } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
          loraCrcErrors++;
          LORA_LOG("RX CRC mismatch (len=%d)", pktLen);
        } else {
          loraLastTxError = st;
          LORA_LOG("readData FAILED %d", st);
        }
      }
      armRx();
    }
  }

  // ── TX hard timeout — TxDone never arrived (T1.4) ──
  if (radioState == RS_TX && (uint32_t)(millis() - txStartMs) > TX_HARD_TIMEOUT_MS) {
    loraTxErrors++;
    LORA_LOG("TX hard timeout — forcing standby + RX");
    radio.standby();
    armRx();
  }

  servicePendingTx();
  serviceBeacon();
  serviceEcho();
  serviceMail();
  serviceTxQueue();
  serviceRxWatchdog();
  serviceNodePrune();
}

// ─────────────────────────────────────────────
//  Helpers used by the sketch / portal
// ─────────────────────────────────────────────
String loraGetSignal() { return loraReady ? String(loraLastRSSI) + " dBm" : "offline"; }

char factionChar() {
  if (myFaction == "BLACK") return 'B';
  if (myFaction == "WHITE") return 'W';
  if (myFaction == "RED")   return 'R';
  if (myFaction == "GREEN") return 'G';
  return '?';
}

// Per-node feed for the radar UI (T2.1 / T3.4).
String loraNodesJson() {
  String j = "[";
  for (int i = 0; i < knownCount; i++) {
    KnownNode* n = &knownNodes[i];
    if (i) j += ",";
    j += "{\"id\":\""      + chipIdStr(n->chip_id) + "\",";
    j += "\"name\":\""     + nodeNameFromId(n->chip_id) + "\",";
    j += "\"level\":"      + String(n->level) + ",";
    j += "\"faction\":\""  + String(n->faction) + "\",";
    j += "\"rssi\":"       + String(n->rssi) + ",";
    j += "\"avgRssi\":"    + String(nodeAvgRssi(n)) + ",";
    j += "\"snr\":"        + String(n->snr10 / 10.0f, 1) + ",";
    j += "\"bars\":"       + String(nodeSignalBars(n)) + ",";
    j += "\"proximity\":\""+ String(nodeProximity(n)) + "\",";
    j += "\"status\":\""   + String(nodeStatusText(n)) + "\",";
    j += "\"ageMs\":"      + String(ageMs(n->last_seen_ms)) + ",";
    j += "\"recon\":"      + String(n->recon_count) + ",";
    j += "\"hacked\":"     + String(n->hack_attempted ? "true" : "false") + ",";
    j += "\"hackWon\":"    + String(n->hack_won ? "true" : "false") + ",";
    j += "\"unread\":"     + String(n->msg_unread ? "true" : "false");
    j += "}";
  }
  j += "]";
  return j;
}

// Packet delivery ratio for the T0.4 bench test.
// Counts reliable frames only, so beacons don't skew it. -1 = no data yet.
float loraDeliveryRatio() {
  int attempts = loraAcksRecv + loraTimeouts;
  if (attempts == 0) return -1.0f;
  return (float)loraAcksRecv * 100.0f / (float)attempts;
}

// T0.3 — every counter in one JSON blob.
String loraDiagJson() {
  String j = "{";
  j += "\"version\":\""     + String(FIRMWARE_VERSION) + "\",";
  j += "\"status\":\""      + loraStatus + "\",";
  j += "\"ready\":"         + String(loraReady ? "true" : "false") + ",";
  j += "\"initError\":"     + String(loraInitError) + ",";
  j += "\"radioState\":"    + String((int)radioState) + ",";
  j += "\"pktSent\":"       + String(loraPktSent) + ",";
  j += "\"pktRecv\":"       + String(loraPktRecv) + ",";
  j += "\"beacons\":"       + String(loraBeaconsSent) + ",";
  j += "\"acksSent\":"      + String(loraAcksSent) + ",";
  j += "\"acksRecv\":"      + String(loraAcksRecv) + ",";
  j += "\"retries\":"       + String(loraRetries) + ",";
  j += "\"timeouts\":"      + String(loraTimeouts) + ",";
  j += "\"dupsDropped\":"   + String(loraDupsDropped) + ",";
  j += "\"crcErrors\":"     + String(loraCrcErrors) + ",";
  // Both of these existed as counters and neither was ever reported. badSig is
  // the only signal that frames are being rejected by the HMAC rather than lost
  // — without it a signing mistake looks exactly like being out of range. heap
  // is the only memory number the firmware produces at all.
  j += "\"badSig\":"        + String(loraBadSig) + ",";
  j += "\"replaysDropped\":"+ String(loraReplaysDropped) + ",";
  j += "\"heap\":"          + String((unsigned long)ESP.getFreeHeap()) + ",";
  j += "\"minHeap\":"       + String((unsigned long)ESP.getMinFreeHeap()) + ",";
  j += "\"txErrors\":"      + String(loraTxErrors) + ",";
  j += "\"lastTxError\":"   + String(loraLastTxError) + ",";
  j += "\"notForMe\":"      + String(loraDroppedNotForMe) + ",";
  j += "\"cadBusy\":"       + String(loraCadBusy) + ",";
  j += "\"txqDrops\":"      + String(loraTxQueueDrops) + ",";
  j += "\"txqDepth\":"      + String(txQueueDepth()) + ",";
  j += "\"watchdog\":"      + String(loraWatchdogFires) + ",";
  j += "\"reinits\":"       + String(loraReinits) + ",";
  j += "\"nodesEvicted\":"  + String(loraNodesEvicted) + ",";
  j += "\"dutyCyclePct\":"  + String(dutyCyclePct(), 3) + ",";
  j += "\"dutyDeferred\":"  + String(loraDutyDeferred) + ",";
  j += "\"beaconFast\":"    + String(((int32_t)(millis() - loraFastUntilMs) < 0) ? "true" : "false") + ",";
  j += "\"deliveryPct\":"   + String(loraDeliveryRatio(), 1) + ",";
  j += "\"lastPktType\":\"" + String(pktTypeName(loraLastPktType)) + "\",";
  j += "\"lastPktLen\":"    + String(loraLastPktLen) + ",";
  j += "\"rssi\":"          + String(loraLastRSSI) + ",";
  j += "\"snr\":"           + String(loraLastSNR, 1) + ",";
  j += "\"action\":\""      + String(loraActionText()) + "\",";
  j += "\"actionLabel\":\"" + loraActionLabel + "\",";
  j += "\"actionTries\":"   + String(loraActionTries) + ",";
  j += "\"echoNodes\":"     + String(echoCount) + ",";
  j += "\"echoesSent\":"    + String(loraEchoesSent) + ",";
  j += "\"echoesRecv\":"    + String(loraEchoesRecv) + ",";
  j += "\"mailPending\":"   + String(mailPending()) + ",";
  j += "\"mailCarried\":"   + String(mailCarriedCount()) + ",";
  j += "\"mailDelivered\":" + String(loraMailDelivered) + ",";
  j += "\"mailHandedOff\":" + String(loraMailHandedOff) + ",";
  j += "\"mailExpired\":"   + String(loraMailExpired) + ",";
  j += "\"nodes\":"         + String(knownCount) + ",";
  j += "\"uptimeMs\":"      + String(millis());
  j += "}";
  return j;
}
