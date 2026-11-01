import {cp,mkdir,readdir,readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const root=fileURLToPath(new URL('../',import.meta.url)),src=path.join(root,'public'),out=path.join(root,'dist');
async function walk(dir,prefix=''){const files=[];for(const e of await readdir(dir,{withFileTypes:true})){const name=prefix+e.name;if(e.isDirectory())files.push(...await walk(path.join(dir,e.name),name+'/'));else files.push(name);}return files;}
const files=(await walk(src)).sort();
const manifest=JSON.parse(await readFile(path.join(src,'manifest.webmanifest'),'utf8'));
for(const icon of manifest.icons)if(!files.includes(icon.src))throw Error('Missing icon '+icon.src);
const html=await readFile(path.join(src,'index.html'),'utf8'),ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);
if(ids.length!==new Set(ids).size)throw Error('Duplicate HTML IDs');
const hash=createHash('sha256');for(const name of files){hash.update(name);hash.update(await readFile(path.join(src,name)));}
const revision=hash.digest('hex').slice(0,12);
await mkdir(out,{recursive:true});await cp(src,out,{recursive:true});
const precache=['./',...files.map(f=>'./'+f)];
const sw=`const CACHE='staromiska-v22-${revision}';
const ASSETS=${JSON.stringify(precache)};
self.addEventListener('install',event=>event.waitUntil(caches.open(CACHE).then(cache=>cache.addAll(ASSETS))));
self.addEventListener('activate',event=>event.waitUntil(caches.keys().then(keys=>Promise.all(keys.filter(k=>k.startsWith('staromiska-')&&k!==CACHE).map(k=>caches.delete(k)))).then(()=>self.clients.claim())));
self.addEventListener('message',event=>{if(event.data?.type==='SKIP_WAITING')self.skipWaiting();});
self.addEventListener('fetch',event=>{
 const url=new URL(event.request.url);if(event.request.method!=='GET'||url.origin!==self.location.origin)return;
 const key=url.pathname.slice(new URL('./',self.location).pathname.length);
 if(event.request.mode==='navigate'){event.respondWith(fetch(event.request).catch(()=>caches.match('./index.html')));return;}
 if(!ASSETS.includes('./'+key))return;
 event.respondWith(caches.open(CACHE).then(cache=>cache.match('./'+key).then(cached=>cached||fetch(event.request))));
});
`;
await writeFile(path.join(out,'sw.js'),sw);await writeFile(path.join(out,'.nojekyll'),'');
console.log('Built V22: '+files.length+' assets, revision '+revision);
