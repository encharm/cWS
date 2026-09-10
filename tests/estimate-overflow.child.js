// Child for the estimate() overflow test: sends an incompressible >64 KB message with the
// frame built on the JS thread (CWS_SEND_THREAD=0 or CWS_CORK=0), which is where
// WebSocketTransformer::estimate sizes the buffer. cWS aborts with a FATAL line if the
// framed message does not fit what estimate promised.
const { WebSocket } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');
const port = 3621;
const payload = crypto.randomBytes(+(process.env.SIZE || 65530));
const server = new WebSocket.Server({ port, perMessageDeflate: { threshold: 0 } }, () => {
  const c = new WS(`ws://localhost:${port}`, { perMessageDeflate: true, maxPayload: 1 << 30 });
  c.on('message', (m) => {
    const ok = Buffer.from(m).equals(payload);
    console.log(JSON.stringify({ ok, length: m.length }));
    c.close(); server.close(() => process.exit(ok ? 0 : 1));
  });
  c.on('error', () => process.exit(3));
});
server.on('connection', ws => ws.send(payload));
setTimeout(() => { console.log(JSON.stringify({ ok: false, timeout: true })); process.exit(2); }, 10000);
