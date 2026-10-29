"use strict";
(()=>{
const $=id=>document.getElementById(id),BASE='garden/controller';
let client=null,connectionId=0,availability='unknown',state=null,lastStateAt=0,desired=null,commandAt=0,commandTimeout=null,confirmedAt=0,failedTarget=null,setpointAttempt=0;
const fmt=(v,s='')=>typeof v==='number'&&Number.isFinite(v)?v.toFixed(1)+s:'—';
// Accept numeric MQTT strings for the outdoor display without coercing missing data to 0.
function outdoorTemperature(value){
  if(typeof value==='number')return Number.isFinite(value)?value:null;
  if(typeof value!=='string')return null;
  const text=value.trim();
  if(!/^[+-]?(?:\d+(?:[.,]\d+)?|[.,]\d+)$/.test(text))return null;
  const number=Number(text.replace(',','.'));
  return Number.isFinite(number)?number:null;
}
// Match refreshOutdoorTemp() in Garden_U_V35_9_FAST_START.ino.
// Display-only weather data; never used for heating or watering commands.
const WEATHER_URL='https://api.open-meteo.com/v1/forecast?latitude=48.53&longitude=25.04&current=temperature_2m&timezone=Europe%2FKyiv';
let outdoorWeather=null,outdoorWeatherAt=0,outdoorWeatherBusy=false,outdoorWeatherError=false;
function renderOutdoor(){
  const fresh=outdoorWeatherAt>0&&Date.now()-outdoorWeatherAt<30*60*1000;
  const value=fresh?outdoorWeather:null;
  $('outdoor').textContent=value===null?'—':fmt(value,' °C');
  const description=value!==null?
    'Open-Meteo · 48.53, 25.04 · оновлено '+new Date(outdoorWeatherAt).toLocaleTimeString('uk-UA'):
    outdoorWeatherBusy?'Завантаження погоди Open-Meteo…':
    outdoorWeatherError?'Open-Meteo недоступний. Повторимо запит автоматично.':'Очікування погоди Open-Meteo';
  $('outdoor').title=description;
  $('outdoorSource').textContent=value!==null?(outdoorWeatherError?'Open-Meteo · повторюємо':'Open-Meteo'):
    outdoorWeatherBusy?'Завантаження…':'Погода недоступна';
  $('outdoorSource').title=description;
}
async function refreshOutdoorWeather(){
  if(outdoorWeatherBusy)return;
  outdoorWeatherBusy=true;
  renderOutdoor();
  const controller=new AbortController();
  const timeout=setTimeout(()=>controller.abort(),12000);
  try{
    const response=await fetch(WEATHER_URL,{cache:'no-store',signal:controller.signal});
    if(!response.ok)throw new Error('HTTP '+response.status);
    const data=await response.json();
    const value=outdoorTemperature(data?.current?.temperature_2m);
    if(value===null)throw new Error('Відсутня температура');
    outdoorWeather=value;
    outdoorWeatherAt=Date.now();
    outdoorWeatherError=false;
  }catch(error){
    outdoorWeatherError=true;
  }finally{
    clearTimeout(timeout);
    outdoorWeatherBusy=false;
    renderOutdoor();
  }
}

// BEGIN SAVED CONNECTION SETTINGS V19
const SETTINGS_KEY='staromiska78a.connection.v1';
let settingsRevision=0;
let restoredSettings=false,resumeAfterPageHide=false;
function validConnectionSettings(value){
  if(!value||value.version!==1||typeof value.broker!=='string'||typeof value.username!=='string'||typeof value.password!=='string'||!value.username.trim()||!value.password)return false;
  try{const url=new URL(value.broker);return url.protocol==='wss:'&&Boolean(url.hostname)&&!url.username&&!url.password;}catch{return false;}
}
function settingsMessage(message,error=false){
  $('settingsFeedback').textContent=message;
  $('settingsFeedback').className='note'+(error?' error':'');
}
function restoreConnectionSettings(){
  try{
    const raw=localStorage.getItem(SETTINGS_KEY);
    if(!raw)return;
    $('forgetSettings').hidden=false;
    const saved=JSON.parse(raw);
    if(!validConnectionSettings(saved))throw new Error('Invalid saved settings');
    $('broker').value=saved.broker;
    $('username').value=saved.username;
    $('password').value=saved.password;
    restoredSettings=true;
    settingsMessage('Збережені налаштування відновлено. Підключаємося автоматично…');
  }catch{settingsMessage('Не вдалося прочитати збережені налаштування. Введіть їх вручну.',true);}
}
function persistConnectionSettings(settings){
  if(!validConnectionSettings(settings)){settingsMessage('Введіть коректну WSS-адресу, користувача та пароль.',true);return false;}
  try{
    const serialized=JSON.stringify(settings);
    localStorage.setItem(SETTINGS_KEY,serialized);
    if(localStorage.getItem(SETTINGS_KEY)!==serialized)throw new Error('Storage verification failed');
    $('forgetSettings').hidden=false;
    settingsMessage('Адресу, користувача та пароль збережено. Наступного разу вони підставляться автоматично.');
    return true;
  }catch{settingsMessage('Підключення можливе, але браузер не зберіг пароль. Перевірте дозвіл на збереження даних цього сайту.',true);return false;}
}
$('saveSettings').addEventListener('click',()=>{
  settingsRevision++;
  persistConnectionSettings({version:1,broker:$('broker').value.trim(),username:$('username').value.trim(),password:$('password').value});
});
$('forgetSettings').addEventListener('click',()=>{
  settingsRevision++;
  try{
    localStorage.removeItem(SETTINGS_KEY);
    if(localStorage.getItem(SETTINGS_KEY)!==null)throw new Error('Removal failed');
    $('forgetSettings').hidden=true;
    $('password').value='';
    settingsMessage('Збережені дані видалено. Поточне підключення не змінено.');
  }catch{settingsMessage('Не вдалося видалити збережені дані. Спробуйте очистити дані сайту в браузері.',true);}
});
for(const id of ['broker','username','password'])$(id).addEventListener('input',()=>{settingsRevision++;settingsMessage('Зміни збережуться після успішного підключення або кнопкою «Зберегти налаштування».');});
restoreConnectionSettings();
// END SAVED CONNECTION SETTINGS V19

const log=s=>{const line=document.createElement('div');line.textContent=new Date().toLocaleTimeString('uk-UA')+' · '+s;$('log').prepend(line);while($('log').children.length>80)$('log').lastElementChild.remove()};
const note=(s,cls='')=>{$('tempFeedback').textContent=s;$('tempFeedback').className='note '+cls;log(s)};
const connected=()=>Boolean(client?.connected);
const live=()=>connected()&&state&&availability==='online'&&Date.now()-lastStateAt<90000;
function nav(id){document.querySelectorAll('.page').forEach(el=>el.classList.toggle('active',el.id===id));window.scrollTo(0,0)}
document.querySelectorAll('[data-go]').forEach(b=>b.addEventListener('click',()=>nav(b.dataset.go)));
setInterval(()=>{$('headerClock').textContent=$('clock').textContent},1000);$('today').textContent=new Date().toLocaleDateString('uk-UA');setInterval(()=>$('clock').textContent=new Date().toLocaleTimeString('uk-UA',{hour:'2-digit',minute:'2-digit'}),1000);$('clock').textContent=new Date().toLocaleTimeString('uk-UA',{hour:'2-digit',minute:'2-digit'});
function renderSetpointIndicators(s,ok){
  const hasActual=typeof s.boiler_setpoint==='number'&&Number.isFinite(s.boiler_setpoint);
  $('actualSetpoint').textContent=fmt(s.boiler_setpoint,' °C');
  $('actualSetpointCard').className='setpoint-card '+(ok&&hasActual?'is-live':'is-stale');
  $('actualSetpointStatus').textContent=!hasActual?'Немає даних від ESP32':
    !ok?'Останнє отримане · дані неактуальні':
    'Отримано о '+new Date(lastStateAt).toLocaleTimeString('uk-UA');
  const target=desired??failedTarget;
  $('pendingSetpoint').textContent=fmt(target,' °C');
  $('pendingSetpointCard').className='setpoint-card '+(desired!==null?'is-waiting':failedTarget!==null?'is-failed':'is-idle');
  $('pendingSetpointStatus').textContent=desired!==null?
    (connected()?'Очікуємо відповідь ESP32…':'Зв’язок втрачено · підтвердження немає'):
    failedTarget!==null?'Не підтверджено · можна повторити':'Немає команди в очікуванні';
}
function render(){const ok=live(),s=state||{},mode={0:'OFF',1:'MANUAL',2:'AUTO'}[s.boiler_mode]||'—';$('connection').textContent=connected()?'MQTT підключено':'MQTT не підключено';$('connection').className='pill '+(connected()?'good':'bad');$('lastSeen').textContent=lastStateAt?'Оновлено '+new Date(lastStateAt).toLocaleTimeString('uk-UA'):'Немає даних';$('stale').classList.toggle('hidden',Boolean(ok));$('connect').disabled=Boolean(client);$('disconnect').disabled=!client;$('temperature').textContent=fmt(s.temperature,' °C');$('homeTemp').textContent=fmt(s.temperature,' °C');$('roomTemp').textContent=fmt(s.temperature,' °C');renderOutdoor();$('humidity').textContent=fmt(s.xiaomi_humidity,' %');$('boilerState').textContent=typeof s.boiler_on==='boolean'?(s.boiler_on?'Увімкнено':'Вимкнено'):'—';$('homeBoiler').textContent=s.boiler_on?'Котел увімкнено':'Котел вимкнено';$('boilerMode').textContent=mode;$('homeWifi').textContent=availability==='online'?'ESP32 онлайн':'ESP32 офлайн';$('setpoint').textContent=fmt(desired??failedTarget??s.boiler_setpoint,' °C');renderSetpointIndicators(s,ok);$('retrySetpoint').hidden=failedTarget===null;$('discardSetpoint').hidden=failedTarget===null;$('retrySetpoint').disabled=!ok||desired!==null||s.vacation===true;$('vacationNotice').hidden=s.vacation!==true;$('tempDown').disabled=!ok||desired!==null||s.vacation===true;$('tempUp').disabled=!ok||desired!==null||s.vacation===true;$('resetSchedule').disabled=!ok||desired!==null;document.querySelectorAll('[data-mode]').forEach(b=>b.disabled=!ok||desired!==null);$('stopWater').disabled=!connected();$('waterState').textContent=s.watering?'Полив активний · зона '+(s.watering_zone||'—'):'Полив вимкнений';$('rain').textContent=s.rain_blocked?'⚠️ Полив заблокований через дощ':'Полив дозволений';$('availability').textContent=availability;$('firmware').textContent=s.firmware||'—';$('ip').textContent=s.ip||'—';$('battery').textContent=typeof s.xiaomi_battery==='number'?s.xiaomi_battery+' %':'—';$('sensorName').textContent=s.xiaomi_sensors?.[0]?.name||s.temperature_source||'Xiaomi';$('updatedAt').textContent=lastStateAt?new Date(lastStateAt).toLocaleTimeString('uk-UA'):'—';document.querySelectorAll('[data-light]').forEach(b=>{const i=Number(b.dataset.light)-1,on=s.lights?.[i]===true;b.classList.toggle('active',on);b.textContent='💡 Світло '+(i+1)+' · '+(on?'УВІМКНЕНО':'ВИМКНЕНО');b.disabled=!ok});document.querySelectorAll('[data-zone]').forEach(b=>{const i=Number(b.dataset.zone);b.classList.toggle('active',Boolean(s.watering&&s.watering_zone===i));b.disabled=!ok||s.rain_blocked===true});}
function clearPending(){if(commandTimeout)clearTimeout(commandTimeout);commandTimeout=null;desired=null;commandAt=0;}
function send(path,payload,label){if(!live()){log('Команду не надіслано: немає актуального стану ESP32');return false;}client.publish(BASE+'/'+path,String(payload),{qos:0,retain:false},err=>log(err?'Помилка '+label+': '+err.message:'Надіслано '+label+' (очікуємо підтвердження станом ESP32)'));return true;}
function submitSetpoint(next){
  if(!live()||desired!==null||state.vacation===true)return;
  if(!Number.isFinite(next)||next<5||next>30){note('Допустимий діапазон 5–30 °C','error');return;}
  failedTarget=null;
  desired=next;
  commandAt=Date.now();
  const attempt=++setpointAttempt;
  const session=connectionId;
  function fail(message){
    if(attempt!==setpointAttempt||session!==connectionId||desired!==next)return;
    clearPending();
    failedTarget=next;
    note(message+' Вибрано '+next.toFixed(1)+' °C, але виконання НЕ підтверджено.','error');
    render();
  }
  note('Вибрано '+next.toFixed(1)+' °C · очікуємо підтвердження ESP32…','waiting');
  render();
  commandTimeout=setTimeout(()=>fail('ESP32 не відповіла за 18 секунд.'),18000);
  try{
    client.publish(BASE+'/boiler/setpoint/set',next.toFixed(1),{qos:0,retain:false},err=>{
      if(attempt!==setpointAttempt||session!==connectionId||desired!==next)return;
      if(err){fail('Помилка надсилання: '+err.message);return;}
      log('Команду '+next.toFixed(1)+' °C надіслано брокеру; чекаємо фактичну уставку ESP32');
    });
  }catch(error){fail('Помилка надсилання: '+error.message);}
}
function adjust(delta){
  const old=desired??failedTarget??state?.boiler_setpoint;
  if(old===null||old===undefined||!Number.isFinite(Number(old)))return;
  submitSetpoint(Math.round((Number(old)+delta)*2)/2);
}
function process(topic,payload,packet={}){if(topic===BASE+'/availability'){availability=payload.trim().toLowerCase();log('ESP32: '+availability);render();return;}if(topic===BASE+'/state'){let obj;try{obj=JSON.parse(payload);if(!obj||typeof obj.boiler_on!=='boolean')return;}catch(e){log('Помилка JSON стану');return;}state=obj;lastStateAt=Date.now();if((desired!==null||failedTarget!==null)&&packet.retain!==true&&obj.vacation!==true&&typeof obj.boiler_setpoint==='number'&&Math.abs(obj.boiler_setpoint-(desired??failedTarget))<0.05){const target=desired??failedTarget;clearPending();failedTarget=null;confirmedAt=Date.now();note('ESP32 підтвердила уставку '+target.toFixed(1)+' °C.','');}else if(desired!==null){log('Очікуємо '+desired.toFixed(1)+' °C, ESP32 повідомляє '+fmt(obj.boiler_setpoint,' °C'));}render();return;}if(topic.startsWith(BASE+'/'))log(topic.slice(BASE.length+1)+': '+payload.slice(0,120));}
function disconnect(){resumeAfterPageHide=false;connectionId++;if(client){const c=client;client=null;c.end(true)}clearPending();failedTarget=null;state=null;availability='unknown';lastStateAt=0;render()}
function connectMqtt(){if(client)return;if(!window.mqtt){$('error').textContent='Бібліотека MQTT не завантажена';return;}const url=$('broker').value.trim(),username=$('username').value.trim(),password=$('password').value;if(!/^wss:\/\//.test(url)||!username||!password){$('error').textContent='Заповніть WSS, користувача і пароль';return;}$('error').textContent='';const revisionAtConnect=settingsRevision;const settingsAtConnect={version:1,broker:url,username,password};let firstConnect=true;if(client)client.end(true);const id=++connectionId;state=null;lastStateAt=0;availability='unknown';clearPending();failedTarget=null;const c=mqtt.connect(url,{username,password,clientId:'staromiska_v11_'+Math.random().toString(16).slice(2,12),clean:true,connectTimeout:12000,reconnectPeriod:4000,protocolVersion:4});client=c;c.on('connect',()=>{if(id!==connectionId)return;if(firstConnect){firstConnect=false;if(settingsRevision===revisionAtConnect)persistConnectionSettings(settingsAtConnect);}log('MQTT підключено');c.subscribe(BASE+'/#',{qos:0},err=>{if(err)$('error').textContent='Помилка підписки: '+err.message});nav('home');render()});c.on('message',(t,b,packet)=>{if(id===connectionId)process(t,b.toString(),packet)});c.on('close',()=>{if(id!==connectionId)return;availability='unknown';render()});c.on('error',err=>{if(id!==connectionId)return;$('error').textContent='MQTT: '+err.message;render()});render()}
$('connect').addEventListener('click',connectMqtt);$('disconnect').addEventListener('click',disconnect);
for(let i=1;i<=3;i++){const b=document.createElement('button');b.dataset.light=i;b.disabled=true;b.addEventListener('click',()=>{const on=state?.lights?.[i-1]===true;send('light/'+i+'/set',on?'OFF':'ON','Світло '+i+' '+(on?'OFF':'ON'))});$('lightsGrid').append(b)}
for(let i=1;i<=4;i++){const b=document.createElement('button');b.dataset.zone=i;b.textContent='Зона '+i+' — 1 хв';b.disabled=true;b.addEventListener('click',()=>{if(!live())return;if(!window.confirm('Запустити полив зони '+i+'?'))return;send('water/'+i+'/set','START','Полив зони '+i)});$('zones').append(b)}
$('stopWater').addEventListener('click',()=>{if(!connected())return;client.publish(BASE+'/water/stop/set','STOP',{qos:0,retain:false},err=>log(err?'STOP: '+err.message:'STOP надіслано · очікуємо стан ESP32'))});
$('retrySetpoint').addEventListener('click',()=>{if(failedTarget!==null)submitSetpoint(failedTarget)});$('discardSetpoint').addEventListener('click',()=>{failedTarget=null;note('Показано останню уставку ESP32. Команду скасування контролеру не надсилали.');render()});
$('tempDown').addEventListener('click',()=>adjust(-0.5));$('tempUp').addEventListener('click',()=>adjust(0.5));
document.querySelectorAll('[data-mode]').forEach(b=>b.addEventListener('click',()=>{const m=b.dataset.mode;if(m==='OFF'&&!window.confirm('Вимкнути опалення?'))return;send('boiler/mode/set',m,'Режим котла '+m)}));$('resetSchedule').addEventListener('click',()=>{if(send('boiler/setpoint/reset','RESET','Повернути розклад')){failedTarget=null;render()}});$('clearLog').addEventListener('click',()=>$('log').replaceChildren());
refreshOutdoorWeather();setInterval(refreshOutdoorWeather,600000);window.addEventListener('online',refreshOutdoorWeather);setInterval(render,5000);window.addEventListener('pagehide',()=>{
  resumeAfterPageHide=Boolean(client);
  if(client){const previous=client;client=null;connectionId++;previous.end(true);}
});
window.addEventListener('pageshow',event=>{
  if(event.persisted&&resumeAfterPageHide){resumeAfterPageHide=false;connectMqtt();}
});
render();
if(restoredSettings){connectMqtt();}else{nav('settings');}

})();