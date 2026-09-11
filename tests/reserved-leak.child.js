// A/B leak check for the dropped-`reserved` bug. Same workload both modes; only the send path
// differs. MODE=shared uses sendShared, whose reserved carries a SharedCallback wrapper (a pinned
// Persistent<Function>); if onEnd passes nullptr instead of reserved, that wrapper leaks per
// queued frame. MODE=plain is the control (its SendCallbackData travels in callbackData and is
// always freed). The client pauses its socket so the server's frames stay queued until terminate,
// which is exactly when onEnd's cancelled-callback drain runs. --expose-gc for a clean heap read.
const { WebSocket, PreparedMessage } = require('../dist');
const WS = require('ws');
const PORT = 5200 + (+(process.env.PORT_OFFSET || 0));
const MODE = process.env.MODE || 'shared';
const CYCLES = +(process.env.CYCLES || 1200), PER = +(process.env.PER || 400);
const payload = Buffer.alloc(256, 0xab), prefix = Buffer.alloc(64, 1);
const pm = new PreparedMessage(payload);
const server = new WebSocket.Server({ port: PORT, perMessageDeflate: false });
server.on('connection', (ws) => {
  ws.on('error', () => {});
  for (let i = 0; i < PER; i++) {
    if (MODE === 'shared') ws.send(pm, { prefix, binary: true }, () => {});
    else ws.send(payload, { binary: true }, () => {});
  }
  setImmediate(() => ws.terminate());   // frames still queued behind the paused client -> onEnd drains them
});
function cycle(n, done) {
  if (n === 0) return done();
  const c = new WS(`ws://127.0.0.1:${PORT}`);
  let next = false;
  const go = () => { if (next) return; next = true; try { c.terminate(); } catch (e) {} setImmediate(() => cycle(n - 1, done)); };
  c.on('open', () => { c._socket.pause(); setTimeout(go, 8); });
  c.on('close', go); c.on('error', go);
}
setTimeout(() => cycle(CYCLES, () => setTimeout(() => {
  for (let i = 0; i < 6; i++) if (global.gc) global.gc();
  console.log(JSON.stringify({ mode: MODE, heapMB: +(process.memoryUsage().heapUsed / 1048576).toFixed(1) }));
  process.exit(0);
}, 400)), 150);
setTimeout(() => { console.log(JSON.stringify({ timeout: true })); process.exit(3); }, 100000);
