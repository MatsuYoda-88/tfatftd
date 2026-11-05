var out='';
var Module={print:(t)=>{out+=t+'\n';}, onRuntimeInitialized:()=>{ Module.callMain(['vusnap.bin','3000']); postMessage(out); }};
onmessage=(e)=>{ importScripts(e.data); };
