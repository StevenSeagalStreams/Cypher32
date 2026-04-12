# Cypher32

Cypher32 is a **real-world competitive hacking game** you play on physical hardware at events, hackathons, or with friends. Each player carries a **Heltec Wireless Paper V1.2** — a credit-card-sized board with an e-ink display, ESP32-S3 processor, and long-range LoRa radio. Devices automatically find each other over LoRa without any internet or phone connection, then players run recon, attempt hacks, and earn XP to level up their character.

There are no apps to install and no accounts to create. You flash the firmware once, power on the device, pick your faction, and you're in the game. The device's e-ink screen shows your character at all times. Everything else is controlled through a phone-friendly web portal served directly from the device over Wi-Fi.

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

## First-time setup

### Step 1 — Flash the firmware

**Arduino IDE:**
1. Install the **Heltec ESP32** board package via Boards Manager.
2. Install these libraries via Library Manager:
   - **RadioLib** (version 7.1 or later)
   - **heltec-eink-modules**
3. Open `cypher32_v52.ino`. All source files (`cypher32_packets.h`, `cypher32_lora.h`) must be in the same folder.
4. Select board: **Heltec Wireless Paper**.
5. Click **Upload**.

**PlatformIO:**
```
pio run -t upload
```
Dependencies are declared in `platformio.ini` and downloaded automatically.

---

### Step 2 — Power on

Connect a LiPo battery or plug in USB. The e-ink display will show the Cypher32 boot screen and then the **setup screen**, which means the device is ready for first-time configuration.

The device now broadcasts an open Wi-Fi network called **`Cypher32`** with no password.

---

### Step 3 — Connect your phone

On your phone or laptop, open Wi-Fi settings and join the **`Cypher32`** network. No password required.

Once connected, open a browser and go to:

```
192.168.4.1
```

You will see the Cypher32 web portal.

---

### Step 4 — Choose your faction and set a password

The setup page asks for two things:

1. **Faction** — this sets your starting bonus and combat style (see [Factions](#factions) below). Choose carefully; it can only be changed with a factory reset.
2. **Password** — this secures your device's web portal after setup. Pick something you'll remember.

Press **Save & Reboot**.

---

### Step 5 — After reboot

The device reboots, generates your **hacker name** (derived from the chip ID — same device always gets the same name), and starts the game. The e-ink display now shows:

- Your hacker name, faction, and level
- Your character sprite (mood changes with wins and losses)
- A speech bubble with idle quips or status messages
- XP bar and skill bars along the bottom

The Wi-Fi network is now renamed to `C32_<Faction>_<Name>` and the portal requires your password. Your device has also started transmitting LoRa beacons every 30 seconds so other players can discover you.

> **Factory reset:** hold the **PRG** button for 5 seconds at any time to wipe all progress and return to the setup screen.

---

## Using the web portal

Connect to your device's Wi-Fi and open **192.168.4.1**.

| Tab | What you can do |
|-----|-----------------|
| **HUD** | View level, XP, skills, battery %, LoRa diagnostics, send a manual beacon |
| **Nodes** | See all discovered players, run recon, attempt hacks |
| **Skills** | Spend skill points earned from levelling up |
| **Messages** | Send and receive short LoRa messages (max 32 chars) |
| **Settings** | Change your portal password, view LoRa info, factory reset |

---

## Factions

Choose your faction during first-time setup. Each faction starts with a skill bonus and has a unique combat perk and risk.

| Faction | Starting bonus | Combat perk | Risk |
|---------|---------------|-------------|------|
| **BLACK** | +3 Brute Force | +20 % XP on every successful hack | Full 15 XP loss on fail (no Firewall reduction) |
| **WHITE** | +3 Firewall | Can only attack BLACK and RED; fail penalty halved | Restricted target pool |
| **RED** | +3 Stealth | +25 % XP vs GREEN targets | 15 % chance of XP loss even on a win |
| **GREEN** | +1 to all skills | +10 % XP vs BLACK targets | 25 % XP-loss risk when attacking WHITE |

---

## Skills

Earn one **Skill Point (SP)** per level-up. Spend it in the **Skills** tab of the web portal.

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

## How hacking works

1. Wait for other players' devices to appear in the **Nodes** tab. Nodes are discovered automatically when their LoRa beacon is received.
2. Run up to **3 recon attempts** on a target to reveal their stats (Brute / Stealth / Firewall). Each recon request is sent over LoRa and the target device replies automatically — no action needed on their end.
3. When ready, press **Hack (1 attempt)**. One roll is made and the result appears on both devices' e-ink displays.
4. A successful hack locks the target for **7 days** — they cannot be hacked again until the cooldown expires.
5. A failed hack triggers a **12-hour retry cooldown** on that node for you.

**Hit chance formula:**
```
base 60 %
+ 5 % per recon completed (max +15 %)
+ 2 % per point of (your Brute Force − their Firewall)
clamped between 25 % and 90 %
```

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

## Display

The 250 × 122 e-ink screen updates only on game events (hack result, incoming message, level-up, mood drift every 10 min). This keeps refresh-lag imperceptible during normal use.

- **Header:** name, faction initial, level, battery %
- **Character sprite:** reacts to mood (idle / focused / victory / defeat)
- **Speech bubble:** idle quips, scan status, hack results
- **Footer:** XP bar, skill bars (B / S / F)

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
