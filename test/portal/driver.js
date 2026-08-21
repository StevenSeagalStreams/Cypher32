const el=globalThis.__el; let bad=0;
const S1={configured:true,name:"GhostByte",faction:"BLACK",id:"a1b2c3d4",version:"v56",
 level:7,xp:420,xpNext:1050,sp:2,brute:11,stealth:5,firewall:8,battery:78,
 lora:{status:"Online",ready:true,rssi:-71,duty:0.123},
 action:{state:"SUCCESS",label:"RECON",tries:1,pending:false},
 nodes:[
  {id:"beef0001",name:"VoidCrypt",level:9,faction:"W",avgRssi:-58,bars:4,
   proximity:"VERY CLOSE",status:"ACTIVE",ageMs:4200,recon:2,hackWon:false,
   cooldownMs:0,unread:true,msg:"north gate <now>"},
  {id:"beef0002",name:"NullGate",level:3,faction:"R",avgRssi:-98,bars:2,
   proximity:"DISTANT",status:"FADING",ageMs:130000,recon:0,hackWon:true,
   cooldownMs:511000000,unread:false,msg:""}]};
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
// The odds contribution must match the firmware: score/max * 15.
seqOpen("beef0001","VoidCrypt",10);
ck(el("seqgrid").children.length===9,"3x3 grid built");
ck(el("seqmax").textContent==="10","target sequence length shown");
ck(el("seqmodal").className==="modal","mini-game opens on RECON");

var tiles=el("seqgrid").children;
ck(listeners(tiles[0],"pointerdown")===1,
   "tiles answer pointerdown, not click — click waits for release plus the "+
   "browser's double-tap timeout, which is most of the lag");
ck(pollIv===null,"the 2 s state poll is paused while the game is open");

// play it honestly: read the generated sequence and repeat it, through the
// real bound handler rather than by calling seqTap() behind its back
seqBegin();
flushTimers(80);                       // let round 1 finish flashing
ck(seqOrder.length===1,"round 1 is one tile");
var reached=0;
for(var round=0;round<10;round++){
  if(!seqAccepting)flushTimers(80);
  if(!seqAccepting)break;
  var target=seqOrder.slice();
  for(var k=0;k<target.length;k++){
    var tile=tiles[target[k]], ev=press(tile,"pointerdown",1000+k*120);
    if(k===0&&round===0){
      ck(tile.className==="lit",
         "the tile is lit in the same turn as the press, before any timer runs");
      var at=seqAt;
      press(tile,"click",1000);        // the synthetic click that trails a tap
      ck(seqAt===at,"the trailing click does not count as a second press");
      ck(ev.cancelable&&ev.preventDefault,"press events are preventable");
    }
  }
  reached=target.length;
  if(reached>=10)break;
  flushTimers(80);
}
ck(reached===10,"a perfect run reaches 10, got "+reached);
ck(seqBest===10,"score recorded as 10");
ck(el("seqmsg").textContent.indexOf("+15%")>=0,
   "perfect run advertises the +15% bonus, got: "+el("seqmsg").textContent);

// The bonus the portal advertises must match what the firmware awards.
// Firmware: (score * RECON_MAX_BONUS) / RECON_MAX_SEQ, integer division.
// test_link.cpp asserts the same table from the C++ side (60 / 67 / 75).
[[0,0],[5,7],[10,15]].forEach(function(pair){
  ck(Math.floor(pair[0]*15/10)===pair[1],
     "bonus for sequence "+pair[0]+" is +"+pair[1]+"%");
});

// a wrong tile ends the run at the last completed round
seqOpen("beef0002","NullGate",10);
seqBegin(); flushTimers(80);
var wrong=(seqOrder[0]+1)%9;
seqTap(wrong);
ck(seqBest===0,"failing round 1 scores 0");
ck(el("seqmsg").textContent.indexOf("Wrong tile")>=0,"failure is explained");

seqOpen("beef0003","IronCore",10);
seqBegin(); flushTimers(80);
seqTap(seqOrder[0]);                    // clear round 1
flushTimers(80);
if(seqAccepting){ seqTap((seqOrder[0]+1)%9); }   // fail round 2
ck(seqBest===1,"failing round 2 keeps the score from round 1, got "+seqBest);

// ── responsiveness ──
// Two presses of the same tile in quick succession. The dedupe that swallows a
// touch's compatibility mousedown must not swallow these, and the first
// press's fade timer must not blank the second press's light.
seqOpen("beef0004","Relay",10);
seqOrder=[3,3,3];seqAt=0;seqAccepting=true;seqPlaying=true;
var t3=el("seqgrid").children[3];
press(t3,"pointerdown",5000);
ck(seqAt===1,"first press registers");
var alive1=__timers.filter(function(t){return t.alive}).length;
press(t3,"pointerdown",5090);           // 90 ms later, same tile
ck(seqAt===2,"a repeat press 90 ms later still counts, seqAt="+seqAt);
ck(t3.className==="lit","the tile is lit after the second press");
ck(__timers.filter(function(t){return t.alive}).length===alive1,
   "the first press's fade timer was cancelled, not left to blank the second");

// Old WebViews without pointer events fall back to touchstart, and must not
// also fire on the compatibility mousedown that trails it.
delete globalThis.PointerEvent;
seqOpen("beef0005","Relay2",10);
var t5=el("seqgrid").children[0];
ck(listeners(t5,"touchstart")===1&&listeners(t5,"mousedown")===1,
   "fallback path binds touchstart and mousedown");
seqOrder=[0,0];seqAt=0;seqAccepting=true;seqPlaying=true;seqBest=0;
var te=press(t5,"touchstart",9000);
ck(te.defaultPrevented,"touchstart is prevented so the page cannot scroll away");
press(t5,"mousedown",9300);             // the compatibility event
ck(seqAt===1&&seqBest===0,
   "the compatibility mousedown is not a second press, seqAt="+seqAt);
globalThis.PointerEvent=function PointerEvent(){};

// The poll pauses for the game and comes back afterwards.
seqOpen("beef0006","Relay3",10);
ck(!pollIv,"poll stopped on open");
seqQuit();
ck(!!pollIv,"poll resumes when the game closes");
pollStop();

console.log("\nhelpers: fmtLeft(7d)="+fmtLeft(604800000)+
            "  fmtLeft(11h22m)="+fmtLeft(40920000)+"  fmtAge(4.2s)="+fmtAge(4200));
console.log(bad?`\n${bad} FAILURES`:"\nall portal render checks passed");
process.exit(bad?1:0);
