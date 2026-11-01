// tfatftd LAN bot host, browser copy of the server's C# LanBot (same logic) + a local LAN hub between the iframes.
// Used by duo.html?hub: the games talk to each other and to the bot without any server.
(() => {
  const hx = (h) => Uint8Array.from(h.match(/../g).map((x) => parseInt(x, 16)));
  const MAC = [2, 0x84, 0, 0, 0, 0xfe], IP = [10, 84, 0, 254];
  const NAME = 'TOKYO BOT';
  const log = (s) => { (window.BOTLOG = window.BOTLOG || []).push(Math.round(performance.now()) + ' ' + s); };
  const T = {};
  fetch('bot2-data.json?v=' + Date.now()).then((r) => r.json()).then((d) => Object.assign(T, d));
  const now = () => performance.now();
  const u16 = (b, o) => b[o] | (b[o + 1] << 8);
  const lo = (v) => v & 255, hi = (v) => (v >> 8) & 255;
  let ipId = 0x100, outbox = null;
  function putName(b, at, room, name) { for (let i = 0; i < room; i++) b[at + i] = 0; for (let i = 0; i < name.length && i < room - 1; i++) b[at + i] = name.charCodeAt(i); }
  function rename(b) {
    for (let i = 0; i + 6 <= b.length; i++) {
      if (b[i] === 0x50 && b[i + 1] === 0x4c && b[i + 2] === 0x41 && b[i + 3] === 0x59 && b[i + 4] === 0x45 && b[i + 5] === 0x52) {
        const counted = i >= 4 && b[i - 4] === 6 && !b[i - 3] && !b[i - 2] && !b[i - 1];
        if (counted) { for (let k = 0; k < 6; k++) b[i + k] = 'SERVER'.charCodeAt(k); i += 5; }
        else { putName(b, i, 16, NAME); i += 15; }
      }
    }
  }
  function send(dstMac, dstIp, sport, dport, payload) {
    const len = 28 + payload.length, f = new Uint8Array(Math.max(60, 14 + len));
    f.set(dstMac, 0); f.set(MAC, 6); f[12] = 8; f[13] = 0;
    const id = ipId++ & 0xffff;
    f.set([0x45, 0, len >> 8, len & 255, id >> 8, id & 255, 0, 0, 0x78, 17, 0, 0, ...IP, ...dstIp], 14);
    let s = 0; for (let i = 14; i < 34; i += 2) s += (f[i] << 8) | f[i + 1]; while (s >> 16) s = (s & 0xffff) + (s >> 16); s = ~s & 0xffff; f[24] = s >> 8; f[25] = s & 255;
    const ul = 8 + payload.length; f.set([sport >> 8, sport & 255, dport >> 8, dport & 255, ul >> 8, ul & 255, 0, 0], 34); f.set(payload, 42);
    outbox(f);
  }
  const msg = (ty, body) => { const m = new Uint8Array(4 + body.length); m.set([lo(ty), hi(ty), lo(m.length), hi(m.length)]); m.set(body, 4); return m; };
  const packet = (msgs) => { const n = msgs.reduce((a, m) => a + m.length, 0); const p = new Uint8Array(6 + n); p[2] = lo(4 + n); p[3] = hi(4 + n); let o = 6; for (const m of msgs) { p.set(m, o); o += m.length; } return p; };
  const peers = new Map();
  let sessionStarted = false, startAt = 0;
  const members = () => [...peers.values()].filter((p) => p.joined).sort((a, b) => a.slot - b.slot);
  function reliable(p, ty, body) { const m = msg(ty, [lo(p.R), hi(p.R), ...body]); p.pending.push({ seq: p.R, m, sent: now() }); p.R++; return m; }
  function toPeer(p, msgs) { if (!msgs.length) return; if (p.racing) { p.extra.push(...msgs); return; } send(p.mac, p.ip, 8667, 8667, packet(msgs)); }
  function lobbyFor(nc) {
    const others = members().filter((m) => m !== nc && m.record);
    const pre = hx(T.pre); rename(pre);
    pre[126] = 1 + others.length; pre[128] = 1 + others.length; pre[151] = others.length;
    const parts = [pre, hx(T.rec)];
    for (const m of others) {
      const e = new Uint8Array(13 + m.tail.length + 1); e[1] = 0x0d; e.set(m.ip, 2); e[6] = 0xdb; e[7] = 0x21; e[10] = 1; e[11] = m.slot; e.set(m.tail, 13); e[e.length - 1] = 1;
      parts.push(e, m.record);
    }
    const n = parts.reduce((a, b) => a + b.length, 0), all = new Uint8Array(n); let o = 0; for (const x of parts) { all.set(x, o); o += x.length; }
    all[2] = lo(n); all[3] = hi(n); all[4] = (n >> 16) & 255; all[5] = 0;
    return all;
  }
  function fragments(p, data) {
    const out = [];
    for (let o = 0, idx = 0; o < data.length; o += 1200, idx++) {
      const n = Math.min(1200, data.length - o);
      const body = [idx, lo(data.length), hi(data.length), (data.length >> 16) & 255, 0, lo(n), hi(n), 0, 0, ...data.subarray(o, o + n)];
      out.push(reliable(p, 0x6f, body)); p.pending[p.pending.length - 1].sent = Infinity;
    }
    return out;
  }
  function sendFrag(p, f) { for (const q of p.pending) if (q.m === f) q.sent = now(); p.fragWait = u16(f, 4); }
  function leave(p, why) { peers.delete(p.key); if (p.joined) log(`player ${p.key} ${why}`); if (!members().length && sessionStarted) { sessionStarted = false; startAt = 0; log('lobby open again'); } }
  function startSession() {
    sessionStarted = true; log('starting session with ' + members().length);
    for (const x of members()) toPeer(x, [reliable(x, 0x4f, [0, 0, 1]), reliable(x, 0xfaf, [0x30, 0x75, 0, 0]), reliable(x, 0xfaf, [0x20, 0x75, 0, 0]), reliable(x, 0xfaf, [255, 255, 255, 255]), reliable(x, 0x4f, [0, 0, 0]), reliable(x, 0xfcb, [0, 0, 0x20, 0x41])]);
    startAt = now() + 1500;
  }
  function game(p, pl) {
    let out = [];
    for (let o = 6; o + 4 <= pl.length;) {
      const ty = u16(pl, o), ln = u16(pl, o + 2); if (ln < 4 || o + ln > pl.length) break;
      const m = pl.slice(o, o + ln); o += ln; const seq = ln >= 6 ? u16(m, 4) : 0;
      if (ty === 0x0b) {
        if (sessionStarted) continue;
        if (p.joined) { leave(p, 'rejoined'); const np = newPeer(p.key, p.mac, p.ip); peers.set(p.key, np); p = np; out = []; }
        if (members().length >= 3) continue;
        const used = new Set(members().map((x) => x.slot)); p.slot = [1, 2, 3].find((s) => !used.has(s));
        if (ln > 12) p.tail = m.slice(8, ln); const nl = Math.min(15, m[8]); let nm = ''; for (let i = 0; i < nl; i++) nm += String.fromCharCode(m[12 + i]); if (nm) p.name = nm;
        p.joined = true;
        for (const x of members()) {
          if (x === p) continue;
          const b = new Uint8Array(12 + p.tail.length); b.set(p.ip, 0); b[4] = 0xdb; b[5] = 0x21; b[8] = 1; b[9] = 1; b[10] = p.slot; b.set(p.tail, 12);
          toPeer(x, [reliable(x, 0x27, b)]);
        }
        const first = [msg(0x1e, [lo(p.S), hi(p.S), lo(seq), hi(seq)])]; p.S++;
        p.ping46 = p.S; p.last46 = now(); p.k46 = 1; first.push(msg(0x46, [lo(p.S), hi(p.S), 1])); p.S++;
        p.frags = fragments(p, lobbyFor(p));
        const f0 = p.frags.shift(); sendFrag(p, f0); first.push(f0);
        send(p.mac, p.ip, 8667, 8667, packet(first));
        log(`player ${p.key} joined slot ${p.slot}`);
        continue;
      }
      if (ty === 0x1e) {
        const acked = u16(m, 6); p.pending = p.pending.filter((q) => q.seq !== acked);
        if (acked === p.fragWait) { p.fragWait = -1; if (p.frags.length) { const nf = p.frags.shift(); sendFrag(p, nf); send(p.mac, p.ip, 8667, 8667, packet([nf])); } }
        continue;
      }
      if (ty === 0x46) { if (seq === p.ping) { p.ping = p.S; out.push(msg(0x4a, [lo(p.S), hi(p.S), m[6], 0x10, 0, 0, 0, 0, 0])); p.S++; } continue; }
      if (ty === 0x4a) { if (seq === p.ping46) { p.ping46 = p.S; p.last46 = now(); p.k46 = (m[6] + 1) & 255; out.push(msg(0x46, [lo(p.S), hi(p.S), p.k46])); p.S++; } continue; }
      if (ty === 0x42 || !p.joined) continue;
      out.push(msg(0x1e, [lo(p.S), hi(p.S), lo(seq), hi(seq)])); p.S++;
      if (ty === 0xfa3) {
        if (ln >= 8 + 1184) p.record = m.slice(8, 8 + 1184);
        if (p.ping < 0) { p.ping = p.S; out.push(msg(0x4a, [lo(p.S), hi(p.S), 1, 0x10, 0, 0, 0, 0, 0])); p.S++; }
      } else if (ty === 0x4f && ln >= 9) {
        const ready = m[8] === 1;
        if (!sessionStarted && ready !== p.ready) {
          p.ready = ready; log(`player ${p.key} ${ready ? 'ready' : 'not ready'}`);
          for (const x of members()) toPeer(x, [reliable(x, 0x4f, [p.slot, 0, ready ? 1 : 0])]);
          if (members().every((x) => x.ready)) startSession();
        }
      } else if (ty === 0x33) {
        out.push(reliable(p, 0x33, []), reliable(p, 0xfab, [0, 0, 0x60, 0x40]));
        if (!p.racing) { log(`player ${p.key} on track`); toPeer(p, out); out = []; p.racing = true; }
      } else if (ty === 0x58) { leave(p, 'left'); return; }
      else log(`player ${p.key} msg ${ty.toString(16)}`);
    }
    toPeer(p, out);
  }
  function newPeer(key, mac, ip) { return { key, mac: Array.from(mac), ip: Array.from(ip), name: 'PLAYER', tail: hx('06000000504c415945520000000000000000000000'), slot: 0, S: 1, R: 1, ping: -1, ping46: -1, k46: 0, last46: 0, lastSeen: now(), lastPose: 0, joined: false, ready: false, racing: false, record: null, frags: [], fragWait: -1, pending: [], extra: [] }; }
  function feed(f) {
    if (f.length < 42) return;
    const et = (f[12] << 8) | f[13];
    if (et === 0x806 && f[21] === 1 && f[38] === 10 && f[39] === 84 && f[40] === 0 && f[41] === 254) {
      const r = new Uint8Array(60); r.set(f.subarray(6, 12), 0); r.set(MAC, 6); r[12] = 8; r[13] = 6;
      r.set([0, 1, 8, 0, 6, 4, 0, 2], 14); r.set(MAC, 22); r.set(IP, 28); r.set(f.subarray(22, 28), 32); r.set(f.subarray(28, 32), 38);
      outbox(r); return;
    }
    if (et !== 0x800 || f[23] !== 17) return;
    const u = 14 + (f[14] & 15) * 4, dp = (f[u + 2] << 8) | f[u + 3], ul = (f[u + 4] << 8) | f[u + 5];
    const pl = f.slice(u + 8, u + ul);
    if (dp === 8666 && pl.length === 16) {
      if (sessionStarted || !T.search) return;
      const r = hx(T.search); r.set(pl.subarray(12, 16), 12); rename(r); r[24] = 1 + members().length; r.set(IP, 132);
      send([255, 255, 255, 255, 255, 255], [255, 255, 255, 255], 1215, 8666, r); return;
    }
    if (dp !== 8667 || f[30] !== 10 || f[31] !== 84 || f[32] !== 0 || f[33] !== 254) return;
    const key = Array.from(f.subarray(26, 30)).join('.');
    let p = peers.get(key); if (!p) { p = newPeer(key, f.subarray(6, 12), f.subarray(26, 30)); peers.set(key, p); }
    p.lastSeen = now();
    game(p, pl);
  }
  setInterval(() => {
    const t = now();
    for (const p of [...peers.values()]) if (t - p.lastSeen > 20000) leave(p, 'timed out');
    if (startAt && t >= startAt) {
      startAt = 0; const ms = members(); const grid = [0, 255, 255, 255]; ms.slice(0, 3).forEach((m, i) => { grid[i + 1] = m.slot; });
      for (const x of ms) toPeer(x, [reliable(x, 0x2f, [0x50, 0x14, 0, 0, ...grid])]);
    }
    for (const p of peers.values()) {
      if (p.ping46 >= 0 && t - p.last46 > 1000) {
        const l = []; p.ping46 = p.S; p.last46 = t; l.push(msg(0x46, [lo(p.S), hi(p.S), p.k46])); p.S++;
        p.ping = p.S; l.push(msg(0x4a, [lo(p.S), hi(p.S), p.k46, 0x10, 0, 0, 0, 0, 0])); p.S++; toPeer(p, l);
      }
      const due = p.pending.filter((q) => q.sent < t && t - q.sent > 700); due.forEach((q) => (q.sent = t));
      if (due.length) { if (p.racing) p.extra.push(...due.map((q) => q.m)); else send(p.mac, p.ip, 8667, 8667, packet(due.map((q) => q.m))); }
      if (p.racing && t - p.lastPose >= 110 && T.pose) {
        p.lastPose = t; const m42 = hx(T.pose); m42[4] = lo(p.S); m42[5] = hi(p.S); p.S++;
        const l = [m42, ...p.extra]; p.extra = []; send(p.mac, p.ip, 8667, 8667, packet(l));
      }
    }
  }, 50);
  // local LAN hub: every game's frames go to every other game and to the bot; bot frames go to every game
  window.hubAttach = (wins) => {
    const games = wins.map((w) => w.Module);
    const toGame = (M, f) => { const p = M._malloc(f.length); M.HEAPU8.set(f, p); M._ps2web_net_rx(p, f.length); M._free(p); };
    outbox = (f) => { for (const M of games) toGame(M, f); };
    games.forEach((M, i) => {
      const tx = M._ps2web_net_tx;
      M._ps2web_net_tx = (b, n) => { const l = tx(b, n); if (l > 0) { const f = M.HEAPU8.slice(b, b + l); setTimeout(() => { games.forEach((G, j) => { if (j !== i) toGame(G, f); }); feed(f); }, 0); } return l; };
    });
    log('hub attached to ' + games.length + ' games');
  };
  window.BOT2 = { peers, members, log };
})();
