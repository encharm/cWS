// Sends an incompressible payload on a socket whose per-message-deflate tier is configured with
// a non-default memLevel/windowBits, on the JS-thread framing path (CWS_SEND_THREAD=0). cWS
// aborts with a FATAL line if estimate under-sized the frame buffer; a clean delivery is the pass.
const { WebSocket } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');
const port = 5000 + (+(process.env.PORT_OFFSET || 0));
const size = +(process.env.SIZE || 262144);
const payload = crypto.randomBytes(size);
const server = new WebSocket.Server({ port, perMessageDeflate: {
  serverNoContextTakeover: false, level: 6, memLevel: +(process.env.MEMLEVEL||8), windowBits: +(process.env.WBITS||15), threshold: 0 } }, () => {
  const c = new WS(`ws://localhost:${port}`, { perMessageDeflate: true, maxPayload: 1 << 30 });
  c.on('message', (m) => { const ok = Buffer.from(m).equals(payload); console.log(JSON.stringify({ ok, length: m.length })); c.close(); server.close(() => process.exit(ok?0:1)); });
  c.on('error', () => process.exit(3));
});
server.on('connection', ws => ws.send(payload));
setTimeout(() => { console.log(JSON.stringify({ ok:false, timeout:true })); process.exit(2); }, 10000);
