// Drives the forked server (recv-heap-cap-server.js): opens a few raw WebSocket clients, tells the
// server to block, then floods 256 KB messages for the block's duration. Reports the server's RSS
// growth. Without the heap cap this is ~2 GB; with it, tens of MB.
const { fork } = require('child_process');
const WS = require('ws');
const crypto = require('crypto');
const port = 6800 + (+(process.env.PORT_OFFSET || 0));
const child = fork(`${__dirname}/recv-heap-cap-server.js`, [String(port)]);
const buf = crypto.randomBytes(262144);
child.once('message', async (m) => {
  if (m !== 'ready') return;
  const cs = [];
  for (let i = 0; i < 4; i++) {
    const c = new WS(`ws://127.0.0.1:${port}`, { maxPayload: 1 << 30 });
    await new Promise((res) => { c.on('open', res); c.on('error', res); });
    cs.push(c);
  }
  const blocked = new Promise((res) => child.once('message', res));
  child.send('block');
  const t0 = Date.now();
  while (Date.now() - t0 < 2400) { for (const c of cs) for (let i = 0; i < 200; i++) c.send(buf); await new Promise((r) => setImmediate(r)); }
  const res = await blocked;
  console.log(JSON.stringify(res));
  child.send('exit');
  setTimeout(() => process.exit(0), 300);
});
setTimeout(() => { console.log(JSON.stringify({ timeout: true })); process.exit(0); }, 15000);
