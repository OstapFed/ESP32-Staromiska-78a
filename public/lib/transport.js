import {isFresh,nativeCommand,parseDeviceState,rpcEnvelope} from './protocol.js';
export class GardenTransport extends EventTarget{
 constructor(){super();this.client=null;this.id='pwa-'+crypto.randomUUID().replaceAll('-','');this.pending=new Map();this.rpcPending=new Map();this.received=0;this.state=null;this.connected=false;this.gateway=false;this.config=null;this.timer=setInterval(()=>this.emit('health'),1000);}
 emit(type,detail){this.dispatchEvent(new CustomEvent(type,{detail}));}
 get fresh(){return this.connected&&isFresh(this.received);}
 connect(config){
  this.disconnect();this.config=config;
  this.client=globalThis.mqtt.connect(config.url,{username:config.username,password:config.password,clientId:this.id,protocolVersion:4,clean:true,keepalive:60,reconnectPeriod:3000,connectTimeout:12000,resubscribe:false,queueQoSZero:false});
  this.client.on('connect',()=>{
   this.connected=true;this.received=0;this.gateway=false;
   this.client.subscribe([`${config.base}/state`,`${config.base}/availability`,`${config.base}/pwa/availability`,`${config.base}/pwa/reply/${this.id}`],{qos:0},err=>{if(err)this.emit('error','Не вдалося підписатися на дані');});
   this.emit('health');this.emit('connection',true);
  });
  this.client.on('message',(topic,data,packet)=>this.onMessage(topic,data.toString(),packet));
  this.client.on('close',()=>{this.connected=false;this.gateway=false;this.rejectPending('З’єднання втрачено. Команда не буде повторена автоматично.');this.emit('health');});
  this.client.on('error',err=>this.emit('error',this.errorText(err)));
  this.client.on('reconnect',()=>this.emit('reconnect'));
 }
 errorText(err){return /auth|credential|not authorized|refused/i.test(err.message)?'Брокер відхилив вхід. Перевірте логін, пароль і доступ до topic.':'Не вдалося підключитися до брокера. Перевірте WSS-адресу та інтернет.';}
 onMessage(topic,raw,packet={}){
  if(!this.config)return;const base=this.config.base;
  try{
   if(topic===`${base}/state`){
    const state=parseDeviceState(raw);this.state=state;
    if(!packet.retain)this.received=Date.now();
    this.emit('state',{state,fresh:!packet.retain});
    if(!packet.retain)for(const [id,p] of this.pending)if(p.matches(state)){clearTimeout(p.timer);this.pending.delete(id);p.resolve(state);}
    this.emit('health');
   }else if(topic===`${base}/availability`&&raw==='offline'){
    this.received=0;this.rejectPending('Контролер недоступний');this.emit('health');
   }else if(topic===`${base}/pwa/availability`){
    if(raw==='offline'){this.gateway=false;this.emit('health');}
    else if(this.connected)this.rpc('config.get',{},true).then(c=>{this.gateway=true;this.emit('config',c);this.emit('health');}).catch(()=>{this.gateway=false;this.emit('health');});
   }else if(topic===`${base}/pwa/reply/${this.id}`){
    if(packet.retain)return;const reply=JSON.parse(raw),p=this.rpcPending.get(reply.id);if(!p)return;
    clearTimeout(p.timer);this.rpcPending.delete(reply.id);
    reply.ok?p.resolve(reply.data):p.reject(Error(reply.error||'Шлюз відхилив запит'));
   }
  }catch(err){this.emit('error',err.message);}
 }
 async command(action,args={}){
  if(!this.fresh)throw Error('Немає свіжого стану контролера. Дочекайтеся з’єднання.');
  if(this.gateway)return this.rpc(action,args);
  const cmd=nativeCommand(action,args,this.state),id=crypto.randomUUID();
  return new Promise((resolve,reject)=>{
   const timer=setTimeout(()=>{this.pending.delete(id);reject(Error('Контролер не підтвердив дію. Перевірте стан перед повторенням.'));},18000);
   this.pending.set(id,{...cmd,resolve,reject,timer});
   this.client.publish(this.config.base+cmd.suffix,cmd.payload,{qos:0,retain:false},err=>{if(err){clearTimeout(timer);this.pending.delete(id);reject(Error('Команду не надіслано'));}});
  });
 }
 rpc(op,args={},probe=false){
  if(!this.connected||(!probe&&!this.gateway))return Promise.reject(Error('Локальний шлюз не підключений'));
  const request=rpcEnvelope(this.id,op,args),id=request.id;
  return new Promise((resolve,reject)=>{
   const timer=setTimeout(()=>{this.rpcPending.delete(id);reject(Error('Шлюз не відповів. Запит не повторюється автоматично.'));},22000);
   this.rpcPending.set(id,{resolve,reject,timer});
   this.client.publish(`${this.config.base}/pwa/request`,JSON.stringify(request),{qos:0,retain:false},err=>{if(err){clearTimeout(timer);this.rpcPending.delete(id);reject(Error('Запит не надіслано'));}});
  });
 }
 rejectPending(reason){for(const map of [this.pending,this.rpcPending]){for(const p of map.values()){clearTimeout(p.timer);p.reject(Error(reason));}map.clear();}}
 disconnect(){if(this.client){this.client.removeAllListeners();this.client.end(true);this.client=null;}this.connected=false;this.gateway=false;this.received=0;this.rejectPending('З’єднання закрито');this.emit('health');}
 destroy(){this.disconnect();clearInterval(this.timer);}
}
