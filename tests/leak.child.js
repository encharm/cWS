// Leak hunt: run a workload in many rounds, sampling RSS and the block pool's own accounting
// between rounds, after forcing GC and letting the loop settle. A leak in native code shows up
// as RSS that never comes back down while the JS heap stays flat. Run with --expose-gc.
const { WebSocket, PreparedMessage } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');

const port = 3900 + (+(process.env.PORT_OFFSET || 0));
const kase = process.env.CASE || 'connect-churn';
const rounds = +(process.env.ROUNDS || 12);
const perRound = +(process.env.PER_ROUND || 40);
const pmd = process.env.MODE === 'off' ? false
  : process.env.MODE === 'takeover' ? { serverNoContextTakeover: false, level: +(process.env.LEVEL || 1), threshold: 0 }
  : { threshold: 0 };

const settle = () => new Promise(r => setTimeout(r, 120));
async function sample() {
  if (global.gc) { global.gc(); global.gc(); }
  await settle();
  const m = process.memoryUsage();
  const st = server ? server.stats : {};
  return { rss: m.rss, heap: m.heapUsed, ext: m.external, ab: m.arrayBuffers,
           poolBytes: st.poolFreeBytes || 0, poolBlocks: st.poolFreeBlocks || 0,
           messages: st.messages || 0 };
}

let server;
const openClient = () => new Promise((resolve, reject) => {
  const c = new WS(`ws://localhost:${port}`, { perMessageDeflate: pmd !== false, maxPayload: 1 << 30 });
  c.on('open', () => resolve(c));
  c.on('error', reject);
});

async function main() {
  server = new WebSocket.Server({ port, perMessageDeflate: pmd });
  const body = crypto.randomBytes(20000);
  const prepared = new PreparedMessage(body);

  server.on('connection', (ws) => {
    ws.on('message', () => {});
    if (kase === 'abandoned-sends') {
      // Queue a lot, then kill the socket immediately: every queued message, the in-flight
      // worker op, its scratch arena and the takeover window must be reclaimed.
      for (let i = 0; i < 40; i++) ws.send(crypto.randomBytes(100000));
      setImmediate(() => ws.terminate());
      return;
    }
    if (kase === 'callback-churn') {
      // Sends with callbacks allocate a per-message callback record; make sure they are freed
      // whether the send completes or the socket dies first.
      for (let i = 0; i < 30; i++) ws.send(body, {}, () => {});
      if (Math.random() < 0.5) setImmediate(() => ws.terminate());
      return;
    }
    if (kase === 'server-sends' || kase === 'prepared-churn' || kase === 'huge-prefix') {
      const n = kase === 'huge-prefix' ? 5 : 20;
      for (let i = 0; i < n; i++) {
        if (kase === 'prepared-churn') ws.send(prepared, { prefix: Buffer.from(`p${i}:`) });
        else if (kase === 'huge-prefix') ws.send(prepared, { prefix: Buffer.alloc(40000, 0x41) });
        else ws.send(body);
      }
    }
  });

  await settle();
  const samples = [];
  for (let r = 0; r < rounds; r++) {
    const clients = [];
    for (let i = 0; i < perRound; i++) {
      const c = await openClient();
      clients.push(c);
      if (kase === 'client-sends') { for (let k = 0; k < 20; k++) c.send(crypto.randomBytes(5000)); }
      if (kase === 'prepared-per-conn') {
        // a new PreparedMessage per connection: native handles must be released by the FinalizationRegistry
        const p = new PreparedMessage(crypto.randomBytes(10000));
        for (const conn of server.clients || []) { /* no-op: the server side sends below */ }
      }
    }
    await settle();
    // terminate half, close half: both lifetime paths
    clients.forEach((c, i) => (i % 2 ? c.terminate() : c.close()));
    await settle();
    samples.push(await sample());
  }
  // compare the tail against the middle: startup growth is expected, sustained growth is not
  // Least-squares slope over the second half, in MB per round: a plateau is ~0, a leak is a
  // steady positive slope. Reporting the slope rather than two points makes noise obvious.
  const tail = samples.slice(Math.floor(samples.length / 2));
  const n = tail.length;
  const meanX = (n - 1) / 2;
  const meanY = tail.reduce((a, s) => a + s.rss, 0) / n;
  let num = 0, den = 0;
  tail.forEach((s, i) => { num += (i - meanX) * (s.rss - meanY); den += (i - meanX) ** 2; });
  const slopeMB = den ? (num / den) / 1048576 : 0;
  const mid = samples[Math.floor(samples.length / 2)];
  const last = samples[samples.length - 1];
  const growthMB = (last.rss - mid.rss) / (1024 * 1024);
  const extMB = (last.ext - mid.ext) / (1024 * 1024);
  console.log(JSON.stringify({
    case: kase, mode: process.env.MODE || 'shared', rounds,
    rssMB: +(last.rss / 1048576).toFixed(1),
    growthMB: +growthMB.toFixed(2), slopeMBPerRound: +slopeMB.toFixed(3), extMB: +extMB.toFixed(2),
    series: samples.map(s => +(s.rss / 1048576).toFixed(1)),
    poolMB: samples.map(s => +(s.poolBytes / 1048576).toFixed(1)),
    // RSS not accounted for by the pool's parked blocks, relative to the mid sample: this is
    // what a real leak grows and allocator retention does not.
    unaccountedMB: +(((last.rss - mid.rss) - (last.poolBytes - mid.poolBytes)) / 1048576).toFixed(2),
  }));
  server.close(() => process.exit(0));
}
main().catch(e => { console.log(JSON.stringify({ error: e.message })); process.exit(3); });
setTimeout(() => { console.log(JSON.stringify({ timeout: true })); process.exit(2); }, 180000);
