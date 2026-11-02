export function platformInfo({userAgent='',platform='',maxTouchPoints=0,standalone=false,displayStandalone=false}={}){
 const ios=/iPhone|iPad|iPod/.test(userAgent)||(platform==='MacIntel'&&maxTouchPoints>1);
 const safari=ios&&/Safari/.test(userAgent)&&!/CriOS|FxiOS|EdgiOS|OPiOS|GSA|FBAN|FBAV|Instagram/.test(userAgent);
 return {ios,safari,standalone:standalone===true||displayStandalone===true};
}
export function currentPlatform(){return platformInfo({userAgent:navigator.userAgent,platform:navigator.platform,maxTouchPoints:navigator.maxTouchPoints,standalone:navigator.standalone,displayStandalone:matchMedia('(display-mode: standalone)').matches});}
