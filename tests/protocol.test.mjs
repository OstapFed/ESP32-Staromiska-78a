import test from 'node:test';
import assert from 'node:assert/strict';
import {isFresh,nativeCommand,parseDeviceState,validateConnection,rpcEnvelope,HEAT_BUTTONS} from '../public/lib/protocol.js';
import {demoState,demoConfig,applyDemo} from '../public/lib/demo.js';
import {LocalSecrets} from '../public/lib/secrets.js';
import {GardenTransport} from '../public/lib/transport.js';

test('stale and retained state cannot enable commands',async()=>{
 const t=new GardenTransport();t.config={base:'test'};t.connected=true;
 try{t.onMessage('test/state',JSON.stringify(demoState()),{retain:true});assert.equal(t.fresh,false);await assert.rejects(t.command('light.set',{on:true}),/свіжого/);
 t.onMessage('test/state',JSON.stringify(demoState()),{retain:false});assert.equal(t.fresh,true);
 t.onMessage('test/availability','offline');assert.equal(t.fresh,false);}finally{t.destroy();}
 assert.equal(isFresh(100,45100),false);assert.equal(isFresh(100,99),false);
});
test('manual temperature never issues a mode command and blocks vacation alias',()=>{
 const s=demoState(),c=nativeCommand('heat.manual',{value:22.5},s);assert.equal(c.suffix,'/boiler/setpoint/set');assert.equal(c.payload,'22.5');assert.equal(c.matches({...s,manual_setpoint:22.5}),true);assert.equal(c.matches({...s,manual_setpoint:22.5,boiler_mode:2}),false);
 assert.throws(()=>nativeCommand('heat.manual',{value:23},{...s,vacation:true}),/шлюз/);
});
test('all modes yield one confirmed selection; settings retain mode',()=>{
 for(const command of HEAT_BUTTONS){const s=demoState(),c=demoConfig();applyDemo(s,c,'heat.mode',{command});assert.equal(HEAT_BUTTONS.filter((_,i)=>i===s.heating_selected_command).length,1);const mode=s.boiler_mode,selected=s.heating_selected_command,vac=s.vacation;
 applyDemo(s,c,'heat.manual',{value:20});applyDemo(s,c,'heat.auto.save',{dayTemp:23});applyDemo(s,c,'heat.vacation.save',{vacTemp:15});assert.equal(s.boiler_mode,mode);assert.equal(s.heating_selected_command,selected);assert.equal(s.vacation,vac);}
 assert.equal(nativeCommand('heat.mode',{command:'manual-off'}).payload,'OFF');assert.equal(nativeCommand('heat.mode',{command:'manual-off'}).matches({...demoState(),heating_selected_command:0}),true);
});
test('gate editing is staged and isolated from other gate and motion',()=>{const s=demoState(),c=demoConfig(),before=structuredClone(s.gates);const draft={gate:0,name:'Нові ворота',pinEnabled:true,reedsEnabled:false,pulseMs:500};assert.deepEqual(s.gates,before);applyDemo(s,c,'gate.save',draft);assert.deepEqual(s.gates[1],before[1]);assert.equal(s.gates[0].relay_active,false);assert.equal(s.gates[0].last_command,before[0].last_command);});
test('credentials stored once, isolated by device, restored and removable',()=>{const map=new Map(),storage={getItem:k=>map.get(k),setItem:(k,v)=>map.set(k,v),removeItem:k=>map.delete(k)},vault=new LocalSecrets(storage);vault.set('brokerA','gate:0','1234');vault.set('brokerA','wifi:home','secret');assert.equal(new LocalSecrets(storage).get('brokerA','gate:0'),'1234');assert.equal(vault.get('brokerB','gate:0'),'');assert.equal(vault.get('brokerA','gate:1'),'');vault.clear();assert.equal(vault.get('brokerA','wifi:home'),'');});
test('WSS only and valid topic/state',()=>{assert.throws(()=>validateConnection({url:'ws://host/mqtt',base:'x',username:'x'}));assert.throws(()=>validateConnection({url:'wss://host/mqtt',base:'x/#',username:'x'}));assert.throws(()=>parseDeviceState('{}'));assert.equal(parseDeviceState(JSON.stringify(demoState())).firmware,'Garden_U_V36.1');const r=rpcEnvelope('pwa-test','config.get',{},100,'id');assert.equal(r.expires-r.created,20000);});
test('native publishes never retained or replayed after disconnect',async()=>{const t=new GardenTransport();let published;t.config={base:'garden/test'};t.connected=true;t.received=Date.now();t.state=demoState();t.client={publish:(...a)=>published=a,removeAllListeners(){},end(){}};try{const p=t.command('light.set',{on:true});assert.deepEqual(published.slice(0,3),['garden/test/light/1/set','ON',{qos:0,retain:false}]);t.onMessage('garden/test/state',JSON.stringify({...demoState(),lights:[true]}),{retain:true});assert.equal(t.pending.size,1);t.disconnect();await assert.rejects(p,/закрито/);assert.equal(t.pending.size,0);}finally{t.destroy();}});
