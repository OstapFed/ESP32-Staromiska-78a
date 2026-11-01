export const demoState=()=>({device:'esp32-s3-garden',firmware:'Garden_U_V36.1',ip:'192.168.1.128',wifi_ssid:'Домашня мережа',wifi_rssi:-54,wifi_ap_mode:false,remote_enabled:true,remote_connected:true,mqtt_connected:true,boiler_mode:1,boiler_on:false,heating_selected_command:1,manual_setpoint:21,boiler_setpoint:21,vacation:false,vacation_temperature:15,temperature:21.5,temperature_source:'XIAOMI_1_Вітальня',watering:false,watering_zone:0,watering_all:false,rain:false,rain_blocked:false,lights:[false],outdoor_temperature:8.4,weather_ok:true,rain_forecast_mm:0,uptime_s:8640,free_heap:182400,time_synced:true,homekit_mode:0,homekit_started:false,homespan_available:true,xiaomi_heating_source:0,xiaomi_sensors:[{name:'Вітальня',enabled:true,online:true,mac:'AA:BB:CC:00:00:01',temperature:21.5,humidity:48,battery:93,rssi:-54},{name:'Спальня',enabled:true,online:true,mac:'AA:BB:CC:00:00:02',temperature:20.8,humidity:51,battery:87,rssi:-67},{name:'Кімната 3',enabled:false,online:false,mac:'',temperature:null},{name:'Кімната 4',enabled:false,online:false,mac:'',temperature:null}],gates:[{name:'Ворота заїзд',state:0,state_text:'Закриті',state_confirmed:true,pin_required:false,reeds_enabled:true,pulse_ms:2000,last_command:0,relay_active:false,open_allowed:true,close_allowed:false},{name:'Ворота гараж',state:0,state_text:'Закриті (розрахунковий)',state_confirmed:false,pin_required:true,reeds_enabled:false,pulse_ms:2000,last_command:0,relay_active:false,open_allowed:true,close_allowed:false}]});
export const demoConfig=()=>({heat:{dayTemp:21.5,nightTemp:19,dayTime:'06:30',nightTime:'23:00'},water:Array.from({length:4},(_,i)=>({enabled:true,time:`06:${String(i*15).padStart(2,'0')}`,duration:10,days:[1,3,5],intervalEnabled:false,intervalHours:24,intervalMinutes:0})),light:{enabled:true,onTime:'19:00',offTime:'23:00',days:[0,1,2,3,4,5,6]},rain:{enabled:true,hours:24},extra:false,memo:'Староміська 78а\nПам’ятка демонстраційного режиму.',ssid:'Домашня мережа'});
export function applyDemo(state,config,op,args){
 switch(op){
 case 'heat.mode':{const code={'stop':0,'manual-on':1,'manual-off':2,'auto-on':3,'auto-off':4,'vacation-on':5,'vacation-off':6}[args.command];state.heating_selected_command=code;state.boiler_mode=code===1?1:(code===3||code===5)?2:0;state.vacation=code===5;break;}
 case 'heat.manual':state.manual_setpoint=Number(args.value);break;
 case 'heat.auto.save':config.heat={...args};break;
 case 'heat.vacation.save':state.vacation_temperature=Number(args.vacTemp);break;
 case 'gate.command':{const g=state.gates[args.gate],open=args.command==='open';Object.assign(g,{state:open?1:0,state_text:open?'Відкриті':'Закриті',last_command:open?1:0,open_allowed:!open,close_allowed:open});break;}
 case 'gate.save':{const g=state.gates[args.gate];Object.assign(g,{name:args.name,pin_required:args.pinEnabled,reeds_enabled:args.reedsEnabled,pulse_ms:Number(args.pulseMs),state_confirmed:args.reedsEnabled});break;}
 case 'water.start':state.watering=true;state.watering_zone=Number(args.zone);state.watering_all=false;state.watering_simultaneous=false;break;
 case 'water.all':state.watering=true;state.watering_all=true;state.watering_simultaneous=args.action==='alltogether';state.watering_zone=state.watering_simultaneous?0:1;break;
 case 'water.stop':state.watering=false;state.watering_zone=0;state.watering_all=false;state.watering_simultaneous=false;break;
 case 'water.save':config.water[args.zone]={...args.config};break;
 case 'rain.save':config.rain={...args};break;
 case 'light.set':state.lights[0]=args.on;break;
 case 'light.save':config.light={...args};break;
 case 'sensors.save':state.xiaomi_sensors=state.xiaomi_sensors.map((s,i)=>({...s,...args.sensors[i]}));state.xiaomi_heating_source=Number(args.source);break;
 case 'extra.save':config.extra=args.enabled;break;
 case 'memo.save':config.memo=args.text;break;
 case 'homekit.save':state.homekit_mode=Number(args.mode);state.homekit_started=Number(args.mode)===1;break;
 case 'wifi.save':config.ssid=args.ssid;state.wifi_ssid=args.ssid;break;
 }
 state.boiler_setpoint=state.vacation?state.vacation_temperature:state.boiler_mode===2?Number(config.heat.dayTemp):state.manual_setpoint;
 state.boiler_on=state.boiler_mode!==0&&state.temperature<state.boiler_setpoint-.25;
 return {state,config};
}
