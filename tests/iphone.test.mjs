import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile,access} from 'node:fs/promises';
import {platformInfo} from '../public/lib/platform.js';
const root=new URL('../public/',import.meta.url);
test('recognises iPhone Safari, home-screen mode and iPad desktop user agent',()=>{
 const ua='Mozilla/5.0 (iPhone; CPU iPhone OS 26_0 like Mac OS X) AppleWebKit/605.1.15 Version/26.0 Mobile/15E148 Safari/604.1';
 assert.deepEqual(platformInfo({userAgent:ua}),{ios:true,safari:true,standalone:false});
 assert.equal(platformInfo({userAgent:ua,standalone:true}).standalone,true);
 assert.equal(platformInfo({userAgent:ua,displayStandalone:true}).standalone,true);
 assert.equal(platformInfo({userAgent:ua.replace('Version/26.0','CriOS/130.0')}).safari,false);
 assert.equal(platformInfo({platform:'MacIntel',maxTouchPoints:5}).ios,true);
 assert.equal(platformInfo({platform:'MacIntel',maxTouchPoints:0}).ios,false);
 assert.equal(platformInfo({userAgent:'Android Chrome'}).ios,false);
});
test('installation page shares app identity and launches main app, not instructions',async()=>{
 const html=await readFile(new URL('install.html',root),'utf8'),manifest=JSON.parse(await readFile(new URL('manifest.webmanifest',root),'utf8'));
 assert.equal(manifest.id,'./');assert.equal(manifest.start_url,'./');assert.equal(manifest.scope,'./');assert.equal(manifest.display,'standalone');
 assert.match(html,/apple-mobile-web-app-capable/);assert.match(html,/href="manifest.webmanifest"/);assert.match(html,/Open as Web App/);
 assert.ok(!html.includes('vendor/mqtt.min.js'),'Installer must not connect to hardware');
 for(const [,asset]of html.matchAll(/(?:src|href)="([^"#]+)"/g)){if(!asset.includes('://')&&asset!=='./')await access(new URL(asset,root));}
});
