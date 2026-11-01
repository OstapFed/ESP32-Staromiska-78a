export const VERSION='22.0.0';
export const FRESH_MS=45000;
export const HEAT_BUTTONS=['stop','manual-on','manual-off','auto-on','auto-off','vacation-on','vacation-off'];
export const modeName=s=>s?.vacation?'Відпустка':({0:'Зупинено',1:'Ручний',2:'Авто'}[s?.boiler_mode]??'Немає даних');
export const isFresh=(received,now=Date.now())=>Number.isFinite(received)&&received>0&&now-received>=0&&now-received<FRESH_MS;
export const escapeHTML=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
export const halfDegree=value=>Math.max(5,Math.min(30,Math.round(Number(value)*2)/2));
export function validateConnection(input){
 const url=new URL(input.url);if(url.protocol!=='wss:')throw Error('Вкажіть захищену адресу wss://…/mqtt');
 if(url.username||url.password||url.hash||url.search)throw Error('Адреса брокера не повинна містити пароль або параметри');
 const base=String(input.base??'').trim().replace(/\/$/,'');
 if(!base||base.length>160||/[+#\u0000-\u001f]/.test(base))throw Error('Некоректний базовий topic');
 if(!String(input.username??'').trim())throw Error('Вкажіть ім’я користувача MQTT');
 return {...input,url:url.toString(),base,username:String(input.username).trim()};
}
export function parseDeviceState(raw){
 const s=typeof raw==='string'?JSON.parse(raw):raw;
 if(!s||typeof s!=='object'||Array.isArray(s)||typeof s.firmware!=='string'||!Number.isInteger(s.boiler_mode)||![0,1,2].includes(s.boiler_mode))throw Error('Повідомлення не є станом Garden_U');
 if(!Array.isArray(s.lights)||!Array.isArray(s.gates)||s.gates.length>2||!Array.isArray(s.xiaomi_sensors))throw Error('Неповний стан контролера');
 return s;
}
export function nativeCommand(action,args={},state){
 switch(action){
  case 'heat.mode':{
   const map={stop:['/boiler/mode/set','OFF',0],'manual-on':['/boiler/mode/set','MANUAL',1],'manual-off':['/boiler/mode/set','OFF',0],'auto-on':['/boiler/mode/set','AUTO',3],'auto-off':['/boiler/mode/set','OFF',0],'vacation-on':['/vacation/set','ON',5],'vacation-off':['/vacation/set','OFF',6]};
   const item=map[args.command];if(!item)throw Error('Невідома команда опалення');
   return {suffix:item[0],payload:item[1],matches:s=>s.heating_selected_command===item[2]};
  }
  case 'heat.manual':{
   if(state?.vacation)throw Error('Для зміни ручної уставки під час Відпустки потрібен шлюз');
   const value=halfDegree(args.value);if(!Number.isFinite(value))throw Error('Некоректна температура');
   return {suffix:'/boiler/setpoint/set',payload:String(value),matches:s=>Math.abs(s.manual_setpoint-value)<.01&&s.boiler_mode===state.boiler_mode&&s.vacation===state.vacation};
  }
  case 'light.set':{
   const on=!!args.on;return {suffix:'/light/1/set',payload:on?'ON':'OFF',matches:s=>s.lights[0]===on};
  }
  case 'water.start':{
   const zone=Number(args.zone);if(![1,2,3,4].includes(zone))throw Error('Невідома зона');
   if(state?.rain_blocked)throw Error('Полив заблоковано захистом від дощу');
   return {suffix:`/water/${zone}/set`,payload:'START',matches:s=>s.watering&&s.watering_zone===zone};
  }
  case 'water.stop':return {suffix:'/water/stop/set',payload:'STOP',matches:s=>!s.watering};
  case 'water.all':{
   if(!['all','alltogether'].includes(args.action))throw Error('Невідомий спосіб поливу');
   if(state?.rain_blocked)throw Error('Полив заблоковано захистом від дощу');
   if(state?.mqtt_water_all_supported!==true)throw Error('Для поливу всіх зон через MQTT встановіть Garden_U V36.2 або запустіть локальний шлюз.');
   const together=args.action==='alltogether';
   return {suffix:`/water/${args.action}/set`,payload:'START',matches:s=>s.watering===true&&s.watering_all===true&&!!s.watering_simultaneous===together};
  }
  case 'gate.command':{
   const gate=Number(args.gate),open=args.command==='open';if(![0,1].includes(gate)||!['open','close'].includes(args.command))throw Error('Невідома команда воріт');
   const g=state?.gates[gate];if(!g||(open?!g.open_allowed:!g.close_allowed))throw Error('Ця команда зараз недоступна для воріт');
   return {suffix:`/gate/${gate+1}/set`,payload:open?'OPEN':'CLOSE',matches:s=>s.gates[gate]?.last_command===(open?1:0)&&s.gates[gate]?.relay_active===true};
  }
  default:throw Error('Ця дія потребує локального шлюзу');
 }
}
export function rpcEnvelope(client,op,args={},now=Date.now(),id=crypto.randomUUID()){
 return {version:1,id,client,op,args,created:now,expires:now+20000};
}
export function gateDescription(g){
 if(!g)return 'Немає даних';
 if(!g.reeds_enabled)return (g.state_text||'Стан невідомий').replace(/\s*\(розрахунковий\)/,'')+' · за останньою командою';
 return g.state_text||'Стан невідомий';
}
