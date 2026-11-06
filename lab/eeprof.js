// lab: sample which game function the EE thread is in (PS2_FN marker, PS2_EE_PROF builds only)
// usage in the page: await EEPROF.start(); ...drive...; const top = await EEPROF.stop(40);
(() => {
  const src = `
    let u32, idx, counts = new Map(), on = false, n = 0;
    const nap = new Int32Array(new SharedArrayBuffer(4));
    onmessage = (e) => {
      if (e.data.cmd === 'start') { u32 = new Uint32Array(e.data.buf); idx = e.data.addr >>> 2; counts = new Map(); n = 0; on = true; loop(); }
      else if (e.data.cmd === 'stop') { on = false; postMessage({ n, counts: Array.from(counts.entries()) }); }
    };
    function loop() {
      const t0 = performance.now();
      while (on && performance.now() - t0 < 200) {
        const f = Atomics.load(u32, idx);
        counts.set(f, (counts.get(f) || 0) + 1); n++;
        Atomics.wait(nap, 0, 0, 0.2);
      }
      if (on) setTimeout(loop, 0);
    }`;
  let w = null;
  window.EEPROF = {
    start() {
      w = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
      w.postMessage({ cmd: 'start', buf: Module.HEAPU8.buffer, addr: Module._ps2web_ee_fn_addr() });
    },
    stop(top = 40) {
      return new Promise((res) => {
        w.onmessage = (e) => {
          const { n, counts } = e.data; w.terminate();
          counts.sort((a, b) => b[1] - a[1]);
          res({ n, top: counts.slice(0, top).map(([f, c]) => [f === 1 ? 'vsync-wait' : f.toString(16).padStart(8, '0'), +(100 * c / n).toFixed(2)]) });
        };
        w.postMessage({ cmd: 'stop' });
      });
    },
  };
})();
