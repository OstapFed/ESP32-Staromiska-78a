import {validateConnection} from './protocol.js';
export const CONNECTION_KEY='staromiska78a.connection.v1';
export function readConnection(storage){
 const saved=JSON.parse(storage.getItem(CONNECTION_KEY)||'null');if(!saved)return null;
 if(saved.version!==1)throw Error('Невідома версія налаштувань MQTT');
 return validateConnection({url:saved.broker,username:saved.username,password:saved.password,base:saved.base||'garden/controller',remember:true});
}
export function writeConnection(storage,config){storage.setItem(CONNECTION_KEY,JSON.stringify({version:1,broker:config.url,username:config.username,password:config.password,base:config.base}));}
