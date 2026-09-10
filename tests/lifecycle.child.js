// Adversarial lifecycle: close, terminate and backpressure racing sends, with compression and
// the send/receive workers on. Each case asserts the server survives and, where delivery is
// promised, that the bytes are exact. CASE selects the scenario.
const { WebSocket, PreparedMessage } = require('../dist');
const WS = require('ws');
const crypto = require('crypto');
const port = 3700 + (+(process.env.PORT_OFFSET || 0));
const kase = process.env.CASE;
const pmd = { serverNoContextTakeover: process.env.TAKEOVER === '1' ? false : true, level: +(process.env.LEVEL || 1), threshold: 0 };
const done = (ok, extra = {}) => { console.log(JSON.stringify({ ok, case: kase, ...extra })); process.exit(ok ? 0 : 1); };

const server = new WebSocket.Server({ port, perMessageDeflate: pmd }, () => {
  const client = new WS(`ws://localhost:${port}`, { perMessageDeflate: true, maxPayload: 1 << 30 });
  let received = 0, bytes = 0;
  client.on('message', (d) => { received++; bytes += d.length; });
  client.on('error', () => {});
  client.on('close', () => {
    // for the close cases the client must have seen everything queued before close()
    if (kase === 'close-after-burst') { done(received === 50, { received }); }
  });
  if (kase === 'slow-then-close') { client.on('open', () => client.pause()); }
});

server.on('connection', (ws) => {
  const big = crypto.randomBytes(300 * 1024);       // incompressible, several writes each
  switch (kase) {
    case 'close-after-burst':
      for (let i = 0; i < 50; i++) ws.send(crypto.randomBytes(20000));
      ws.close();                                    // everything queued must still go out
      setTimeout(() => done(true, { note: 'server survived' }), 3000);
      break;
    case 'terminate-mid-burst':
      for (let i = 0; i < 30; i++) ws.send(big);
      setTimeout(() => ws.terminate(), 5);            // kill with frames in flight and in scratch
      setTimeout(() => done(true), 2000);
      break;
    case 'slow-then-close':
      for (let i = 0; i < 30; i++) ws.send(big);      // client never reads: queue backs up
      setTimeout(() => ws.close(), 50);
      setTimeout(() => done(true), 2500);
      break;
    case 'send-after-close':
      ws.close();
      for (let i = 0; i < 10; i++) { try { ws.send(big); } catch (e) { return done(false, { threw: e.message }); } }
      setTimeout(() => done(true), 1000);
      break;
    case 'prepared-outlives-socket': {
      const payload = crypto.randomBytes(50000);
      const prepared = new PreparedMessage(payload);
      for (let i = 0; i < 20; i++) ws.send(prepared, { prefix: Buffer.alloc(30000, 0x51) });
      setTimeout(() => ws.terminate(), 5);            // payload refs must survive the socket
      setTimeout(() => { global.gc && global.gc(); done(true); }, 2000);
      break;
    }
    case 'ping-storm-while-sending': {
      for (let i = 0; i < 20; i++) { ws.send(big); ws.ping(Buffer.alloc(125, 0x53)); }
      setTimeout(() => done(true), 3000);
      break;
    }
    default: done(false, { note: 'unknown case' });
  }
});
setTimeout(() => done(false, { timeout: true }), 30000);
