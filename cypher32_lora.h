#pragma once
#include <RadioLib.h>
#include "cypher32_packets.h"

// ─────────────────────────────────────────────
//  CYPHER32 LORA — v54
//
//  Roadmap Phase 0 (instrumentation) + Phase 1 (link layer).
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

// Beacon cadence (T1.7) — used by the sketch's loop().
// For the T0.4 bench test, drop these to 5000 / 5000.
#define LORA_BEACON_MIN_MS  25000
#define LORA_BEACON_MAX_MS  35000

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

String    pendingMsg     = "";
String    pendingMsgFrom = "";

// Inbound hack notification for the defender's display. Previously impossible:
// HACK_RESULT was never transmitted by anyone.
bool      pendingHackAlert       = false;
String    pendingHackFrom        = "";
bool      pendingHackAttackerWon = false;

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
    default:              return "UNKNOWN";
  }
}

#if LORA_DEBUG
  #define LORA_LOG(fmt, ...) Serial.printf("[%8lu] LORA " fmt "\n", millis(), ##__VA_ARGS__)
#else
  #define LORA_LOG(fmt, ...) do {} while (0)
#endif

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
    knownNodes[oldest].chip_id = chip_id;
    knownNodes[oldest].faction = '?';
    return &knownNodes[oldest];
  }
  KnownNode* n = &knownNodes[knownCount++];
  memset(n, 0, sizeof(KnownNode));
  n->chip_id = chip_id; n->faction = '?';
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
};
TxFrame txq[TXQ_SIZE];

uint8_t txSeq = 0;   // rolling per-sender counter

bool enqueueTx(const void* pkt, int len, uint32_t delayMs = 0) {
  if (len <= 0 || len > 64) return false;
  for (int i = 0; i < TXQ_SIZE; i++) {
    if (txq[i].active) continue;
    memcpy(txq[i].buf, pkt, len);
    txq[i].len         = (uint8_t)len;
    txq[i].sendAfterMs = millis() + delayMs;
    txq[i].cadTries    = 0;
    txq[i].active      = true;
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
bool loraSendUnreliable(void* pkt, int len) {
  if (!loraReady) return false;
  PktHeader* h = (PktHeader*)pkt;
  h->seq = txSeq++;
  return enqueueTx(pkt, len);
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
  if (!loraReady || len <= 0 || len > 64) return false;
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
  if (loraSendUnreliable(&pkt, sizeof(pkt))) loraBeaconsSent++;
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

void loraSendHackReq(uint32_t target_id) {
  PktHackReq pkt;
  fillHdr(&pkt.hdr, PKT_HACK_REQ, target_id);
  pkt.brute = (uint8_t)skillBrute;
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
  enqueueTx(&ack, sizeof(ack), REPLY_DELAY_MIN_MS);
  loraAcksSent++;
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
    for (PendingTx* s : { &pendingUser, &pendingReply }) {
      if (s->active && s->to_id == a->from_id && s->seq == a->seq) {
        LORA_LOG("ACK matched seq=%u — %s delivered", a->seq, pktTypeName(s->type));
        clearSlot(*s, true);
        break;
      }
    }
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

  // Any unicast asking for an ACK gets one, whatever the type.
  if (hdr->flags & PKTFLAG_ACK_REQ) sendAck(hdr->from_id, hdr->seq, hdr->type);

  switch (hdr->type) {
    case PKT_BEACON: {
      if (len < (int)sizeof(PktBeacon)) return;
      PktBeacon* p = (PktBeacon*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (!n) return;
      n->level = p->level; n->faction = (char)p->faction; n->last_seen_ms = millis();
      break;
    }
    case PKT_RECON_REQ: {
      if (len < (int)sizeof(PktReconReq)) return;
      uint8_t stats[3] = {(uint8_t)skillBrute,(uint8_t)skillStealth,(uint8_t)skillFirewall};
      uint8_t types[3] = {STAT_BRUTE,STAT_STEALTH,STAT_FIREWALL};
      int pick = random(0,3);
      PktReconReply reply;
      fillHdr(&reply.hdr, PKT_RECON_REPLY, hdr->from_id);
      reply.stat_type = types[pick]; reply.stat_value = stats[pick];
      deferReply(&reply, sizeof(reply));
      KnownNode* n = findOrAddNode(hdr->from_id);
      if (n) n->last_seen_ms = millis();
      break;
    }
    case PKT_RECON_REPLY: {
      if (len < (int)sizeof(PktReconReply)) return;
      PktReconReply* p = (PktReconReply*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (!n) return;
      n->last_seen_ms = millis();
      for (int i = 0; i < n->recon_count; i++)
        if (n->recon_types[i] == p->stat_type) return;   // already know this stat
      if (n->recon_count < 3) {
        n->recon_types[n->recon_count]  = p->stat_type;
        n->recon_values[n->recon_count] = p->stat_value;
        n->recon_count++;
      }
      break;
    }
    case PKT_HACK_REQ: {
      if (len < (int)sizeof(PktHackReq)) return;
      PktHackReply reply;
      fillHdr(&reply.hdr, PKT_HACK_REPLY, hdr->from_id);
      reply.firewall = (uint8_t)skillFirewall;
      reply.faction  = myFaction.length() > 0 ? myFaction.charAt(0) : '?';
      deferReply(&reply, sizeof(reply));
      KnownNode* n = findOrAddNode(hdr->from_id);
      if (n) n->last_seen_ms = millis();
      break;
    }
    case PKT_HACK_REPLY: {
      if (len < (int)sizeof(PktHackReply)) return;
      PktHackReply* p = (PktHackReply*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (!n) return;
      n->faction = (char)p->faction; n->last_seen_ms = millis();
      bool haveFw = false;
      for (int i = 0; i < n->recon_count; i++)
        if (n->recon_types[i] == STAT_FIREWALL) haveFw = true;
      if (!haveFw && n->recon_count < 3) {
        n->recon_types[n->recon_count]  = STAT_FIREWALL;
        n->recon_values[n->recon_count] = p->firewall;
        n->recon_count++;
      }
      break;
    }
    case PKT_HACK_RESULT: {
      if (len < (int)sizeof(PktHackResult)) return;
      PktHackResult* p = (PktHackResult*)buf;
      KnownNode* n = findOrAddNode(hdr->from_id);
      if (n) n->last_seen_ms = millis();
      // Surface it — the defender previously had no idea they'd been hit.
      pendingHackAlert       = true;
      pendingHackFrom        = chipIdStr(hdr->from_id);
      pendingHackAttackerWon = (p->outcome == HACK_WIN);
      break;
    }
    case PKT_MSG: {
      if (len < (int)sizeof(PktMsg)) return;
      PktMsg* p = (PktMsg*)buf;
      KnownNode* n = findOrAddNode(p->hdr.from_id);
      if (n) {
        strncpy(n->msg_inbox, p->text, 32); n->msg_inbox[32] = '\0';
        n->msg_unread = true; n->last_seen_ms = millis();
        pendingMsg = String(p->text); pendingMsgFrom = chipIdStr(p->hdr.from_id);
      }
      break;
    }
    case PKT_PING:
      break;   // the ACK above is the entire point of a ping
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

  int idx = -1;
  uint32_t now = millis();
  for (int i = 0; i < TXQ_SIZE; i++) {
    if (!txq[i].active) continue;
    if ((int32_t)(now - txq[i].sendAfterMs) < 0) continue;
    idx = i; break;
  }
  if (idx < 0) return;

  // Listen before transmit.
  radio.standby();
  int cad = radio.scanChannel();
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

  txq[idx].active = false;
  radioState      = RS_TX;
  txStartMs       = millis();
  loraPktSent++;
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
          loraHandlePacket(buf, pktLen);
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
  serviceTxQueue();
  serviceRxWatchdog();
}

// ─────────────────────────────────────────────
//  Helpers used by the sketch / portal
// ─────────────────────────────────────────────
String loraGetSignal() { return loraReady ? String(loraLastRSSI) + " dBm" : "offline"; }

String statTypeName(uint8_t t) {
  if (t == STAT_BRUTE)    return "Brute Force";
  if (t == STAT_STEALTH)  return "Stealth";
  if (t == STAT_FIREWALL) return "Firewall";
  return "Unknown";
}

char factionChar() {
  if (myFaction == "BLACK") return 'B';
  if (myFaction == "WHITE") return 'W';
  if (myFaction == "RED")   return 'R';
  if (myFaction == "GREEN") return 'G';
  return '?';
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
  j += "\"txErrors\":"      + String(loraTxErrors) + ",";
  j += "\"lastTxError\":"   + String(loraLastTxError) + ",";
  j += "\"notForMe\":"      + String(loraDroppedNotForMe) + ",";
  j += "\"cadBusy\":"       + String(loraCadBusy) + ",";
  j += "\"txqDrops\":"      + String(loraTxQueueDrops) + ",";
  j += "\"txqDepth\":"      + String(txQueueDepth()) + ",";
  j += "\"watchdog\":"      + String(loraWatchdogFires) + ",";
  j += "\"reinits\":"       + String(loraReinits) + ",";
  j += "\"deliveryPct\":"   + String(loraDeliveryRatio(), 1) + ",";
  j += "\"lastPktType\":\"" + String(pktTypeName(loraLastPktType)) + "\",";
  j += "\"lastPktLen\":"    + String(loraLastPktLen) + ",";
  j += "\"rssi\":"          + String(loraLastRSSI) + ",";
  j += "\"snr\":"           + String(loraLastSNR, 1) + ",";
  j += "\"action\":\""      + String(loraActionText()) + "\",";
  j += "\"actionLabel\":\"" + loraActionLabel + "\",";
  j += "\"actionTries\":"   + String(loraActionTries) + ",";
  j += "\"nodes\":"         + String(knownCount) + ",";
  j += "\"uptimeMs\":"      + String(millis());
  j += "}";
  return j;
}
