const made={};
function _el(id){return made[id]||(made[id]={id,style:{},dataset:{},className:"",
  textContent:"",innerHTML:"",value:"",disabled:false,children:[],
  appendChild(c){this.children.push(c)},addEventListener(){}})}
globalThis.__made=made; globalThis.__el=_el;
function _mk(tag){return {tagName:tag,className:"",textContent:"",innerHTML:"",
  dataset:{},style:{},children:[],onclick:null,
  appendChild(c){this.children.push(c)},addEventListener(){}}}
globalThis.document={getElementById:_el,createElement:_mk,
  querySelectorAll:s=>s===".fc"?[]:s===".tab"?[{dataset:{t:"hud"},className:""}]:
                     s===".sk"?[{disabled:false},{disabled:false},{disabled:false}]:[]};
globalThis.localStorage={_d:{},getItem(k){return this._d[k]||null},
  setItem(k,v){this._d[k]=v},removeItem(k){delete this._d[k]}};
globalThis.fetch=()=>Promise.resolve({json:()=>Promise.resolve({}),status:200});
// Timers, modelled well enough for the mini-game: an interval re-queues itself
// until cleared, which is what the sequence playback relies on. Running a
// repeating callback exactly once — as this shim first did — makes the game
// look stuck at round one.
globalThis.__timers=[];
let __tid=1;
globalThis.setTimeout=(f)=>{const t={fn:f,repeat:false,id:__tid++,alive:true};
  globalThis.__timers.push(t);return t.id};
globalThis.setInterval=(f)=>{const t={fn:f,repeat:true,id:__tid++,alive:true};
  globalThis.__timers.push(t);return t.id};
globalThis.clearTimeout=(id)=>{globalThis.__timers.forEach(t=>{if(t.id===id)t.alive=false})};
globalThis.clearInterval=globalThis.clearTimeout;
globalThis.flushTimers=(n)=>{
  for(let i=0;i<(n||200);i++){
    const t=globalThis.__timers.shift(); if(!t) break;
    if(!t.alive) continue;
    try{t.fn()}catch(e){}
    if(t.repeat && t.alive) globalThis.__timers.push(t);
  }
};
globalThis.prompt=()=>"secret";globalThis.confirm=()=>true;globalThis.location={reload(){}};
