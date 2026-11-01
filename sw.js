const CACHE='staromiska-v22-7852a66d5839';
const ASSETS=["./","./app.js","./apple-touch-icon.png","./favicon-32.png","./icon-192.png","./icon-512.png","./icons/mark.svg","./index.html","./lib/demo.js","./lib/protocol.js","./lib/secrets.js","./lib/transport.js","./manifest.webmanifest","./style.css","./upgrade.css","./vendor/MQTT-LICENSE.md","./vendor/mqtt.min.js","./vendor/version.json"];
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
