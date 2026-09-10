// Adversarial send matrix in a child process, so every framing path is covered: which path a
// send takes depends on cork x send worker x compression mode (see CLAUDE.md), and a test that
// does not vary those silently exercises one path. Sizes straddle every internal boundary:
// the 4 KB inline body, the 16 KB pool/slab classes, the 64 KB stored-block and header steps.
// Payload shapes matter too: random data EXPANDS under deflate, repetitive data shrinks.
// Verifies every message arrives byte-exact and in order, and that cWS never prints its
// FATAL frame-overflow line.
const { WebSocket, PreparedMessage } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');

const port = 3630 + (+(process.env.PORT_OFFSET || 0));
const mode = process.env.MODE || 'shared';
const pmd = mode === 'off' ? false
  : mode === 'shared' ? { threshold: 0 }
  : mode === 'takeover1' ? { serverNoContextTakeover: false, level: 1, threshold: 0 }
  : { serverNoContextTakeover: false, level: 2, threshold: 0 };   // zlib-ng per socket

const SIZES = [0, 1, 125, 126, 127, 4095, 4096, 4097, 16271, 16272, 16383, 16384, 16385, 65534, 65535, 65536, 70000, 131072];
const shapes = {
  random: (n) => crypto.randomBytes(n),                       // incompressible: deflate expands it
  repeat: (n) => Buffer.alloc(n, 0x41),                       // highly compressible
  mixed:  (n) => Buffer.concat([crypto.randomBytes(n >> 1), Buffer.alloc(n - (n >> 1), 0x42)]),
};

const expected = [];
const server = new WebSocket.Server({ port, perMessageDeflate: pmd }, () => {
  const client = new WS(`ws://localhost:${port}`, { perMessageDeflate: mode !== 'off', maxPayload: 1 << 30 });
  const got = [];
  client.on('message', (data) => {
    got.push(Buffer.from(data));
    if (got.length === expected.length) {
      let bad = -1;
      for (let i = 0; i < expected.length; i++) { if (!got[i].equals(expected[i])) { bad = i; break; } }
      console.log(JSON.stringify({ ok: bad < 0, count: got.length, firstBad: bad,
        badLen: bad >= 0 ? got[bad].length : 0, wantLen: bad >= 0 ? expected[bad].length : 0 }));
      client.close(); server.close(() => process.exit(bad < 0 ? 0 : 1));
    }
  });
  client.on('error', (e) => { console.log(JSON.stringify({ ok: false, error: e.message })); process.exit(3); });
  // a reader that stalls: forces partial writes, requeueing and unscratch
  if (process.env.SLOW === '1') {
    client.on('open', () => { const tick = () => { client.pause(); setTimeout(() => { client.resume(); setTimeout(tick, 3); }, 3); }; tick(); });
  }
});

server.on('connection', (ws) => {
  for (const [shapeName, make] of Object.entries(shapes)) {
    for (const size of SIZES) {
      const buf = make(size);
      expected.push(buf);
      ws.send(buf, { binary: true });
    }
  }
  // prepared messages with prefixes across the same boundaries, on the same socket
  const payload = crypto.randomBytes(3000);
  const prepared = new PreparedMessage(payload);
  for (const plen of [0, 100, 4096, 16271, 16272, 20000, 70000]) {
    const prefix = Buffer.alloc(plen, 0x50);
    expected.push(Buffer.concat([prefix, payload]));
    ws.send(prepared, { prefix });
  }
  // interleave empty and tiny sends, which have their own flush edge cases
  for (const b of [Buffer.alloc(0), Buffer.alloc(0), Buffer.from('x'), Buffer.alloc(0)]) {
    expected.push(b); ws.send(b, { binary: true, compress: mode !== 'off' });
  }
});
setTimeout(() => { console.log(JSON.stringify({ ok: false, timeout: true, got: expected.length })); process.exit(2); }, 60000);
