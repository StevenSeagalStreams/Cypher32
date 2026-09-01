const el=globalThis.__el; let bad=0;
const S1={configured:true,name:"GhostByte",faction:"BLACK",id:"a1b2c3d4",version:"v56",
 level:7,xp:420,xpNext:1050,sp:2,brute:11,stealth:5,firewall:8,battery:78,
 lora:{status:"Online",ready:true,rssi:-71,duty:0.123},
 action:{state:"SUCCESS",label:"RECON",tries:1,pending:false},
 probe:{id:"00000000",state:"idle"},
 nodes:[
  {id:"beef0001",name:"VoidCrypt",level:9,faction:"W",avgRssi:-58,bars:4,
   proximity:"VERY CLOSE",status:"ACTIVE",ageMs:4200,recon:2,hackWon:false,
   intel:10,pwned:true,brute:6,stealth:4,firewall:9,reconMax:10,
   cooldownMs:0,unread:true,msg:"north gate <now>"},
  {id:"beef0002",name:"NullGate",level:3,faction:"R",avgRssi:-98,bars:2,
   proximity:"DISTANT",status:"FADING",ageMs:130000,recon:0,hackWon:true,
   intel:6,pwned:false,brute:-1,stealth:-1,firewall:-1,reconMax:10,
   cooldownMs:511000000,unread:false,msg:""}]};
// An unidentified contact: everything the device refuses to hand over yet.
const ANON={id:"beef0003",name:"UNKNOWN-0003",level:0,faction:"?",avgRssi:-70,
 bars:3,proximity:"NEARBY",status:"ACTIVE",ageMs:1000,recon:0,hackWon:false,
 intel:0,pwned:false,brute:-1,stealth:-1,firewall:-1,reconMax:10,
 cooldownMs:0,canRecon:true,canHack:true,unread:false,msg:""};
function ck(c,w){ if(!c){console.log("  FAIL:",w);bad++} }

S=S1; render();
console.log("render() with 2 nodes:");
ck(el("nodelist").innerHTML.length>200,"node list populated");
ck(el("hxpbar").style.width==="40%","xp bar 420/1050 = 40%");
ck(el("hfac").className==="pill fB","faction pill class");
ck(el("hname").textContent==="GhostByte","name shown");
ck((el("mto").innerHTML.match(/<option/g)||[]).length===2,"2 message targets");
ck((el("inbox").innerHTML.match(/class=\"msg\"/g)||[]).length===1,"1 inbox entry");
ck(!el("inbox").innerHTML.includes("<now>"),"node text escaped (no XSS)");
ck(el("inbox").innerHTML.includes("&lt;now&gt;"),"escaped form present");
ck(el("nodelist").innerHTML.includes("OWNED"),"7-day lock labelled OWNED");
ck(el("nodelist").innerHTML.includes("fade"),"FADING node dimmed");
ck(el("nodelist").innerHTML.indexOf("VoidCrypt")<el("nodelist").innerHTML.indexOf("NullGate"),
   "sorted by signal, strongest first");
ck(el("nodelist").innerHTML.includes("●●○"),"recon pips 2 of 3");
ck(el("hduty").textContent.includes("0.12"),"duty cycle shown");

S={configured:false}; render();
ck(el("app").className==="hide","unconfigured hides app");
ck(el("setup").className==="wrap","unconfigured shows wizard");

S={...S1,nodes:[]}; render();
ck(el("nonodes").className==="card","empty-state card shown");
ck(el("mto").innerHTML.includes("No nodes"),"empty target list message");

S={...S1,action:{state:"WAITING FOR REPLY",label:"HACK",tries:2,pending:true}}; render();
ck(el("banner").className==="banner wait","pending action shows waiting banner");
ck(el("banner").textContent.includes("2/4"),"shows attempt count");
S={...S1,action:{state:"NO RESPONSE",label:"HACK",tries:4,pending:false}}; render();
ck(el("banner").className==="banner bad","timeout shows failure banner");
ck(el("banner").textContent.includes("out of range"),"timeout explains why");

S={...S1,sp:0}; render();
// training dummy — present at LVL 1, explained, hackable with no cooldown
S={...S1,nodes:[{id:"00000001",name:"TRAINING",level:1,faction:"G",avgRssi:-40,
  bars:4,proximity:"SIMULATED",status:"ACTIVE",ageMs:0,recon:0,reconScore:0,
  reconMax:10,hackWon:false,cooldownMs:0,canHack:true,canRecon:true,
  training:true,odds:-1,unread:false,msg:""}]};
render();
ck(el("nodelist").innerHTML.indexOf("TRAINING")>=0,"training node listed");
ck(el("nodelist").innerHTML.indexOf("Practice target")>=0,"training node explains itself");
ck(el("nodelist").innerHTML.indexOf("odds unknown")>=0,"odds hidden until recon reveals firewall");

// event log
S={...S1,events:[
  {t:"you breached",who:"NullGate",xp:42,ageMs:9000},
  {t:"scouted you", who:"VoidCrypt",xp:0, ageMs:65000},
  {t:"discovered",  who:"IronCore", xp:0, ageMs:400000}]};
render();
ck(el("lcount").textContent==="3 recent","log count shown");
ck((el("loglist").innerHTML.match(/class="msg"/g)||[]).length===3,"three entries rendered");
ck(el("loglist").innerHTML.indexOf("+42 XP")>=0,"positive XP shown with a sign");
ck(el("loglist").innerHTML.indexOf("NullGate")>=0,"peer name shown");
S={...S1,events:[]}; render();
ck(el("loglist").innerHTML.indexOf("Nothing has happened")>=0,"empty log explains itself");

// battery: no cell attached must read as USB, not a fabricated 0%
S={...S1,battery:-1,onUsb:true}; render();
ck(el("hbat").textContent==="USB","no battery reads as USB, not 0%");
S={...S1,battery:64,onUsb:false}; render();
ck(el("hbat").textContent==="64%","a real battery still shows a percentage");

// ── password flow: must work without prompt(), which the captive-portal
// ── webview silently ignores ──
S=S1; forgetPw(); render();
ck(el("pwstate").textContent.indexOf("not set")>=0,"config shows password not set");
// The shim does not parse markup, so seed the class the HTML starts with.
el("pwmodal").className="modal hide";
closePw();
ck(el("pwmodal").className.indexOf("hide")>=0,"modal hidden at rest");
act("beacon");
ck(el("pwmodal").className==="modal","no password -> modal opens instead of prompt()");
ck(el("pwerr").textContent.length>0,"modal explains why it opened");

el("pwin").value="hunter2"; savePw();
ck(pw()==="hunter2","password captured from the real input field");
ck(el("pwmodal").className.indexOf("hide")>=0,"modal closes after unlock");
render();
ck(el("pwstate").textContent.indexOf("saved")>=0,"config shows password saved");

// a 401 from the device must reopen the field, not dead-end
forgetPw(); el("curpw").value="letmein"; unlockFromCfg();
ck(pw()==="letmein","password can also be set from the Config tab");
ck(el("curpw").value==="","field cleared after saving");

// storage being unavailable must not break anything
var realLS=globalThis.localStorage;
globalThis.localStorage={getItem(){throw new Error("denied")},
  setItem(){throw new Error("denied")},removeItem(){throw new Error("denied")}};
setPw("x"); ck(pw()==="x","works when localStorage throws (private/restricted mode)");
globalThis.localStorage=realLS;

// factory reset needs two taps, since confirm() is unavailable
wipeArmed=false; el("wipebtn").textContent="FACTORY RESET";
wipe();
ck(wipeArmed===true,"first tap arms the wipe");
ck(el("wipebtn").textContent.indexOf("TAP AGAIN")>=0,"button asks for confirmation");

// ── recon mini-game ──
//
// Recon is a conversation with the device now: POST a=recon opens the link and
// parks the target's file, GET /api/reveal?n=<round> draws down one tier as
// each round lands, POST a=reconend closes the run out. These tests drive that
// conversation through the real fetch path, so a change that stops asking for
// a tier — or asks for the wrong one — fails here rather than on the bench.

const settle = () => new Promise(r => setImmediate(r));
// Timers and promises interleave: a reveal is dispatched from a tap handler and
// resolves on the microtask queue, while playback runs on the timer queue.
async function pump(rounds) {
  for (let i = 0; i < (rounds || 8); i++) { flushTimers(200); await settle(); }
}

let probeState = "ready";
const REVEALS = {2:["name","CODENAME","VoidCrypt"], 4:["faction","FACTION","W"],
                 6:["level","LEVEL","9"],   7:["brute","BRUTE","6"],
                 8:["stealth","STEALTH","4"], 9:["firewall","FIREWALL","9"],
                 10:["pwned","BACKDOOR","OPEN"]};
__routes["/api/action"] = () => ({ok:true});
__routes["/api/state"]  = () => ({...S1, probe:{id:"beef0001", state:probeState}});
__routes["/api/reveal"] = (url) => {
  const n = Number((url.match(/[?&]n=(\d+)/)||[])[1]);
  const t = REVEALS[n];
  return t ? {n, field:t[0], label:t[1], value:t[2]} : {n};
};

// Play honestly: read the generated sequence and repeat it, through the real
// bound handler rather than by calling seqTap() behind its back.
async function play(upTo) {
  const tiles = el("seqgrid").children;
  seqBegin();
  await pump();
  let reached = 0;
  for (let round = 0; round < 12; round++) {
    if (!seqAccepting) await pump();
    if (!seqAccepting) break;
    const target = seqOrder.slice();
    for (let k = 0; k < target.length; k++)
      press(tiles[target[k]], "pointerdown", 1000 + round*2000 + k*120);
    reached = target.length;
    await settle();                       // let the reveal fetch resolve
    if (upTo && reached >= upTo) break;
    if (reached >= 10) break;
    await pump(2);
  }
  return reached;
}

(async function () {

setPw("secret");

// ── an unidentified contact ──
S = {...S1, nodes:[ANON]}; render();
const anonHtml = el("nodelist").innerHTML;
ck(anonHtml.includes("UNKNOWN-0003"), "unscouted node has no codename");
ck(!anonHtml.includes("LVL 0"), "unscouted node does not claim level 0");
ck(anonHtml.includes("LVL ?"), "unscouted level is withheld, not faked");
ck(anonHtml.includes("Clear 2 rounds"), "the card says what recon would buy");
ck(anonHtml.includes("intel 0/10"), "intel tier shown on the card");
ck(!anonHtml.includes("BRU "), "no stat line before tier 7");
S = {...S1}; render();
ck(el("nodelist").includes === undefined || el("nodelist").innerHTML.includes("PWNED"),
   "a backdoored node is marked PWNED");
ck(el("nodelist").innerHTML.includes("RE-ENTER"),
   "a backdoored node offers RE-ENTER instead of RECON");
ck(el("nodelist").innerHTML.includes("BRU 6"), "tier 7+ shows the stat line");

// ── opening recon probes first, and waits ──
__net.length = 0;
probeState = "wait";
seqOpen("beef0002", "NullGate", 10, {intel:6, level:3, faction:"R",
                                     brute:-1, stealth:-1, firewall:-1});
ck(el("seqmodal").className === "modal", "mini-game opens on RECON");
ck(el("seqgrid").children.length === 9, "3x3 grid built");
ck(pollIv === null, "the 2 s state poll is paused while the game is open");
ck(el("seqbtn").disabled === true, "START is locked until the target answers");
await settle();
ck(netTo("/api/action").length === 1, "opening recon sends one probe");
ck(String(netTo("/api/action")[0].opts.body).includes("a=recon"),
   "the probe is the recon action");
ck(el("intel").innerHTML.includes("CODENAME"), "the dossier panel is drawn");
ck((el("intel").innerHTML.match(/class="ir"/g) || []).length === 7 ||
   (el("intel").innerHTML.match(/ir_/g) || []).length === 14,
   "seven tiers listed");
await pump(3);
ck(el("seqbtn").disabled === true, "still locked while the probe is unanswered");

probeState = "ready";
await pump(4);
ck(el("seqbtn").disabled === false, "START unlocks once the dossier is staged");
ck(el("seqmsg").textContent.indexOf("Link up") >= 0, "the link is announced");

// ── tiers land as rounds land ──
__net.length = 0;
const reached = await play();
ck(reached === 10, "a perfect run reaches 10, got " + reached);
ck(seqBest === 10, "score recorded as 10");

const asked = netTo("/api/reveal").map(r => Number((r.url.match(/[?&]n=(\d+)/)||[])[1]));
ck(asked.length === 10, "one reveal per round cleared, got " + asked.length);
ck(asked.join(",") === "1,2,3,4,5,6,7,8,9,10",
   "reveals are asked for in order, got " + asked.join(","));
ck(netTo("/api/reveal").every(r => r.url.indexOf("pw=secret") > 0),
   "every reveal carries the password — it commits state, so it is a mutation");
ck(el("iv_name").textContent === "VoidCrypt", "round 2 revealed the codename");
ck(el("iv_faction").textContent === "BLACK" || el("iv_faction").textContent === "WHITE",
   "round 4 revealed the faction, spelled out: " + el("iv_faction").textContent);
ck(el("iv_level").textContent === "9", "round 6 revealed the level");
ck(el("iv_brute").textContent === "6", "round 7 revealed brute");
ck(el("iv_stealth").textContent === "4", "round 8 revealed stealth");
ck(el("iv_firewall").textContent === "9", "round 9 revealed firewall");
ck(el("iv_pwned").textContent === "OPEN", "round 10 opened the backdoor");
ck(el("ir_name").className.indexOf("got") >= 0, "a revealed row is marked earned");
await pump(14);                            // let the perfect run close itself out

// ── a run that stops early reveals only what it earned ──
__net.length = 0;
probeState = "ready";
seqOpen("beef0002", "NullGate", 10, {intel:0});
await pump(4);
const five = await play(5);
ck(five === 5, "stopped at round 5, got " + five);
ck(el("iv_name").textContent === "VoidCrypt", "the codename was earned at 2");
ck(el("ir_level").className.indexOf("got") < 0,
   "level stays locked at 5, row=" + el("ir_level").className);
ck(el("ir_firewall").className.indexOf("got") < 0, "firewall stays locked at 5");
seqFail(0);
ck(el("seqmsg").textContent.indexOf("Round 6") >= 0,
   "the end screen names what the next round would have bought: " + el("seqmsg").textContent);

// ── closing the run reports it exactly once ──
__net.length = 0;
await pump(6);
const ends = netTo("/api/action").filter(r => String(r.opts.body).includes("a=reconend"));
ck(ends.length === 1, "the run is closed out once, got " + ends.length);
ck(String(ends[0].opts.body).includes("score=5"), "the closing score is reported");
seqQuit();
await settle();
ck(netTo("/api/action").filter(r => String(r.opts.body).includes("a=reconend")).length === 1,
   "quitting after the run has already closed does not report it twice");
ck(!!pollIv, "poll resumes when the game closes");

// ── a target that never answers costs the player nothing ──
__net.length = 0;
probeState = "failed";
seqOpen("beef0002", "NullGate", 10, {intel:0});
await pump(4);
ck(el("seqbtn").disabled === true, "START never unlocks against a silent target");
ck(el("seqbtn").textContent === "NO LINK", "the dead link is named");
ck(netTo("/api/reveal").length === 0, "nothing is revealed without a dossier");
seqQuit(); await settle();
probeState = "ready";

// ── cancelling and re-opening must not leave two probe loops racing ──
// The abandoned loop keeps its pending fetch. If it is still allowed to act
// when the modal comes back, it drives a second copy of the same game.
__net.length = 0;
probeState = "wait";
seqOpen("beef0001", "VoidCrypt", 10, {intel:10, pwned:true});
await settle();                             // first probe loop is now in flight
seqQuit();
await settle();
probeState = "ready";
seqOpen("beef0001", "VoidCrypt", 10, {intel:10, pwned:true});
await pump(14);
const twice = netTo("/api/reveal").map(r => Number((r.url.match(/[?&]n=(\d+)/)||[])[1]));
ck(twice.join(",") === "2,4,6,7,8,9,10",
   "the abandoned probe loop does not run a second copy of the game, got " +
   twice.join(","));
await pump(14);

// ── a backdoored node skips the game entirely ──
__net.length = 0;
seqOpen("beef0001", "VoidCrypt", 10, {intel:10, pwned:true});
await pump(10);
const back = netTo("/api/reveal").map(r => Number((r.url.match(/[?&]n=(\d+)/)||[])[1]));
ck(back.join(",") === "2,4,6,7,8,9,10",
   "re-entering pulls every tier and plays no game, got " + back.join(","));
ck(el("iv_pwned").textContent === "OPEN", "the backdoor row is filled straight away");
ck(seqBest === 10, "a backdoor run scores a full 10");
await pump(12);

// The bonus the portal advertises must match what the firmware awards.
// Firmware: (score * RECON_MAX_BONUS) / RECON_MAX_SEQ, integer division.
// test_link.cpp asserts the same table from the C++ side (60 / 67 / 75).
[[0,0],[5,7],[10,15]].forEach(function (pair) {
  ck(Math.floor(pair[0]*15/10) === pair[1],
     "bonus for sequence " + pair[0] + " is +" + pair[1] + "%");
});

// ── responsiveness ──
// Two presses of the same tile in quick succession. The dedupe that swallows a
// touch's compatibility mousedown must not swallow these, and the first press's
// fade timer must not blank the second press's light.
seqOpen("beef0004", "Relay", 10, {intel:0});
await pump(4);
seqOrder = [3,3,3]; seqAt = 0; seqAccepting = true; seqPlaying = true;
const t3 = el("seqgrid").children[3];
press(t3, "pointerdown", 5000);
ck(seqAt === 1, "first press registers");
ck(t3.className === "lit",
   "the tile is lit in the same turn as the press, before any timer runs");
const alive1 = __timers.filter(t => t.alive).length;
press(t3, "pointerdown", 5090);           // 90 ms later, same tile
ck(seqAt === 2, "a repeat press 90 ms later still counts, seqAt=" + seqAt);
ck(t3.className === "lit", "the tile is lit after the second press");
ck(__timers.filter(t => t.alive).length === alive1,
   "the first press's fade timer was cancelled, not left to blank the second");
const at = seqAt;
press(t3, "click", 5100);                 // the synthetic click that trails a tap
ck(seqAt === at, "the trailing click does not count as a second press");
seqQuit(); await settle();

// Old WebViews without pointer events fall back to touchstart, and must not
// also fire on the compatibility mousedown that trails it.
delete globalThis.PointerEvent;
seqOpen("beef0005", "Relay2", 10, {intel:0});
await pump(4);
const t5 = el("seqgrid").children[0];
ck(listeners(t5, "touchstart") === 1 && listeners(t5, "mousedown") === 1,
   "fallback path binds touchstart and mousedown");
seqOrder = [0,0]; seqAt = 0; seqAccepting = true; seqPlaying = true; seqBest = 0;
const te = press(t5, "touchstart", 9000);
ck(te.defaultPrevented, "touchstart is prevented so the page cannot scroll away");
press(t5, "mousedown", 9300);             // the compatibility event
ck(seqAt === 1 && seqBest === 0,
   "the compatibility mousedown is not a second press, seqAt=" + seqAt);
globalThis.PointerEvent = function PointerEvent() {};
seqQuit(); await settle();
pollStop();

// ── standings and census ──
// The rule under test is that recon gates both. A contact you have not read to
// tier 6 cannot be ranked, and one you have not read to tier 4 counts as an
// unknown faction — so neither view can be used to skip the mini-game.
S = {...S1, stats:{won:12,lost:5,breached:3,held:9,met:41,bestSeq:10},
     nodes:[
       {...S1.nodes[0], intel:10, level:9,  faction:"W", pwned:true},   // fully read
       {...S1.nodes[1], intel:6,  level:3,  faction:"R"},               // level known
       // Unscouted, but the DEVICE knows their faction is BLACK. The portal
       // must not count it: this row is what proves the tier-4 gate is real
       // rather than an accident of the JSON already saying "?".
       {...ANON,        intel:0,  level:0,  faction:"B"},
       {...ANON, id:"beef0009", name:"UNKNOWN-0009", intel:4, level:0, faction:"G"},
       // A second WHITE, so the bars have different lengths and a flat render
       // is distinguishable from a working one.
       {...S1.nodes[0], id:"beef000a", name:"PaleFork", intel:10, level:5,
        faction:"W", pwned:false},
     ]};
render();

const st = standings();
ck(st.known.length === 4, "only tier-6+ contacts are ranked, plus you: " + st.known.length);
ck(st.unknown === 2, "the other two are counted as unrankable, got " + st.unknown);
ck(st.known.every((p,i,a) => i === 0 || a[i-1].lvl >= p.lvl),
   "the board is sorted by level, descending");
ck(st.known.some(p => p.me), "you are on your own board");
ck(!st.known.some(p => String(p.name).indexOf("UNKNOWN") === 0),
   "an unidentified contact never appears as a ranked row");
ck(el("board").innerHTML.includes("(you)"), "your row is marked");
ck(el("boardnote").textContent.includes("2 contacts cannot be ranked"),
   "the note says how many are unplaced: " + el("boardnote").textContent);

// tier 4 is enough for the census even though tier 6 is not enough for the board
const c = censusOf();
ck(c.W === 2, "tier-10 contacts count to their faction, got " + c.W);
ck(c.R === 1, "a tier-6 contact counts to its faction");
ck(c.G === 1, "tier 4 is enough to be counted, even unrankable");
ck(c["?"] === 1, "a tier-0 contact counts as unknown, not as a guess");
ck(c.B === 1, "you count yourself — and an unscouted BLACK is NOT counted with you");
ck(el("census").innerHTML.includes("UNKNOWN"), "the census names the unknown bucket");
// The bars are share-of-room. If every fill comes out the same width the
// widget is decorative, which is what happened when .f was left inline.
const widths = (el("census").innerHTML.match(/width:(\d+)%/g) || []);
ck(widths.length === 5, "five bars drawn, got " + widths.length);
ck(new Set(widths).size > 1,
   "bars differ when the counts differ, got " + widths.join(" "));
ck(widths.every(w => Number(w.match(/\d+/)[0]) <= 100), "no bar exceeds 100%");
ck(el("censusnote").textContent.includes("6 heard"),
   "the census totals everyone heard: " + el("censusnote").textContent);

// the record comes straight from the device and survives nothing being known
ck(el("rwon").textContent === "12" && el("rheld").textContent === "9",
   "the record is shown");
ck(el("rseq").textContent === "10 / 10", "best recon run is shown out of 10");
S = {...S1, stats:undefined, nodes:[]}; render();
ck(el("rwon").textContent === "0", "a device with no record yet shows zeros");
ck(censusOf()["?"] === 0 && censusOf().B === 1,
   "an empty room still counts you and nobody else");
S = {...S1}; render();

// ── contact alert ──
// A new node is on the e-ink for a few seconds and then gone. The phone is the
// thing you are holding, so the phone is what has to tell you.
sfxSeen = null; sfxOn = true; __audio.state = "running"; __audio.started = 0;

ck(newContacts([{id:"a"},{id:"b"}]).length === 0,
   "the first look seeds silently — nobody in the room just arrived");
ck(newContacts([{id:"a"},{id:"b"}]).length === 0, "a quiet poll says nothing");
ck(newContacts([{id:"a"},{id:"b"},{id:"c"}]).join() === "c",
   "an arrival is reported");
ck(newContacts([{id:"c"}]).length === 0, "departures are not arrivals");
ck(newContacts([{id:"c"},{id:"00000001",training:true}]).length === 0,
   "the training dummy never announces itself");
ck(newContacts([{id:"c"},{id:"a"}]).join() === "a",
   "a node that dropped out and came back counts as a new contact");

// end to end, through render()
sfxSeen = null; __audio.started = 0; sfxCount = 0;
S = {...S1, nodes:[S1.nodes[0]]}; render();
ck(sfxCount === 0, "opening the portal on an existing node is silent");
S = {...S1}; render();
ck(sfxCount === 1, "a node appearing in a poll fires the alert");
ck(__audio.started === 6, "three notes, two detuned oscillators each, got " +
   __audio.started);

// muted
sfxOn = false; __audio.started = 0;
S = {...S1, nodes:[...S1.nodes, ANON]}; render();
ck(sfxCount === 2, "the alert is still raised when muted");
ck(__audio.started === 0, "but nothing is played");
sfxOn = true;

// A browser that has not been touched yet refuses to make noise. Asking for
// the cue must wake the context, not play into a suspended one and be lost.
__audio.state = "suspended"; __audio.started = 0; __audio.resumed = 0;
sfx("discover");
ck(__audio.started === 0, "nothing plays while the context is suspended");
ck(__audio.resumed === 1, "and the cue asks for it to be resumed");
__audio.state = "running";

// the switch, and that it survives a reload
sfxToggle();
ck(sfxOn === false && lsGet("sfx") === "0", "the alert can be switched off");
ck(el("sfxbtn").textContent === "ALERT: OFF", "the button says which way it is");
sfxToggle();
ck(sfxOn === true && lsGet("sfx") === "1", "and switched back on");
__audio.started = 0;
sfxTest();
ck(__audio.started === 6, "HEAR IT plays the cue on demand");

console.log("\nhelpers: fmtLeft(7d)=" + fmtLeft(604800000) +
            "  fmtLeft(11h22m)=" + fmtLeft(40920000) + "  fmtAge(4.2s)=" + fmtAge(4200));
console.log(bad ? `\n${bad} FAILURES` : "\nall portal render checks passed");
process.exit(bad ? 1 : 0);

})();
