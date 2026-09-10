// Targeted leak hunt on refcounted / handle-owning objects, using pool-sized messages so the
// block pool accounts for them, and reporting per-round deltas that a leak makes monotonic.
const { WebSocket, PreparedMessage } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');
const port = 3950 + (+(process.env.PORT_OFFSET || 0));
const kase = process.env.CASE, rounds = +(process.env.ROUNDS || 100);
const pmd = process.env.MODE === 'takeover' ? { serverNoContextTakeover: false, level: 1, threshold: 0 } : { threshold: 0 };
const settle = (ms = 60) => new Promise(r => setTimeout(r, ms));

let server;
async function main() {
  server = new WebSocket.Server({ port, perMessageDeflate: pmd });
  server.on('connection', (ws) => {
    ws.on('message', () => {});
    const small = crypto.randomBytes(2000);   // pool-sized: freed blocks return to the freelists
    switch (kase) {
      case 'prepared-per-message':
        // a fresh PreparedMessage per send: each holds a native SharedPayload released via
        // FinalizationRegistry; if a reference is dropped the natives pile up
        for (let i = 0; i < 30; i++) ws.send(new PreparedMessage(small), { prefix: Buffer.from('p:') });
        break;
      case 'prepared-abandoned':
        for (let i = 0; i < 30; i++) ws.send(new PreparedMessage(small), { prefix: Buffer.from('p:') });
        setImmediate(() => ws.terminate());   // payload refs must drop even though the socket died
        break;
      case 'window-churn':
        for (let i = 0; i < 20; i++) ws.send(small, { compress: true });
        setImmediate(() => ws.terminate());   // per-socket deflate window must be freed
        break;
      case 'callback-abandoned':
        for (let i = 0; i < 30; i++) ws.send(small, {}, () => {});
        setImmediate(() => ws.terminate());   // pending callback records must be released
        break;
      case 'idle':
        break;                                  // control: connections only, no sends
      case 'ping-churn':
        for (let i = 0; i < 50; i++) ws.ping(Buffer.alloc(100, 1));
        break;
    }
  });
  await settle(200);
  const series = [];
  for (let r = 0; r < rounds; r++) {
    const cs = [];
    for (let i = 0; i < 10; i++) {
      const c = new WS(`ws://localhost:${port}`, { perMessageDeflate: true });
      await new Promise((res, rej) => { c.on('open', res); c.on('error', rej); });
      cs.push(c);
    }
    await settle();
    cs.forEach((c, i) => (i % 2 ? c.terminate() : c.close()));
    await settle();
    if (global.gc) { global.gc(); global.gc(); }
    await settle();
    const m = process.memoryUsage(), st = server.stats;
    series.push({ rss: m.rss, ext: m.external, pool: st.poolFreeBytes, msgs: st.messages });
  }
  const tail = series.slice(Math.floor(series.length / 2));
  const n = tail.length, meanX = (n - 1) / 2;
  const meanY = tail.reduce((a, s) => a + (s.rss - s.pool), 0) / n;
  let num = 0, den = 0;
  tail.forEach((s, i) => { num += (i - meanX) * ((s.rss - s.pool) - meanY); den += (i - meanX) ** 2; });
  console.log(JSON.stringify({
    case: kase, mode: process.env.MODE || 'shared', rounds, messages: series[series.length - 1].msgs,
    rssMB: +(series[series.length - 1].rss / 1048576).toFixed(1),
    // slope of RSS minus pool-parked bytes: what a genuine leak grows
    netSlopeKBPerRound: +((den ? num / den : 0) / 1024).toFixed(1),
    extKB: +((series[series.length - 1].ext - series[Math.floor(series.length / 2)].ext) / 1024).toFixed(1),
    rssSeriesMB: series.filter((_, i) => i % 20 === 0).map(s => +(s.rss / 1048576).toFixed(1)),
  }));
  server.close(() => process.exit(0));
}
main().catch(e => { console.log(JSON.stringify({ error: e.message })); process.exit(3); });
setTimeout(() => { console.log(JSON.stringify({ timeout: true })); process.exit(2); }, 300000);
