# Cypher32 — Roadmap v52 → v60

**Goal:** LoRa that actually works between 2–8 devices in the field, and a portal a non-technical friend can use without being told anything.

**Hard constraints (do not violate):**
- Hardware stays: Heltec Wireless Paper V1.2, ESP32-S3, SX1262, 250×122 e-ink.
- The avatar stays exactly as-is — same sprites, same moods (idle / focused / victory / defeat), same speech bubble, same footer layout. UI work happens in the **web portal only**.
- Single-sketch Arduino/PlatformIO build. No RTOS rewrite, no LoRaWAN, no mesh library.

---

## Status

Current firmware: **v60**.

| Phase | State |
|-------|-------|
| **Phase 0 — instrumentation** | ✅ done, except **T0.4, which needs two real boards** |
| **Phase 1 — link layer** | ✅ done |
| **Phase 2 — presence & RSSI** | ✅ done |
| **Phase 3 — portal UX** | ✅ done, **except T3.2's ESPAsyncWebServer swap** (see below) |
| **Phase 4 — integrity & polish** | ✅ done |

### The one task deliberately not done, and why

**T3.2 asked for a move to `ESPAsyncWebServer`.** The single-page portal and the
`/api/state` + `/api/action` JSON split were built as specified — that is the
part that removes the stall, since a request now moves a few hundred bytes
instead of regenerating 20 KB of HTML inside a blocking handler.

The library swap itself was not done. `ESPAsyncWebServer` has several
incompatible forks on ESP32 (`me-no-dev` vs `esphome`, `AsyncTCP` vs
`AsyncTCP-esphome`) and this work was written without an ESP32 toolchain to
compile against. Picking the wrong fork means the firmware does not build at
all. The built-in `WebServer` ships with the core and is guaranteed to compile.

If you want it after the firmware is confirmed working on hardware, it is a
contained change: five `server.on()` registrations and the request/response
calls inside them.

### What "verified" means here

`test/` compiles the real headers against Arduino/RadioLib stubs and runs:

- **137 link-layer checks** — duplicate suppression, ACK/retry/timeout, slot
  separation, deferred replies, CAD, RX watchdog, node ageing and eviction,
  duty-cycle accounting, signing, replay protection, hack verdicts
- **24 two-node checks** — a full end-to-end simulation over a lossy
  half-duplex channel
- **portal render checks** — the real HTML blob extracted from
  `cypher32_portal.h`, run against a DOM shim

Measured in simulation: recon completes 100% of the time up to 40% packet loss,
87% at 55%, 63% at 70%. Timeouts surface in ~2.1 s. SHA-256 and HMAC match the
NIST and RFC 4231 vectors.

**This proves logic, not radios.** It says nothing about RadioLib behaviour, SPI
timing, interrupt latency, or RF range, and the firmware has never been compiled
for the target. **T0.4 and the field test protocol below remain required before
any of this can be called working.**

---

## Diagnosis — why LoRa "doesn't work"

Read from `cypher32_lora.h` as it stood at v52. These are the concrete faults, in order of how much damage they do:

| # | Fault | Evidence in code | Symptom in the field |
|---|-------|------------------|----------------------|
| D1 | **Zero reliability layer.** Every packet is fire-and-forget. No ACK, no retry, no sequence number. | `loraSend()` returns `true` the moment `radio.transmit()` succeeds — that only means *we transmitted*, not that anyone heard it. | Recon "does nothing." Hacks vanish. Messages never arrive. Randomly works at 2 m, never at 30 m. |
| D2 | **Beacon collision lock-step.** Fixed 30 s interval, no jitter. | `loraSendBeacon()` is called on a fixed cadence with no randomisation. | Two devices that drift into phase collide *every single beacon* and never discover each other — permanently. This is the classic "they can't see each other" bug. |
| D3 | **Instant turnaround race.** Replies are sent from inside the RX handler with no delay. | `PKT_RECON_REQ` → `loraSendHackReply()` / reply `loraSend()` called synchronously inside `loraHandlePacket()`. | Requester is still re-arming RX (`startReceive()` after a blocking `transmit()`) when the reply lands. Reply lost. Looks like the target "ignored" you. |
| D4 | **Blocking TX starves everything.** | `radio.transmit()` is the blocking variant — ~40–60 ms at SF7 for these payloads, radio deaf the whole time, loop stalled. | Portal feels frozen; incoming packets dropped during any TX; worse with 3+ devices. |
| D5 | **No RX watchdog.** If the radio falls out of RX, nothing recovers it. | `loraTick()` returns immediately when `!loraRxFlag`. Nothing ever verifies the SX1262 is still armed. | Device goes silently deaf until reboot. "It worked yesterday." |
| D6 | **No CAD (pure ALOHA).** Never listens before transmitting. | No `radio.scanChannel()` anywhere. | Collision rate rises sharply past 2 devices — exactly when the game gets interesting. |
| D7 | **RSSI is global, not per-node.** | `loraLastRSSI` is one int overwritten by whatever arrived last. | Portal can't show *how close* anyone is. No radar, no "warm/cold." |
| D8 | **Nodes never expire, `millis()` rollover unsafe.** | `last_seen_ms` is written but nothing prunes; raw comparisons. | Nodes tab fills with ghosts of people who left an hour ago. Breaks entirely at 49 days uptime. |
| D9 | **No TX diagnostics.** | `loraSend()` discards the RadioLib error code on failure. | Impossible to tell "no one in range" from "radio is broken." |
| D10 | **`loraSendReconStat()` ignores its argument.** | `(void)wantedStat;` then calls generic recon. | The API lies. Remove it or implement it. |
| D11 | **Encryption is a stub; packets are forgeable.** | `#define LORA_ENCRYPT 0`, `loraEncrypt()` is a no-op. | Anyone with an SX1262 on 868/SF7/sync 0x12 can inject `HACK_RESULT` and hand themselves XP. |
| D12 | **`platformio.ini` is documented but absent from the repo.** | README file table lists it; repo root has only 3 source files + README. | `pio run -t upload` fails for anyone who clones. |
| **D13** | **The hack never used the radio at all.** *(found during Phase 1 — not in the original diagnosis)* | `handleHack()` resolved the hack entirely from cached recon data. `loraSendHackReq()`, `loraSendHackResult()` and `PKT_ACK` had **no callers anywhere in the codebase**. | The defender was never told they had been attacked. "Hacks vanish" was not only a reliability problem — the hack packets were never transmitted in the first place. |
| **D14** | **Web handlers block the radio for seconds at a time.** *(found during Phase 1)* | `handleRecon()` busy-waited 5 s; `handleHack()` called `delay(4000)` / `delay(5000)`; the message path in `loop()` called `delay(5000)`. | Radio deaf for up to 9 s immediately after a hack — exactly when the defender's ACK and retries are in flight. |
| **D15** | **The portal password did nothing.** *(found during Phase 3)* | Setup collected and stored a password, but no handler ever checked it and the AP is open. | Anyone in radio range could open the portal and spend your skill points, send messages as you, or factory-reset your device. Now every mutation via `POST /api/action` requires it. |

**Root cause summary:** the LoRa layer is a *driver*, not a *protocol*. Phase 1 is about adding the missing link layer. That single change fixes D1, D3, and most of the perceived flakiness.

**Verification note.** D1–D11 were all confirmed present in the code before work
started. D12 was already fixed on the feature branch. D13 and D14 were found
while implementing Phase 1, D15 while implementing Phase 3; all are addressed.

---

## Phase 0 — Instrumentation (v53)

You cannot fix a radio you can't see. Nothing else starts until this ships.

| | Task | Description | Acceptance criteria |
|---|------|-------------|---------------------|
| ✅ | **T0.1** | Add `LORA_DEBUG` serial channel: log every TX (type, dest, len, result code) and RX (type, src, len, RSSI, SNR) with `millis()` timestamp. | Plug two boards into one laptop, run two serial monitors, see the full conversation of both sides. |
| ✅ | **T0.2** | Capture RadioLib error codes on failure. Add `loraTxErrors`, `loraLastTxError`, `loraCrcErrors`, `loraDroppedNotForMe`. | `readData()` returning `RADIOLIB_ERR_CRC_MISMATCH` increments a counter instead of silently returning. |
| ✅ | **T0.3** | `/api/diag` JSON endpoint exposing every counter already in the file (`loraPktSent`, `loraPktRecv`, `loraBeaconsSent`, `loraInitError`, `loraLastPktType`, `loraLastPktLen`) plus the new ones. | `curl 192.168.4.1/api/diag` returns valid JSON with all counters. |
| ⬜ | **T0.4** | **Loopback bench test.** Two boards on a desk, beacon interval forced to 5 s, run 30 min unattended. | Log a *packet delivery ratio*. This number is the baseline you improve against for the rest of the roadmap. Write it down. |
| ✅ | **T0.5** | Commit the missing `platformio.ini` (D12). Pin RadioLib `^7.1`, `heltec-eink-modules`, board `heltec_wifi_lora_32_V3`-class env for Wireless Paper. | Fresh `git clone` → `pio run -t upload` succeeds on a clean machine. |

**T0.4 is the one task here that cannot be done without hardware.** Set
`LORA_BEACON_MIN_MS` / `LORA_BEACON_MAX_MS` in `cypher32_lora.h` to `5000` for
the run, then read `deliveryPct` from `/api/diag`. Record the number here.

---

## Phase 1 — Link layer: make LoRa reliable (v54)

This is the phase that makes the project work. Everything below is small and self-contained.

### ✅ T1.1 — Sequence numbers + duplicate suppression

Extend `PktHeader` in `cypher32_packets.h`:

```c
typedef struct __attribute__((packed)) {
  uint8_t  type;
  uint8_t  seq;        // NEW: per-sender rolling counter
  uint8_t  flags;      // NEW: bit0 = ACK_REQUESTED, bit1 = IS_ACK
  uint32_t from_id;
  uint32_t to_id;
} PktHeader;
```

Keep a small ring of recently-seen `(from_id, seq)` pairs (16 entries is plenty) and drop repeats in `loraHandlePacket()` before the switch.

**Acceptance:** a deliberately duplicated packet is processed exactly once; counter `loraDupsDropped` increments.

### ✅ T1.2 — ACK + retry queue

One outbound slot is enough for this game — you never have two hacks in flight.

```c
struct PendingTx {
  uint8_t  buf[64];
  uint8_t  len;
  uint32_t to_id;
  uint8_t  seq;
  uint8_t  triesLeft;      // start at 4
  uint32_t nextAttemptMs;
  bool     active;
} pendingTx;
```

- `loraSendReliable()` fills the slot, transmits, sets `nextAttemptMs = millis() + 400 + random(0,300)`.
- Receiver of any unicast with `ACK_REQUESTED` immediately queues a bare ACK carrying the same `seq`.
- Matching ACK clears the slot. `triesLeft` hitting zero raises a **timeout event** — do not fail silently, this is what the UI shows.
- Beacons stay unreliable broadcast (no ACK).

**Acceptance:** with one board powered off, a hack attempt reports `TIMEOUT` in the portal within ~3 s instead of hanging forever. With both on, delivery ratio at 30 m ≥ 95%.

### ✅ T1.3 — Deferred replies (fixes D3)

Never call `loraSend()` from inside `loraHandlePacket()`. Push the reply into a small outbound queue with `sendAfterMs = millis() + 60 + random(0, 60)` and drain it from `loraTick()`.

**Acceptance:** recon reply success rate on the bench jumps measurably vs. the T0.4 baseline.

### ✅ T1.4 — Non-blocking TX (fixes D4)

Swap `radio.transmit()` for `radio.startTransmit()` + a `txDone` flag on the DIO1 action, with a state machine: `IDLE → TX_PENDING → TX_DONE → re-arm RX`. Guard with a 500 ms hard timeout that forces `standby()` + `startReceive()`.

**Acceptance:** main loop never blocks >5 ms; portal stays responsive while a hack is transmitting.

### ✅ T1.5 — Channel Activity Detection (fixes D6)

Before any TX: `radio.scanChannel()`. If busy, back off `random(20, 120)` ms and retry, max 5 attempts, then send anyway.

**Acceptance:** with 3 boards beaconing at 5 s, collision-induced loss drops vs. baseline.

### ✅ T1.6 — RX watchdog (fixes D5)

Every 10 s, if no packet has been received *and* no TX is pending, re-issue `startReceive()` and check its return. Three consecutive failures → full `loraSetup()` re-init and set `loraStatus = "Recovered"`.

**Acceptance:** yank the radio into a bad state deliberately (force an error path); device self-recovers within 30 s without reboot.

### ✅ T1.7 — Beacon jitter (fixes D2)

```c
nextBeaconMs = millis() + 25000 + random(0, 10000);   // 25–35 s
```

Plus: beacon **immediately on boot**, then again at +3 s and +8 s, so a device joining a group is discovered in seconds rather than up to half a minute.

**Acceptance:** two devices booted simultaneously discover each other in <10 s, repeatably, 10 trials.

---

## ✅ Phase 2 — Presence & discovery (v55)

| Task | Description | Acceptance criteria |
|------|-------------|---------------------|
| **T2.1** | Per-node RSSI and SNR stored in `KnownNode` (fixes D7). Keep a 4-sample rolling average so the bar doesn't jitter. | `/api/nodes` returns per-node `rssi`, `snr`, `avg_rssi`. |
| **T2.2** | Rollover-safe age everywhere: `(uint32_t)(millis() - n->last_seen_ms)` (fixes D8). | Fake `millis()` near `UINT32_MAX` in a unit test; ages stay correct. |
| **T2.3** | Node TTL: `ACTIVE` <90 s, `FADING` 90 s–5 min, then evict. Evicting frees the slot for a real neighbour. | Walk out of range; node greys out then disappears. Nodes tab never shows >5 min stale entries. |
| **T2.4** | Adaptive beacon: 25–35 s when nodes are visible, 12–18 s for the first 3 min after boot or after any new node appears. | Faster discovery without raising average airtime. |
| **T2.5** | **EU duty-cycle budget.** Track cumulative airtime in a rolling 60 min window for the 868 g1 sub-band; soft-cap at 0.8% and defer non-urgent TX above it. | `/api/diag` shows `dutyCyclePct`. Retry storms cannot push you over the legal limit. |

---

## ✅ Phase 3 — Portal UX (v56–v57) — avatar untouched

The e-ink screen is unchanged. All of this is the web portal.

### ✅ T3.1 — Captive portal (the single biggest UX win)

Right now: join an SSID with no internet → the phone silently drops back to cellular → "the website doesn't load." Fix:

- Run `DNSServer` on port 53, wildcard `*` → `192.168.4.1`.
- Respond to the OS probe paths (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, `/connecttest.txt`) with a redirect.
- Add mDNS: `cypher32.local`.

**Acceptance:** join the Wi-Fi on stock iOS and stock Android — the portal opens **by itself**. No typed IP. Test on both.

### ◐ T3.2 — Async server + single-page portal (SPA done, library swap deferred)

Move to `ESPAsyncWebServer`. Serve one gzipped HTML/CSS/JS blob from PROGMEM. All state via `GET /api/state` (poll 2 s) and actions via `POST /api/action`. No page reloads anywhere.

**Acceptance:** no request handler blocks the LoRa loop; measured packet loss during heavy portal use is unchanged from idle.

### ✅ T3.3 — Mobile-first redesign

- Bottom tab bar, thumb-reachable, min 44 px tap targets.
- Dark terminal aesthetic that matches the e-ink avatar's world — but readable in daylight, which means real contrast, not grey-on-black.
- Everything must work one-handed on a 360 px-wide screen.

### ✅ T3.4 — Nodes tab → **Radar**

The centrepiece. Replace the flat list with:
- Signal bars driven by `avg_rssi` (▁▃▅▇), plus plain-language distance: `VERY CLOSE / CLOSE / DISTANT / FADING`.
- Faction colour chip, level, and last-seen countdown per node.
- Recon progress as `●●○` — 2 of 3 stats known, at a glance.
- Lockout timers rendered as live countdowns (`6d 4h` won / `11h 22m` locked out), not raw timestamps.
- Node cards sorted by signal strength, strongest first.

### ✅ T3.5 — Feedback on every action (depends on T1.2)

This is why the ACK layer had to come first. Every action gets visible state:

`SENDING… → WAITING FOR REPLY (2/4) → SUCCESS` or `NO RESPONSE — target out of range`

**Acceptance:** no button in the portal can be pressed and produce no visible result. Zero exceptions.

### ✅ T3.6 — Onboarding wizard

Faction choice is permanent and currently presented as four names with no context. Replace with a 3-step wizard: what Cypher32 is → faction cards showing bonus/perk/risk in plain language → password + confirm, with a strength hint and an explicit *"this cannot be changed without a full wipe"* warning.

**Acceptance:** someone who has never seen the project picks a faction and reaches the HUD without asking a question.

### ✅ T3.7 — Diagnostics tab (hidden)

Long-press the header to reveal. Surfaces everything from T0.3, a live packet log, RSSI history sparkline, duty-cycle gauge, and a **"Ping node"** button that sends a reliable no-op and reports round-trip time. This is your field debugging tool.

---

## ✅ Phase 4 — Game integrity & polish (v58–v60)

| Task | Description | Acceptance criteria |
|------|-------------|---------------------|
| **T4.1** | **HMAC packet signing** (fixes D11). Shared game secret compiled in; append truncated 4-byte HMAC-SHA256 over the packet. Reject bad signatures. Not real security — it stops trivial XP injection, which is the actual problem. | Forged `HACK_RESULT` from a third board is rejected; counter `loraBadSig` increments. |
| **T4.2** | Replay protection: reject any `HACK_RESULT` whose `seq` is not newer than the last seen from that sender. | Replaying a captured winning packet grants no XP. |
| **T4.3** | Server-authoritative-ish hack resolution: the *defender* computes the roll and returns the outcome, so the attacker's firmware can't just declare a win. | Modified attacker firmware cannot force a win. |
| **T4.4** | Persist `knownNodes` lockout timers to NVS so a reboot doesn't reset a 7-day lock. | Power-cycle mid-lock; timer survives. |
| **T4.5** | Fix or delete `loraSendReconStat()` (D10). Recommendation: **delete it** — random stat reveal is better game design than letting players pick. | No dead API surface. |
| **T4.6** | E-ink refresh budget: cap full refreshes, use partial where the library allows. Avatar rendering itself unchanged. | Fewer visible flashes over a 1 h session; battery life measured and recorded. |
| **T4.7** | Repo hygiene: rename `cypher32_v52.ino` → `cypher32.ino` with a `FIRMWARE_VERSION` constant, add git tags per release, update README's file table to match reality. | Version is in one place, shown in the HUD footer and `/api/diag`. |

---

## Field test protocol

Run this at the end of Phase 1 and again at the end of Phase 3. Same route both times, so the numbers compare.

1. **Bench (0 m):** 2 devices, 30 min, delivery ratio ≥ 99%.
2. **Indoor (through 2 walls):** ≥ 90%.
3. **Outdoor LOS 300 m:** ≥ 85%, discovery <15 s.
4. **Urban 150 m, buildings between:** ≥ 60%, no permanent deafness.
5. **Group of 4, one room, 15 min:** every device sees all 3 others within 60 s; no device gets starved out.
6. **Battery:** LiPo full → empty, normal play. Record hours. Baseline for T4.6.

---

## Sequencing

```
Phase 0  ─── instrumentation ─── you cannot skip this
   │
Phase 1  ─── LoRa link layer ─── "I want LoRa to work" lives here
   │
Phase 2  ─── presence & RSSI ─── feeds the radar UI
   │
Phase 3  ─── portal UX ─────── T3.5 depends on T1.2 (ACKs)
   │
Phase 4  ─── integrity & polish
```

**All phases are now implemented. The remaining work is hardware
verification — T0.4 and the field test protocol — not more code.**

**If you only do one phase, do Phase 1.** D1 (no ACK/retry) and D2 (no beacon jitter) are, between them, almost certainly the entire "LoRa doesn't work" experience. T1.7 is roughly a five-line change and may fix discovery on its own.

---

## Explicitly out of scope

- Any hardware change — no external antenna, no different board, no added sensors.
- Any change to the avatar sprites, moods, speech bubble, or e-ink layout.
- Mesh routing / multi-hop. Direct range only.
- Cloud, accounts, internet, telemetry. The premise is no infrastructure; keep it.
- Native mobile app. The captive portal *is* the app.
