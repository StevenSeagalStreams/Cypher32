#pragma once
#include <Arduino.h>

// T4.7 — the single source of truth for the version. Shown in the portal
// Config tab and in /api/diag.
#define FIRMWARE_VERSION "v71"

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
#define PKT_ECHO        0x0A  // broadcast: "here is who I can hear"
#define PKT_MAIL        0x0B  // a message being carried on somebody's behalf

// ── Reach beyond direct range ────────────────
//  Two features, one idea: a device can tell you things about places your
//  radio cannot hear, but it may never act on your behalf in those places.
//
//  ECHO is a periodic broadcast of the sender's own direct neighbours. You
//  hear it from someone in range and learn who *they* can hear — two hops of
//  visibility, and no further. It is NEVER forwarded, which is the whole
//  safety argument: with nothing rebroadcast there is no flood, no TTL to get
//  wrong, no duplicate-suppression window to size, and a routing loop is not
//  merely unlikely but unconstructable.
//
//  Flooding was the obvious alternative and it is not affordable here. One
//  beacon relayed once by each of twenty devices costs every device 51.5 ms x
//  20 per beacon interval = 3.43% duty cycle. EU 868 g1 allows 1% and this
//  firmware self-caps at 0.8%, so plain flooding is illegal at six devices and
//  four times over budget at twenty. An echo costs one frame per node per
//  ECHO_INTERVAL_MS regardless of how many nodes there are.
//
//  MAIL is the other half. An echo tells you which of your neighbours can
//  reach someone you cannot, so a message can be handed to that neighbour and
//  delivered when they are next in range of the recipient. Nothing is relayed
//  in real time; the message waits in a pocket. The carrier is chosen, not
//  broadcast to, so one message costs one frame per hop and never N.
//
//  What deliberately does NOT travel: hacks, recon, XP, intel, locks. An echo
//  contact has no faction, no level and no stats, cannot be scouted, cannot be
//  attacked, and is counted by nothing. The game's only proximity check is the
//  fact that your radio heard them — see findNode() at the action handler —
//  and extending presence without extending consequence is what keeps it.
#define ECHO_MAX_PEERS   8          // neighbours named per echo frame
#define MAIL_TEXT_MAX    32

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

// ── RANGE PROFILE ────────────────────────────
//  EVERY DEVICE IN YOUR GAME MUST USE THE SAME PROFILE. Spreading factor and
//  frequency are both part of how a LoRa receiver locks onto a signal: two
//  devices on different profiles are not "weakly connected", they are deaf to
//  each other and will never appear on each other's radar.
//
//  Where the range comes from. LoRa's advertised 2-15 km is SF11/SF12 at full
//  legal power with line of sight. This project shipped SF7 at 14 dBm, which
//  is the SHORTEST-range configuration LoRa has — chosen for airtime, not for
//  distance. Two independent levers move it:
//
//    Spreading factor   every step up is +2.5 dB of receiver sensitivity and
//                       doubles the airtime. SF7 -> SF9 is +5 dB.
//    Sub-band           ETSI splits 868 MHz into bands with different limits.
//                         g1  868.0-868.6   14 dBm (25 mW)   1 % duty
//                         g3  869.4-869.65  27 dBm (500 mW) 10 % duty
//                       Moving to g3 is +8 dB (the SX1262 caps at 22 dBm) AND
//                       ten times the airtime budget. It is the same band
//                       Meshtastic uses for its EU region, for these reasons.
//
//  Range scales roughly as 10^(dB/(10*n)) with n~2.7 outdoors in mixed terrain.
//
//  NOTE ON THE OLD FREQUENCY: this was 868.0 MHz, which with 125 kHz of
//  bandwidth occupies 867.94-868.06 — the lower half sits BELOW the g1 band
//  edge at 868.0. 868.1 is the conventional g1 centre and is what FAST uses now.
//
//  NOTE ON "SF9 crashed WDT": that comment dated from before T1.4, when TX was
//  a blocking call. Transmission is a non-blocking state machine now and the
//  task watchdog is disabled deliberately in setup(). Untested on hardware.
#define LORA_PROFILE_FAST  0   // SF7  @ 868.1  14 dBm — shortest range, snappiest
#define LORA_PROFILE_LONG  1   // SF9  @ 869.5  22 dBm — ~3x the range
#define LORA_PROFILE_EPIC  2   // SF11 @ 869.5  22 dBm — ~4.6x, slow and chatty
//
//  HOW MANY PEOPLE EACH PROFILE HOLDS. Duty cycle is per transmitter, but the
//  channel is shared, and a CAD-gated ALOHA channel starts losing frames to
//  collisions above roughly a third occupancy. Measured by test_mesh at idle:
//
//    FAST  0.30 % per node ->  6 % of the channel at 20 devices — comfortable
//    LONG  0.70 % per node -> 14 % at 20 devices — fine
//    EPIC  1.81 % per node -> 36 % at 20 devices — TOO CROWDED
//
//  So EPIC is for a handful of people spread across a city, not for a room
//  with twenty in it. Range and capacity are the same budget spent twice.

#ifndef LORA_PROFILE
#define LORA_PROFILE LORA_PROFILE_LONG
#endif

#if   LORA_PROFILE == LORA_PROFILE_FAST
  #define LORA_FREQ      868.1
  #define LORA_SF        7
  #define LORA_PWR       14
  #define LORA_BAND      "g1"
  #define LORA_DUTY_LEGAL_PCT  1.0f
  #define LORA_DUTY_SELF_PCT   0.8f
  #define LORA_BEACON_MIN_MS      25000
  #define LORA_BEACON_MAX_MS      35000
  #define PROFILE_NAME   "FAST"
#elif LORA_PROFILE == LORA_PROFILE_LONG
  #define LORA_FREQ      869.525
  #define LORA_SF        9
  #define LORA_PWR       22
  #define LORA_BAND      "g3"
  #define LORA_DUTY_LEGAL_PCT  10.0f
  // Well under the legal 10 %. The limit that actually bites is not the law,
  // it is the shared channel: twenty devices at 1 % each is 20 % occupancy,
  // and a CAD-gated ALOHA channel starts losing frames to collisions above
  // roughly a third. So the self-cap is set by how many people are in the
  // room, not by what the regulator allows.
  #define LORA_DUTY_SELF_PCT   1.5f
  #define LORA_BEACON_MIN_MS      40000
  #define LORA_BEACON_MAX_MS      55000
  #define PROFILE_NAME   "LONG"
#elif LORA_PROFILE == LORA_PROFILE_EPIC
  #define LORA_FREQ      869.525
  #define LORA_SF        11
  #define LORA_PWR       22
  #define LORA_BAND      "g3"
  #define LORA_DUTY_LEGAL_PCT  10.0f
  #define LORA_DUTY_SELF_PCT   2.5f
  #define LORA_BEACON_MIN_MS      90000
  #define LORA_BEACON_MAX_MS     120000
  #define PROFILE_NAME   "EPIC"
#else
  #error "LORA_PROFILE must be FAST, LONG or EPIC"
#endif

// ── LoRa radio settings (SX1262, EU 868 MHz) ─
#define LORA_BW         125.0   // kHz — narrower would be more sensitive still,
#define LORA_BW_HZ      125000UL// but 125 is what fits g3's 250 kHz with margin
#define LORA_CR         5       // coding rate 4/5
#define LORA_SYNC       0x12    // private network sync word (not 0x34=LoRaWAN)
#define LORA_PREAMBLE   8

// ── Time-on-air at compile time ──────────────
//  Semtech AN1200.13, in integers so the link-layer timeouts can be DERIVED
//  from the profile rather than hardcoded for one spreading factor. They were
//  all written for SF7: TX_HARD_TIMEOUT_MS was 500 ms, and the largest frame
//  at SF10 takes 657 ms — the "recovery" timer would have aborted every large
//  frame mid-transmission, on a radio that was working perfectly.
constexpr uint32_t toaSymUs(int sf) {
  return (uint32_t)(((uint64_t)(1ULL << sf) * 1000000ULL) / LORA_BW_HZ);
}
// Low-data-rate optimise is mandatory once a symbol exceeds 16 ms (SF11/SF12
// at 125 kHz) and changes the payload maths, so it is not optional here.
constexpr int toaDe(int sf) { return toaSymUs(sf) > 16000 ? 1 : 0; }
constexpr int toaBlocks(int payload, int sf) {
  return (8 * payload - 4 * sf + 44) <= 0 ? 0
       : (8 * payload - 4 * sf + 44 + 4 * (sf - 2 * toaDe(sf)) - 1)
         / (4 * (sf - 2 * toaDe(sf)));
}
constexpr uint32_t toaUs(int payload, int sf) {
  return ((4 * LORA_PREAMBLE + 17) * toaSymUs(sf)) / 4
       + (8 + toaBlocks(payload, sf) * LORA_CR) * toaSymUs(sf);
}
constexpr uint32_t toaMs(int payload, int sf) { return (toaUs(payload, sf) + 999) / 1000; }

// The biggest thing that can be on the air, and the round trip it implies.
#define LORA_MAX_FRAME      64
constexpr uint32_t LORA_MAX_TOA_MS = toaMs(LORA_MAX_FRAME, LORA_SF);

// Checked against the float loraTimeOnAirMs() the firmware already shipped.
static_assert(toaMs(17, 7) == 52,  "SF7 beacon should be ~51.5 ms");
static_assert(toaMs(64, 7) == 119, "SF7 max frame should be ~118 ms");
static_assert(toaMs(64, 9) == 391, "SF9 max frame should be ~390 ms");
static_assert(toaMs(64, 12) == 2794, "SF12 max frame should be ~2793 ms");

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

// Both of these must live inside the pack(1) region: uint32_t after a uint8_t
// would otherwise take a pad byte, and the wire layout is not ours alone to
// choose. PktEcho is 44 bytes and PktMail 53, against the 60-byte pre-signature
// cap enforced by enqueueTx().
struct PktEcho {
  PktHeader hdr;                    // type=PKT_ECHO, to_id=0
  uint8_t   count;                  // how many of peers[] are populated
  uint32_t  peers[ECHO_MAX_PEERS];  // chip IDs heard DIRECTLY by the sender
};

struct PktMail {
  PktHeader hdr;                    // type=PKT_MAIL, to_id = who we hand it to
  uint32_t  final_id;               // who it is ultimately for
  uint32_t  origin_id;              // who wrote it — not always hdr.from_id
  uint8_t   carried;                // 0 = straight from the author, 1 = relayed
  char      text[MAIL_TEXT_MAX + 1];
};

#pragma pack(pop)

// enqueueTx() refuses anything over 64 - SIG_LEN and returns false, which for a
// reply means the frame is dropped and the peer reports NO RESPONSE — a bug
// that looks exactly like being out of range. Catch it at compile time instead.
// (SIG_LEN is 4, defined in cypher32_lora.h, which includes this header.)
#define PKT_WIRE_MAX 60
static_assert(sizeof(PktBeacon)     <= PKT_WIRE_MAX, "PktBeacon exceeds the frame cap");
static_assert(sizeof(PktReconReply) <= PKT_WIRE_MAX, "PktReconReply exceeds the frame cap");
static_assert(sizeof(PktHackReq)    <= PKT_WIRE_MAX, "PktHackReq exceeds the frame cap");
static_assert(sizeof(PktHackReply)  <= PKT_WIRE_MAX, "PktHackReply exceeds the frame cap");
static_assert(sizeof(PktHackResult) <= PKT_WIRE_MAX, "PktHackResult exceeds the frame cap");
static_assert(sizeof(PktMsg)        <= PKT_WIRE_MAX, "PktMsg exceeds the frame cap");
static_assert(sizeof(PktEcho)       <= PKT_WIRE_MAX, "PktEcho exceeds the frame cap");
static_assert(sizeof(PktMail)       <= PKT_WIRE_MAX, "PktMail exceeds the frame cap");
// And the packed layout must actually be packed — an alignment pad here is a
// wire-format change that only shows up as garbage on the other device.
static_assert(sizeof(PktHeader) == 11, "PktHeader must stay 11 bytes on the wire");
static_assert(sizeof(PktEcho) == 11 + 1 + 4 * ECHO_MAX_PEERS, "PktEcho has padding in it");
static_assert(sizeof(PktMail) == 11 + 4 + 4 + 1 + MAIL_TEXT_MAX + 1, "PktMail has padding in it");

// ── Node presence (T2.3) ─────────────────────
// Presence windows scale with the beacon cadence, which scales with the
// profile. Fixed at 90 s / 5 min these were sized for a 25-35 s beacon; at
// EPIC's 90-120 s cadence every device in the room would show FADING between
// its own beacons and then be evicted while standing right next to you.
// Three missed beacons is FADING, ten is gone.
#define NODE_ACTIVE_MS   ((unsigned long)(3UL * LORA_BEACON_MAX_MS))
#define NODE_FADING_MS   ((unsigned long)(10UL * LORA_BEACON_MAX_MS))
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
