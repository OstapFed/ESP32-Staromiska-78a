import test from 'node:test';
import assert from 'node:assert/strict';
import {ActionLocks,actionResource} from '../public/lib/actions.js';
import {readConnection,writeConnection,CONNECTION_KEY} from '../public/lib/connection.js';
import {GardenTransport} from '../public/lib/transport.js';
import {nativeCommand} from '../public/lib/protocol.js';
import {demoState} from '../public/lib/demo.js';
test('two gate commands can wait independently; repeat on same gate is blocked',()=>{
 const locks=new ActionLocks(),a=actionResource('gate.command',{gate:0}),b=actionResource('gate.command',{gate:1});
 assert.equal(locks.acquire(a),true);assert.equal(locks.acquire(b),true);assert.equal(locks.acquire(a),false);
 locks.release(a);assert.equal(locks.has(b),true);assert.equal(locks.busy,true);locks.release(b);assert.equal(locks.busy,false);
});
test('both gate commands publish before either confirms, and confirm independently',async()=>{
 const t=new GardenTransport(),sent=[];t.config={base:'test'};t.connected=true;t.received=Date.now();t.state=demoState();
 t.client={publish:(...args)=>sent.push(args),removeAllListeners(){},end(){}};
 try{const first=t.command('gate.command',{gate:0,command:'open'}),second=t.command('gate.command',{gate:1,command:'open'});assert.equal(sent.length,2);assert.equal(sent[0][0],'test/gate/1/set');assert.equal(sent[1][0],'test/gate/2/set');assert.equal(t.pending.size,2);
 const s=demoState();s.gates[0]={...s.gates[0],last_command:1,relay_active:true};t.onMessage('test/state',JSON.stringify(s),{retain:false});await first;assert.equal(t.pending.size,1);
 s.gates[1]={...s.gates[1],last_command:1,relay_active:true};t.onMessage('test/state',JSON.stringify(s),{retain:false});await second;assert.equal(t.pending.size,0);
 }finally{t.destroy();}
});
test('both water-all modes use ESP-native execution, enforce capability and rain lock',()=>{
 const s={...demoState(),mqtt_water_all_supported:true};
 for(const action of ['all','alltogether']){const cmd=nativeCommand('water.all',{action},s);assert.equal(cmd.suffix,`/water/${action}/set`);assert.equal(cmd.payload,'START');assert.equal(cmd.matches({...s,watering:true,watering_all:true,watering_simultaneous:action==='alltogether'}),true);assert.equal(cmd.matches({...s,watering:true,watering_all:true,watering_simultaneous:action!=='alltogether'}),false);}
 assert.throws(()=>nativeCommand('water.all',{action:'all'},demoState()),/V36.2/);
 assert.throws(()=>nativeCommand('water.all',{action:'all'},{...s,rain_blocked:true}),/дощу/);
});
test('saved MQTT parameters restore without another password prompt, including V21 schema',()=>{
 const map=new Map(),storage={getItem:k=>map.get(k),setItem:(k,v)=>map.set(k,v)};
 assert.equal(readConnection(storage),null);storage.setItem(CONNECTION_KEY,JSON.stringify({version:1,broker:'wss://broker.example/mqtt',username:'user',password:'test-password'}));
 const old=readConnection(storage);assert.equal(old.password,'test-password');assert.equal(old.base,'garden/controller');assert.equal(old.remember,true);
 writeConnection(storage,{...old,base:'garden/new'});assert.equal(readConnection(storage).base,'garden/new');assert.equal(readConnection(storage).password,'test-password');
});
