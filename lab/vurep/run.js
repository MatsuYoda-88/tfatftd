// worker: run one build of the VU replay on vurec.bin from this site's private browser storage
var out='';
onmessage=async(e)=>{
  const {js, args}=e.data;
  self.Module={print:(t)=>{out+=t+'\n';}, printErr:(t)=>{out+='ERR '+t+'\n';}, onRuntimeInitialized: async ()=>{
    const dir=await navigator.storage.getDirectory(); const file=await (await dir.getFileHandle('vurec.bin')).getFile();
    Module.FS.writeFile('/vurec.bin', new Uint8Array(await file.arrayBuffer()));
    try { Module.callMain(['/vurec.bin'].concat(args)); } catch (err) { out += 'EXC ' + err + '\n'; }
    let res = ''; try { res = Module.FS.readFile('/out.txt', {encoding: 'utf8'}); } catch (err) {}
    postMessage(res + '\n' + out); }};
  importScripts(js);
};
