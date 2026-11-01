// tfatftd LAN bot host prototype (runs in the duo page, talks to the game in iframe 0 through its network adapter)
(() => {
  const hx = (h) => Uint8Array.from(h.replace(/\s+/g, '').match(/../g).map((x) => parseInt(x, 16)));
  const toHex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, '0')).join('');
  const MAC = [0x02, 0x84, 0x00, 0x00, 0x00, 0xfe], IP = [10, 84, 0, 254];
  const log = (...a) => { (window.BOTLOG = window.BOTLOG || []).push(a.join(' ')); if (BOTLOG.length > 2000) BOTLOG.shift(); };
  const sum16 = (b, off, len, init = 0) => { let s = init; for (let i = 0; i < len; i += 2) s += (b[off + i] << 8) | (i + 1 < len ? b[off + i + 1] : 0); while (s >> 16) s = (s & 0xffff) + (s >> 16); return s; };
  function udp(dstMac, dstIp, sport, dport, payload) {
    const len = 20 + 8 + payload.length, f = new Uint8Array(Math.max(60, 14 + len));
    f.set(dstMac, 0); f.set(MAC, 6); f[12] = 8; f[13] = 0;
    f.set([0x45, 0, len >> 8, len & 255, 0, 0, 0, 0, 64, 17, 0, 0, ...IP, ...dstIp], 14);
    const ck = ~sum16(f, 14, 20) & 0xffff; f[24] = ck >> 8; f[25] = ck & 255;
    f.set([sport >> 8, sport & 255, dport >> 8, dport & 255, (8 + payload.length) >> 8, (8 + payload.length) & 255, 0, 0], 34);
    f.set(payload, 42);
    return f;
  }
  // the client we are serving
  const peers = new Map();   // ip string -> {mac, ip, seq, ...}
  function onFrame(f, send) {
    const et = (f[12] << 8) | f[13];
    if (et === 0x806 && f[21] === 1 && f[38] === IP[0] && f[39] === IP[1] && f[40] === IP[2] && f[41] === IP[3]) {   // ARP who-has us
      const r = new Uint8Array(60); r.set(f.subarray(6, 12), 0); r.set(MAC, 6); r[12] = 8; r[13] = 6;
      r.set([0, 1, 8, 0, 6, 4, 0, 2], 14); r.set(MAC, 22); r.set(IP, 28); r.set(f.subarray(22, 28), 32); r.set(f.subarray(28, 32), 38);
      log('arp reply'); send(r); return;
    }
    if (et !== 0x800 || f[23] !== 17) return;
    const u = 14 + (f[14] & 15) * 4, sp = (f[u] << 8) | f[u + 1], dp = (f[u + 2] << 8) | f[u + 3], ul = (f[u + 4] << 8) | f[u + 5];
    const pl = f.subarray(u + 8, u + ul), srcMac = f.subarray(6, 12), srcIp = f.subarray(26, 30);
    if (dp === 8666 && pl.length === 16) {    // "anyone hosting?"
      const r = hx(window.BOT_SEARCH_REPLY);
      r.set(pl.subarray(12, 16), 12);          // echo the search nonce
      r.set(IP, 132);                          // our address
      log('search from', toHex(srcIp), '-> reply');
      send(udp([255, 255, 255, 255, 255, 255], [255, 255, 255, 255], 1215, 8666, r));
      return;
    }
    if (dp === 8667) { log('game', toHex(srcIp), toHex(pl.subarray(0, 48))); if (window.BOT_GAME) window.BOT_GAME(pl, srcMac, srcIp, (p) => send(udp(srcMac, srcIp, 8667, 8667, p))); }
  }
  window.BOT = { onFrame, udp, hx, toHex, MAC, IP, log, peers };
})();
// captured search reply payload (166 B) from a real host named PLAYER
window.BOT_SEARCH_REPLY = '0000ff01000004009200000040313810050000000000100001000000504c41594552000000000000000000000000000000000000000000000000000000000000020000000000000000000000030000005014000000000000040000000900000000000000050000004d1d000004504c4159455200000000000000000000000000000004010a5400d5db21000000000000db21000000000000';
// replay host: answer a fresh join with the captured host lobby, keep the link alive, ack reliable messages,
// and start the session when the player is ready
(() => {
  const { hx, toHex, log } = BOT;
  const H1 = localStorage.getItem('botH1'), H2 = localStorage.getItem('botH2');
  let s = 3, r = 3, stage = 0, ping = -1, ping46 = -1, last46 = 0, k46 = 0, started = false, racing = 0;
  const extra = [];   // in a session, small messages ride along with our car state
  // the bot's own car in a session: parked on the start line (a real host's state message, captured)
  const POSE42 = '42007900020300009e552944cfe2c842890b10c4e32c243c73586338c164363b74fc7f3ffe7fff7ffb7f00800080ff7f3f004000006910000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000';
  const UNREL = new Set([0x1e, 0x46, 0x4a, 0x42]);
  const pending = [];     // reliable messages we sent, resent until acked: {seq, bytes, t}
  const msg = (ty, body) => { const m = new Uint8Array(4 + body.length); m.set([ty & 255, ty >> 8, m.length & 255, m.length >> 8]); m.set(body, 4); return m; };
  const u16 = (v) => [v & 255, (v >> 8) & 255];
  const packet = (msgs) => { const n = msgs.reduce((a, m) => a + m.length, 0); const p = new Uint8Array(6 + n); p.set([0, 0, ...u16(4 + n), 0, 0]); let o = 6; for (const m of msgs) { p.set(m, o); o += m.length; } return p; };
  const reliable = (ty, body) => { const m = msg(ty, [...u16(r), ...body]); pending.push({ seq: r, m, t: 0 }); r++; return m; };
  let replyTo = null;
  setInterval(() => { if (!replyTo) return; const now = performance.now(); if (ping46 >= 0 && now - last46 > 1000) { ping46 = s; last46 = now; const a = msg(0x46, [...u16(s++), k46]); ping = s; const b = msg(0x4a, [...u16(s++), k46, 0x10, 0, 0, 0, 0, 0]); if (racing) extra.push(a, b); else replyTo(packet([a, b])); } const due = pending.filter((p) => now - p.t > 700); if (due.length) { due.forEach((p) => (p.t = now)); replyTo(packet(due.map((p) => p.m))); } }, 100);
  function startSession() {
    if (started) return; started = true; log('starting session');
    replyTo(packet([reliable(0x4f, [0, 0, 1]), reliable(0xfaf, [0x30, 0x75, 0, 0]), reliable(0xfaf, [0x20, 0x75, 0, 0]), reliable(0xfaf, [0xff, 0xff, 0xff, 0xff]), reliable(0x4f, [0, 0, 0]), reliable(0xfcb, [0, 0, 0x20, 0x41])]));
    setTimeout(() => replyTo(packet([reliable(0x2f, [0x50, 0x14, 0, 0, 0, 1, 0xff, 0xff])])), 1500);
  }
  window.BOT_GAME = (pl, mac, ip, reply0) => {
    const reply = (p) => { let o = 6, t = []; while (o + 4 <= p.length) { t.push((p[o] | (p[o + 1] << 8)).toString(16)); o += p[o + 2] | (p[o + 3] << 8) || 1e9; } (window.BOTOUT = window.BOTOUT || []).push(t.join(',')); if (BOTOUT.length > 3000) BOTOUT.shift(); reply0(p); };
    replyTo = reply;
    let o = 6; const msgs = [];
    while (o + 4 <= pl.length) { const ty = pl[o] | (pl[o + 1] << 8), ln = pl[o + 2] | (pl[o + 3] << 8); if (ln < 4) break; msgs.push({ ty, m: pl.subarray(o, o + ln) }); o += ln; }
    const out = [];
    for (const { ty, m } of msgs) {
      const seq = m[4] | (m[5] << 8);
      if (ty === 0x0b && stage === 0) { stage = 1; log('join -> lobby frag1'); reply(hx(H1)); continue; }
      if (ty === 0x1e) { if (stage === 1) { stage = 2; log('ack -> lobby frag2'); reply(hx(H2)); } const acked = m[6] | (m[7] << 8); for (let i = pending.length - 1; i >= 0; i--) if (pending[i].seq === acked) pending.splice(i, 1); continue; }
      if (ty === 0x46) { if (seq === ping) { const k = m[6]; ping = s; out.push(msg(0x4a, [...u16(s++), k, 0x10, 0, 0, 0, 0, 0])); } continue; }
      if (ty === 0x4a) { if (seq === ping46) { const k = m[6]; ping46 = s; last46 = performance.now(); k46 = (k + 1) & 255; out.push(msg(0x46, [...u16(s++), k46])); } continue; }
      if (UNREL.has(ty)) continue;
      out.push(msg(0x1e, [...u16(s++), ...u16(seq)]));   // ack every reliable message
      if (ty === 0xfa3 && ping < 0) { ping = s; out.push(msg(0x4a, [...u16(s++), 1, 0x10, 0, 0, 0, 0, 0])); ping46 = s; last46 = performance.now(); k46 = 1; out.push(msg(0x46, [...u16(s++), 1])); log('car record', m.length); }
      else if (ty === 0x33) { log('player loaded'); out.push(reliable(0x33, [])); out.push(reliable(0xfab, [0, 0, 0x60, 0x40])); if (!racing) { racing = setInterval(() => { const m42 = hx(POSE42); m42[4] = s & 255; m42[5] = (s >> 8) & 255; s++; const ex = extra.splice(0); replyTo(packet([m42, ...ex])); }, 110); } }
      else if (ty === 0x4f) { log('player status', toHex(m)); if (m[6] === 1 && m[8] === 1) startSession(); }
      else log('msg', ty.toString(16), toHex(m));
    }
    if (out.length) { if (racing) extra.push(...out); else reply(packet(out)); }
  };
})();
// wire the bot to the game in iframe 0 (frames the game sends go to the bot; bot replies go into the game)
window.botAttach = (w) => {
  const M = w.Module, tx = M._ps2web_net_tx;
  const toGame = (f) => { const p = M._malloc(f.length); M.HEAPU8.set(f, p); M._ps2web_net_rx(p, f.length); M._free(p); };
  M._ps2web_net_tx = (b, n) => { const l = tx(b, n); if (l > 0) { const f = M.HEAPU8.slice(b, b + l); setTimeout(() => BOT.onFrame(f, toGame), 0); } return l; };
  BOT.log('attached');
};
