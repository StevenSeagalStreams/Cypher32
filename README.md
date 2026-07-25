# Cypher32

You are a ghost in the machine.

Cypher32 is a **physical hacking game** played on hardware you carry. Each device is a node — a silent transmitter hunting for others across the electromagnetic spectrum. When two players come within range their devices find each other automatically over LoRa radio, no internet, no infrastructure, no trail. From there you run recon, probe defenses, and strike. Win enough fights and you level up. Lose and you bleed XP.

No apps. No accounts. No names transmitted. Just chip IDs, stats, and outcomes.

The game runs on a **Heltec Wireless Paper V1.2** — an ESP32-S3 with a 250×122 e-ink display and a long-range SX1262 radio crammed onto a board the size of a credit card. Everything is controlled through a minimal web portal your device serves over its own Wi-Fi signal.

Your character is always watching you from the display. Its mood changes with your performance.

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

Flash it. Power it. Pick a side. That's all it takes to enter the network.

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
Dependencies are declared in `platformio.ini` and pulled automatically.

---

### Step 2 — Power on

Connect a LiPo battery or plug in USB. The e-ink display boots, then holds on the **setup screen** — waiting for you to identify yourself.

The device is already broadcasting. An open Wi-Fi network named **`Cypher32`** appears. No password. No ceremony.

---

### Step 3 — Connect

Join **`Cypher32`** on your phone or laptop. Open a browser:

```
192.168.4.1
```

The portal loads. You have one decision to make before you're locked in.

---

### Step 4 — Choose your faction and set a password

The setup screen asks two things:

1. **Faction** — your allegiance, your combat style, your starting advantage. Not reversible without a full wipe. Choose what fits how you fight (see [Factions](#factions)).
2. **Password** — locks the portal after reboot. Don't forget it.

Hit **Save & Reboot**. The device goes dark for a moment.

---

### Step 5 — You're in

The firmware generates your **hacker name** from your chip ID — deterministic, permanent, the same name every time this specific device boots. No two devices share a name.

The display comes alive:

- Name, faction, and level in the header
- Your character — idle for now, watching
- A speech bubble with something to say
- XP bar and skill indicators along the bottom

Your Wi-Fi SSID has changed to `C32_<Faction>_<Name>`. The portal now requires your password. And every 30 seconds your device pulses a LoRa beacon into the air, announcing your presence to anyone in range.

> **Factory reset:** hold **PRG** for 5 seconds. Everything wipes. You start over as nobody.

---

## Web portal

Your command interface. Connect to the device's Wi-Fi and open **192.168.4.1**.

| Tab | Function |
|-----|----------|
| **HUD** | Level, XP, skills, battery, LoRa signal, manual beacon |
| **Nodes** | All nodes in range — run recon, launch hacks |
| **Skills** | Spend skill points from level-ups |
| **Messages** | Send and receive LoRa text — 32 chars max |
| **Settings** | Change password, LoRa details, factory reset |

---

## Factions

Four factions. Each one plays differently. Pick the one that matches your instinct — you can't change it without losing everything.

| Faction | Bonus | Perk | Risk |
|---------|-------|------|------|
| **BLACK** | +3 Brute Force | +20% XP on every successful hack | Full 15 XP loss on fail — no mitigation |
| **WHITE** | +3 Firewall | Fail penalty halved; attacks restricted to BLACK and RED | Limited target pool |
| **RED** | +3 Stealth | +25% XP against GREEN targets | 15% chance of XP loss even on a win |
| **GREEN** | +1 all skills | +10% XP against BLACK targets | 25% XP-loss risk when attacking WHITE |

**BLACK** hits hard and pays for every failure in full.  
**WHITE** plays a defensive game and picks its fights carefully.  
**RED** is a gambler — even victories carry risk.  
**GREEN** starts balanced but earns less unless it exploits its matchups.

---

## Skills

One **Skill Point** per level-up. Spend it in the **Skills** tab. At high levels every point shifts the math.

| Skill | Effect |
|-------|--------|
| **Brute Force** | Narrows the hack pool — raises hit probability per attempt |
| **Stealth** | More attempts per roll — outlast their defenses |
| **Firewall** | Cuts XP lost when counter-hacked — floor is 5 XP |

Cap: **35** per skill (3 from faction + 32 earned through levels).

---

## Levelling

Every successful hack earns XP. Enough and you level up. The climb gets steeper the higher you go.

- **Max level:** 32
- **XP threshold:** `currentLevel × 150`
- **XP per hack:** `15 + target's Firewall × 5` — tougher targets pay more
- XP floor is 0. At LVL 32, surplus XP is discarded.

---

## How hacking works

This is what it's all for.

**1. Find a target.**  
Nodes appear in the **Nodes** tab when their beacon reaches you. Level and faction are visible. Everything else is dark until you probe for it.

**2. Run recon.**  
Up to 3 attempts. Each request goes out over LoRa and the target's device replies with a random stat — Brute, Stealth, or Firewall. They don't choose what you see. Their device just responds. Build a picture before you commit.

**3. Hack.**  
One attempt. One roll. The result appears on both displays at the same time.

**4. Win** — target locked for **7 days**. They're yours and they know it.

**5. Lose** — locked out of that node for **12 hours**. Move on.

**Hit chance:**
```
base 60%
+ 5% per recon completed  (max +15%)
+ 2% per point Brute Force over their Firewall
floor: 25%   ceiling: 90%
```

No guarantee. Never 100%.

---

## LoRa protocol

Signal only. No names. No location. Nothing beyond what the game requires.

All traffic runs at **868 MHz — SF7 — BW 125 kHz — CR 4/5 — sync 0x12**. Short airtime. Small packets. Devices are never quiet for long.

| Packet | Type | Purpose |
|--------|------|---------|
| `BEACON` | broadcast | Pulse every 30 s — level and faction only |
| `RECON_REQ` | unicast | Probe a target for one stat |
| `RECON_REPLY` | unicast | Target returns one random stat |
| `HACK_REQ` | unicast | Attack initiated — carries attacker's Brute |
| `HACK_REPLY` | unicast | Defender returns Firewall and faction |
| `HACK_RESULT` | unicast | Outcome and XP delta sent to defender |
| `MSG` | unicast | Raw text, 32 chars |
| `ACK` | unicast | Link-layer acknowledgement |
| `PING` | unicast | Reliable no-op — round-trip probe |

Defined in `cypher32_packets.h`.

Every unicast carries a sequence number and is acknowledged. Unacknowledged
frames are retried up to four times before the portal reports `NO RESPONSE` —
an action never just silently disappears. Duplicates are suppressed, replies are
deferred so they don't collide with the requester re-arming its receiver, and the
radio listens before transmitting.

Diagnostics live at `192.168.4.1/api/diag`.

---

## Display

The e-ink screen only refreshes when something happens — a hack result, an incoming message, a level-up, or a mood shift every 10 minutes. No wasted cycles. No flicker mid-play.

- **Header** — name, faction initial, level, battery %
- **Character** — idle / focused / victory / defeat
- **Speech bubble** — quips, scan status, hack outcomes
- **Footer** — XP bar, skill bars (B / S / F)

The character notices when you're losing.

---

## File structure

| File | Purpose |
|------|---------|
| `cypher32_v52.ino` | Main sketch — game logic, web portal, display |
| `cypher32_packets.h` | Packet types, structs, `KnownNode`, name generator |
| `cypher32_lora.h` | Header-only LoRa stack — link layer, retries, diagnostics |
| `platformio.ini` | PlatformIO build config |
| `ROADMAP.md` | Development plan and current status |
| `test/` | Host-side link-layer tests (`cd test && make`) |

---

## License

MIT
