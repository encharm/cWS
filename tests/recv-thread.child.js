// Server child for the receive-thread tests that need their own ring size (env is read once
// per process): CWS_RECV_THREAD=1 CWS_RECV_RING_KB=64. Prints "READY <port>", then one JSON
// result line and exits. Modes: "slow" (a 200 ms busy loop on the first message; every
// message must still arrive, in order) and "large" (messages far larger than the ring).
const { WebSocket, recvThread } = require('../dist');
const mode = process.argv[2];
const port = mode === 'slow' ? 3481 : 3482;
const expectedConnections = +(process.argv[3] || 1);
const expectedPerConnection = +(process.argv[4] || 1);
const wss = new WebSocket.Server({ port, receiveThread: true, perMessageDeflate: mode === 'large' ? { threshold: 0 } : false }, () => console.log(`READY ${port}`));
const connections = [];
let spun = false;
let finished = 0;
function finish(extra) {
  const stats = wss.stats;
  console.log(JSON.stringify({ recvThread: recvThread(), stalls: stats.recvStalls, workerMessages: stats.recvWorkerMessages, connections: connections.map((c) => c.summary), ...extra }));
  process.exit(0);
}
wss.on('connection', (ws) => {
  const conn = { count: 0, order: true, summary: null, sizes: [] };
  connections.push(conn);
  ws.on('message', (m) => {
    if (mode === 'slow') {
      const buf = Buffer.from(m);   // the view is only valid during the handler
      if (buf.readUInt32LE(0) !== conn.count) { conn.order = false; }
      conn.count++;
      if (!spun) { spun = true; const t = Date.now(); while (Date.now() - t < 200) { /* JS thread busy: the ring fills, sockets park */ } }
    } else {
      let sum = 0;
      if (typeof m === 'string') { conn.sizes.push({ text: true, length: m.length, first: m.charCodeAt(0), last: m.charCodeAt(m.length - 1) }); }
      else { const buf = new Uint8Array(m); for (let i = 0; i < buf.length; i += 4099) { sum = (sum + buf[i]) & 0xffff; } conn.sizes.push({ length: buf.length, sum }); }
      conn.count++;
    }
    if (conn.count === expectedPerConnection) {
      conn.summary = { count: conn.count, order: conn.order, sizes: conn.sizes };
      if (++finished === expectedConnections) { finish({}); }
    }
  });
});
setTimeout(() => finish({ timeout: true }), 15000);
