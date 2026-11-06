// lab drive test: my RX-7 snapshot, gas + a left turn, per-second split of the drawing thread's time
window.confirm = () => true;
window.LOADAS = async (n) => { const d = await navigator.storage.getDirectory(); const f = await (await d.getFileHandle(n + '.state')).getFile(); Module.FS.writeFile('/quicksave.state', new Uint8Array(await f.arrayBuffer())); await QS(true); };
window.T = async (secs) => {
  const K = [0, 1, 3, 4, 5, 6, 7], I = (k) => Module._ps2web_idle_ms(k);
  let p = K.map(I), f0 = Module._ps2web_frames(), t0 = performance.now(); const out = [];
  for (let s = 0; s < secs; s++) {
    await W(1000);
    const c = K.map(I), f = Module._ps2web_frames(), t = performance.now(), dt = t - t0, pc = (i) => +((c[i] - p[i]) / dt * 100).toFixed(0);
    const on = (document.getElementById('stats').textContent.match(/\((\d+) on screen\)/) || [])[1];
    out.push({ fps: +((f - f0) * 1000 / dt).toFixed(1), on: +on, emuIdle: pc(0), gsIdle: pc(1), busy: pc(2), yield: pc(3), show: pc(4) - pc(6), tick: pc(6), yps: Math.round((c[5] - p[5]) * 1000 / dt) });
    p = c; f0 = f; t0 = t;
  }
  return out;
};
window.RUN = async (secs = 30) => { window._out = null; await LOADAS('murray'); await W(800); KD('Digit2'); setTimeout(() => KD('ArrowLeft'), 8000); setTimeout(() => KU('ArrowLeft'), 12000); const o = await T(secs); KU('Digit2'); window._out = o; };
window.SUM = () => { const o = (window._out || []).slice(3); const avg = (k) => +(o.reduce((s, x) => s + x[k], 0) / Math.max(1, o.length)).toFixed(1); const r = {}; for (const k of ['fps', 'on', 'emuIdle', 'gsIdle', 'busy', 'yield', 'show', 'tick', 'yps']) r[k] = avg(k); r.minOn = Math.min(...o.map((x) => x.on)); return r; };
