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
console.log("\nhelpers: fmtLeft(7d)="+fmtLeft(604800000)+
            "  fmtLeft(11h22m)="+fmtLeft(40920000)+"  fmtAge(4.2s)="+fmtAge(4200));
console.log(bad?`\n${bad} FAILURES`:"\nall portal render checks passed");
process.exit(bad?1:0);
