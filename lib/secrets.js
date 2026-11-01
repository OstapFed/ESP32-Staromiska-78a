const KEY='staromiska78a.secrets.v22';
export class LocalSecrets{
 constructor(storage=localStorage){this.storage=storage;}
 read(){try{const v=JSON.parse(this.storage.getItem(KEY)||'{}');return v&&typeof v==='object'&&!Array.isArray(v)?v:{};}catch{return {};}}
 get(scope,key){const v=this.read()[scope]?.[key];return typeof v==='string'?v:'';}
 set(scope,key,value){const all=this.read();all[scope]={...(all[scope]||{}),[key]:String(value)};this.storage.setItem(KEY,JSON.stringify(all));}
 clear(){this.storage.removeItem(KEY);}
}
