// Child for the read-scope test: a handler throws on the first message of a burst; the throw must
// reach process 'uncaughtException' and the remaining messages of the same read must still be delivered.
const { WebSocket } = require('../dist'); const net = require('net'); const crypto = require('crypto');
const port = 3480; const seen = []; let uncaught = 0;
process.on('uncaughtException', (e) => { uncaught++; });
const wss = new WebSocket.Server({ port }, () => {
  const s = net.connect(port, '127.0.0.1', () => s.write(`GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  let up = false; s.on('data', () => { if (up) return; up = true;
    const frame = (t) => { const p = Buffer.from(t), m = Buffer.from([1, 2, 3, 4]); return Buffer.concat([Buffer.from([0x81, 0x80 | p.length]), m, Buffer.from(p.map((b, i) => b ^ m[i & 3]))]); };
    s.write(Buffer.concat([frame('one'), frame('two'), frame('three')]));   // one TCP write -> one read -> three messages
  });
});
wss.on('connection', ws => ws.on('message', m => { seen.push(m); if (m === 'one') throw new Error('boom'); if (seen.length === 3) { setTimeout(() => { console.log(JSON.stringify({ seen, uncaught })); process.exit(0); }, 50); } }));
setTimeout(() => { console.log(JSON.stringify({ seen, uncaught, timeout: true })); process.exit(1); }, 3000);
