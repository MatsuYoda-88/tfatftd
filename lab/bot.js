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
