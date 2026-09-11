// The heap cap must not permanently wedge a socket. A message larger than half the ring becomes a
// heap copy; with a cap smaller than the burst, the socket parks on the cap (not on a full ring).
// If park() does not signal the drain to RESUME, that socket never wakes and delivers nothing.
// Blocks the loop so the cap is reached, then checks every message still arrives byte-exact.
const { WebSocket } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');
const port = 6600 + (+(process.env.PORT_OFFSET || 0));
const N = 16;
const exp = [];
const server = new WebSocket.Server({ port, maxPayload: 1 << 20, receiveThread: true }, () => {
  const c = new WS(`ws://localhost:${port}`, { maxPayload: 1 << 30 });
  c.on('open', () => { for (let i = 0; i < N; i++) { const b = crypto.randomBytes(300000); b.writeUInt32BE(i, 0); exp.push(b); c.send(b); } });
});
const got = []; let blocked = false;
function block(ms) { const e = Date.now() + ms; while (Date.now() < e) {} }
server.on('connection', (ws) => ws.on('message', (m) => {
  got.push(Buffer.from(new Uint8Array(m)));
  if (!blocked) { blocked = true; block(700); }   // stall so the burst piles past the cap -> cap-park
  if (got.length === N) {
    let ok = true, ordered = true;
    for (let i = 0; i < N; i++) { if (got[i].readUInt32BE(0) !== i) ordered = false; if (!got[i].equals(exp[i])) ok = false; }
    console.log(JSON.stringify({ received: got.length, ordered, exact: ok, recvStalls: server.stats.recvStalls }));
    process.exit(ok && ordered && got.length === N ? 0 : 1);
  }
}));
setTimeout(() => { console.log(JSON.stringify({ wedged: true, received: got.length, of: N })); process.exit(2); }, 12000);
