const made={};
// Listeners are recorded, not discarded. The mini-game binds its tiles to
// pointerdown to get the press acknowledged on contact, and a shim that threw
// handlers away could only ever test seqTap() directly — i.e. everything
// except the part that decides how fast the tile lights up.
function _listen(t,f){if(!this.__l)this.__l={};(this.__l[t]=this.__l[t]||[]).push(f)}
// innerHTML="" has to actually empty children. A plain field left the old tiles
// in place, so a second seqOpen() appended to the first game's grid and every
// test then pressed a stale, still-bound element.
// Replacing innerHTML destroys the nodes that were there. The memo in made{}
// has to forget them too, or a test reads a stale textContent off an element
// the page has already thrown away and re-created.
function _inner(o){let v="";Object.defineProperty(o,"innerHTML",{
  get(){return v},
  set(x){
    const gone=String(v).match(/id="([^"]+)"/g)||[];
    gone.forEach(m=>{const id=m.slice(4,-1); if(made[id]&&made[id]!==o)delete made[id]});
    v=x; if(x==="")o.children.length=0;
  }});return o}
function _el(id){return made[id]||(made[id]=_inner({id,style:{},dataset:{},className:"",
  textContent:"",value:"",disabled:false,children:[],
  appendChild(c){this.children.push(c)},addEventListener:_listen}))}
globalThis.__made=made; globalThis.__el=_el;
function _mk(tag){return _inner({tagName:tag,className:"",textContent:"",
  dataset:{},style:{},children:[],onclick:null,
  appendChild(c){this.children.push(c)},addEventListener:_listen})}
// Dispatch to whatever the page actually bound. Returns the event so a test can
// assert preventDefault() was called (that is what suppresses the synthetic
// click trailing a touch).
globalThis.press=(el,type,ts)=>{
  const e={type,timeStamp:ts||0,cancelable:true,defaultPrevented:false,
           preventDefault(){this.defaultPrevented=true}};
  (((el||{}).__l||{})[type]||[]).forEach(f=>f(e));
  return e;
};
globalThis.listeners=(el,type)=>(((el||{}).__l||{})[type]||[]).length;
globalThis.PointerEvent=function PointerEvent(){};   // modern path by default
globalThis.document={getElementById:_el,createElement:_mk,
  querySelectorAll:s=>s===".fc"?[]:s===".tab"?[{dataset:{t:"hud"},className:""}]:
                     s===".sk"?[{disabled:false},{disabled:false},{disabled:false}]:[]};
globalThis.localStorage={_d:{},getItem(k){return this._d[k]||null},
  setItem(k,v){this._d[k]=v},removeItem(k){delete this._d[k]}};
// A routable fetch. Recon is a conversation with the device now — probe, then
// one reveal per round cleared — so the tests have to be able to answer it and
// to see exactly what was asked for, in order.
globalThis.__net=[];                    // every request made, in order
globalThis.__routes={};                 // url prefix -> (url, opts) => body
globalThis.fetch=(url,opts)=>{
  const u=String(url);
  globalThis.__net.push({url:u,opts});
  let body={},status=200;
  for(const k of Object.keys(globalThis.__routes)){
    if(u.indexOf(k)===0){const r=globalThis.__routes[k](u,opts)||{};
      if(r.__status){status=r.__status;delete r.__status} body=r;break}
  }
  return Promise.resolve({status,json:()=>Promise.resolve(body)});
};
globalThis.netTo=(frag)=>globalThis.__net.filter(r=>r.url.indexOf(frag)>=0);
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
