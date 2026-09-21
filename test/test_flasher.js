// The browser flasher page, driven in a real browser.
//
// This page has one job and a lot of ways to fail quietly at it: the profile
// picker has to actually change which firmware gets written, and the page has
// to say something useful when it cannot flash at all. Both of those are
// invisible to anyone reading the HTML — the first version of the picker set a
// JS property on a custom element that had not upgraded yet, which looked
// correct and silently offered the wrong build.
//
// Run: node test_flasher.js           (from test/)
const { chromium } = require("playwright");
const http = require("http");
const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..");
const PAGE = path.join(ROOT, "web", "index.html");
const PORT = 8934;

let bad = 0;
function ck(cond, what) { if (!cond) { console.log("  FAIL:", what); bad++; } }

// A stand-in for what CI publishes beside the page.
const SITE = {
  "index.html": fs.readFileSync(PAGE),
  "build-info.json": Buffer.from(JSON.stringify({
    version: "v71", commit: "abc1234def5678", built: "2026-01-01",
    builds: [{ profile: "fast", bytes: 900000 },
             { profile: "long", bytes: 905000 },
             { profile: "epic", bytes: 910000 }],
  })),
};

(async () => {
  const srv = http.createServer((req, res) => {
    const name = (req.url === "/" ? "index.html" : req.url.slice(1)).split("?")[0];
    if (!SITE[name]) { res.writeHead(404); return res.end("not here"); }
    res.writeHead(200, { "Content-Type":
      name.endsWith(".json") ? "application/json" : "text/html" });
    res.end(SITE[name]);
  });
  await new Promise((r) => srv.listen(PORT, r));

  const browser = await chromium.launch();
  const errors = [];

  // ── the page, with the CDN unreachable ──
  // Deliberate: this is the state an ad blocker or an offline machine puts it
  // in, and it must still be a coherent page rather than every slot at once.
  const p = await browser.newPage({ viewport: { width: 400, height: 900 } });
  p.on("pageerror", (e) => errors.push("pageerror: " + e.message));
  await p.route("**unpkg.com**", (r) => r.abort());
  await p.goto(`http://127.0.0.1:${PORT}/`);
  await p.waitForTimeout(500);

  console.log("flasher page");

  ck(await p.getAttribute("#installer", "manifest") === "manifest-long.json",
     "LONG is the profile offered by default");

  for (const profile of ["epic", "fast", "long"]) {
    await p.click(`input[value="${profile}"]`);
    await p.waitForTimeout(50);
    ck(await p.getAttribute("#installer", "manifest") === `manifest-${profile}.json`,
       `picking ${profile.toUpperCase()} changes which firmware is installed`);
  }

  // The conditional slots must stay hidden while the element is un-upgraded,
  // or a blocked CDN shows the install button and a contradictory error at
  // the same time.
  const slotsVisible = await p.evaluate(() =>
    [...document.querySelectorAll('#installer > [slot]')]
      .filter((e) => e.getAttribute("slot") !== "activate")
      .filter((e) => e.offsetParent !== null).length);
  ck(slotsVisible === 0,
     "no conditional slot is visible before the element upgrades");
  ck(await p.isVisible('#installer > [slot="activate"]'),
     "but the install button itself is");

  // And it says so once the script has plainly failed to arrive.
  ck(await p.isHidden("#cdnfail"), "no CDN warning before the grace period");
  await p.evaluate(() => { /* fast-forward the 8 s check */ });
  await p.waitForTimeout(8200);
  ck(await p.isVisible("#cdnfail"),
     "a CDN that never loads is reported rather than left as an inert button");

  // ── build information comes from the file CI writes, not from the HTML ──
  const info = await p.evaluate(() => ({
    rows: document.querySelectorAll("#buildinfo tbody tr").length,
    text: document.querySelector("#buildinfo").textContent.replace(/\s+/g, " "),
    built: [...document.querySelectorAll("p")].map((e) => e.textContent)
             .find((t) => t.includes("Built")) || "",
  }));
  ck(info.rows === 4, "one row per profile, plus a header");
  ck(/FAST/.test(info.text) && /LONG/.test(info.text) && /EPIC/.test(info.text),
     "every profile is listed with its size");
  ck(/v71/.test(info.text), "and the firmware version it was built from");
  ck(info.built.includes("abc1234"),
     "the commit is named, so a stale publish is visible");

  ck(await p.evaluate(() => document.documentElement.scrollWidth <=
                            document.documentElement.clientWidth),
     "the page does not scroll sideways at 400px");

  // ── the browser gate ──
  const q = await browser.newPage();
  await q.addInitScript(() => { delete Object.getPrototypeOf(navigator).serial; });
  await q.route("**unpkg.com**", (r) => r.abort());
  await q.goto(`http://127.0.0.1:${PORT}/`);
  await q.waitForTimeout(200);
  ck(await q.isVisible("#nosupport"),
     "a browser without Web Serial is told so up front");
  const gate = await q.textContent("#nosupport");
  ck(/Firefox/.test(gate) && /Safari/.test(gate) && /Android/.test(gate),
     "and told which browsers those are, including on phones");
  ck(await p.isHidden("#nosupport"),
     "while a browser that can flash sees no such warning");

  // ── what it promises about the install ──
  const body = await p.textContent("body");
  ck(/antenna/i.test(body),
     "the antenna warning is on the page, not only in the README");
  ck(/replaces everything/i.test(body),
     "and it says flashing wipes the device");
  ck(/same profile/i.test(body),
     "and that every device in one game needs the same profile");

  console.log(errors.length ? "\nPAGE ERRORS:\n" + errors.join("\n") : "");
  console.log(bad ? `\n${bad} FAILURES` : "\nflasher page checks passed");
  await browser.close();
  srv.close();
  process.exit(bad ? 1 : 0);
})();
