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

// navigator.serial is an accessor on Navigator.prototype, so `navigator.serial
// = fake` is a silent no-op — and reading it back still returns the native
// function, so it even looks like it worked. defineProperty is the only way in.
async function fakeSerial(page, info) {
  await page.evaluate((usb) => {
    Object.defineProperty(navigator, "serial", {
      configurable: true,
      value: {
        requestPort: async () => {
          if (usb === null) {
            const e = new Error("No port selected by the user.");
            e.name = "NotFoundError";
            throw e;
          }
          return { getInfo: () => usb };
        },
      },
    });
  }, info);
}

// A stand-in for what CI publishes beside the page.
// A stand-in for the esptool-js bundle CI publishes beside the page. The page
// imports it on demand, so the test serves a module with the same two exports
// and a board of its choosing behind them.
function espStub(board) {
  return Buffer.from(`
    export class Transport {
      constructor(port) { this.port = port; globalThis.__espTransports = (globalThis.__espTransports||0)+1; }
      async disconnect() { globalThis.__espDisconnects = (globalThis.__espDisconnects||0)+1; }
    }
    export class ESPLoader {
      constructor(o) { this.transport = o.transport; this.chip = { CHIP_NAME: ${JSON.stringify(board.chip)} }; }
      async main() { ${board.mainThrows ? 'throw new Error("Failed to connect to ESP32-S3");' : ''}
                     return ${JSON.stringify(board.description || board.chip)}; }
      async detectFlashSize() { ${board.flashThrows ? 'throw new Error("no flash id");' : ''}
                                return ${JSON.stringify(board.flashSize)}; }
    }
  `);
}

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

  // ── the gate ──────────────────────────────────────────────────────────────
  // Checked before anything unlocks it, so this is the page as it first loads.
  console.log("install gate");
  const gateState = () => p.evaluate(() => ({
    gate: !document.getElementById("installgate").hidden,
    install: !document.getElementById("installwrap").hidden,
  }));
  let g = await gateState();
  ck(g.gate && !g.install, "the install button is withheld until the board is checked");
  ck(/nobody writes Cypher32 over something/i.test(await p.textContent("#installgate")),
     "and says why it is withheld");

  // A board that passes unlocks it.
  await p.route("**/esptool.js", (r) =>
    r.fulfill({ contentType: "text/javascript",
                body: espStub({ chip: "ESP32-S3", flashSize: "8MB",
                                description: "ESP32-S3 (QFN56) (revision v0.2)" }).toString() }));
  await fakeSerial(p, { usbVendorId: 0x10c4, usbProductId: 0xea60 });
  await p.click("#checkbtn");
  await p.waitForTimeout(400);
  g = await gateState();
  ck(g.install, "a board that passes unlocks the install button");
  ck(!g.gate, "and the explanation for withholding it goes away");
  ck(/consistent with/i.test(await p.textContent("#checkresult")),
     "with the verdict shown");
  ck(/QFN56/.test(await p.textContent("#checkresult")),
     "and the raw reported identity, so it can be judged by eye");
  ck(await p.evaluate(() => globalThis.__espDisconnects > 0),
     "and the port is released so the installer can open it");

  // Now that the install area is on screen, the CDN behaviour can be seen.
  const slotsVisible = await p.evaluate(() =>
    [...document.querySelectorAll('#installer > [slot]')]
      .filter((e) => e.getAttribute("slot") !== "activate")
      .filter((e) => e.offsetParent !== null).length);
  ck(slotsVisible === 0,
     "no conditional slot is visible before the element upgrades");
  ck(await p.isVisible('#installer > [slot="activate"]'),
     "but the install button itself is");
  await p.waitForTimeout(8200);
  ck(await p.isVisible("#cdnfail"),
     "a CDN that never loads is reported rather than left as an inert button");

  // ── the board check ───────────────────────────────────────────────────────
  // The verdict is a pure function, so the whole decision table can be driven
  // without a board. These are the cases that decide whether somebody writes
  // Cypher32 over the wrong device.
  console.log("board verdict");
  const V = (o) => p.evaluate((i) => window.boardVerdict(i), o);
  const CP = { vendorId: 0x10c4, productId: 0xea60 };

  let v = await V({ chip: "ESP32-S3", flashSize: "8MB", ...CP });
  ck(v.level === "ok", "an ESP32-S3 with 8 MB on a CP210x passes");
  ck(/consistent with/i.test(v.headline), "and is described as consistent, not confirmed");
  ck(/WiFi LoRa 32 V3|would look identical/i.test(v.detail),
     "and says plainly what it cannot distinguish");

  // The one the installer itself would NOT catch: right family, wrong size.
  v = await V({ chip: "ESP32-S3", flashSize: "4MB", ...CP });
  ck(v.level === "fail", "an ESP32-S3 with only 4 MB of flash is refused");
  ck(/4MB/.test(v.headline) && /8 MB/.test(v.headline),
     "naming both what it found and what is needed");

  v = await V({ chip: "ESP32-C3", flashSize: "8MB", ...CP });
  ck(v.level === "fail", "a different chip family is refused");
  ck(/ESP32-C3/.test(v.detail), "quoting what the board actually said it is");
  ck(/nothing has been written/i.test(v.detail),
     "and reassuring that the check wrote nothing");

  v = await V({ chip: "ESP32", flashSize: "8MB", ...CP });
  ck(v.level === "fail", "a plain ESP32 is refused, not matched as a prefix");

  v = await V({ chip: "", flashSize: "8MB", ...CP });
  ck(v.level === "fail", "a board that will not say what it is, is refused");

  // Corroboration, never a veto in either direction.
  v = await V({ chip: "ESP32-S3", flashSize: "8MB", vendorId: 0x303a, productId: 0x1001 });
  ck(v.level === "warn", "native USB rather than a CP210x lowers confidence");
  ck(!/fail/.test(v.level), "but does not refuse \u2014 the chip is still right");
  v = await V({ chip: "ESP32-S3", flashSize: "8MB", vendorId: 0x1a86, productId: 0x7523 });
  ck(v.level === "warn", "an unfamiliar bridge lowers confidence");
  ck(/0x1a86/.test(v.notes.join(" ")), "and reports which one it saw");
  v = await V({ chip: "ESP32-C3", flashSize: "8MB", ...CP });
  ck(v.level === "fail", "and the right bridge cannot rescue the wrong chip");

  v = await V({ chip: "ESP32-S3", flashSize: null, ...CP });
  ck(v.level === "warn", "an unreadable flash size is a warning, not a pass");
  ck(/could not be read/i.test(v.notes.join(" ")), "and says so");

  ck((await V({ chip: "esp32-s3", flashSize: "8mb", ...CP })).level === "ok",
     "the comparison is case-insensitive");

  // A bad board must NOT unlock it.
  const q2 = await browser.newPage({ viewport: { width: 400, height: 900 } });
  await q2.route("**unpkg.com**", (r) => r.abort());
  await q2.route("**/esptool.js", (r) =>
    r.fulfill({ contentType: "text/javascript",
                body: espStub({ chip: "ESP32-C3", flashSize: "4MB" }).toString() }));
  await q2.goto(`http://127.0.0.1:${PORT}/`);
  await fakeSerial(q2, { usbVendorId: 0x10c4, usbProductId: 0xea60 });
  await q2.click("#checkbtn");
  await q2.waitForTimeout(400);
  ck(await q2.isHidden("#installwrap"),
     "a board that fails the check does NOT unlock the install button");
  ck(/not an ESP32-S3/i.test(await q2.textContent("#checkresult")),
     "and is told why");

  // A check that cannot run must fail OPEN — otherwise a broken check is a
  // page nobody can install from.
  const q3 = await browser.newPage({ viewport: { width: 400, height: 900 } });
  await q3.route("**unpkg.com**", (r) => r.abort());
  await q3.route("**/esptool.js", (r) => r.abort());
  await q3.goto(`http://127.0.0.1:${PORT}/`);
  await fakeSerial(q3, {});
  await q3.click("#checkbtn");
  await q3.waitForTimeout(500);
  ck(await q3.isVisible("#installwrap"),
     "a check that cannot run still lets you install");
  ck(/could not be identified|could not run/i.test(await q3.textContent("#checkresult")),
     "while saying the board was not identified");
  ck(/says nothing about the board/i.test(await q3.textContent("#checkresult")),
     "and not implying the board is at fault");

  // Cancelling the port picker is not a failure either.
  const q4 = await browser.newPage({ viewport: { width: 400, height: 900 } });
  await q4.route("**unpkg.com**", (r) => r.abort());
  await q4.goto(`http://127.0.0.1:${PORT}/`);
  await fakeSerial(q4, null);   // null = the user cancelled the picker
  await q4.click("#checkbtn");
  await q4.waitForTimeout(300);
  ck(/No port chosen/i.test(await q4.textContent("#checkresult")),
     "cancelling the picker says so plainly");
  ck(/nothing was written/i.test(await q4.textContent("#checkresult")),
     "and confirms nothing was written");

  console.log(errors.length ? "\nPAGE ERRORS:\n" + errors.join("\n") : "");
  console.log(bad ? `\n${bad} FAILURES` : "\nflasher page checks passed");
  await browser.close();
  srv.close();
  process.exit(bad ? 1 : 0);
})();
