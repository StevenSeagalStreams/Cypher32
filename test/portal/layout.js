// Does the page fit the phone?
//
// Two failures the DOM shim can never see, because it has no layout engine:
//
//   1. anything wider than the viewport. The reader has to pan or zoom out,
//      and on a captive-portal browser they may not realise they can.
//   2. a modal taller than the viewport that is centred with align-items.
//      The overflow goes off BOTH ends and the top becomes unreachable — no
//      amount of scrolling brings it back, only pinch-zoom. That is exactly
//      what the recon game did on every phone smaller than an iPhone 13.
//
// Run against real Chromium at real phone sizes, because those are questions
// only a layout engine can answer.
const fs = require("fs"), path = require("path");
let chromium;
try { ({ chromium } = require("playwright")); }
catch (e) { console.log("playwright not installed — skipping layout checks"); process.exit(0); }

const HTML = path.join(__dirname, "portal.html");
const PHONES = [
  ["iPhone SE (1st)",  320, 568],
  ["small Android",    360, 640],
  ["iPhone SE (2nd)",  375, 667],
  ["iPhone 13",        390, 844],
  ["Pixel 7",          412, 915],
];

const STATE = {
  configured: true, name: "GhostByte", faction: "BLACK", id: "a1b2c3d4",
  version: "v65", level: 7, xp: 42, xpNext: 70, sp: 2,
  brute: 11, stealth: 5, firewall: 8, battery: 66, onUsb: false,
  lora: { status: "Online", ready: true, rssi: -58, duty: 0.12 },
  action: { state: "SUCCESS", label: "RECON", tries: 1, pending: false },
  probe: { id: "beef0002", state: "ready" },
  nodes: [
    // Deliberately awkward: the longest name the game can derive, a message
    // with no spaces in it at all, and a cooldown long enough to print wide.
    { id: "beef0001", name: "SpecterGrid", level: 32, faction: "W", avgRssi: -55,
      bars: 4, proximity: "VERY CLOSE", status: "ACTIVE", ageMs: 4200, recon: 3,
      reconScore: 10, reconMax: 10, intel: 10, pwned: true,
      brute: 35, stealth: 35, firewall: 35, odds: 90,
      hackWon: true, cooldownMs: 43100000, canHack: true, canRecon: true,
      unread: true, msg: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" },
    { id: "beef0003", name: "UNKNOWN-0003", level: 0, faction: "?", avgRssi: -91,
      bars: 2, proximity: "DISTANT", status: "ACTIVE", ageMs: 8000, recon: 0,
      reconScore: 0, reconMax: 10, intel: 0, pwned: false,
      brute: -1, stealth: -1, firewall: -1, odds: -1,
      hackWon: false, cooldownMs: 0, canHack: true, canRecon: true,
      unread: false, msg: "" }],
  events: [{ t: "backdoored", who: "SpecterGrid", xp: 0, ageMs: 8000 },
           { t: "you breached", who: "SpecterGrid", xp: 175, ageMs: 900000 }],
};

let bad = 0;
const fail = (w) => { console.log("  FAIL:", w); bad++; };

(async () => {
  if (!fs.existsSync(HTML)) { console.log("no portal.html — run extract.py"); process.exit(1); }
  const html = fs.readFileSync(HTML, "utf8");
  const root = "/opt/pw-browsers";
  const dirs = fs.existsSync(root)
    ? fs.readdirSync(root).filter(d => /^chromium-/.test(d))
        .map(d => `${root}/${d}/chrome-linux/chrome`).filter(fs.existsSync)
    : [];
  let browser;
  try { browser = await chromium.launch(dirs.length ? { executablePath: dirs[0] } : {}); }
  catch (e) { console.log("no chromium — skipping layout checks"); process.exit(0); }

  for (const [label, w, h] of PHONES) {
    const ctx = await browser.newContext({ viewport: { width: w, height: h },
      deviceScaleFactor: 2, isMobile: true, hasTouch: true });
    const page = await ctx.newPage();
    await page.route("**/*", (r) => {
      const u = r.request().url();
      const j = (b) => r.fulfill({ status: 200, contentType: "application/json",
                                   body: JSON.stringify(b) });
      if (u.includes("/api/state"))  return j(STATE);
      if (u.includes("/api/reveal")) return j({ n: 2, field: "name",
                                                label: "CODENAME", value: "SpecterGrid" });
      if (u.includes("/api/diag"))   return j({ "Delivery ratio": "96%" });
      if (u.includes("/api/"))       return j({ ok: true });
      return r.fulfill({ status: 200, contentType: "text/html", body: html });
    });
    await page.goto("http://192.168.4.1/");
    await page.evaluate(() => { try { localStorage.setItem("c32pw", "secret") } catch (e) {} });
    await page.reload();
    await page.waitForFunction(() => window.S && window.S.configured === true);

    // ── 1. nothing sticks out sideways, on any tab ──
    for (const t of ["hud", "radar", "skills", "msgs", "log", "cfg", "diag"]) {
      await page.evaluate((x) => tab(x), t);
      await page.waitForTimeout(90);
      const over = await page.evaluate(() => {
        const vw = document.documentElement.clientWidth, out = [];
        document.querySelectorAll("*").forEach((el) => {
          const r = el.getBoundingClientRect();
          if (!r.width || getComputedStyle(el).display === "none") return;
          if (r.right > vw + 0.5 || r.left < -0.5)
            out.push(`${el.tagName.toLowerCase()}${el.id ? "#" + el.id : ""}` +
                     `${el.className ? "." + String(el.className).split(" ")[0] : ""}` +
                     ` [${Math.round(r.left)}..${Math.round(r.right)}] vw=${vw}`);
        });
        return { out, scrollW: document.documentElement.scrollWidth, vw };
      });
      if (over.out.length) fail(`${label} ${t}: ${over.out.length} element(s) off-screen — ${over.out[0]}`);
      if (over.scrollW > over.vw + 0.5)
        fail(`${label} ${t}: page scrolls sideways (${over.scrollW} > ${over.vw})`);
    }

    // ── 2. the banner pushes content down, it does not sit on top of it ──
    await page.evaluate(() => { tab("hud"); banner("WAITING FOR REPLY (2/4)…", "wait", true) });
    await page.waitForTimeout(90);
    const bnr = await page.evaluate(() => {
      const b = document.getElementById("banner").getBoundingClientRect();
      const first = document.querySelector("#t-hud .card").getBoundingClientRect();
      return { bBottom: Math.round(b.bottom), firstTop: Math.round(first.top),
               bTop: Math.round(b.top) };
    });
    if (bnr.bBottom > bnr.firstTop + 0.5)
      fail(`${label}: the banner covers the first card (banner ends ${bnr.bBottom}, card starts ${bnr.firstTop})`);
    if (bnr.bTop < -0.5) fail(`${label}: the banner is off the top of the screen`);
    await page.evaluate(() => { document.getElementById("banner").className = "banner" });

    // ── 3. every modal is reachable, and the game fits without scrolling ──
    // index 1 is the unidentified contact; index 0 is backdoored and would
    // skip the game entirely, which is not the layout under test here.
    await page.evaluate(() => { tab("radar"); seqFromRadar(1) });
    await page.waitForFunction(() => !document.getElementById("seqbtn").disabled);
    await page.evaluate(() => seqBegin());
    await page.waitForTimeout(260);
    const m = await page.evaluate(() => {
      const c = document.querySelector("#seqmodal .card").getBoundingClientRect();
      const g = document.getElementById("seqgrid").children[0].getBoundingClientRect();
      const el = document.getElementById("seqmodal");
      return { top: Math.round(c.top), bottom: Math.round(c.bottom),
               tile: Math.round(g.width), vh: innerHeight,
               scrolls: getComputedStyle(el).overflowY === "auto" };
    });
    if (m.top < -0.5)
      fail(`${label}: the recon game starts ${-m.top}px above the screen`);
    if (!m.scrolls && m.bottom > m.vh + 0.5)
      fail(`${label}: the recon game runs past the bottom with no way to scroll`);
    if (m.bottom > m.vh + 0.5)
      fail(`${label}: the recon game needs scrolling while you play ` +
           `(${m.bottom - m.vh}px over)`);
    if (m.tile < 44)
      fail(`${label}: recon tiles are ${m.tile}px, under the 44px tap minimum`);

    // The bug class, not just today's instance of it: force the card taller
    // than the screen and check the top is still reachable. A modal centred
    // with align-items pushes its overflow off both ends, and no amount of
    // scrolling brings the top back — only pinch-zoom.
    const tall = await page.evaluate(() => {
      const card = document.querySelector("#seqmodal .card");
      const el = document.getElementById("seqmodal");
      const spacer = document.createElement("div");
      spacer.style.height = "600px";
      card.appendChild(spacer);
      el.scrollTop = 0;
      const top = Math.round(card.getBoundingClientRect().top);
      el.scrollTop = el.scrollHeight;                 // no-op if it cannot scroll
      const bottom = Math.round(card.getBoundingClientRect().bottom);
      card.removeChild(spacer);
      el.scrollTop = 0;
      return { top, bottom, vh: innerHeight };
    });
    if (tall.top < -0.5)
      fail(`${label}: an over-tall modal starts ${-tall.top}px above the screen, ` +
           `and scrolling cannot bring it back`);
    if (tall.bottom > tall.vh + 0.5)
      fail(`${label}: an over-tall modal cannot be scrolled to its bottom ` +
           `(${tall.bottom - tall.vh}px still out of reach)`);

    await ctx.close();
  }
  await browser.close();
  console.log(bad ? `\n${bad} LAYOUT FAILURES` : "layout: fits every phone, nothing off-screen");
  process.exit(bad ? 1 : 0);
})();
