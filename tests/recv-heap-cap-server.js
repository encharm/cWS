// Server child for the heap-cap regression: a receive-worker server that, on request, blocks its
// JS thread for ~2.5s and reports the RSS growth during the block. The parent floods it with
// large messages meanwhile. Measuring the server in its own process keeps the client's send
// buffers out of the number.
const { WebSocket } = require('../dist');
const port = +process.argv[2];
let got = 0;
const server = new WebSocket.Server({ port, maxPayload: 1 << 20, receiveThread: true }, () => process.send('ready'));
server.on('connection', (ws) => { ws.on('message', () => { got++; }); ws.on('error', () => {}); });
function block(ms) { const e = Date.now() + ms; while (Date.now() < e) {} }
process.on('message', (m) => {
  if (m === 'block') {
    const before = process.memoryUsage().rss;
    block(2500);
    const after = process.memoryUsage().rss;
    process.send({ blockedGrowthMB: +((after - before) / 1048576).toFixed(0) });
  } else if (m === 'report') {
    process.send({ got, rssMB: +(process.memoryUsage().rss / 1048576).toFixed(0), recvStalls: server.stats.recvStalls });
  } else if (m === 'exit') { process.exit(0); }
});
