# Cypher32

A portable hacking game for the **Heltec Wireless Paper V1.2** (ESP32-S3 + SX1262 LoRa + e-ink display). Devices discover each other over LoRa, run recon, attempt hacks, and earn XP to level up — all controlled through a phone-friendly web portal over Wi-Fi.

---

## Hardware

| Component | Detail |
|-----------|--------|
| Board | Heltec Wireless Paper V1.2 |
| MCU | ESP32-S3 |
| Display | 250 × 122 px e-ink (landscape) |
| Radio | SX1262 — 868 MHz EU ISM |
| Battery | LiPo via onboard charger |

---

## Quick start

1. Flash `cypher32_v52.ino` to your Wireless Paper (see [Build](#build)).
2. Connect to the **Cypher32** open Wi-Fi network the device broadcasts.
3. Open **192.168.4.1** in a browser.
4. Pick a faction and set a password — the device reboots as your hacker persona.
5. The display shows your character and status at all times.

> **Factory reset:** hold the PRG button for 5 seconds to wipe all progress and reboot to setup.

---

## Factions

| Faction | Starting bonus | Combat perk | Risk |
|---------|---------------|-------------|------|
| **BLACK** | +3 Brute Force | +20 % XP on every successful hack | Full 15 XP loss on fail (no Firewall reduction) |
| **WHITE** | +3 Firewall | Can only attack BLACK and RED; fail penalty halved | Restricted target pool |
| **RED** | +3 Stealth | +25 % XP vs GREEN targets | 15 % chance of XP loss even on a win |
| **GREEN** | +1 to all skills | +10 % XP vs BLACK targets | 25 % XP-loss risk when attacking WHITE |

---

## Skills

Earn one **Skill Point (SP)** per level-up. Spend it in the web portal under **Skills**.

| Skill | Effect |
|-------|--------|
| **Brute Force** | Shrinks the hack pool — raises hit probability each attempt |
| **Stealth** | +1 guess per point — more attempts per hack roll |
| **Firewall** | Reduces XP loss when counter-hacked (min 5 XP loss) |

Maximum skill value: **35** (3 from faction start + 32 from level-ups).

---

## Levelling

- **Max level:** 32
- **XP to next level:** `currentLevel × 150`  (150 XP at LVL 1 → 4 650 XP at LVL 31)
- **XP per successful hack:** `15 + enemyFirewall × 5`  (up to ~55 XP)
- XP cannot go below 0; excess XP at LVL 32 is discarded.

---

## LoRa protocol

All discovery and interaction uses **LoRa at 868 MHz, SF7, 125 kHz BW, CR 4/5, sync word 0x12**.  
No names, GPS, or RSSI data are ever transmitted — only chip IDs and game stats.

| Packet | Direction | Purpose |
|--------|-----------|---------|
| `BEACON` | broadcast | Announce level + faction every 30 s |
| `RECON_REQ` | unicast | Request one random stat from a target |
| `RECON_REPLY` | unicast | Return one stat (Brute / Stealth / Firewall) |
| `HACK_REQ` | unicast | Initiate a hack — carries attacker Brute stat |
| `HACK_REPLY` | unicast | Defender replies with Firewall stat + faction |
| `HACK_RESULT` | unicast | Attacker broadcasts win/loss + XP delta |
| `MSG` | unicast | Plain-text message (max 32 chars) |

All packet types are defined in `cypher32_packets.h`.

---

## Web portal

Connect to the device's Wi-Fi and open **192.168.4.1**.

| Tab | What you can do |
|-----|-----------------|
| **HUD** | View level, XP, skills, battery, LoRa diagnostics, send beacon |
| **Nodes** | See discovered nodes, run recon, attempt hacks |
| **Skills** | Spend skill points |
| **Messages** | Send and receive LoRa messages |
| **Settings** | Change password, LoRa info, factory reset |

---

## Hacking

1. Wait for nodes to appear in the **Nodes** tab (discovered via LoRa beacons).
2. Run up to **3 recon attempts** to reveal Brute / Stealth / Firewall stats.
3. Press **Hack (1 attempt)** — one roll, result shown on the e-ink display.
4. A successful hack locks the target for **7 days**.
5. A failed hack triggers a **12-hour retry cooldown** on that node.

**Hit chance:** base 60 % + 5 % per recon completed + 2 % per point of `(skillBrute − enemyFirewall)`, clamped 25 %–90 %.

---

## Display

The 250 × 122 e-ink screen updates only on game events (hack result, incoming message, level-up, mood drift every 10 min). This keeps refresh-lag imperceptible during normal use.

- **Header:** name, faction initial, level, battery %
- **Character sprite:** reacts to mood (idle / focused / victory / defeat)
- **Speech bubble:** idle quips, scan status, hack results
- **Footer:** XP bar, skill bars (B / S / F)

---

## Build

### Arduino IDE
1. Install **Heltec ESP32** board package.
2. Install libraries: **RadioLib** (≥ 7.1), **heltec-eink-modules**.
3. Open `cypher32_v52.ino` — all source files must be in the same folder.
4. Select board **Heltec Wireless Paper**, upload.

### PlatformIO
```
pio run -t upload
```
Dependencies are declared in `platformio.ini`.

---

## File structure

| File | Purpose |
|------|---------|
| `cypher32_v52.ino` | Main sketch — game logic, display, web portal |
| `cypher32_packets.h` | LoRa packet types, structs, `KnownNode`, `nodeNameFromId()` |
| `cypher32_lora.h` | Header-only LoRa driver (RadioLib SX1262 wrapper) |
| `platformio.ini` | PlatformIO build config |

---

## License

MIT
