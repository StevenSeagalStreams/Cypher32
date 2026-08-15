const made={};
function _el(id){return made[id]||(made[id]={id,style:{},dataset:{},className:"",
  textContent:"",innerHTML:"",value:"",disabled:false,addEventListener(){}})}
globalThis.__made=made; globalThis.__el=_el;
globalThis.document={getElementById:_el,
  querySelectorAll:s=>s===".fc"?[]:s===".tab"?[{dataset:{t:"hud"},className:""}]:
                     s===".sk"?[{disabled:false},{disabled:false},{disabled:false}]:[]};
globalThis.localStorage={_d:{},getItem(k){return this._d[k]||null},
  setItem(k,v){this._d[k]=v},removeItem(k){delete this._d[k]}};
globalThis.fetch=()=>Promise.resolve({json:()=>Promise.resolve({}),status:200});
globalThis.setInterval=()=>{};globalThis.setTimeout=()=>{};globalThis.clearTimeout=()=>{};
globalThis.prompt=()=>"secret";globalThis.confirm=()=>true;globalThis.location={reload(){}};
