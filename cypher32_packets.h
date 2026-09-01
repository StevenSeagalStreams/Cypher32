#pragma once
#include <Arduino.h>

// T4.7 — the single source of truth for the version. Shown in the portal
// Config tab and in /api/diag.
#define FIRMWARE_VERSION "v67"

// ─────────────────────────────────────────────
//  CYPHER32 LORA PACKET PROTOCOL  — v67
// ─────────────────────────────────────────────
//
//  Link layer on top of the raw driver:
//    - per-sender sequence numbers (duplicate suppression)
//    - flags byte carrying ACK_REQUESTED / IS_ACK
//    - 4-byte truncated HMAC-SHA256 tag appended to every frame (T4.1)
//
//  No GPS, no RSSI-position, no names on the wire.
//  Only chip-IDs and game stats are transmitted.
//
//  Packet layout:
//    [1]  type     — packet type byte
//    [1]  seq      — rolling per-sender counter (ACKs echo the acked seq)
//    [1]  flags    — see PKTFLAG_* below
//    [4]  from_id  — sender chip-ID (uint32)
//    [4]  to_id    — recipient chip-ID (uint32, 0=broadcast)
//    [N]  payload  — type-specific data
//
//  Header is 11 bytes. Largest packet (PktMsg) is 44 bytes, plus the 4-byte
//  signature, well under the 64-byte cap enforced by enqueueTx().

// ── Packet type bytes ────────────────────────
#define PKT_BEACON      0x01  // broadcast: "I exist"
#define PKT_RECON_REQ   0x02  // open a scouting link on a target
#define PKT_RECON_REPLY 0x03  // target returns its whole file
#define PKT_HACK_REQ    0x04  // initiate hack — sends attacker brute
#define PKT_HACK_REPLY  0x05  // defender replies with firewall level
#define PKT_HACK_RESULT 0x06  // attacker tells defender the outcome
#define PKT_MSG         0x07  // text message (max 32 chars)
#define PKT_ACK         0x08  // link-layer acknowledgement
#define PKT_PING        0x09  // reliable no-op — round-trip test

// ── Header flags ─────────────────────────────
#define PKTFLAG_ACK_REQ 0x01  // sender wants an ACK for this seq
#define PKTFLAG_IS_ACK  0x02  // this frame *is* an ACK; seq = acked seq

// ── Recon mini-game (sequence memory) ────────
//  Recon is played on the phone: a sequence of tiles flashes and you repeat
//  it, one longer each round. The furthest round you complete is your recon
//  score for that node.
//
//  A perfect run is worth exactly what three button-presses used to be, so
//  the odds curve is unchanged — it just has to be earned now.
//      score 10  ->  +15%  ->  75% base hit chance
#define RECON_MAX_SEQ    10   // longest sequence the mini-game runs to
#define RECON_MAX_BONUS  15   // odds bonus for a perfect run

// ── Intel tiers ──────────────────────────────
//  Every round you clear strips another layer off the target, and the reveal
//  lands the moment the round lands — you watch them come apart as you play.
//  Before round 2 a contact is an anonymous signal on the radar: recon is how
//  anyone gets a name at all.
//
//      2   codename          6   level          9   firewall
//      4   faction           7   brute         10   BACKDOOR
//                            8   stealth
//
//  A perfect run leaves a backdoor open: that node's intel never expires with
//  the lock, its level and faction track its beacons by themselves, and
//  re-opening recon on it costs neither an attempt nor a game — you already
//  own them.
#define RECON_T_NAME      2
#define RECON_T_FACTION   4
#define RECON_T_LEVEL     6
#define RECON_T_BRUTE     7
#define RECON_T_STEALTH   8
#define RECON_T_FIREWALL  9
#define RECON_T_PWNED    10

// ── Hack outcome flags ───────────────────────
#define HACK_WIN  0x01
#define HACK_LOSE 0x00

// ── Shared network key — HMAC frame signing (T4.1) ───
//  CHANGE THIS to your own random 16 bytes before deploying. Every device in
//  your game must use the same key; devices with different keys will reject
//  each other's frames entirely.
//
//  This is not real security. The key is compiled into every device, so anyone
//  who dumps flash can extract it. It exists to stop a player with a spare
//  SX1262 injecting packets to award themselves XP.
static const uint8_t LORA_KEY[16] = {
  0xC3, 0x29, 0xF1, 0x7A, 0x04, 0xBE, 0x58, 0x3D,
  0x91, 0xE6, 0x2C, 0x47, 0xD0, 0x8B, 0xA5, 0x6F
};

// ── LoRa radio settings (SX1262 @ 868 MHz EU) ─
#define LORA_FREQ       868.0   // MHz — EU ISM band
#define LORA_BW         125.0   // kHz
#define LORA_SF         7       // SF7 = ~300m range, ~50ms airtime (SF9 crashed WDT)
#define LORA_CR         5       // coding rate 4/5
#define LORA_SYNC       0x12    // private network sync word (not 0x34=LoRaWAN)
#define LORA_PWR        14      // dBm transmit power (legal EU max = 14)
#define LORA_PREAMBLE   8

// ── SX1262 pin mapping (from Wireless Paper schematic) ─
#define LORA_NSS   8   // GPIO8  = SPI chip select
#define LORA_DIO1  14  // GPIO14 = interrupt / busy indicator
#define LORA_RST   12  // GPIO12 = reset
#define LORA_BUSY  13  // GPIO13 = busy

// ── Packet structs ───────────────────────────

#pragma pack(push, 1)   // no padding between fields

struct PktHeader {
  uint8_t  type;
  uint8_t  seq;         // rolling per-sender counter
  uint8_t  flags;       // PKTFLAG_*
  uint32_t from_id;
  uint32_t to_id;       // 0x00000000 = broadcast
};

struct PktBeacon {
  PktHeader hdr;        // type=PKT_BEACON, to_id=0
  uint8_t   level;      // sender's level (1-32)
  uint8_t   faction;    // 'B','W','R','G'
};

struct PktReconReq {
  PktHeader hdr;        // type=PKT_RECON_REQ
};

// The whole dossier comes back in one reply and the requester gates it locally
// against the score its player actually earned. It could instead be filtered
// here, by the target — but then the reveal could not land round by round, and
// watching the target come apart while you play is the point of the mechanic.
// Nothing defensible is being protected: the score was always self-reported,
// and the check that matters — who won a hack — is still made by the defender.
struct PktReconReply {
  PktHeader hdr;        // type=PKT_RECON_REPLY
  uint8_t   level;      // 1-32
  uint8_t   faction;    // 'B','W','R','G'
  uint8_t   brute;
  uint8_t   stealth;
  uint8_t   firewall;
};

struct PktHackReq {
  PktHeader hdr;         // type=PKT_HACK_REQ
  uint8_t   brute;       // attacker's brute force stat
  uint8_t   recon_score; // best sequence reached against this node (0-10).
                         // The defender clamps it — see loraHandlePacket().
  uint8_t   stealth;     // attacker's stealth stat — see loraHackChancePct()
};

// T4.3: the defender rolls and reports the verdict, so a modified attacker
// cannot simply declare itself the winner.
struct PktHackReply {
  PktHeader hdr;        // type=PKT_HACK_REPLY
  uint8_t   outcome;    // HACK_WIN / HACK_LOSE — decided by the DEFENDER
  uint8_t   firewall;   // defender's firewall stat
  uint8_t   faction;    // defender faction letter
};

struct PktHackResult {
  PktHeader hdr;        // type=PKT_HACK_RESULT, to_id=defender
  uint8_t   outcome;    // HACK_WIN or HACK_LOSE
  int8_t    xp_delta;   // XP gained/lost by attacker (signed)
};

struct PktMsg {
  PktHeader hdr;        // type=PKT_MSG
  char      text[33];   // null-terminated, max 32 chars
};

struct PktAck {
  PktHeader hdr;        // type=PKT_ACK, flags|=PKTFLAG_IS_ACK, seq=acked seq
  uint8_t   ack_type;   // which packet type we're acking (diagnostics only)
};

struct PktPing {
  PktHeader hdr;        // type=PKT_PING — payload-free reliable probe
};

#pragma pack(pop)

// ── Node presence (T2.3) ─────────────────────
#define NODE_ACTIVE_MS   90000UL    // seen within 90 s → ACTIVE
#define NODE_FADING_MS  300000UL    // 90 s – 5 min → FADING, then evicted
#define RSSI_HIST        4          // rolling average depth (T2.1)

// ── Known node entry ─────────────────────────
struct KnownNode {
  uint32_t      chip_id;
  uint8_t       level;
  char          faction;        // 'B','W','R','G','?'
  unsigned long last_seen_ms;
  unsigned long first_seen_ms;

  // Signal (T2.1) — per node, not one global overwritten by whatever arrived last
  int16_t       rssi;                // most recent, dBm
  int16_t       snr10;               // most recent SNR × 10
  int16_t       rssi_hist[RSSI_HIST];
  uint8_t       rssi_idx;
  uint8_t       rssi_n;              // samples collected so far (0..RSSI_HIST)

  // Recon — 3 attempts per lock window; the best sequence is the intel tier.
  // level and faction above are the live values from their beacons; recon_score
  // decides whether we are allowed to look at them yet.
  uint8_t       recon_count;         // attempts spent this window (0-3)
  uint8_t       recon_score;         // best sequence reached here (0-10)
  // What we are allowed to look at. Normally the mini-game score, but talking
  // to us or attacking us identifies you for free, and that must not also hand
  // out the odds bonus — which is why this is a separate number from
  // recon_score rather than the same one with favours added to it.
  uint8_t       intel;
  uint8_t       seen_brute;          // only meaningful at tier RECON_T_BRUTE+
  uint8_t       seen_stealth;
  uint8_t       seen_firewall;
  bool          pwned;               // perfect run — intel persists and refreshes

  // Hack state — manual, one attempt only
  bool          hack_attempted;
  bool          hack_won;
  unsigned long hack_time_ms;

  // Replay protection for inbound HACK_RESULT (T4.2)
  uint8_t       last_result_seq;
  bool          have_result_seq;

  char          msg_inbox[33];
  bool          msg_unread;
  char          msg_sent[33];   // last message WE sent to this node
};

// Is this tier unlocked on this node?
static inline bool reconKnows(const KnownNode* n, uint8_t tier) {
  return n && n->intel >= tier;
}
// Raise the intel tier without touching recon_score, i.e. without paying out
// the hack-odds bonus that only the mini-game earns.
static inline void reconAtLeast(KnownNode* n, uint8_t tier) {
  if (n && n->intel < tier) n->intel = tier;
}

#define MAX_KNOWN_NODES 20

// ── Rollover-safe helpers (T2.2) ─────────────
// Unsigned subtraction stays correct across the 49-day millis() wrap; direct
// comparison of two millis() values does not.
inline uint32_t ageMs(unsigned long stampMs) {
  return (uint32_t)(millis() - stampMs);
}
inline bool elapsed(unsigned long stampMs, uint32_t windowMs) {
  return ageMs(stampMs) >= windowMs;
}

inline int16_t nodeAvgRssi(const KnownNode* n) {
  if (!n || n->rssi_n == 0) return 0;
  int32_t sum = 0;
  for (uint8_t i = 0; i < n->rssi_n; i++) sum += n->rssi_hist[i];
  return (int16_t)(sum / n->rssi_n);
}

// Plain-language range band for the portal's radar (T3.4).
inline const char* nodeProximity(const KnownNode* n) {
  if (!n || n->rssi_n == 0) return "UNKNOWN";
  int16_t r = nodeAvgRssi(n);
  if (r >= -60)  return "VERY CLOSE";
  if (r >= -85)  return "CLOSE";
  if (r >= -105) return "DISTANT";
  return "FADING";
}

inline uint8_t nodeSignalBars(const KnownNode* n) {
  if (!n || n->rssi_n == 0) return 0;
  int16_t r = nodeAvgRssi(n);
  if (r >= -60)  return 4;
  if (r >= -85)  return 3;
  if (r >= -105) return 2;
  return 1;
}

inline bool nodeIsActive(const KnownNode* n) {
  return n && ageMs(n->last_seen_ms) < NODE_ACTIVE_MS;
}
inline bool nodeIsExpired(const KnownNode* n) {
  return n && ageMs(n->last_seen_ms) >= NODE_FADING_MS;
}
inline const char* nodeStatusText(const KnownNode* n) {
  if (!n) return "GONE";
  return nodeIsActive(n) ? "ACTIVE" : (nodeIsExpired(n) ? "GONE" : "FADING");
}

// ── Deterministic name from chip_id ──────────
//  Same chip_id always produces same name on every device.
//  No name is ever transmitted over LoRa.
static const char* _np[] = {
  "Ghost","Void","Null","Iron","Zero","Dark",
  "Neon","Byte","Hex","Root","Rogue","Shade",
  "Flux","Nano","Grim","Echo","Venom","Pixel",
  "Glitch","Surge","Blaze","Specter","Crypt","Nexus"
};
static const char* _ns[] = {
  "Byte","Crypt","Shade","Hex","Wire","Core",
  "Gate","Node","Shell","Mask","Spike","Bit",
  "Link","Pulse","Slash","Probe","Trap","Worm",
  "Key","Lock","Ping","Trace","Frag","Grid"
};
inline String nodeNameFromId(uint32_t id) {
  // Use different bit ranges so IDs differing in upper/lower bytes get different names
  int pi = ((id >> 16) ^ (id >> 24)) % 24;
  int si = ((id)       ^ (id >> 8))  % 24;
  if (si == pi) si = (si + 7) % 24;
  String n = String(_np[pi]) + String(_ns[si]);
  if (n.length() > 10) n = n.substring(0, 10);
  return n;
}
