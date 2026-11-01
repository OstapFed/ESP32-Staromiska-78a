import {LocalSecrets} from './lib/secrets.js';
import {GardenTransport} from './lib/transport.js';
import {HEAT_BUTTONS,escapeHTML as e,halfDegree,modeName,validateConnection,gateDescription,parseDeviceState} from './lib/protocol.js';
import {demoState,demoConfig,applyDemo} from './lib/demo.js';

const $=id=>document.getElementById(id),qa=(s,r=document)=>[...r.querySelectorAll(s)];
const STORAGE='staromiska78a.connection.v1',LOG_KEY='staromiska78a.pwa.log.v22',STATE_KEY='staromiska78a.pwa.state.v22';
const transport=new GardenTransport();let state=null,config=null,activeGate=0,busy=false,configLoaded=false,installPrompt=null,waitingWorker=null,manuallyDisconnected=false;
const secrets=new LocalSecrets(),secretScope=()=>transport.config?transport.config.url+'|'+transport.config.base:'demo';
function rememberSecret(key,value){if(demo)return;try{secrets.set(secretScope(),key,value);}catch{toast('Браузер не дозволив зберегти пароль',true);}}
let demo=new URLSearchParams(location.search).get('demo')==='1',demoReceived=0,history=[],logs=[];
const fresh=()=>demo||transport.fresh,extended=()=>demo||(transport.gateway&&configLoaded&&fresh());
const fmt=(v,suffix=' °C')=>typeof v==='number'&&Number.isFinite(v)?v.toFixed(1)+suffix:'—';
const asTime=(hour,min)=>`${String(hour).padStart(2,'0')}:${String(min).padStart(2,'0')}`;
const knownPages=new Set(qa('.page').map(n=>n.id));
function toast(text,error=false){$('toast').hidden=false;$('toast').textContent=text;$('toast').classList.toggle('error',error);clearTimeout(toast.timer);toast.timer=setTimeout(()=>$('toast').hidden=true,error?7000:4000);}
function log(text){logs.unshift({time:new Date().toISOString(),text});logs=logs.slice(0,150);try{localStorage.setItem(LOG_KEY,JSON.stringify(logs));}catch{}renderLog();}
function renderLog(){$('log').replaceChildren(...logs.map(row=>{const n=document.createElement('div');n.textContent=new Date(row.time).toLocaleString('uk-UA')+' · '+row.text;return n;}));}
function nav(id,replace=false){if(!knownPages.has(id))id='home';qa('.page').forEach(n=>n.classList.toggle('active',n.id===id));if(location.hash!=='#'+id){if(replace)historyReplace(id);else location.hash=id;}window.scrollTo({top:0,behavior:'instant'});if(['auto','vacation','sensors','lights','water','memo','settings'].includes(id))populateForms();}
function historyReplace(id){window.history.replaceState(null,'','#'+id);}
qa('[data-go]').forEach(n=>n.addEventListener('click',ev=>{ev.preventDefault();nav(n.dataset.go);}));
window.addEventListener('hashchange',()=>nav(location.hash.slice(1)||'home',true));
function setText(id,value){$(id).textContent=value;}
function acceptState(next,freshMessage=true){
 state=next;if(freshMessage){demoReceived=Date.now();if(typeof next.temperature==='number'){history.push({at:Date.now(),value:next.temperature});history=history.filter(p=>p.at>Date.now()-6*3600000).slice(-1440);}}
 try{if(!demo)localStorage.setItem(STATE_KEY,JSON.stringify(next));}catch{}render();
}
function render(){
 const s=state||{};setText('temperature',fmt(s.temperature));setText('outdoor',fmt(s.outdoor_temperature));setText('roomTemp',fmt(s.temperature));setText('manualRoom',fmt(s.temperature));setText('manualTarget',fmt(s.manual_setpoint));setText('activeTarget',fmt(s.boiler_setpoint));
 setText('boilerState',s.boiler_on?'Працює':s.boiler_on===false?'Вимкнено':'—');setText('manualBoiler',$('boilerState').textContent);setText('boilerMode',modeName(state));setText('homeMode',modeName(state));setText('homeTarget',fmt(s.boiler_setpoint));setText('homeWifi',s.wifi_ssid||'Немає мережі');
 setText('manualNote',!extended()&&s.vacation?'Під час «Відпустки» ручна уставка змінюється через шлюз. Режим не перемикається.':'Крок 0,5 °C. Зміна температури не перемикає режим.');
 qa('[data-heat]').forEach(n=>{n.classList.toggle('is-selected',HEAT_BUTTONS.indexOf(n.dataset.heat)===s.heating_selected_command);n.setAttribute('aria-pressed',String(HEAT_BUTTONS.indexOf(n.dataset.heat)===s.heating_selected_command));});
 setText('rainState',!state?'Немає даних':s.rain_blocked?'🌧 Полив заблоковано захистом від дощу':s.rain?'🌧 Датчик фіксує дощ':'☀️ Захист від дощу не блокує полив');
 setText('waterState',s.watering?(s.watering_all?'Працює полив усіх зон':`Працює зона ${s.watering_zone}`):'Полив зупинено');
 setText('lightState',s.lights?.[0]?'Світло ввімкнено':s.lights?'Світло вимкнено':'Немає даних');
 $('gateCards').innerHTML=(s.gates||[]).map((g,i)=>`<div class="panel"><div class="row"><h3>${e(g.name)}</h3><span class="badge">${g.reeds_enabled?'Геркони ввімкнено':'Без герконів'}</span></div><div class="gate-state ${!g.reeds_enabled?'calculated':''} ${g.state===5?'error':''}">${e(gateDescription(g))}</div><div class="two"><button data-gate="${i}" data-cmd="open" data-control ${!g.open_allowed?'data-unavailable="1"':''}>Відкрити</button><button data-gate="${i}" data-cmd="close" data-control ${!g.close_allowed?'data-unavailable="1"':''}>Закрити</button></div><button class="gate-settings-link" data-gate-settings="${i}">⚙️ Налаштування цих воріт ›</button></div>`).join('')||'<div class="panel empty">Підключіться, щоб побачити стан воріт.</div>';
 $('sensorCards').innerHTML=`<div class="panel sensor-grid">${(s.xiaomi_sensors||[]).map((x,i)=>`<div><h4>${e(x.name||'Датчик '+(i+1))}</h4><div class="sensor-number">${fmt(x.temperature)}</div><p class="note">${x.enabled?(x.online?'Онлайн':'Немає свіжих даних'):'Вимкнено'}<br>Вологість ${typeof x.humidity==='number'?Math.round(x.humidity)+'%':'—'} · Батарея ${typeof x.battery==='number'?x.battery+'%':'—'}<br>RSSI ${typeof x.rssi==='number'?x.rssi+' dBm':'—'}</p></div>`).join('')||'<p class="note">Очікування даних датчиків.</p>'}</div>`;
 const details=[['ESP32',fresh()?'Є свіжі дані':'Немає свіжих даних'],['Прошивка',s.firmware||'—'],['IP-адреса',s.ip||'—'],['Wi-Fi',s.wifi_ssid||'—'],['Сигнал',typeof s.wifi_rssi==='number'?s.wifi_rssi+' dBm':'—'],['Джерело тепла',s.temperature_source||'—'],['Час роботи',s.uptime_s!=null?Math.floor(s.uptime_s/3600)+' год '+Math.floor(s.uptime_s%3600/60)+' хв':'—'],['Вільна пам’ять',s.free_heap!=null?Math.round(s.free_heap/1024)+' КБ':'—'],['Час NTP',s.time_synced?'Синхронізовано':'Не синхронізовано'],['HomeSpan',s.homespan_available?'Вбудовано':'—']];
 $('deviceDetails').innerHTML=details.map(([k,v])=>`<div>${e(k)}<b>${e(v)}</b></div>`).join('');
 setText('homekitState',s.homekit_mode===1?(s.homekit_started?'HomeSpan запущено':'HomeSpan очікує Wi-Fi'):s.homekit_mode===2?'Через Home Assistant / MQTT':'Вимкнено');
 if(s.ip&&/^\d{1,3}(\.\d{1,3}){3}$/.test(s.ip)){$('localPanel').href='http://'+s.ip+'/';$('localPanel').removeAttribute('aria-disabled');}else $('localPanel').setAttribute('aria-disabled','true');
 renderHealth();renderChart();
}
function renderHealth(){
 const connected=demo||transport.connected,live=fresh();setText('connection',demo?'Демонстрація':connected?(live?'MQTT · ESP32 онлайн':'MQTT · очікування ESP32'):'MQTT не підключено');$('connection').className='pill '+(live?'good':'bad');$('stale').hidden=live;
 const received=demo?demoReceived:transport.received;setText('lastSeen',received?'Оновлено '+new Date(received).toLocaleTimeString('uk-UA'):'Очікування свіжих даних');
 qa('[data-control]').forEach(n=>n.disabled=!live||busy||n.dataset.unavailable==='1');qa('[data-extended]').forEach(n=>n.disabled=!extended()||busy);
 qa('[data-gateway-form]').forEach(f=>{const unavailable=!extended();f.dataset.unavailable=String(unavailable);qa('input,select,textarea',f).forEach(n=>n.disabled=unavailable||busy);});
 $('tempDown').disabled=!live||busy||(!extended()&&state?.vacation)||!(state?.manual_setpoint>5);$('tempUp').disabled=!live||busy||(!extended()&&state?.vacation)||!(state?.manual_setpoint<30);
 if(state?.rain_blocked)qa('[data-zone],[data-water-all]').forEach(n=>n.disabled=true);
 qa('.gateway-note').forEach(n=>n.textContent=extended()?'':'Для збереження потрібен локальний шлюз. Основне керування через MQTT працює без нього.');
 setText('gatewayStatus',extended()?'Шлюз підключений: усі налаштування доступні.':transport.gateway?'Шлюз знайдено, очікуємо дані контролера.':'Шлюз не підключений. Доступне основне MQTT-керування.');
 $('disconnect').disabled=!connected&&!transport.client;document.body.dataset.live=String(live);
}
function renderChart(){const samples=history.filter(x=>Number.isFinite(x.value));if(samples.length<2){$('temperatureChart').replaceChildren();return;}const values=samples.map(x=>x.value),min=Math.min(...values)-.2,max=Math.max(...values)+.2,t0=samples[0].at,dt=Math.max(1,samples.at(-1).at-t0);$('temperatureChart').innerHTML=`<polyline points="${samples.map(p=>`${20+(p.at-t0)/dt*500},${84-(p.value-min)/(max-min)*65}`).join(' ')}"/><text x="20" y="13">${max.toFixed(1)} °C</text><text x="20" y="99">${new Date(t0).toLocaleTimeString('uk-UA',{hour:'2-digit',minute:'2-digit'})}</text><text x="470" y="99">${new Date(samples.at(-1).at).toLocaleTimeString('uk-UA',{hour:'2-digit',minute:'2-digit'})}</text>`;setText('chartNote',`${samples.length} свіжих вимірювань · ${min.toFixed(1)}–${max.toFixed(1)} °C`);}
async function run(op,args={},label='Зміни',button){
 if(busy)return;busy=true;button?.classList.add('is-pending');renderHealth();
 try{
  let result;if(demo){await new Promise(r=>setTimeout(r,350));result=applyDemo(state,config,op,args);acceptState(result.state);populateForms();}else{result=await transport.command(op,args);if(result?.state)acceptState(result.state);if(result?.config){config=result.config;configLoaded=true;populateForms();}}
  log(label+' — підтверджено контролером'+(demo?' (демо)':''));toast(label+' — підтверджено');return result??true;
 }catch(err){toast(err.message,true);log(label+' — '+err.message);return false;}finally{busy=false;button?.classList.remove('is-pending');renderHealth();}
}
function confirmAction(title,text,needPin=false){return new Promise(resolve=>{const dialog=$('confirmDialog');setText('confirmTitle',title);setText('confirmText',text);$('pinLabel').hidden=!needPin;$('gatePin').value='';dialog.returnValue='cancel';dialog.addEventListener('close',()=>resolve(dialog.returnValue==='ok'?{pin:$('gatePin').value}:null),{once:true});dialog.showModal();});}
function fillForm(id,values,force=false){const f=$(id);if(!force&&f.dataset.dirty==='1')return;for(const [key,value]of Object.entries(values||{})){const field=f.elements.namedItem(key);if(!field)continue;if(field.type==='checkbox')field.checked=!!value;else if(value!=null)field.value=String(value);}if(force)f.dataset.dirty='0';}
function days(container,values){qa('input',$(container)).forEach(n=>n.checked=(values||[]).includes(Number(n.value)));}
function populateForms(force=false){
 if(!state)return;
 fillForm('vacationForm',{vacTemp:state.vacation_temperature},force);fillForm('homekitForm',{mode:state.homekit_mode},force);
 if(!config)return;
 fillForm('autoForm',config.heat,force);fillForm('rainForm',config.rain,force);fillForm('extraForm',{enabled:config.extra},force);fillForm('memoForm',{text:config.memo},force);fillForm('wifiForm',{ssid:config.ssid,password:secrets.get(secretScope(),'wifi:'+config.ssid)},force);
 const zone=Number($('scheduleZone').value),wc=config.water?.[zone];if(wc){fillForm('waterForm',wc,force);if(force||$('waterForm').dataset.dirty!=='1')days('waterDays',wc.days);}
 if(config.light){fillForm('lightForm',config.light,force);if(force||$('lightForm').dataset.dirty!=='1')days('lightDays',config.light.days);}
 if(force||$('sensorForm').dataset.dirty!=='1'){
  $('sensorFields').innerHTML=Array.from({length:4},(_,i)=>{const x=state.xiaomi_sensors?.[i]||{};return `<div class="sensor-editor"><h4>Датчик ${i+1}</h4><label class="check"><input type="checkbox" name="en${i}" ${x.enabled?'checked':''}> Використовувати</label><label>Назва кімнати<input name="name${i}" maxlength="32" value="${e(x.name||'Кімната '+(i+1))}"></label><label>MAC-адреса<input name="mac${i}" placeholder="AA:BB:CC:DD:EE:FF" pattern="(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}" value="${e(x.mac||'')}"></label></div>`;}).join('');fillForm('sensorForm',{source:state.xiaomi_heating_source},force);
 }
 renderHealth();
}
function formValues(id){const form=$(id),out={};for(const n of form.elements)if(n.name&&!n.disabled)out[n.name]=n.type==='checkbox'?n.checked:n.value;return out;}
function checkedDays(container){return qa('input:checked',$(container)).map(n=>Number(n.value));}
function openGateSettings(index){activeGate=index;const g=state?.gates[index];if(!g)return;setText('gateSettingsTitle','⚙️ '+g.name);fillForm('gateForm',{name:g.name,pinEnabled:g.pin_required,reedsEnabled:g.reeds_enabled,pulseMs:g.pulse_ms,newPin:''},true);setText('gatePins',index===0?'Геркони: GPIO15 — відкрито; GPIO16 — закрито.':'Геркони: GPIO17 — відкрито; GPIO18 — закрито.');nav('gate-settings');}
document.addEventListener('click',async ev=>{
 const button=ev.target.closest('button');if(!button)return;
 if(button.dataset.heat){await run('heat.mode',{command:button.dataset.heat},'Режим опалення',button);}
 else if(button.dataset.light){await run('light.set',{on:button.dataset.light==='on'},'Освітлення',button);}
 else if(button.dataset.zone){await run('water.start',{zone:Number(button.dataset.zone)},'Запуск поливу зони '+button.dataset.zone,button);}
 else if(button.dataset.waterAll){if(await confirmAction('Полив усіх зон','Запустити всі зони '+(button.dataset.waterAll==='all'?'послідовно?':'одночасно?')))await run('water.all',{action:button.dataset.waterAll},'Полив усіх зон',button);}
 else if(button.dataset.gateSettings!==undefined)openGateSettings(Number(button.dataset.gateSettings));
 else if(button.dataset.gate!==undefined){
  const gate=Number(button.dataset.gate),g=state.gates[gate],cmd=button.dataset.cmd,cached=demo?'':secrets.get(secretScope(),'gate:'+gate);
  const result=await confirmAction((cmd==='open'?'Відкрити':'Закрити')+' — '+g.name,extended()?'Команда подасть імпульс на реле воріт.'+(g.pin_required?(cached?' Використаємо збережений PIN.':' Введений PIN буде збережений на цьому пристрої.'):''):'MQTT-команда подасть імпульс на реле. Авторизація — обліковий запис брокера; локальний PIN прошивки в цьому каналі не перевіряється.',extended()&&g.pin_required&&!cached);
  if(result){const pin=cached||result.pin;const ok=await run('gate.command',{gate,command:cmd,pin},'Команда воріт',button);if(ok&&extended()&&pin)rememberSecret('gate:'+gate,pin);if(!ok&&cached)rememberSecret('gate:'+gate,'');}
 }
});
$('tempDown').addEventListener('click',()=>run('heat.manual',{value:halfDegree(state.manual_setpoint-.5)},'Ручна температура',$('tempDown')));
$('tempUp').addEventListener('click',()=>run('heat.manual',{value:halfDegree(state.manual_setpoint+.5)},'Ручна температура',$('tempUp')));
$('stopWater').addEventListener('click',()=>run('water.stop',{},'Полив зупинено',$('stopWater')));
$('zones').innerHTML=Array.from({length:4},(_,i)=>`<button data-zone="${i+1}" data-control>▶ Зона ${i+1}</button>`).join('');
for(const [container,prefix]of [['waterDays','wd'],['lightDays','ld']])$(container).innerHTML=['Нд','Пн','Вт','Ср','Чт','Пт','Сб'].map((label,i)=>`<label for="${prefix+i}">${label}<input id="${prefix+i}" type="checkbox" value="${i}" aria-label="${label}"></label>`).join('');
qa('[data-gateway-form]').forEach(f=>{f.addEventListener('input',()=>f.dataset.dirty='1');f.addEventListener('change',()=>f.dataset.dirty='1');});
$('scheduleZone').addEventListener('change',()=>{$('waterForm').dataset.dirty='0';populateForms();});
function bindSave(id,op,read,label){$(id).addEventListener('submit',async ev=>{ev.preventDefault();if(!extended())return toast('Потрібен підключений шлюз',true);const args=read();const result=await run(op,args,label,$(id).querySelector('button[type=submit]'));if(result){$(id).dataset.dirty='0';if(op==='gate.save'){if(args.newPin)rememberSecret('gate:'+args.gate,args.newPin);$('gateForm').elements.newPin.value='';}await refreshConfig();}});}
bindSave('autoForm','heat.auto.save',()=>formValues('autoForm'),'Налаштування Авто');
bindSave('vacationForm','heat.vacation.save',()=>formValues('vacationForm'),'Температура Відпустки');
bindSave('gateForm','gate.save',()=>({gate:activeGate,...formValues('gateForm')}),'Налаштування воріт');
bindSave('waterForm','water.save',()=>({zone:Number($('scheduleZone').value),config:{...formValues('waterForm'),days:checkedDays('waterDays')}}),'Розклад зони');
bindSave('lightForm','light.save',()=>({...formValues('lightForm'),days:checkedDays('lightDays')}),'Розклад освітлення');
bindSave('rainForm','rain.save',()=>formValues('rainForm'),'Захист від дощу');
bindSave('extraForm','extra.save',()=>formValues('extraForm'),'DS18B20');
bindSave('memoForm','memo.save',()=>formValues('memoForm'),'Пам’ятка');
bindSave('sensorForm','sensors.save',()=>{const v=formValues('sensorForm');return {source:Number(v.source),sensors:Array.from({length:4},(_,i)=>({enabled:!!v['en'+i],name:v['name'+i],mac:v['mac'+i]}))};},'Налаштування датчиків');
for(const [id,op,label]of [['homekitForm','homekit.save','HomeKit'],['wifiForm','wifi.save','Wi-Fi']])$(id).addEventListener('submit',async ev=>{ev.preventDefault();if(!extended())return;if(!await confirmAction('Зберегти '+label+'?','Контролер перезапуститься. MQTT-з’єднання тимчасово зникне.'))return;const values=formValues(id),ok=await run(op,values,label+' збережено — очікуємо перезапуск');if(ok){if(id==='wifiForm'&&values.password)rememberSecret('wifi:'+values.ssid,values.password);$(id).dataset.dirty='0';}});
$ ('wifiForm').elements.ssid.addEventListener('input',ev=>{$('wifiForm').elements.password.value=secrets.get(secretScope(),'wifi:'+ev.target.value);});
async function refreshConfig(){if(demo){populateForms(true);return;}if(!transport.gateway)return;try{const c=await transport.rpc('config.get');config=c;configLoaded=true;if(c.state)acceptState(c.state);populateForms();}catch(err){toast(err.message,true);}}
$('refreshConfig').addEventListener('click',refreshConfig);
transport.addEventListener('state',event=>acceptState(event.detail.state,event.detail.fresh));
transport.addEventListener('config',event=>{config=event.detail;configLoaded=true;if(config.state)acceptState(config.state);populateForms();});
transport.addEventListener('health',renderHealth);
transport.addEventListener('error',event=>{setText('connectionError',event.detail);toast(event.detail,true);});
transport.addEventListener('reconnect',()=>setText('connectionError','Відновлюємо MQTT-з’єднання…'));
let connectionCandidate=null;
transport.addEventListener('connection',()=>{setText('connectionError','');if(connectionCandidate){try{const v=connectionCandidate;if(v.remember)localStorage.setItem(STORAGE,JSON.stringify({version:1,broker:v.url,username:v.username,password:v.password,base:v.base}));else localStorage.removeItem(STORAGE);}catch{toast('Браузер не дозволив зберегти підключення',true);}}log('З’єднання з MQTT-брокером встановлено');});
$('connectionForm').addEventListener('submit',ev=>{ev.preventDefault();try{const values=validateConnection(formValues('connectionForm'));demo=false;$('demoBanner').hidden=true;manuallyDisconnected=false;connectionCandidate=values;transport.connect(values);toast('Підключаємося до брокера…');}catch(err){setText('connectionError',err.message);}});
$('disconnect').addEventListener('click',()=>{manuallyDisconnected=true;transport.disconnect();toast('Відключено. Автопідключення призупинене до наступного відкриття.');});
$('forgetSettings').addEventListener('click',()=>{try{localStorage.removeItem(STORAGE);secrets.clear();$('password').value='';$('wifiForm').elements.password.value='';connectionCandidate=null;toast('Збережені паролі та PIN видалено з цього пристрою');}catch{toast('Не вдалося видалити дані браузера',true);}});
$('clearLog').addEventListener('click',()=>{logs=[];localStorage.removeItem(LOG_KEY);renderLog();});
$('exportLog').addEventListener('click',()=>{const link=document.createElement('a'),url=URL.createObjectURL(new Blob([JSON.stringify(logs,null,2)],{type:'application/json'}));link.href=url;link.download='garden-events.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);});
$('enterDemo').addEventListener('click',()=>{transport.disconnect();demo=true;startDemo();nav('home');});
$('leaveDemo').addEventListener('click',()=>{demo=false;$('demoBanner').hidden=true;state=null;config=null;configLoaded=false;render();nav('settings');});
function startDemo(){demo=true;state=demoState();config=demoConfig();configLoaded=true;$('demoBanner').hidden=false;acceptState(state);populateForms(true);}
window.addEventListener('beforeinstallprompt',ev=>{ev.preventDefault();installPrompt=ev;});
$('installApp').addEventListener('click',async()=>{if(installPrompt){await installPrompt.prompt();installPrompt=null;}else toast('Safari: Поділитися → На початковий екран. Android: меню → Встановити додаток.');});
$('applyUpdate').addEventListener('click',async()=>{if(busy)return toast('Дочекайтеся підтвердження поточної дії',true);transport.disconnect();waitingWorker?.postMessage({type:'SKIP_WAITING'});});
if('serviceWorker'in navigator){let refreshing=false;const hadController=!!navigator.serviceWorker.controller;navigator.serviceWorker.addEventListener('controllerchange',()=>{if(hadController&&!refreshing){refreshing=true;location.reload();}});window.addEventListener('load',()=>navigator.serviceWorker.register('./sw.js',{scope:'./'}).then(reg=>{if(reg.waiting){waitingWorker=reg.waiting;$('updateBanner').hidden=false;}reg.addEventListener('updatefound',()=>{const worker=reg.installing;worker.addEventListener('statechange',()=>{if(worker.state==='installed'&&navigator.serviceWorker.controller){waitingWorker=reg.waiting;$('updateBanner').hidden=false;}});});}).catch(()=>toast('Офлайн-кеш недоступний. Відкрийте сайт через HTTPS або localhost.',true)));}
window.addEventListener('online',()=>{if(!manuallyDisconnected&&transport.client&&!transport.client.connected)transport.client.reconnect();});
document.addEventListener('visibilitychange',()=>{if(!document.hidden){renderHealth();if(!manuallyDisconnected&&transport.client&&!transport.client.connected)transport.client.reconnect();}});
function tick(){const now=new Date(),opts={timeZone:'Europe/Kyiv',hour:'2-digit',minute:'2-digit'};setText('clock',now.toLocaleTimeString('uk-UA',opts));setText('headerClock',now.toLocaleTimeString('uk-UA',opts));setText('today',now.toLocaleDateString('uk-UA',{timeZone:'Europe/Kyiv',day:'numeric',month:'long',weekday:'short'}));renderHealth();}
try{logs=JSON.parse(localStorage.getItem(LOG_KEY)||'[]').slice(0,150);const cached=localStorage.getItem(STATE_KEY);if(cached)state=parseDeviceState(cached);}catch{}
renderLog();nav(location.hash.slice(1)||'home',true);
if(demo)startDemo();else{
 try{const saved=JSON.parse(localStorage.getItem(STORAGE)||'null');if(saved&&saved.version===1){const c=validateConnection({url:saved.broker,username:saved.username,password:saved.password,base:saved.base||'garden/controller',remember:true});fillForm('connectionForm',c);$('broker').value=c.url;connectionCandidate=c;transport.connect(c);}else nav('settings');}catch{nav('settings');toast('Перевірте збережені налаштування MQTT',true);}
 render();
}
tick();setInterval(tick,1000);
