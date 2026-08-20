#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────
//  CYPHER32 PORTAL — single page            Phase 3
// ─────────────────────────────────────────────
//
//  One HTML/CSS/JS blob served from PROGMEM. All state comes from
//  GET /api/state (polled), all mutations go through POST /api/action.
//  Nothing reloads the page, so no request has to regenerate 20 KB of markup
//  while the radio is waiting to be serviced.
//
//  Design rules (T3.3):
//    - mobile first, works one-handed at 360 px
//    - bottom tab bar, 44 px minimum tap targets
//    - dark terminal aesthetic, but real contrast so it reads in daylight
//    - every action shows SENDING -> WAITING -> SUCCESS / NO RESPONSE (T3.5)

static const char PORTAL_HTML[] PROGMEM = R"PORTAL(<!DOCTYPE html><html lang="en">
<head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Cypher32</title><style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:#05080a;color:#c8f5c8;font:14px/1.45 ui-monospace,"SF Mono",Menlo,Consolas,monospace;
 padding-bottom:calc(64px + env(safe-area-inset-bottom))}
h1,h2,h3{margin:0 0 8px;font-weight:600;letter-spacing:.5px}
a{color:#5fe08a}
.wrap{padding:14px;max-width:640px;margin:0 auto}
.card{background:#0b1114;border:1px solid #1d2f26;border-radius:10px;padding:14px;margin-bottom:12px}
.ct{font-size:11px;letter-spacing:1.4px;color:#6fbf8a;text-transform:uppercase;margin-bottom:10px}
.row{display:flex;justify-content:space-between;align-items:center;gap:10px}
.mut{color:#7a927f}.sm{font-size:12px}.xs{font-size:11px}
.big{font-size:26px;font-weight:700;color:#7dffa8;line-height:1.1}
button,.btn{display:block;width:100%;min-height:44px;padding:11px 14px;background:#123020;color:#8dffb4;
 border:1px solid #2f6b47;border-radius:8px;font:inherit;font-weight:600;cursor:pointer;text-align:center}
button:active{background:#1b4a30}
button[disabled]{opacity:.4;cursor:not-allowed}
.btn.ghost{background:transparent;color:#8dffb4}
.btn.danger{background:transparent;color:#ff8080;border-color:#8a3030}
.btn.inline{display:inline-block;width:auto;min-height:38px;padding:8px 14px;font-size:12px;margin:0}
input,select,textarea{width:100%;min-height:44px;padding:10px;background:#05090b;color:#c8f5c8;
 border:1px solid #24402f;border-radius:8px;font:inherit;margin-top:6px}
textarea{min-height:64px;resize:none}
.bar{height:7px;background:#0d1a12;border:1px solid #23402e;border-radius:4px;overflow:hidden}
.bf{height:100%;background:linear-gradient(90deg,#2f9e5c,#7dffa8)}
.tabs{position:fixed;left:0;right:0;bottom:0;display:flex;background:#080d10;border-top:1px solid #1d2f26;
 padding-bottom:env(safe-area-inset-bottom);z-index:50}
.tab{flex:1;min-height:60px;display:flex;flex-direction:column;align-items:center;justify-content:center;
 gap:3px;color:#5c7a66;font-size:10px;letter-spacing:.6px;cursor:pointer;border:none;background:none;border-radius:0}
.tab .ic{font-size:17px;line-height:1}
.tab.on{color:#7dffa8;box-shadow:inset 0 2px 0 #7dffa8}
.pill{display:inline-block;font-size:10px;padding:2px 8px;border-radius:99px;border:1px solid;letter-spacing:.6px}
.fB{color:#7ab0ff;border-color:#3a6bb5}.fW{color:#dfe9f5;border-color:#8fa3b8}
.fR{color:#ff8b8b;border-color:#b54545}.fG{color:#84e6a0;border-color:#3f9159}
.node{border:1px solid #1d2f26;border-radius:10px;padding:12px;margin-bottom:10px;background:#0a1013}
.node.fade{opacity:.5}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px;vertical-align:-2px}
.bars i{width:4px;background:#24402f;border-radius:1px}
.bars i.on{background:#7dffa8}
.bars i:nth-child(1){height:4px}.bars i:nth-child(2){height:7px}
.bars i:nth-child(3){height:10px}.bars i:nth-child(4){height:14px}
.pips{letter-spacing:3px;color:#7dffa8}
.banner{position:fixed;left:10px;right:10px;top:10px;z-index:100;padding:12px 14px;border-radius:10px;
 font-size:13px;font-weight:600;display:none;border:1px solid;box-shadow:0 6px 24px #000a}
.banner.go{display:block;background:#0d2418;color:#8dffb4;border-color:#2f6b47}
.banner.bad{display:block;background:#2a1010;color:#ffb0b0;border-color:#8a3030}
.banner.wait{display:block;background:#0f1c2a;color:#a8d4ff;border-color:#2f4f6b}
.hide{display:none!important}
.grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
.stat{background:#0a1013;border:1px solid #1d2f26;border-radius:8px;padding:10px;text-align:center}
.stat b{display:block;font-size:19px;color:#7dffa8}
.kv{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #12201a;font-size:12px}
.kv:last-child{border:none}
.msg{padding:9px 0;border-bottom:1px solid #12201a}
.msg:last-child{border:none}
.dot{width:8px;height:8px;border-radius:50%;background:#7dffa8;display:inline-block;vertical-align:1px}
.dot.off{background:#3a4a40}
.fc{border:1px solid #24402f;border-radius:10px;padding:12px;margin-bottom:10px;cursor:pointer;background:#0a1013}
.fc.sel{border-color:#7dffa8;background:#0f2418}
.step{display:none}.step.on{display:block}
.modal{position:fixed;inset:0;background:#000c;z-index:200;display:flex;align-items:center;
 justify-content:center;padding:16px}
.modal .card{max-width:380px;width:100%;margin:0}
.seq{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin:12px 0}
.seq div{aspect-ratio:1;border:1px solid #24402f;border-radius:10px;background:#0a1013;
 cursor:pointer;transition:background .08s,border-color .08s}
.seq div.lit{background:#7dffa8;border-color:#7dffa8}
.seq div.bad{background:#ff8080;border-color:#ff8080}
.seq.locked div{cursor:default}
.seqhead{display:flex;justify-content:space-between;align-items:baseline}
.seqhead b{font-size:22px;color:#7dffa8}
</style></head><body>
<div id="banner" class="banner"></div>

<!-- Password entry. A real field, not prompt(): the captive-portal browser
     that opens this page silently ignores prompt/alert/confirm. -->
<div id="pwmodal" class="modal hide">
  <div class="card">
    <div class="ct">Device password</div>
    <p class="xs mut">Needed for anything that changes the device.</p>
    <input type="password" id="pwin" placeholder="Password" autocomplete="current-password">
    <div class="xs" id="pwerr" style="color:#ff9a9a;min-height:15px;margin-top:4px"></div>
    <button onclick="savePw()">UNLOCK</button>
    <button class="btn ghost" onclick="closePw()">Cancel</button>
    <p class="xs mut" style="margin-bottom:0">Forgotten it? On the device: tap
    <b>RST</b> twice, then hold <b>PRG</b> for 5 seconds to wipe and start over.</p>
  </div>
</div>

<!-- Recon mini-game: sequence memory. Watch, then repeat. -->
<div id="seqmodal" class="modal hide">
  <div class="card">
    <div class="seqhead"><div class="ct" style="margin:0">Recon &mdash; <span id="seqtarget">?</span></div>
      <b id="seqlen">0</b></div>
    <div class="xs mut" id="seqmsg">Watch the sequence, then repeat it.</div>
    <div class="seq" id="seqgrid"></div>
    <div class="xs mut">Each round adds one step. Reach <b id="seqmax">10</b> for the
    full <b>+15%</b> hack bonus. Your best run is kept, and you get three attempts per node before its cooldown has to run out.</div>
    <button id="seqbtn" onclick="seqBegin()">START</button>
    <button class="btn ghost" onclick="seqQuit()">Cancel</button>
  </div>
</div>

<!-- ══ SETUP WIZARD (T3.6) ══ -->
<div id="setup" class="wrap hide">
  <h1>CYPHER32</h1>

  <div class="step on" id="s1">
    <div class="card">
      <div class="ct">What this is</div>
      <p class="sm">Cypher32 is a hacking game played on real hardware. Your device
      finds other players over long-range radio — no internet, no accounts.</p>
      <p class="sm">You scout other players, attempt hacks, and earn XP. Everything
      is controlled from this page.</p>
      <button onclick="step(2)">BEGIN</button>
    </div>
  </div>

  <div class="step" id="s2">
    <div class="card">
      <div class="ct">Choose your faction</div>
      <p class="xs mut">This sets how you fight. It cannot be changed without wiping
      the device, so pick the one that matches how you want to play.</p>
      <div class="fc" data-f="BLACK" onclick="pick(this)">
        <div class="row"><b>BLACK HAT</b><span class="pill fB">+3 BRUTE</span></div>
        <div class="xs mut">Hits hardest. +20% XP on every win — but a loss costs the
        full 15 XP with no reduction.</div></div>
      <div class="fc" data-f="WHITE" onclick="pick(this)">
        <div class="row"><b>WHITE HAT</b><span class="pill fW">+3 FIREWALL</span></div>
        <div class="xs mut">Defensive. Losses cost half — but you may only attack
        BLACK and RED, so you get fewer targets.</div></div>
      <div class="fc" data-f="RED" onclick="pick(this)">
        <div class="row"><b>RED HAT</b><span class="pill fR">+3 STEALTH</span></div>
        <div class="xs mut">A gambler. +25% XP against GREEN — but a 15% chance to
        lose XP even when you win.</div></div>
      <div class="fc" data-f="GREEN" onclick="pick(this)">
        <div class="row"><b>GREEN HAT</b><span class="pill fG">+1 ALL</span></div>
        <div class="xs mut">Balanced start. +10% XP against BLACK, but a 25% risk of
        losing XP when attacking WHITE.</div></div>
      <button id="s2next" disabled onclick="step(3)">CONTINUE</button>
    </div>
  </div>

  <div class="step" id="s3">
    <div class="card">
      <div class="ct">Set a password</div>
      <p class="xs mut">Your device broadcasts an open Wi-Fi network, so anyone
      nearby can open this page. The password stops them changing anything.</p>
      <input type="password" id="p1" placeholder="Password (min 6 characters)" autocomplete="new-password">
      <input type="password" id="p2" placeholder="Confirm password" autocomplete="new-password">
      <div class="xs" id="pwmsg" style="color:#ff9a9a;min-height:16px;margin-top:6px"></div>
      <p class="xs mut">Chosen faction: <b id="fsel">—</b>. This is permanent.</p>
      <button onclick="finish()">ESTABLISH UPLINK</button>
      <button class="btn ghost" onclick="step(2)">Back</button>
    </div>
  </div>
</div>

<!-- ══ MAIN ══ -->
<div id="app" class="hide">
  <div class="wrap">

    <!-- HUD -->
    <div id="t-hud" class="pane">
      <div class="card">
        <div class="row">
          <div><div class="big" id="hname">—</div>
            <div class="sm mut">LVL <span id="hlvl">1</span> ·
              <span id="hfac" class="pill">—</span></div></div>
          <div style="text-align:right"><div class="sm" id="hbat">—</div>
            <div class="xs mut"><span class="dot" id="hdot"></span>
              <span id="hlora">—</span></div></div>
        </div>
        <div style="margin-top:12px">
          <div class="row xs mut"><span>XP</span><span id="hxp">0 / 0</span></div>
          <div class="bar" style="margin-top:4px"><div class="bf" id="hxpbar" style="width:0"></div></div>
        </div>
      </div>
      <div class="grid">
        <div class="stat"><b id="hbr">0</b><span class="xs mut">BRUTE</span></div>
        <div class="stat"><b id="hst">0</b><span class="xs mut">STEALTH</span></div>
        <div class="stat"><b id="hfw">0</b><span class="xs mut">FIREWALL</span></div>
      </div>
      <div class="card">
        <div class="ct">Network</div>
        <div class="kv"><span class="mut">Nodes in range</span><span id="hnodes">0</span></div>
        <div class="kv"><span class="mut">Signal</span><span id="hrssi">—</span></div>
        <div class="kv"><span class="mut">Airtime used</span><span id="hduty">—</span></div>
        <button class="btn ghost" onclick="act('beacon')">SEND BEACON</button>
        <button class="btn ghost" onclick="act('showqr')">SHOW JOIN QR ON DEVICE</button>
        <p class="xs mut">Puts a Wi-Fi QR on the e-ink for 60 seconds. Someone
        can point a camera at it instead of hunting for your SSID.</p>
      </div>
    </div>

    <!-- RADAR (T3.4) -->
    <div id="t-radar" class="pane hide">
      <div class="row" style="margin-bottom:10px">
        <h3 style="margin:0">RADAR</h3><span class="xs mut" id="rcount">—</span>
      </div>
      <div id="nodelist"></div>
      <div class="card hide" id="nonodes">
        <div class="ct">Nothing in range</div>
        <p class="sm mut">Your device is listening. Other players appear here
        automatically when their beacon reaches you — usually within 15 seconds
        of them coming into range.</p>
      </div>
    </div>

    <!-- SKILLS -->
    <div id="t-skills" class="pane hide">
      <div class="card">
        <div class="ct">Skill points</div>
        <div class="row"><span class="sm mut">Available</span><span class="big" id="ksp">0</span></div>
        <p class="xs mut">You earn one point per level. Maximum 35 per skill.</p>
      </div>
      <div class="card"><div class="row"><b>BRUTE FORCE</b><span id="kbr">0</span></div>
        <p class="xs mut">+2% hit chance per point over the target's Firewall &mdash; big swings against soft targets.</p>
        <button class="sk" data-s="brute" onclick="act('skill',{s:'brute'})">+1 BRUTE</button></div>
      <div class="card"><div class="row"><b>STEALTH</b><span id="kst">0</span></div>
        <p class="xs mut">+1% hit chance per point. Unlike Brute Force, a target's Firewall cannot cancel it.</p>
        <button class="sk" data-s="stealth" onclick="act('skill',{s:'stealth'})">+1 STEALTH</button></div>
      <div class="card"><div class="row"><b>FIREWALL</b><span id="kfw">0</span></div>
        <p class="xs mut">Cuts the XP you lose when someone hacks you.</p>
        <button class="sk" data-s="firewall" onclick="act('skill',{s:'firewall'})">+1 FIREWALL</button></div>
    </div>

    <!-- MESSAGES -->
    <div id="t-msgs" class="pane hide">
      <div class="card">
        <div class="ct">Send a message</div>
        <select id="mto"></select>
        <textarea id="mtxt" maxlength="32" placeholder="Max 32 characters"></textarea>
        <div class="xs mut" id="mcount">0 / 32</div>
        <button onclick="sendMsg()">TRANSMIT</button>
      </div>
      <div class="card"><div class="ct">Inbox</div><div id="inbox"></div></div>
    </div>

    <!-- EVENT LOG -->
    <div id="t-log" class="pane hide">
      <div class="row" style="margin-bottom:10px">
        <h3 style="margin:0">EVENT LOG</h3><span class="xs mut" id="lcount">—</span>
      </div>
      <div class="card"><div id="loglist"></div></div>
      <p class="xs mut">The last 20 things that happened. Kept in memory only,
      so it clears when the device restarts.</p>
    </div>

    <!-- SETTINGS -->
    <div id="t-cfg" class="pane hide">
      <div class="card">
        <div class="ct">Identity</div>
        <div class="kv"><span class="mut">Name</span><span id="cname">—</span></div>
        <div class="kv"><span class="mut">Faction</span><span id="cfac">—</span></div>
        <div class="kv"><span class="mut">Chip ID</span><span id="cid">—</span></div>
        <div class="kv"><span class="mut">Firmware</span><span id="cver">—</span></div>
      </div>
      <div class="card">
        <div class="ct">Password</div>
        <div class="kv"><span class="mut">This browser</span><span id="pwstate">—</span></div>
        <p class="xs mut">Enter the device password once and this browser keeps it.</p>
        <input type="password" id="curpw" placeholder="Device password" autocomplete="current-password">
        <button class="btn ghost" onclick="unlockFromCfg()">SAVE PASSWORD</button>
      </div>
      <div class="card">
        <div class="ct">Change password</div>
        <input type="password" id="npw" placeholder="New password (min 6)">
        <button class="btn ghost" onclick="changePw()">UPDATE PASSWORD</button>
      </div>
      <div class="card">
        <div class="ct">Maintenance</div>
        <button class="btn ghost" onclick="act('clearnodes')">CLEAR NODE LIST</button>
        <button class="btn ghost" onclick="showDiag()">DIAGNOSTICS</button>
        <button class="btn danger" id="wipebtn" onclick="wipe()">FACTORY RESET</button>
        <p class="xs mut">Factory reset erases your character permanently.</p>
        <p class="xs mut">Locked out or forgot the password? Do it on the device:
        <b>tap RST twice quickly, then hold PRG for 5 seconds.</b> The screen
        will confirm before it wipes. That works without the password &mdash;
        holding the button <i>is</i> the proof you own it.</p>
      </div>
    </div>

    <!-- DIAGNOSTICS (T3.7) -->
    <div id="t-diag" class="pane hide">
      <div class="row" style="margin-bottom:10px"><h3 style="margin:0">DIAGNOSTICS</h3>
        <button class="btn inline ghost" onclick="tab('cfg')">Close</button></div>
      <div class="card"><div class="ct">Link</div><div id="diagbody" class="xs"></div></div>
      <div class="card"><div class="ct">Ping a node</div>
        <select id="pingto"></select>
        <button class="btn ghost" onclick="doPing()">PING</button>
        <div class="xs" id="pingout" style="margin-top:8px"></div></div>
    </div>
  </div>

  <nav class="tabs">
    <button class="tab on" data-t="hud"   onclick="tab('hud')"><span class="ic">▣</span>HUD</button>
    <button class="tab"    data-t="radar" onclick="tab('radar')"><span class="ic">◎</span>RADAR</button>
    <button class="tab"    data-t="skills"onclick="tab('skills')"><span class="ic">▲</span>SKILL</button>
    <button class="tab"    data-t="msgs"  onclick="tab('msgs')"><span class="ic">✉</span>MSGS</button>
    <button class="tab"    data-t="log"   onclick="tab('log')"><span class="ic">≡</span>LOG</button>
    <button class="tab"    data-t="cfg"   onclick="tab('cfg')"><span class="ic">⚙</span>CFG</button>
  </nav>
</div>

<script>
var S={},cur="hud",faction="",lastAction="",busy=false;
function $(i){return document.getElementById(i)}
function esc(s){return String(s==null?"":s).replace(/[&<>"]/g,function(c){
  return {"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]})}

// ── password ──
// Held in a variable first and mirrored to localStorage. The captive-portal
// webview may have storage disabled or throw on access, so every use is
// guarded and the in-memory copy is what actually matters for this session.
var pwCache="";
function lsGet(k){try{return localStorage.getItem(k)}catch(e){return null}}
function lsSet(k,v){try{localStorage.setItem(k,v)}catch(e){}}
function lsDel(k){try{localStorage.removeItem(k)}catch(e){}}
function pw(){return pwCache||lsGet("c32pw")||""}
function setPw(v){pwCache=v;lsSet("c32pw",v)}
function forgetPw(){pwCache="";lsDel("c32pw")}

function needPw(msg){
  $("pwin").value="";
  $("pwerr").textContent=msg||"";
  $("pwmodal").className="modal";
  setTimeout(function(){try{$("pwin").focus()}catch(e){}},60)}
function closePw(){$("pwmodal").className="modal hide"}
function savePw(){
  var v=$("pwin").value;
  if(!v){$("pwerr").textContent="Enter the password.";return}
  setPw(v);closePw();banner("Password saved","go")}
function unlockFromCfg(){
  var v=$("curpw").value;
  if(!v){banner("Enter the password first","bad");return}
  setPw(v);$("curpw").value="";banner("Password saved","go");render()}

function banner(msg,kind,hold){
  var b=$("banner");b.className="banner "+kind;b.textContent=msg;
  clearTimeout(b._t);if(hold!==true)b._t=setTimeout(function(){b.className="banner"},3200)}

function tab(t){cur=t;
  ["hud","radar","skills","msgs","log","cfg","diag"].forEach(function(x){
    var e=$("t-"+x);if(e)e.className="pane"+(x===t?"":" hide")});
  Array.prototype.forEach.call(document.querySelectorAll(".tab"),function(b){
    b.className="tab"+(b.dataset.t===t?" on":"")});
  if(t==="diag")loadDiag()}

// ── setup wizard ──
function step(n){["s1","s2","s3"].forEach(function(s,i){
  $(s).className="step"+(i===n-1?" on":"")})}
function pick(el){Array.prototype.forEach.call(document.querySelectorAll(".fc"),function(c){
  c.className="fc"});el.className="fc sel";faction=el.dataset.f;
  $("s2next").disabled=false;$("fsel").textContent=faction}
function finish(){
  var a=$("p1").value,b=$("p2").value;
  if(a.length<6){$("pwmsg").textContent="Password must be at least 6 characters.";return}
  if(a!==b){$("pwmsg").textContent="Passwords do not match.";return}
  if(!faction){$("pwmsg").textContent="Pick a faction first.";return}
  $("pwmsg").textContent="";
  setPw(a);
  banner("Configuring device…","wait",true);
  post("/api/setup",{f:faction,p:a}).then(function(r){
    // The device rejects bad input with an err field. Saying "configured"
    // regardless — as this used to — hides the reason and drops you back at
    // the start of the wizard with no idea why.
    if(r&&r.err){$("pwmsg").textContent=r.err;banner(r.err,"bad");return}
    awaitReboot();
  }).catch(function(e){
    // The device reboots as it replies, so a dropped connection here is the
    // expected outcome, not a failure. Confirm by asking it, don't assume.
    awaitReboot();
  })}

// Poll until the device comes back up and reports itself configured. Replaces
// a blind 6-second reload that could not tell success from failure.
function awaitReboot(){
  banner("Device rebooting…","wait",true);
  var tries=0;
  var iv=setInterval(function(){
    tries++;
    if(tries>30){clearInterval(iv);
      banner("Device did not come back. Check its screen, then reload.","bad");
      return}
    fetch("/api/state",{cache:"no-store"}).then(function(r){return r.json()})
      .then(function(j){
        if(j&&j.configured){
          clearInterval(iv);
          banner("Ready — "+j.name,"go");
          setTimeout(function(){location.reload()},800)}
      }).catch(function(){/* still down, keep waiting */})
  },1000)}

// ── transport ──
function post(url,data){
  var body=Object.keys(data).map(function(k){
    return encodeURIComponent(k)+"="+encodeURIComponent(data[k])}).join("&");
  return fetch(url,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:body})
    .then(function(r){
      if(r.status===401){forgetPw();var e=new Error("Wrong password");e.code=401;throw e}
      return r.json().catch(function(){return{}})})}

// Every action reports SENDING -> WAITING -> SUCCESS / NO RESPONSE (T3.5).
function act(what,extra){
  if(busy)return;
  if(!pw()){needPw("Enter the device password to continue.");return}
  var d=extra||{};d.a=what;d.pw=pw();
  busy=true;lastAction=what;
  banner("SENDING…","wait",true);
  post("/api/action",d).then(function(r){
    busy=false;
    if(r&&r.err){banner(r.err,"bad");return}
    if(r&&r.instant){banner(r.msg||"Done","go");refresh();return}
    banner("WAITING FOR REPLY…","wait",true);   // poll picks it up from here
    refresh()
  }).catch(function(e){busy=false;
    if(e.code===401){needPw("That password was not accepted. Try again.");return}
    banner(e.message||"Failed","bad")})}

function sendMsg(){var t=$("mtxt").value.trim();if(!t)return;
  act("msg",{id:$("mto").value,txt:t});$("mtxt").value="";$("mcount").textContent="0 / 32"}
function changePw(){var p=$("npw").value;
  if(p.length<6){banner("Password must be at least 6 characters","bad");return}
  if(!pw()){needPw("Enter the current password first.");return}
  post("/api/action",{a:"setpw",np:p,pw:pw()}).then(function(r){
    if(r&&r.err){banner(r.err,"bad");return}
    setPw(p);$("npw").value="";banner("Password updated","go")})
   .catch(function(e){
    if(e.code===401){needPw("Current password was not accepted.");return}
    banner(e.message||"Failed","bad")})}
var wipeArmed=false;
function wipe(){
  var b=$("wipebtn");
  if(!wipeArmed){
    wipeArmed=true;b.textContent="TAP AGAIN TO ERASE EVERYTHING";
    setTimeout(function(){wipeArmed=false;b.textContent="FACTORY RESET"},5000);
    return}
  wipeArmed=false;b.textContent="FACTORY RESET";
  act("reset")}

// ── recon mini-game (sequence memory) ──
//
// Round n flashes n tiles; repeat them to advance. The furthest round you
// complete is the score, and it is sent with the recon action. Deliberately
// forgiving on the first rounds and unforgiving after: one wrong tile ends it,
// which is what makes a long run worth something.
var seqTarget="",seqOrder=[],seqAt=0,seqBest=0,seqPlaying=false,seqAccepting=false;
var SEQ_MAX=10;

function seqOpen(id,name,max){
  seqTarget=id; SEQ_MAX=max||10;
  seqOrder=[];seqAt=0;seqBest=0;seqPlaying=false;seqAccepting=false;
  $("seqtarget").textContent=name;
  $("seqmax").textContent=String(SEQ_MAX);
  $("seqlen").textContent="0";
  $("seqmsg").textContent="Watch the sequence, then repeat it.";
  $("seqbtn").textContent="START";$("seqbtn").disabled=false;
  var g=$("seqgrid");g.className="seq";g.innerHTML="";
  for(var i=0;i<9;i++){
    var d=document.createElement("div");
    d.dataset.i=String(i);
    d.onclick=(function(k){return function(){seqTap(k)}})(i);
    g.appendChild(d);
  }
  $("seqmodal").className="modal";
}
function seqQuit(){seqPlaying=false;seqAccepting=false;$("seqmodal").className="modal hide"}

function seqTiles(){return $("seqgrid").children}
function seqFlash(i,cls,ms){
  var t=seqTiles()[i]; if(!t)return;
  t.className=cls||"lit";
  setTimeout(function(){t.className=""},ms||300);
}

function seqBegin(){
  if(seqPlaying)return;
  seqPlaying=true;$("seqbtn").disabled=true;
  seqOrder=[];seqBest=0;
  seqNextRound();
}
function seqNextRound(){
  seqAccepting=false;
  seqOrder.push(Math.floor(Math.random()*9));
  $("seqlen").textContent=String(seqOrder.length);
  $("seqmsg").textContent="Watch…";
  $("seqgrid").className="seq locked";
  var i=0;
  var iv=setInterval(function(){
    if(i>=seqOrder.length){
      clearInterval(iv);
      seqAccepting=true;seqAt=0;
      $("seqgrid").className="seq";
      $("seqmsg").textContent="Your turn — repeat it.";
      return;
    }
    seqFlash(seqOrder[i],"lit",320);
    i++;
  },520);
}
function seqTap(i){
  if(!seqAccepting)return;
  if(i!==seqOrder[seqAt]){ seqFail(i); return; }
  seqFlash(i,"lit",160);
  seqAt++;
  if(seqAt>=seqOrder.length){
    seqAccepting=false;
    seqBest=seqOrder.length;
    if(seqBest>=SEQ_MAX){ seqDone("Perfect run."); return; }
    $("seqmsg").textContent="Correct — next round.";
    setTimeout(seqNextRound,700);
  }
}
function seqFail(i){
  seqAccepting=false;seqPlaying=false;
  seqFlash(i,"bad",500);
  seqDone("Wrong tile.");
}
function seqDone(why){
  seqPlaying=false;seqAccepting=false;
  $("seqgrid").className="seq locked";
  var bonus=Math.floor(seqBest*15/SEQ_MAX);
  $("seqmsg").textContent=why+" Sequence "+seqBest+" — +"+bonus+"% hack odds.";
  $("seqbtn").textContent="SENDING RECON…";$("seqbtn").disabled=true;
  var id=seqTarget;
  setTimeout(function(){
    $("seqmodal").className="modal hide";
    act("recon",{id:id,score:seqBest});
  },1400);
}

// ── rendering ──
function fmtAge(ms){var s=Math.floor(ms/1000);
  if(s<60)return s+"s ago";if(s<3600)return Math.floor(s/60)+"m ago";
  return Math.floor(s/3600)+"h ago"}
function fmtLeft(ms){if(ms<=0)return"";
  var m=Math.floor(ms/60000),h=Math.floor(m/60),d=Math.floor(h/24);
  if(d>0)return d+"d "+(h%24)+"h";if(h>0)return h+"h "+(m%60)+"m";
  return m+"m"}
function bars(n){var o="";for(var i=1;i<=4;i++)o+='<i class="'+(i<=n?"on":"")+'"></i>';
  return '<span class="bars">'+o+'</span>'}

function render(){
  if(!S.configured){$("setup").className="wrap";$("app").className="hide";return}
  $("setup").className="wrap hide";$("app").className="";

  $("hname").textContent=S.name;$("hlvl").textContent=S.level;
  $("hfac").textContent=S.faction;$("hfac").className="pill f"+S.faction.charAt(0);
  $("hbat").textContent=(S.onUsb||S.battery<0)?"USB":S.battery+"%";
  $("hlora").textContent=S.lora.status;
  $("hdot").className="dot"+(S.lora.ready?"":" off");
  $("hxp").textContent=S.xp+" / "+S.xpNext;
  $("hxpbar").style.width=Math.min(100,S.xpNext?S.xp*100/S.xpNext:0)+"%";
  $("hbr").textContent=S.brute;$("hst").textContent=S.stealth;$("hfw").textContent=S.firewall;
  $("hnodes").textContent=S.nodes.length;
  $("hrssi").textContent=S.lora.rssi+" dBm";
  $("hduty").textContent=S.lora.duty.toFixed(2)+"% of 1% limit";

  $("ksp").textContent=S.sp;$("kbr").textContent=S.brute;
  $("kst").textContent=S.stealth;$("kfw").textContent=S.firewall;
  Array.prototype.forEach.call(document.querySelectorAll(".sk"),function(b){b.disabled=S.sp<1});

  // radar, strongest first
  var ns=S.nodes.slice().sort(function(a,b){return b.avgRssi-a.avgRssi});
  $("rcount").textContent=ns.length+" in range";
  $("nonodes").className="card"+(ns.length?" hide":"");
  $("nodelist").innerHTML=ns.map(function(n){
    var pips="";for(var i=0;i<3;i++)pips+=(i<n.recon?"●":"○");
    var cd=n.cooldownMs>0;
    var lock=n.hackWon?"OWNED · "+fmtLeft(n.cooldownMs):
             (cd?"LOCKED OUT · "+fmtLeft(n.cooldownMs):"");
    return '<div class="node'+(n.status==="FADING"?" fade":"")+'">'+
      '<div class="row"><div><b>'+esc(n.name)+'</b> '+
        '<span class="pill f'+esc(n.faction)+'">'+esc(n.faction)+'</span></div>'+
        '<div class="xs mut">LVL '+n.level+'</div></div>'+
      '<div class="row xs mut" style="margin-top:6px">'+
        '<span>'+bars(n.bars)+' '+esc(n.proximity)+'</span>'+
        '<span>'+fmtAge(n.ageMs)+'</span></div>'+
      '<div class="row" style="margin-top:8px">'+
        '<span class="xs mut">Recon <span class="pips">'+pips+'</span>'+
          ' &middot; seq '+(n.reconScore||0)+'/'+(n.reconMax||10)+
          (n.odds>=0?' &middot; odds '+n.odds+'%':' &middot; odds unknown')+'</span>'+
        (lock?'<span class="xs mut">'+lock+'</span>':'')+'</div>'+
      (n.training?'<div class="xs mut" style="margin-top:6px">'+
        'Practice target. Scout it, watch your odds appear, then hack it. '+
        'It disappears once you reach LVL 2.</div>':'')+
      (n.canHack===false?'<div class="xs mut" style="margin-top:6px">'+
        'Immune &mdash; WHITE can only attack BLACK and RED.</div>':'')+
      (n.canRecon===false?'<div class="xs mut" style="margin-top:6px">'+
        (cd?'Recon locked until the cooldown ends &mdash; then 3 fresh attempts.'
           :'Recon spent. Hack it, or wait out the cooldown for 3 more.')+'</div>':'')+
      '<div class="row" style="margin-top:10px;gap:8px">'+
        '<button class="btn inline ghost" '+(n.canRecon===false?"disabled":"")+
          ' onclick="seqOpen(\''+n.id+'\',\''+esc(n.name)+'\','+(n.reconMax||10)+')">RECON</button>'+
        '<button class="btn inline" '+((cd||n.canHack===false)?"disabled":"")+
          ' onclick="act(\'hack\',{id:\''+n.id+'\'})">HACK</button>'+
      '</div></div>'}).join("");

  // message targets + inbox
  var opts=ns.map(function(n){return'<option value="'+n.id+'">'+esc(n.name)+'</option>'}).join("");
  ["mto","pingto"].forEach(function(id){
    var e=$(id),keep=e.value;e.innerHTML=opts||'<option value="">No nodes in range</option>';
    if(keep)e.value=keep});
  var inbox=S.nodes.filter(function(n){return n.msg});
  $("inbox").innerHTML=inbox.length?inbox.map(function(n){
    return'<div class="msg"><div class="xs mut">'+esc(n.name)+
      (n.unread?' <span class="dot"></span>':'')+'</div><div>'+esc(n.msg)+'</div></div>'
    }).join(""):'<div class="xs mut">No messages yet.</div>';

  var ev=S.events||[];
  $("lcount").textContent=ev.length?ev.length+" recent":"nothing yet";
  $("loglist").innerHTML=ev.length?ev.map(function(e){
    var xp=e.xp?'<span style="color:'+(e.xp>0?"#8dffb4":"#ffb0b0")+'">'+
      (e.xp>0?"+":"")+e.xp+' XP</span>':'';
    return '<div class="msg"><div class="row">'+
      '<span>'+esc(e.t)+(e.who?' <b>'+esc(e.who)+'</b>':'')+'</span>'+
      '<span class="xs mut">'+fmtAge(e.ageMs)+'</span></div>'+
      (xp?'<div class="xs">'+xp+'</div>':'')+'</div>';
  }).join(""):'<div class="xs mut">Nothing has happened yet. Wait for a node to '+
    'appear, or scout one from the Radar.</div>';

  $("pwstate").textContent=pw()?"password saved":"not set — actions will ask";
  $("cname").textContent=S.name;$("cfac").textContent=S.faction;
  $("cid").textContent=S.id;$("cver").textContent=S.version;

  // action feedback driven by the device, not guessed by the browser (T3.5)
  var a=S.action;
  if(a.pending){banner(a.label+": WAITING FOR REPLY ("+a.tries+"/4)…","wait",true)}
  else if(a.state==="NO RESPONSE"&&a.label){banner(a.label+": NO RESPONSE — out of range?","bad")}
  else if(a.state==="SUCCESS"&&a.label){banner(a.label+": SUCCESS","go")}
}

function refresh(){
  return fetch("/api/state").then(function(r){return r.json()})
    .then(function(j){S=j;render()}).catch(function(){})}

function loadDiag(){
  fetch("/api/diag").then(function(r){return r.json()}).then(function(d){
    $("diagbody").innerHTML=Object.keys(d).map(function(k){
      return'<div class="kv"><span class="mut">'+esc(k)+'</span><span>'+esc(d[k])+'</span></div>'
    }).join("")})}
function showDiag(){tab("diag")}
function doPing(){var id=$("pingto").value;if(!id)return;
  $("pingout").textContent="Pinging…";
  fetch("/api/ping?id="+encodeURIComponent(id)).then(function(r){return r.json()})
    .then(function(p){$("pingout").innerHTML=p.ok?
      '<span style="color:#8dffb4">Reply in '+p.rttMs+' ms after '+p.tries+' attempt(s), '+p.rssi+' dBm</span>':
      '<span style="color:#ffb0b0">No response after '+p.tries+' attempts</span>'})}

$("mtxt").addEventListener("input",function(){
  $("mcount").textContent=this.value.length+" / 32"});

refresh();setInterval(refresh,2000);
</script></body></html>)PORTAL";
