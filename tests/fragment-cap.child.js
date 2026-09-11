// A message split into never-finished continuation frames (each under maxPayload) must not grow
// the reassembly buffer without bound: the server force-closes once the accumulated size exceeds
// maxPayload. Uses a raw socket to craft FIN=0 fragments. RECV=1 exercises the receive worker.
const { WebSocket } = require('../dist');
const net = require('net'), crypto = require('crypto');
const port = 6900 + (+(process.env.PORT_OFFSET || 0));
const recv = process.env.RECV === '1';
const maxPayload = 1 << 20;
const server = new WebSocket.Server({ port, maxPayload, receiveThread: recv }, () => {
  const s = net.connect(port, '127.0.0.1', () => {
    const key = crypto.randomBytes(16).toString('base64');
    s.write(`GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
  });
  let up = false, closed = false, sent = 0;
  const before = process.memoryUsage().rss;
  const chunk = Array.from(crypto.randomBytes(60000));
  function frame(opcode, fin, payload) {
    const mask = crypto.randomBytes(4), masked = Buffer.from(payload.map((b, i) => b ^ mask[i & 3]));
    const h = Buffer.alloc(4); h[0] = (fin ? 0x80 : 0) | opcode; h[1] = 0x80 | 126; h.writeUInt16BE(payload.length, 2);
    return Buffer.concat([h, mask, masked]);
  }
  s.on('data', () => {
    if (up) return; up = true;
    s.write(frame(0x2, false, chunk)); sent += 60000;
    const iv = setInterval(() => {
      if (closed) { clearInterval(iv); return; }
      s.write(frame(0x0, false, chunk)); sent += 60000;
      if (sent > 30 * 1024 * 1024) clearInterval(iv);   // if never closed, cap the attempt
    }, 2);
  });
  const report = () => { const after = process.memoryUsage().rss;
    console.log(JSON.stringify({ closedByServer: closed, sentMB: (sent / 1048576) | 0, rssGrowthMB: ((after - before) / 1048576) | 0 })); process.exit(0); };
  s.on('close', () => { closed = true; setTimeout(report, 100); });
  s.on('error', () => {});
  setTimeout(report, 6000);
});
server.on('connection', (ws) => { ws.on('message', () => {}); ws.on('error', () => {}); });
setTimeout(() => { console.log(JSON.stringify({ timeout: true })); process.exit(2); }, 12000);
