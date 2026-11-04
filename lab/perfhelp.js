// lab perf helpers (test harness only)
window.W = ms => new Promise(r => setTimeout(r, ms));
window.KD = (code) => { const o = { code, key: code, bubbles: true }; window.dispatchEvent(new KeyboardEvent('keydown', o)); document.dispatchEvent(new KeyboardEvent('keydown', o)); };
window.KU = (code) => { const o = { code, key: code, bubbles: true }; window.dispatchEvent(new KeyboardEvent('keyup', o)); document.dispatchEvent(new KeyboardEvent('keyup', o)); };
window.K = (code, ms = 120) => { KD(code); setTimeout(() => KU(code), ms); };
window.pk = (a, n) => { const p = Module._malloc(n * 4); Module._ps2web_peek(a, p, n); const f = Array.from(new Float32Array(HEAPU8.buffer.slice(p, p + n * 4))); const u = Array.from(new Uint32Array(HEAPU8.buffer.slice(p, p + n * 4))); Module._free(p); return [f, u]; };
window.ME = () => { const me = pk(0x5FAE70, 1)[1][0]; if (!me) return null; const m = pk(me + 0x10, 16)[0]; return { r: [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]], p: [m[12], m[13], m[14]] }; };
window.INWORLD = () => { try { const b = Module._malloc(64); const ok = Module._ps2web_mp_local(b); const w = new Float32Array(Module.HEAPU8.buffer, b, 16)[15]; Module._free(b); return ok === 1 && w >= 1; } catch (e) { return false; } };
window.GOCITY = async () => {
  while (!/play/i.test(document.getElementById('start').textContent)) await W(1000);
  document.getElementById('start').click(); await W(15000);
  for (let i = 0; i < 40 && !INWORLD(); i++) { const inDemo = ME && ME() && !INWORLD(); K(inDemo || i < 2 ? 'Enter' : 'KeyZ'); await W(3500); }
  window.INCITY = INWORLD(); return INCITY;
};
window.HZ = async (n = 180) => { const ts = []; await new Promise(r => { const f = t => { ts.push(t); if (ts.length < n + 1) requestAnimationFrame(f); else r(); }; requestAnimationFrame(f); }); return +(n * 1000 / (ts[n] - ts[0])).toFixed(1); };
window.PROF = async (n = 60) => {
  const f = Module._ps2web_frames(); Module._ps2web_gpu_profile(f + 20, f + 20 + n); const t0 = performance.now();
  while (Module._ps2web_frames() < f + 25 + n && performance.now() - t0 < 40000) await W(250);
  const txt = Module.FS.readFile('/gpuprof.txt', { encoding: 'utf8' }); const L = txt.trim().split('\n').map(l => l.split(' '));
  const frames = new Set(L.map(l => l[0])); const by = {}, cnt = {};
  for (const l of L) { by[l[2]] = (by[l[2]] || 0) + +l[3]; cnt[l[2]] = (cnt[l[2]] || 0) + 1; }
  const nf = frames.size; const per = Object.fromEntries(Object.entries(by).map(([k, v]) => [k, [+(v / nf / 1e6).toFixed(2), Math.round(cnt[k] / nf)]]));
  const total = Object.values(by).reduce((a, b) => a + b, 0) / nf / 1e6;
  return { nf, total_ms_per_frame: +total.toFixed(2), per };
};
window.TOP = (n = 14) => {
  const txt = Module.FS.readFile('/gpuprof.txt', { encoding: 'utf8' }); const L = txt.trim().split('\n').map(l => l.split(' '));
  const fr = L[Math.floor(L.length / 2)][0]; const one = L.filter(l => l[0] === fr);
  return one.filter(l => l[2] === 'raster').sort((a, b) => b[3] - a[3]).slice(0, n).map(l => (l[3] / 1e6).toFixed(2) + 'ms p' + l[6] + ' t' + l[7] + ' e' + l[8] + ' psm=' + (+l[11]).toString(16) + ' flags=' + (+l[12]).toString(16));
};
'perfhelp loaded';
// fair A/B: save the moment once, reload it before every run (same scene each time)
window.FPS = async (ms = 3000) => { const f0 = Module._ps2web_frames(), t0 = performance.now(); const h = await HZ(Math.round(ms * 0.06)); return [h, +((Module._ps2web_frames() - f0) * 1000 / (performance.now() - t0)).toFixed(1)]; };
window.QS = (load) => new Promise((res, rej) => { const prev = Module.onQuickState; Module.onQuickState = (l, ok) => { Module.onQuickState = prev; ok ? res() : rej(new Error('state failed')); }; if (!Module._ps2web_quick_state(load ? 1 : 0)) { Module.onQuickState = prev; rej(new Error('busy')); } });
window.BENCH = async (rounds = 2, ms = 3000, hold = null) => {
  await QS(false); const out = { on: [], off: [] };
  for (let r = 0; r < rounds; r++) for (const on of [1, 0]) {
    Module._ps2web_fast_lane(on); await QS(true); if (hold) KD(hold); await W(700);
    const f = await FPS(ms); if (hold) KU(hold); out[on ? 'on' : 'off'].push(f);
  }
  Module._ps2web_fast_lane(1); window.BENCHRES = out; return out;
};
'perfhelp loaded';
