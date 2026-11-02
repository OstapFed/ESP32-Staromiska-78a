import {currentPlatform} from './lib/platform.js';
const platform=currentPlatform(),note=document.getElementById('platformNote'),root=new URL('./',location.href),installURL=new URL('install.html',root);
if(platform.standalone){note.hidden=false;note.textContent='Додаток уже відкритий в окремому вікні. Натисніть «Відкрити додаток», щоб перейти до керування.';document.getElementById('showSteps').textContent='Перейти до керування';document.getElementById('showSteps').href=root.href;}
else if(platform.ios&&!platform.safari){note.hidden=false;note.textContent='Для встановлення відкрийте це посилання у Safari. Скопіювати його можна нижче.';}
else if(!platform.ios){note.hidden=false;note.textContent='Відкрийте це посилання на iPhone у Safari. Кнопка нижче скопіює адресу встановлення.';}
document.getElementById('showSteps').addEventListener('click',()=>{if(!platform.standalone)document.getElementById('steps').focus({preventScroll:true});});
document.getElementById('copyLink').addEventListener('click',async()=>{
 try{if(!navigator.clipboard)throw Error();await navigator.clipboard.writeText(installURL.href);document.getElementById('copyStatus').textContent='Посилання скопійовано. Відкрийте його в Safari на iPhone.';}
 catch{document.getElementById('manualCopy').hidden=false;const input=document.getElementById('installAddress');input.value=installURL.href;input.focus();input.select();document.getElementById('copyStatus').textContent='Натисніть і утримуйте адресу, щоб скопіювати її.';}
});
if('serviceWorker'in navigator)navigator.serviceWorker.register('./sw.js',{scope:'./'}).catch(()=>{});
