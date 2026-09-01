// Screenshot the real portal.
//
// Not a mock-up: this loads the same HTML blob the ESP32 serves, straight out
// of cypher32_portal.h, and answers its API calls with a plausible device. If
// the page changes, the screenshots change with it, because there is no second
// copy of the UI anywhere.
//
//   node shoot_portal.js <out-dir>
const { chromium } = require("playwright");
const fs = require("fs");
const path = require("path");

const OUT = process.argv[2] || "../docs/img";
const HTML = path.join(__dirname, "portal", "portal.html");

// One believable radar: somebody you own, somebody you have half-read, and a
// contact that is still just a signal. Those three states are the whole point
// of the recon rewrite, so all three have to be in the picture.
const STATE = {
  configured: true, name: "GhostByte", faction: "BLACK", id: "a1b2c3d4",
  version: "v63", level: 7, xp: 42, xpNext: 70, sp: 2,
  brute: 11, stealth: 5, firewall: 8, battery: 66, onUsb: false,
  lora: { status: "Online", ready: true, rssi: -58, duty: 0.12 },
  // Resting state: a banner is real UI, but a stale one sitting over the header
  // in every screenshot is just noise.
  action: { state: "", label: "", tries: 0, pending: false },
  probe: { id: "beef0002", state: "ready" },
  stats: { won: 12, lost: 5, breached: 3, held: 9, met: 41, bestSeq: 10 },
  nodes: [
    { id: "beef0001", name: "VoidShell", level: 9, faction: "W", avgRssi: -55,
      bars: 4, proximity: "VERY CLOSE", status: "ACTIVE", ageMs: 4200, recon: 1,
      reconScore: 10, reconMax: 10, intel: 10, pwned: true,
      brute: 6, stealth: 4, firewall: 9, odds: 71,
      hackWon: false, cooldownMs: 0, canHack: true, canRecon: true,
      unread: true, msg: "north gate in ten" },
    { id: "beef0002", name: "NullGate", level: 3, faction: "R", avgRssi: -74,
      bars: 3, proximity: "CLOSE", status: "ACTIVE", ageMs: 26000, recon: 1,
      reconScore: 5, reconMax: 10, intel: 5, pwned: false,
      brute: -1, stealth: -1, firewall: -1, odds: -1,
      hackWon: false, cooldownMs: 0, canHack: true, canRecon: true,
      unread: false, msg: "" },
    { id: "beef0003", name: "UNKNOWN-0003", level: 0, faction: "?", avgRssi: -91,
      bars: 2, proximity: "DISTANT", status: "ACTIVE", ageMs: 8000, recon: 0,
      reconScore: 0, reconMax: 10, intel: 0, pwned: false,
      brute: -1, stealth: -1, firewall: -1, odds: -1,
      hackWon: false, cooldownMs: 0, canHack: true, canRecon: true,
      unread: false, msg: "" },
    { id: "beef0005", name: "AshVector", level: 6, faction: "B", avgRssi: -68,
      bars: 3, proximity: "CLOSE", status: "ACTIVE", ageMs: 15000, recon: 2,
      reconScore: 7, reconMax: 10, intel: 7, pwned: false,
      brute: 5, stealth: 2, firewall: 4, odds: 66,
      hackWon: false, cooldownMs: 0, canHack: true, canRecon: true,
      unread: false, msg: "" },
    { id: "beef0006", name: "UNKNOWN-0006", level: 0, faction: "?", avgRssi: -94,
      bars: 1, proximity: "DISTANT", status: "ACTIVE", ageMs: 32000, recon: 0,
      reconScore: 0, reconMax: 10, intel: 0, pwned: false,
      brute: -1, stealth: -1, firewall: -1, odds: -1,
      hackWon: false, cooldownMs: 0, canHack: true, canRecon: true,
      unread: false, msg: "" },
    { id: "beef0004", name: "IronCore", level: 12, faction: "G", avgRssi: -80,
      bars: 2, proximity: "DISTANT", status: "FADING", ageMs: 210000, recon: 3,
      reconScore: 9, reconMax: 10, intel: 9, pwned: false,
      brute: 14, stealth: 3, firewall: 11, odds: 47,
      hackWon: true, cooldownMs: 27000000, canHack: true, canRecon: false,
      unread: false, msg: "" }],
  events: [
    { t: "discovered",    who: "UNKNOWN-0003", xp: 0,   ageMs: 8000 },
    { t: "you scouted",   who: "NullGate",     xp: 5,   ageMs: 64000 },
    { t: "backdoored",    who: "VoidShell",    xp: 0,   ageMs: 300000 },
    { t: "you breached",  who: "IronCore",     xp: 70,  ageMs: 900000 },
    { t: "levelled up",   who: "",             xp: 0,   ageMs: 900000 },
    { t: "breached you",  who: "VoidShell",    xp: -9,  ageMs: 3600000 },
    { t: "message from",  who: "VoidShell",    xp: 0,   ageMs: 3700000 },
    { t: "firewall held", who: "IronCore",     xp: 4,   ageMs: 7200000 }],
};

const REVEALS = {
  2:  ["name",     "CODENAME", "NullGate"],
  4:  ["faction",  "FACTION",  "R"],
  6:  ["level",    "LEVEL",    "3"],
  7:  ["brute",    "BRUTE",    "7"],
  8:  ["stealth",  "STEALTH",  "2"],
  9:  ["firewall", "FIREWALL", "6"],
  10: ["pwned",    "BACKDOOR", "OPEN"],
};

(async () => {
  if (!fs.existsSync(HTML)) {
    console.error("run portal/extract.py first — no portal.html");
    process.exit(1);
  }
  const html = fs.readFileSync(HTML, "utf8");
  fs.mkdirSync(OUT, { recursive: true });

  // Whatever build of Chromium this machine actually has; the pinned path
  // moves between images.
  const glob = fs.readdirSync("/opt/pw-browsers")
    .filter(d => /^chromium-/.test(d))
    .map(d => `/opt/pw-browsers/${d}/chrome-linux/chrome`)
    .filter(fs.existsSync);
  const browser = await chromium.launch(
    glob.length ? { executablePath: glob[0] } : {});
  const ctx = await browser.newContext({
    viewport: { width: 400, height: 860 },
    deviceScaleFactor: 2,
    isMobile: true, hasTouch: true,
  });
  const page = await ctx.newPage();

  const json = (b) => ({ status: 200, contentType: "application/json",
                         body: JSON.stringify(b) });
  await page.route("**/*", (route) => {
    const u = route.request().url();
    if (u.includes("/api/state"))  return route.fulfill(json(STATE));
    if (u.includes("/api/reveal")) {
      const n = Number((u.match(/[?&]n=(\d+)/) || [])[1]);
      const t = REVEALS[n];
      return route.fulfill(json(t ? { n, field: t[0], label: t[1], value: t[2] }
                                  : { n }));
    }
    if (u.includes("/api/action")) return route.fulfill(json({ ok: true }));
    if (u.includes("/api/diag"))
      return route.fulfill(json({ Firmware: "v63", Uptime: "2h 14m",
        "Packets sent": 1284, "Packets received": 973, ACKs: 611, Retries: 42,
        Timeouts: 3, "Duplicates dropped": 17, "Replays rejected": 0,
        "Delivery ratio": "96%", "Duty cycle": "0.12% of 1%" }));
    return route.fulfill({ status: 200, contentType: "text/html", body: html });
  });

  await page.goto("http://192.168.4.1/");
  await page.evaluate(() => { try { localStorage.setItem("c32pw", "secret") } catch (e) {} });
  await page.reload();
  await page.waitForFunction(() => window.S && window.S.configured === true);
  await page.waitForTimeout(150);

  const shots = [];
  // Fit the viewport to the tab rather than shooting a fixed phone and leaving
  // half a screen of black under the short ones. The tab bar is position:fixed,
  // so it follows the resize and stays where it belongs.
  async function shot(name, note, fixedHeight) {
    await page.waitForTimeout(220);
    const h = fixedHeight || await page.evaluate(() =>
      Math.min(1500, Math.max(520, document.body.scrollHeight + 8)));
    await page.setViewportSize({ width: 400, height: h });
    await page.waitForTimeout(180);
    const file = path.join(OUT, `portal-${name}.png`);
    await page.screenshot({ path: file, fullPage: false });
    shots.push(name);
    console.log(`  ${name.padEnd(10)} ${String(h).padStart(4)}px  ${note}`);
  }

  await shot("hud", "level, XP, skills, radio");
  await page.evaluate(() => tab("radar"));
  await shot("radar", "owned / half-read / unidentified");

  // Play the mini-game far enough that the dossier is visibly coming apart.
  await page.evaluate(() => seqFromRadar(1));
  await page.waitForFunction(() => document.getElementById("seqbtn").disabled === false);
  await page.evaluate(async () => {
    const press = (i) => document.getElementById("seqgrid").children[i]
      .dispatchEvent(new PointerEvent("pointerdown", { bubbles: true, cancelable: true }));
    const wait = (ms) => new Promise(r => setTimeout(r, ms));
    seqBegin();
    for (let round = 0; round < 7; round++) {
      while (!seqAccepting) await wait(60);
      const target = seqOrder.slice();
      for (const k of target) { press(k); await wait(70); }
      if (target.length >= 7) break;
      await wait(220);
    }
    // Freeze on the player's turn with a tile lit, so the picture shows a game
    // in play rather than a results screen. Waiting for seqAccepting means the
    // playback interval has finished and will not overwrite the caption.
    while (!seqAccepting) await wait(60);
    document.getElementById("seqgrid").children[4].className = "lit";
    document.getElementById("seqmsg").textContent = "Your turn — repeat it.";
  });
  const modalH = await page.evaluate(() =>
    document.querySelector("#seqmodal .card").getBoundingClientRect().height + 40);
  await shot("recon", "the dossier coming apart, round by round", Math.ceil(modalH));
  await page.evaluate(() => { seqAccepting = false; seqPlaying = false;
                              document.getElementById("seqmodal").className = "modal hide" });

  await page.evaluate(() => tab("log"));
  await shot("log", "the last 20 things that happened");
  await page.evaluate(() => tab("msgs"));
  await shot("msgs", "32 characters over LoRa");
  await page.evaluate(() => tab("cfg"));
  await shot("cfg", "contact alert, password, reset");

  // First run, before any of the above exists. Step 2 is the interesting one:
  // the choice you cannot take back without wiping the character.
  await page.evaluate(() => { S = { configured: false }; render(); step(2) });
  await shot("setup", "first boot — pick a faction");

  await browser.close();
  console.log(`${shots.length} portal screens -> ${OUT}/`);
})();
