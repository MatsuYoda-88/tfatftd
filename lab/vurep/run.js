// worker: run one build of the VU replay on vurec.bin from this site's private browser storage
var out='';
onmessage=async(e)=>{
  const {js, args}=e.data;
  self.Module={print:(t)=>{out+=t+'\n';}, printErr:(t)=>{out+='ERR '+t+'\n';}, onRuntimeInitialized: async ()=>{
    const d=await navigator.storage.getDirectory(); const f=await (await d.getFileHandle('vurec.bin')).getFile();
    Module.FS.writeFile('/vurec.bin', new Uint8Array(await f.arrayBuffer()));
    try { Module.callMain(['/vurec.bin'].concat(args)); } catch (err) { out += 'EXC ' + err + '\n'; } let f = ''; try { f = Module.FS.readFile('/out.txt', {encoding: 'utf8'}); } catch (err) {} postMessage(f + '\n' + out); }};
  importScripts(js);
};
