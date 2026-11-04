// tfatftd.xyz Handling Editor: live tuning of every handling value the game keeps in memory.
// Loads /handling/catalog.json (built from the physics reports), finds each part of the player's car in guest RAM,
// and lets you read and write its fields while driving. Toggle: H key or the "Handling" button.
(() => {
  'use strict';
  const CATALOG_URL = '/handling/catalog.json';
  const LS_PRESETS = 'td-handling-presets', LS_OPEN = 'td-handling-open', LS_LIVE = 'td-handling-live';
  const $ = (s, el = document) => el.querySelector(s);
  const h = (tag, attrs = {}, ...kids) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') e.className = v; else if (k === 'text') e.textContent = v;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v); else if (v !== undefined && v !== null) e.setAttribute(k, v);
    }
    for (const k of kids) if (k !== null && k !== undefined) e.append(k);
    return e;
  };
  const getLS = (k, d) => { try { const v = localStorage.getItem(k); return v === null ? d : v; } catch (e) { return d; } };
  const setLS = (k, v) => { try { localStorage.setItem(k, v); } catch (e) {} };

  // ---------------- guest memory ----------------
  let scratch = 0;
  const ready = () => typeof Module !== 'undefined' && Module.running && Module._ps2web_peek && Module._ps2web_poke;
  function u32(addr) {
    if (!addr || addr >= 0x2000000) return 0;
    if (!scratch) scratch = Module._malloc(64);
    Module._ps2web_peek(addr >>> 0, scratch, 1);
    return new Uint32Array(Module.HEAPU8.buffer, scratch, 1)[0] >>> 0;
  }
  const f32buf = new Float32Array(1), u32view = new Uint32Array(f32buf.buffer);
  const asF = (u) => { u32view[0] = u >>> 0; return f32buf[0]; };
  const asU = (f) => { f32buf[0] = f; return u32view[0] >>> 0; };
  const okPtr = (p) => p && p < 0x2000000 && (p & 3) === 0;
  // a guest function call, run on the game thread (cruise only)
  async function call(fn, ...args) {
    if (!Module._ps2web_mp_call) throw new Error('this build has no call()');
    const a = Module._malloc(32), done = Module._malloc(4);
    new Uint32Array(Module.HEAPU8.buffer, a, 8).set(args.map((x) => x >>> 0));
    Module._ps2web_mp_call_result(done);
    const before = new Int32Array(Module.HEAPU8.buffer, done, 1)[0];
    Module._ps2web_mp_call(fn >>> 0, a, args.length);
    try {
      for (let i = 0; i < 200; i++) {
        await new Promise((r) => setTimeout(r, 20));
        const r = Module._ps2web_mp_call_result(done);
        if (new Int32Array(Module.HEAPU8.buffer, done, 1)[0] !== before) return r >>> 0;
      }
      throw new Error('the game did not run the call (only works while driving)');
    } finally { Module._free(a); Module._free(done); }
  }

  // ---------------- value types ----------------
  // read(raw u32) -> number shown; write(number) -> raw u32
  const TYPES = {
    f32: { r: (u) => asF(u), w: (v) => asU(v) },
    f32_sqrt: { r: (u) => { const s = asF(u); return s * s; }, w: (v) => asU(Math.sqrt(Math.max(0, v))) },
    f32_rad_as_deg: { r: (u) => asF(u) * 180 / Math.PI, w: (v) => asU(v * Math.PI / 180) },
    f32_rpm_from_radps: { r: (u) => asF(u) * 60 / (2 * Math.PI), w: (v) => asU(v * 2 * Math.PI / 60) },
    f32_neg: { r: (u) => -asF(u), w: (v) => asU(-v) },
    i32: { r: (u) => (u | 0), w: (v) => (Math.round(v) | 0) >>> 0 },
    u8: { r: (u) => u & 0xff, w: (v, old) => ((old & ~0xff) | (Math.round(v) & 0xff)) >>> 0, byte: true },
    bool32: { r: (u) => (u ? 1 : 0), w: (v) => (v ? 1 : 0), bool: true },
  };
  const typeOf = (p) => {
    const t = TYPES[p.type];
    if (t) return t;
    // f32 scaled by a constant: {"type":"f32_scale","scale":k} shows raw*k
    if (p.type === 'f32_scale') return { r: (u) => asF(u) * (p.scale || 1), w: (v) => asU(v / (p.scale || 1)) };
    return TYPES.f32;
  };

  // ---------------- locators ----------------
  let catalog = null;
  function compList() {
    // every vehicle component of the player's car; recipe from catalog.locators.components
    const L = catalog && catalog.locators && catalog.locators.components;
    const car = u32(0x5FAE70);
    if (!L || !car) return [];
    const out = [];
    try {
      if (L.kind === 'chassis-list') {        // {kind, base:"chassis", count:off, array:off, inline:bool}
        const base = resolve(L.base || 'chassis');
        if (!okPtr(base)) return [];
        const n = u32(base + parseInt(L.count, 16));
        const arr = L.inline ? base + parseInt(L.array, 16) : u32(base + parseInt(L.array, 16));
        for (let i = 0; i < Math.min(n, 256); i++) out.push(u32(arr + 4 * i));
      } else if ((L.kind === 'global-list' || L.kind === 'chassis-linked-list') && L.obj) {
        // circular list with a sentinel node: list = [head]; node = [list+next]; while node != list: obj = [node+obj]
        const headAddr = L.kind === 'global-list' ? parseInt(L.head, 16) : resolve(L.base || 'chassis') + parseInt(L.head, 16);
        const list = L.headIsPtr ? u32(headAddr) : headAddr, nx = parseInt(L.next, 16), ob = parseInt(L.obj, 16);
        let node = u32(list + nx), guard = 0;
        while (okPtr(node) && node !== list && guard++ < 8192) {
          const c = u32(node + ob);
          if (okPtr(c) && (!L.owner || L.kind !== 'global-list' || u32(c + parseInt(L.owner, 16)) === car)) out.push(c);
          node = u32(node + nx);
        }
      } else if (L.kind === 'global-list') {  // {kind, head:abs, next:off, owner:off} singly linked, or {count, array}
        if (L.array) {
          const n = u32(parseInt(L.count, 16)), arr = L.arrayIsPtr ? u32(parseInt(L.array, 16)) : parseInt(L.array, 16);
          for (let i = 0; i < Math.min(n, 4096); i++) { const c = u32(arr + 4 * i); if (c && (!L.owner || u32(c + parseInt(L.owner, 16)) === car)) out.push(c); }
        } else {
          let c = u32(parseInt(L.head, 16)), guard = 0;
          while (okPtr(c) && guard++ < 4096) { if (!L.owner || u32(c + parseInt(L.owner, 16)) === car) out.push(c); c = u32(c + parseInt(L.next, 16)); }
        }
      }
    } catch (e) {}
    return out.filter(okPtr);
  }
  let compCache = { car: 0, at: 0, list: [] };
  function components() {
    const car = u32(0x5FAE70), now = performance.now();
    if (compCache.car !== car || now - compCache.at > 2000) compCache = { car, at: now, list: compList() };
    return compCache.list;
  }
  // locator grammar: car | engine | clutch | gearbox | chassis | axle0 | wheel01 | tyre10 | tyredata11 | diff1 | kin0 |
  // comp:<vtable hex>[#n] | abs:<hex> | <name>+<hexoff> (pointer at that offset) | any key of catalog.locators.named
  function resolve(loc) {
    if (!loc) return 0;
    const car = u32(0x5FAE70);
    if (loc.startsWith('abs:')) return parseInt(loc.slice(4), 16);
    if (!okPtr(car)) return 0;
    const named = catalog && catalog.locators && catalog.locators.named;
    if (named && named[loc] && typeof named[loc] === 'string' && named[loc] !== loc) return resolve(named[loc]);
    if (loc.includes('->')) {   // a->+0x10  : pointer chain
      const [b, ...rest] = loc.split('->');
      let p = resolve(b.trim());
      for (const r of rest) { if (!okPtr(p)) return 0; p = u32(p + parseInt(r.trim().replace('+', ''), 16)); }
      return p;
    }
    let m;
    if (loc === 'car' || loc === 'mover') return car;
    if (loc === 'engine') return u32(car + 0x94c);
    if (loc === 'clutch') return u32(car + 0x948);
    if (loc === 'gearbox') return u32(car + 0x950);
    if (loc === 'chassis') return u32(car + 0x944);
    if (loc === 'actor') return u32(car + 0xf0);
    if ((m = /^axle(\d)$/.exec(loc))) return u32(car + 0x914 + 4 * +m[1]);
    if ((m = /^wheel(\d)(\d)$/.exec(loc))) { const a = resolve('axle' + m[1]); return okPtr(a) ? u32(a + 0x74 + 4 * +m[2]) : 0; }
    if ((m = /^tyre(\d)(\d)$/.exec(loc))) { const w = resolve('wheel' + m[1] + m[2]); return okPtr(w) ? u32(w + 0x70) : 0; }
    if ((m = /^tyredata(\d)(\d)$/.exec(loc))) { const t = resolve('tyre' + m[1] + m[2]); return okPtr(t) ? u32(t + 0x104) : 0; }
    if ((m = /^diff(\d)$/.exec(loc))) { const a = resolve('axle' + m[1]); return okPtr(a) ? u32(a + 0x11c) : 0; }
    if ((m = /^kin(\d)$/.exec(loc))) { const a = resolve('axle' + m[1]); return okPtr(a) ? u32(a + 0x118) : 0; }
    if ((m = /^comp:([0-9a-fx]+)(?:\[?#(\d+)\]?)?$/i.exec(loc))) {
      const vt = parseInt(m[1], 16), nth = +(m[2] || 0), mt = resolve.match;
      let k = 0;
      for (const c of components()) {
        if (u32(c + 8) !== vt) continue;
        if (mt) { if (u32(c + parseInt(mt.off, 16)) === (mt.value >>> 0)) return c; continue; }
        if (k++ === nth) return c;
      }
      return 0;
    }
    return 0;
  }
  // a param may repeat per wheel / per axle: expand into instances {key, loc, tag}
  function instances(p) {
    if (p.perWheel) {
      const list = Array.isArray(p.perWheel) ? p.perWheel : ['00', '01', '10', '11'];
      return list.map((aw) => ({ key: p.id + '@' + aw, loc: p.loc.replace(/\{a\}/g, aw[0]).replace(/\{w\}/g, aw[1]), tag: wheelName(aw) }));
    }
    if (p.perAxle) {
      const list = Array.isArray(p.perAxle) ? p.perAxle : ['0', '1'];
      return list.map((a) => ({ key: p.id + '@' + a, loc: p.loc.replace(/\{a\}/g, a), tag: axleName(a) }));
    }
    return [{ key: p.id, loc: p.loc, tag: '' }];
  }
  const axle0Front = () => { const f = catalog && catalog.locators && catalog.locators.axle0IsFront; return f !== false; };
  const axleName = (a) => ((a === '0') === axle0Front() ? 'Front' : 'Rear');
  const wheelName = (aw) => axleName(aw[0]) + ' ' + (aw[1] === '0' ? 'L' : 'R');
  function addrOf(p, inst) {
    resolve.match = p.match || null;
    const base = resolve(inst.loc);
    resolve.match = null;
    if (!okPtr(base) && !inst.loc.startsWith('abs:')) return 0;
    if (p.vt && u32(base + 8) !== parseInt(p.vt, 16)) return 0;   // object is not the class we expect
    return (base + parseInt(p.off || '0', 16)) >>> 0;
  }
  const readRaw = (addr) => u32(addr & ~3);
  function readVal(p, addr) {
    const t = typeOf(p); let raw = readRaw(addr);
    if (t.byte) raw = (raw >>> (8 * (addr & 3))) & 0xff;
    return t.r(raw);
  }
  function writeVal(p, addr, v) {
    const t = typeOf(p);
    if (t.byte) {
      const sh = 8 * (addr & 3), old = readRaw(addr);
      Module._ps2web_poke(addr & ~3, ((old & ~(0xff << sh)) | ((Math.round(v) & 0xff) << sh)) >>> 0);
    } else Module._ps2web_poke(addr, t.w(v, readRaw(addr)));
  }

  // ---------------- state ----------------
  const stock = new Map();   // carPtr:key -> stock value (first value seen on that car)
  let carNow = 0;
  const rows = [];           // {p, inst, el, input, range, valEl}
  const stockKey = (inst) => carNow.toString(16) + ':' + inst.key;

  // ---------------- UI ----------------
  const css = `
  #hx{position:fixed;top:0;right:0;bottom:0;width:min(430px,100vw);z-index:60;background:var(--hx-bg);color:var(--hx-fg);
    font:13px/1.4 system-ui,Segoe UI,sans-serif;display:flex;flex-direction:column;box-shadow:-8px 0 30px rgba(0,0,0,.45);border-left:1px solid var(--hx-line)}
  #hx{--hx-bg:#12151b;--hx-fg:#e8ecf2;--hx-dim:#8d97a8;--hx-line:#283040;--hx-acc:#ff7a1a;--hx-card:#191e27;--hx-in:#0d1016}
  #hx[hidden]{display:none}
  #hx header{padding:10px 12px;border-bottom:1px solid var(--hx-line);display:flex;gap:8px;align-items:center;flex-wrap:wrap}
  #hx header b{font-size:15px;letter-spacing:.02em;margin-right:auto}
  #hx button{background:#232a36;color:var(--hx-fg);border:1px solid var(--hx-line);border-radius:6px;padding:4px 9px;font:inherit;cursor:pointer}
  #hx button:hover{border-color:var(--hx-acc)} #hx button.acc{background:var(--hx-acc);color:#000;border-color:var(--hx-acc);font-weight:600}
  #hx .tools{padding:8px 12px;border-bottom:1px solid var(--hx-line);display:flex;gap:6px;flex-wrap:wrap;align-items:center}
  #hx input[type=search],#hx select{background:var(--hx-in);color:var(--hx-fg);border:1px solid var(--hx-line);border-radius:6px;padding:5px 8px;font:inherit}
  #hx input[type=search]{flex:1;min-width:140px}
  #hx .status{padding:6px 12px;color:var(--hx-dim);font-size:12px;border-bottom:1px solid var(--hx-line)}
  #hx .status.bad{color:#ff9b6b}
  #hx .list{overflow:auto;flex:1;padding:4px 0 40px}
  #hx details{border-bottom:1px solid var(--hx-line)} #hx summary{padding:9px 12px;cursor:pointer;font-weight:600;list-style:none;display:flex;justify-content:space-between}
  #hx summary::-webkit-details-marker{display:none} #hx summary small{color:var(--hx-dim);font-weight:400}
  #hx .row{margin:0 8px 8px;padding:8px 10px;background:var(--hx-card);border:1px solid var(--hx-line);border-radius:8px}
  #hx .row.changed{border-color:var(--hx-acc)} #hx .row.dead{opacity:.45}
  #hx .top{display:flex;align-items:baseline;gap:6px} #hx .lbl{font-weight:600;flex:1} #hx .tag{color:var(--hx-dim);font-size:11px}
  #hx .ctl{display:flex;gap:8px;align-items:center;margin-top:6px}
  #hx input[type=range]{flex:1;accent-color:var(--hx-acc);min-width:0}
  #hx input[type=number]{width:96px;background:var(--hx-in);color:var(--hx-fg);border:1px solid var(--hx-line);border-radius:5px;padding:3px 5px;font:12px ui-monospace,Consolas,monospace}
  #hx .unit{color:var(--hx-dim);font-size:11px;min-width:34px}
  #hx .desc{color:var(--hx-dim);font-size:12px;margin-top:6px}
  #hx .meta{display:flex;gap:10px;flex-wrap:wrap;margin-top:5px;font-size:11px;color:var(--hx-dim)}
  #hx .meta .apply{color:#9ad1ff} #hx .meta .risk{color:#ff9b6b}
  #hx .sw{position:relative;width:38px;height:20px;flex:none} #hx .sw input{opacity:0;width:0;height:0}
  #hx .sw i{position:absolute;inset:0;background:#39414f;border-radius:20px;transition:.15s} #hx .sw i:before{content:"";position:absolute;width:16px;height:16px;left:2px;top:2px;background:#fff;border-radius:50%;transition:.15s}
  #hx .sw input:checked+i{background:var(--hx-acc)} #hx .sw input:checked+i:before{transform:translateX(18px)}
  #hx table.tb{width:100%;border-collapse:collapse;margin-top:6px;font:12px ui-monospace,Consolas,monospace}
  #hx table.tb td{padding:2px 3px} #hx table.tb input{width:78px}
  #hx canvas.curve{width:100%;height:110px;background:var(--hx-in);border-radius:6px;margin-top:6px}
  #hx-btn{margin-left:4px}
  #hx .goals{display:flex;gap:4px;padding:8px 12px 0;flex-wrap:wrap} #hx .goals button{border-radius:14px;padding:3px 11px}
  #hx .goals button.on{background:var(--hx-acc);color:#000;border-color:var(--hx-acc);font-weight:600}
  #hx .ginfo{padding:6px 12px 0;color:var(--hx-dim);font-size:12px}
  #hx .tip{margin-top:6px;padding:6px 8px;border-radius:6px;background:#2a2116;color:#ffd7b0;font-size:12px}
  #hx .ends{display:flex;justify-content:space-between;gap:8px;font-size:11px;color:#ffd7b0;margin:2px 132px 0 0} #hx .ends span:last-child{text-align:right}
  #hx .tip b{color:#ffb070} #hx .best{font-size:11px;font-weight:700;color:#000;background:#ffb070;border-radius:4px;padding:0 5px;margin-left:4px}
  @media (max-width:600px){#hx{width:100vw}}`;
  let root = null, listEl = null, statusEl = null, searchEl = null, onlyChanged = false, goal = getLS('td-handling-goal', '');
  const GOALS = [['', 'All parts'], ['grip', 'Grip'], ['drift', 'Drift'], ['highway', 'Highway'], ['drag', 'Drag'], ['launch', 'Launch'], ['stability', 'Stability']];
  let goalBar = null, goalInfo = null;

  function build() {
    if (root) return;
    document.head.append(h('style', { text: css }));
    statusEl = h('div', { class: 'status', text: 'Loading the handling catalog…' });
    searchEl = h('input', { type: 'search', placeholder: 'Search (spring, grip, lock, nos…)', oninput: filter });
    listEl = h('div', { class: 'list' });
    const presetSel = h('select', { title: 'Saved presets' });
    const refreshPresets = () => {
      const all = presets(); presetSel.replaceChildren(h('option', { value: '', text: 'Presets…' }), ...Object.keys(all).sort().map((n) => h('option', { value: n, text: n })));
    };
    refreshPresets();
    presetSel.addEventListener('change', () => { const n = presetSel.value; if (n) applyPreset(presets()[n], n); presetSel.value = ''; });
    root = h('aside', { id: 'hx', hidden: '' },
      h('header', {}, h('b', { text: 'Handling Editor' }),
        h('button', { text: 'Reset all', title: 'Put every value back to this car\'s stock', onclick: resetAll }),
        h('button', { text: '✕', title: 'Close (H)', onclick: () => toggle(false) })),
      (goalBar = h('div', { class: 'goals' }, ...GOALS.map(([g, n]) => h('button', { 'data-g': g, text: n, title: g ? 'Settings that matter for ' + n.toLowerCase() : 'Every setting, by car part', onclick: () => { goal = g; setLS('td-handling-goal', g); render(); } })))),
      (goalInfo = h('div', { class: 'ginfo' })),
      h('div', { class: 'tools' }, searchEl,
        h('label', { title: 'Show only values you changed' }, h('input', { type: 'checkbox', onchange: (e) => { onlyChanged = e.target.checked; filter(); } }), ' changed'),
        presetSel,
        h('button', { text: 'Save', title: 'Save current changes as a preset', onclick: () => { const n = prompt('Preset name'); if (n) { savePreset(n); refreshPresets(); } } }),
        h('button', { text: 'Export', title: 'Download your changes as a file', onclick: exportFile }),
        h('button', { text: 'Import', title: 'Load a changes file', onclick: importFile })),
      statusEl, listEl);
    document.body.append(root);
    const bar = document.getElementById('bar'), anchor = document.getElementById('game-settings');
    if (bar) {
      const b = h('button', { id: 'hx-btn', type: 'button', text: 'Handling', title: 'Handling Editor (H)', onclick: () => toggle() });
      if (anchor) anchor.before(b); else bar.append(b);
    }
  }
  function toggle(on) {
    build();
    const show = on === undefined ? root.hidden : on;
    root.hidden = !show; setLS(LS_OPEN, show ? '1' : '0');
    if (show && !catalog) load();
  }
  window.addEventListener('keydown', (e) => {
    const t = e.target, typing = t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable);
    if (e.code === 'KeyH' && !e.repeat && !typing && !e.ctrlKey && !e.altKey && !e.metaKey) { toggle(); e.preventDefault(); }
    if (root && !root.hidden && root.contains(t)) e.stopPropagation();   // typing in the panel never drives the car
  }, true);

  async function load() {
    try {
      const r = await fetch(CATALOG_URL, { cache: 'no-cache' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      catalog = await r.json();
      render();
    } catch (e) { statusEl.textContent = 'Could not load the catalog: ' + e.message; statusEl.classList.add('bad'); }
  }
  function fmt(v, p) {
    if (!isFinite(v)) return String(v);
    const st = p.step || 0.01, d = Math.max(0, Math.min(6, Math.ceil(-Math.log10(st))));
    return Number(v).toFixed(d);
  }
  function render() {
    listEl.replaceChildren(); rows.length = 0;
    for (const b of goalBar.querySelectorAll('button')) b.classList.toggle('on', b.dataset.g === goal);
    const gd = catalog.goal_descriptions || {};
    goalInfo.textContent = goal ? (gd[goal] || '') + ' Only the settings that matter for this are shown, with what turning them up or down does.' : 'Every setting, grouped by car part. Pick a goal above to see just what matters for it.';
    const groups = new Map();
    for (const p of catalog.params || []) { if (goal && !(p.goals && p.goals[goal])) continue; if (!groups.has(p.group)) groups.set(p.group, []); groups.get(p.group).push(p); }
    for (const [g, ps] of groups) {
      const body = h('div');
      const det = h('details', {}, h('summary', {}, h('span', { text: g }), h('small', { text: ps.length + ' settings' })), body);
      for (const p of ps) for (const inst of instances(p)) body.append(rowFor(p, inst));
      listEl.append(det);
    }
    tables.length = 0;
    for (const t of catalog.tables || []) { if (goal && !(t.goals && t.goals[goal])) continue; for (const inst of instances(t)) { const r = tableRow(t, inst); if (r) listEl.append(r); } }
    if (!goal && (catalog.not_tunable || []).length) {
      const body = h('div');
      for (const n of catalog.not_tunable) body.append(h('div', { class: 'row dead' }, h('div', { class: 'top' }, h('span', { class: 'lbl', text: n.label || n.id || n.name })), h('div', { class: 'desc', text: n.reason || n.desc || '' })));
      listEl.append(h('details', {}, h('summary', {}, h('span', { text: 'Not changeable (baked into code)' }), h('small', { text: catalog.not_tunable.length })), body));
    }
    filter();
  }
  function rowFor(p, inst) {
    const isBool = typeOf(p).bool || p.type === 'bool32';
    const valEl = h('input', { type: 'number', step: p.step ?? 'any', min: p.min, max: p.max });
    const range = isBool ? null : h('input', { type: 'range', min: p.min ?? 0, max: p.max ?? 1, step: p.step ?? 'any' });
    const chk = isBool ? h('input', { type: 'checkbox' }) : null;
    const stockEl = h('span', {});
    const row = h('div', { class: 'row' },
      h('div', { class: 'top' }, h('span', { class: 'lbl', text: p.label || p.id }), h('span', { class: 'tag', text: inst.tag })),
      h('div', { class: 'ctl' }, isBool ? h('label', { class: 'sw' }, chk, h('i')) : range, isBool ? null : valEl, h('span', { class: 'unit', text: p.unit || '' }),
        h('button', { text: '↺', title: 'Back to stock', onclick: () => { const s = stock.get(stockKey(inst)); if (s !== undefined) set(r, s); } })),
      goal && p.goals && p.goals[goal] ? h('div', { class: 'tip' }, h('b', { text: (GOALS.find((x) => x[0] === goal) || [])[1] + ': ' }), p.goals[goal], p.goals_best && p.goals_best[goal] ? h('span', { class: 'best', text: { up: '▲ usually up', down: '▼ usually down', balance: '◆ balance' }[p.goals_best[goal]] || p.goals_best[goal] }) : null) : null,
      endsFor(p) ? h('div', { class: 'ends' }, h('span', { text: '◀ ' + endsFor(p).low }), h('span', { text: endsFor(p).high + ' ▶' })) : null,
      p.desc ? h('div', { class: 'desc', text: p.desc }) : null,
      h('div', { class: 'meta' }, stockEl, p.apply && p.apply !== 'live' ? h('span', { class: 'apply', text: '⟳ ' + p.apply }) : h('span', { class: 'apply', text: 'live' }), p.risk ? h('span', { class: 'risk', text: '⚠ ' + p.risk }) : null));
    const r = { p, inst, row, valEl, range, chk, stockEl, editing: 0, search: ((p.label || '') + ' ' + p.id + ' ' + (p.desc || '') + ' ' + p.group + ' ' + inst.tag + ' ' + Object.values(p.goals || {}).join(' ')).toLowerCase() };
    const commit = (v) => set(r, v);
    if (range) { range.addEventListener('input', () => { r.editing = performance.now(); valEl.value = range.value; commit(+range.value); }); }
    if (valEl) { valEl.addEventListener('change', () => { r.editing = performance.now(); commit(+valEl.value); }); valEl.addEventListener('focus', () => { r.editing = performance.now() + 1e9; }); valEl.addEventListener('blur', () => { r.editing = performance.now(); }); }
    if (chk) chk.addEventListener('change', () => commit(chk.checked ? 1 : 0));
    rows.push(r);
    return row;
  }
  // what each end of a slider does: the catalog's own labels, else built from which goals each direction helps
  function endsFor(p) {
    if (p.ends && (p.ends.low || p.ends.high)) return { low: p.ends.low || '', high: p.ends.high || '' };
    const best = p.goals_best || {}, name = Object.fromEntries(GOALS);
    const up = [], down = [];
    for (const [g, d] of Object.entries(best)) { if (d === 'up') up.push(name[g] || g); else if (d === 'down') down.push(name[g] || g); }
    if (!up.length && !down.length) return null;
    return { low: down.length ? 'More ' + down.join(' / ').toLowerCase() : 'Less', high: up.length ? 'More ' + up.join(' / ').toLowerCase() : 'More' };
  }
  async function set(r, v) {
    if (!ready()) return;
    const addr = addrOf(r.p, r.inst);
    if (!addr) { status('That part of the car is not loaded right now (are you driving?)', true); return; }
    if (r.p.min !== undefined && v < r.p.min) v = r.p.min;
    if (r.p.max !== undefined && v > r.p.max) v = r.p.max;
    const sk = stockKey(r.inst);
    if (!stock.has(sk)) stock.set(sk, readVal(r.p, addr));
    const keep = r.p.after && r.p.after.length && !r.quiet ? changes() : null;
    writeVal(r.p, addr, v);
    try { await afterWrite(r.p, r.inst); } catch (e) { status(r.p.label + ': ' + e.message, true); }
    if (keep) {   // some follow-up calls re-run the car's setup and undo other edits: put them back
      for (const o of rows) {
        if (o === r || keep[o.inst.key] === undefined || (!o.valEl && !o.chk)) continue;
        const a2 = addrOf(o.p, o.inst); if (a2 && Math.abs(readVal(o.p, a2) - keep[o.inst.key]) > 1e-6) writeVal(o.p, a2, keep[o.inst.key]);
      }
      for (const t of tables) for (const c of t.cells) if (keep[c.key] !== undefined) writeVal({ type: c.c.type || 'f32' }, c.addr, keep[c.key]);
    }
    refreshRow(r, true);
    remember();
  }
  // follow-up work some fields need (catalog "after": [{call:"0x273480", args:["car","axle{a}"]} or {write:{loc,off,type,from:"sqrt"}}])
  async function afterWrite(p, inst) {
    const steps = (p.after || []).slice();
    // tyre formula numbers: the game caches B/C/D/E per tyre (re-done only when the load moves 50 N): clear every tyre's cache
    if (p.id === 'tbl_pacejka' || /^ty_/.test(p.id || '')) for (const aw of ['00', '01', '10', '11']) steps.unshift({ poke: 'tyre' + aw, off: '0xe8', value: 0 });
    for (const step of steps) {
      if (step.poke) { const b = resolve(step.poke); if (okPtr(b)) Module._ps2web_poke(b + parseInt(step.off, 16), step.value >>> 0); continue; }
      if (step.call) {
        const sub = (s) => (typeof s === 'number' ? s : resolve(String(s).replace(/\{a\}/g, inst.key.split('@')[1]?.[0] ?? '0').replace(/\{w\}/g, inst.key.split('@')[1]?.[1] ?? '0')));
        await call(parseInt(step.call, 16), ...(step.args || []).map(sub));
      }
    }
  }
  function refreshRow(r, force) {
    if (!ready()) return;
    const addr = addrOf(r.p, r.inst);
    r.row.classList.toggle('dead', !addr);
    if (!addr) { r.stockEl.textContent = 'not on this car'; return; }
    const v = readVal(r.p, addr), sk = stockKey(r.inst);
    if (!stock.has(sk)) stock.set(sk, v);
    const s = stock.get(sk);
    r.stockEl.textContent = 'stock ' + fmt(s, r.p);
    r.row.classList.toggle('changed', Math.abs(v - s) > Math.max(1e-6, Math.abs(s) * 1e-6));
    if (!force && performance.now() - r.editing < 600) return;
    if (r.chk) r.chk.checked = !!v;
    if (r.range) {
      // widen the slider if the game's value is outside the editor range
      if (r.p.min === undefined || v < +r.range.min) r.range.min = Math.min(v, +r.range.min || 0);
      if (r.p.max === undefined || v > +r.range.max) r.range.max = Math.max(v, +r.range.max || 1);
      r.range.value = v;
    }
    if (r.valEl && document.activeElement !== r.valEl) r.valEl.value = fmt(v, r.p);
  }
  function filter() {
    const q = (searchEl && searchEl.value || '').trim().toLowerCase();
    for (const r of rows) {
      const hit = !q || q.split(/\s+/).every((w) => r.search.includes(w));
      r.row.hidden = !hit || (onlyChanged && !r.row.classList.contains('changed'));
    }
    for (const d of listEl.querySelectorAll('details')) {
      const any = [...d.querySelectorAll('.row')].some((x) => !x.hidden);
      d.hidden = !any; if (q && any) d.open = true;
    }
  }
  function status(t, bad) { if (statusEl) { statusEl.textContent = t; statusEl.classList.toggle('bad', !!bad); } }

  // ---------------- tables (torque curve, gear ratios, Pacejka block...) ----------------
  // catalog table: {id, group, label, desc, kind:"pairs"|"floats", loc, off(ptr field) | inline:true, countOff, count, stride, cols:[{label,off,type}], after}
  function tableRow(t, inst) {
    const box = h('div');
    const row = h('div', { class: 'row' }, h('div', { class: 'top' }, h('span', { class: 'lbl', text: t.label || t.id }), h('span', { class: 'tag', text: t.group || '' })),
      goal && t.goals && t.goals[goal] ? h('div', { class: 'tip' }, h('b', { text: (GOALS.find((x) => x[0] === goal) || [])[1] + ': ' }), t.goals[goal]) : null,
      t.desc ? h('div', { class: 'desc', text: t.desc }) : null, box,
      t.apply && t.apply !== 'live' ? h('div', { class: 'meta' }, h('span', { class: 'apply', text: '⟳ ' + t.apply })) : null);
    const det = h('details', {}, h('summary', {}, h('span', { text: (t.group ? t.group + ' · ' : '') + (t.label || t.id) + (inst.tag ? ' · ' + inst.tag : '') }), h('small', { text: 'table' })), row);
    const state = { t, inst, box, row, cells: [], canvas: null, search: ((t.label || '') + ' ' + t.id + ' ' + (t.desc || '') + ' ' + (t.group || '')).toLowerCase() };
    det.addEventListener('toggle', () => { if (det.open) drawTable(state); });
    tables.push(state);
    rows.push({ row: det, search: state.search + ' ' + inst.tag.toLowerCase(), p: { id: t.id }, inst: { key: inst.key }, refresh: () => det.open && drawTable(state, true) });
    return det;
  }
  const tables = [];
  function tableBase(t, loc) {
    const obj = resolve(loc);
    if (!okPtr(obj) && !String(loc).startsWith('abs:')) return null;
    if (t.vt && u32(obj + 8) !== parseInt(t.vt, 16)) return null;
    let data = t.inline ? obj + parseInt(t.off || '0', 16) : u32(obj + parseInt(t.off || '0', 16));
    const count = t.countOff !== undefined ? u32(obj + parseInt(t.countOff, 16)) : (t.count || 0);
    if (!okPtr(data) || count <= 0 || count > 512) return null;
    return { data, count, stride: t.stride || (t.cols ? 4 * t.cols.length : 4) };
  }
  function drawTable(s, soft) {
    if (!ready()) { s.box.textContent = 'Start driving to see this table.'; return; }
    const b = tableBase(s.t, s.inst.loc);
    if (!b) { s.box.textContent = 'Not loaded on this car right now.'; return; }
    const cols = s.t.cols || [{ label: 'value', off: '0', type: 'f32' }];
    if (!soft || s.cells.length !== b.count * cols.length) {
      s.cells = [];
      const tb = h('table', { class: 'tb' });
      tb.append(h('tr', {}, h('td', { text: '#' }), ...cols.map((c) => h('td', { text: c.label }))));
      for (let i = 0; i < b.count; i++) {
        const tr = h('tr', {}, h('td', { text: (s.t.rowLabels && s.t.rowLabels[i]) || (s.t.id === 'tbl_gears' ? 'Gear ' + (i + 1) : String(i + 1)) }));
        for (const c of cols) {
          const inp = h('input', { type: 'number', step: c.step ?? 'any' });
          const addr = b.data + i * b.stride + parseInt(c.off || '0', 16);
          const key = s.inst.key + '#' + i + '.' + c.label;
          inp.addEventListener('change', async () => {
            const sk = carNow.toString(16) + ':' + key, cp = { type: c.type || 'f32' };
            if (!stock.has(sk)) stock.set(sk, readVal(cp, addr));
            writeVal(cp, addr, +inp.value);
            try { await afterWrite(s.t, s.inst); } catch (e) { status(e.message, true); }
            remember(); drawTable(s, true);
          });
          s.cells.push({ inp, addr, c, key });
          tr.append(h('td', {}, inp));
        }
        tb.append(tr);
      }
      s.canvas = s.t.kind === 'pairs' ? h('canvas', { class: 'curve', width: 400, height: 110 }) : null;
      s.box.replaceChildren(...(s.canvas ? [s.canvas] : []), tb);
    }
    for (const cell of s.cells) if (document.activeElement !== cell.inp) cell.inp.value = fmt(readVal({ type: cell.c.type || 'f32' }, cell.addr), { step: cell.c.step || 0.01 });
    if (s.canvas) {   // x = column 0, y = column 1
      const g = s.canvas.getContext('2d'), W = s.canvas.width, H = s.canvas.height, pts = [];
      for (let i = 0; i < b.count; i++) pts.push([+s.cells[i * cols.length].inp.value, +s.cells[i * cols.length + 1].inp.value]);
      g.clearRect(0, 0, W, H);
      const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
      const x0 = Math.min(...xs), x1 = Math.max(...xs), y1 = Math.max(...ys, 1);
      g.strokeStyle = '#ff7a1a'; g.lineWidth = 2; g.beginPath();
      pts.forEach((p, i) => { const x = 8 + (p[0] - x0) / Math.max(1e-6, x1 - x0) * (W - 16), y = H - 8 - p[1] / y1 * (H - 16); i ? g.lineTo(x, y) : g.moveTo(x, y); });
      g.stroke(); g.fillStyle = '#8d97a8'; g.font = '11px system-ui'; g.fillText(Math.round(y1) + ' max', 8, 12);
    }
  }

  // ---------------- presets / remember ----------------
  function changes() {
    const out = {};
    for (const r of rows) {
      if (!r.valEl && !r.chk) continue;
      const addr = addrOf(r.p, r.inst); if (!addr) continue;
      const v = readVal(r.p, addr), s = stock.get(stockKey(r.inst));
      if (s !== undefined && Math.abs(v - s) > Math.max(1e-6, Math.abs(s) * 1e-6)) out[r.inst.key] = v;
    }
    for (const s of tables) for (const c of s.cells) {
      const sk = carNow.toString(16) + ':' + c.key, st = stock.get(sk);
      if (st === undefined) continue;
      const v = readVal({ type: c.c.type || 'f32' }, c.addr);
      if (Math.abs(v - st) > 1e-6) out[c.key] = v;
    }
    return out;
  }
  const presets = () => { try { return JSON.parse(getLS(LS_PRESETS, '{}')) || {}; } catch (e) { return {}; } };
  function savePreset(name) { const all = presets(); all[name] = changes(); setLS(LS_PRESETS, JSON.stringify(all)); status('Saved preset "' + name + '" (' + Object.keys(all[name]).length + ' changes)'); }
  async function applyPreset(ch, name) {
    if (!ch) return;
    let n = 0;
    for (const r of rows) if (ch[r.inst.key] !== undefined && (r.valEl || r.chk)) { await set(r, ch[r.inst.key]); n++; }
    status('Loaded ' + (name ? '"' + name + '" ' : '') + '(' + n + ' values)');
  }
  function remember() { setLS(LS_LIVE, JSON.stringify(changes())); }
  async function resetAll() {
    let n = 0;
    for (const r of rows) {
      if (!r.valEl && !r.chk) continue;
      const s = stock.get(stockKey(r.inst)), addr = addrOf(r.p, r.inst);
      if (s === undefined || !addr) continue;
      if (Math.abs(readVal(r.p, addr) - s) > 1e-6) { await set(r, s); n++; }
    }
    for (const t of tables) for (const c of t.cells) { const st = stock.get(carNow.toString(16) + ':' + c.key); if (st !== undefined) writeVal({ type: c.c.type || 'f32' }, c.addr, st); }
    setLS(LS_LIVE, '{}'); status('Back to stock (' + n + ' values)');
  }
  function exportFile() {
    const blob = new Blob([JSON.stringify({ tfatftdHandling: 1, changes: changes() }, null, 2)], { type: 'application/json' });
    const a = h('a', { href: URL.createObjectURL(blob), download: 'tokyo-drift-handling.json' }); document.body.append(a); a.click(); a.remove();
  }
  function importFile() {
    const inp = h('input', { type: 'file', accept: '.json,application/json' });
    inp.addEventListener('change', async () => {
      try { const j = JSON.parse(await inp.files[0].text()); await applyPreset(j.changes || j, inp.files[0].name); } catch (e) { status('That file is not a handling file', true); }
    });
    inp.click();
  }

  // ---------------- tick ----------------
  let lastTick = 0, lastCarKey = '';
  function tick(t) {
    requestAnimationFrame(tick);
    if (!root || root.hidden || !catalog || t - lastTick < 400) return;
    lastTick = t;
    if (!ready()) { status('Waiting for the game…'); return; }
    const car = u32(0x5FAE70);
    if (!okPtr(car) || u32(car + 8) !== 0x00521480) { status('Get into a car (free roam or a race) to tune it.'); carNow = 0; return; }
    if (car !== carNow) {
      carNow = car;
      // a new car (or a reload): put back the values you had on, if you asked for that
      const live = (() => { try { return JSON.parse(getLS(LS_LIVE, '{}')); } catch (e) { return {}; } })();
      if (Object.keys(live).length && getLS('td-handling-keep', '1') === '1') setTimeout(() => applyPreset(live, 'your last changes'), 300);
    }
    const n = rows.filter((r) => r.row.classList.contains('changed')).length;
    status('Tuning your car' + (n ? ' · ' + n + ' changed' : '') + ' · H to hide');
    for (const r of rows) { if (r.refresh) r.refresh(); else if (!r.row.hidden) refreshRow(r); }
    if (onlyChanged) filter();
  }
  requestAnimationFrame(tick);

  window.TDHandling = { toggle, resolve, call, catalog: () => catalog, changes, reload: () => { catalog = null; load(); } };
  if (getLS(LS_OPEN, '0') === '1') setTimeout(() => toggle(true), 1500);
})();
