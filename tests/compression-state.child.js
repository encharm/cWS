// Adversarial compression state: a context-takeover window is stateful, so the server's
// deflate history and the client's inflate history must stay in step through every mix of
// compressed, uncompressed, prepared (spliced, independently compressed) and empty messages.
// A desync here is silent on the server and shows up as garbage or a dropped connection on the
// client, so this asserts on the DECODED bytes for a long interleaved sequence.
const { WebSocket, PreparedMessage } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');
const port = 3800 + (+(process.env.PORT_OFFSET || 0));
const level = +(process.env.LEVEL || 1);
const rounds = +(process.env.ROUNDS || 60);

const expected = [];
const server = new WebSocket.Server({ port,
  perMessageDeflate: { serverNoContextTakeover: false, level, threshold: +(process.env.THRESHOLD || 0) } }, () => {
  const client = new WS(`ws://localhost:${port}`, { perMessageDeflate: true, maxPayload: 1 << 30 });
  const got = [];
  client.on('message', (d) => {
    got.push(Buffer.from(d));
    if (got.length === expected.length) {
      let bad = -1;
      for (let i = 0; i < expected.length; i++) if (!got[i].equals(expected[i])) { bad = i; break; }
      console.log(JSON.stringify({ ok: bad < 0, count: got.length, firstBad: bad }));
      client.close(); server.close(() => process.exit(bad < 0 ? 0 : 1));
    }
  });
  client.on('error', (e) => { console.log(JSON.stringify({ ok: false, error: e.message, got: expected.length })); process.exit(3); });
  client.on('close', (code) => { if (got.length < expected.length) { console.log(JSON.stringify({ ok: false, closedEarly: code, got: got.length, want: expected.length })); process.exit(4); } });
});

server.on('connection', (ws) => {
  // The same repeated body is what makes takeover pay off, so it is also what exposes a desync.
  const body = Buffer.from(JSON.stringify({ items: Array.from({ length: 30 }, (_, i) => ({ id: 'jam' + i, users: i })) }));
  const prepared = new PreparedMessage(body);
  let seq = 0;
  const tagged = (n) => { const b = Buffer.alloc(n); b.write(`msg${seq++}:`); crypto.randomFillSync(b.subarray(20)); return b; };
  for (let r = 0; r < rounds; r++) {
    // compressed
    let m = Buffer.concat([Buffer.from(`c${r}:`), body]); expected.push(m); ws.send(m, { compress: true });
    // explicitly uncompressed on a compressed connection (RSV1 clear mid-stream)
    m = Buffer.from(`u${r}:` + 'z'.repeat(200)); expected.push(m); ws.send(m, { compress: false });
    // prepared: independently compressed blob spliced behind a per-connection prefix, with the
    // history-sync entry keeping the window in step
    const prefix = Buffer.from(`p${r}:`);
    expected.push(Buffer.concat([prefix, body])); ws.send(prepared, { prefix });
    // empty and tiny, which have their own flush behaviour
    expected.push(Buffer.alloc(0)); ws.send(Buffer.alloc(0), { compress: true });
    // incompressible, which expands
    m = tagged(3000); expected.push(m); ws.send(m, { compress: true });
  }
});
setTimeout(() => { console.log(JSON.stringify({ ok: false, timeout: true })); process.exit(2); }, 45000);
