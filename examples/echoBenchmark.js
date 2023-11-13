const { WebSocket: { Server } } = require('../dist');
const WebSocket = require('ws');

const numClients = 1000; // Number of concurrent clients
const messageSize = 10240; // Size of the message (in bytes)
const numMessagesPerClient = 10; // Number of messages each client will send
const numRuns = 10; // Total number of benchmark runs
let currentRun = 0;
const runTimes = []; // Store total times for each run

function runBenchmark() {
  let disconnectedClients = 0; // Counter for disconnected clients
  let start; // Start time of the run
  const wss = new Server({ port: 3000 }, () => {
    console.log(`Server is running on port 3000, Run: ${currentRun + 1}`);
    start = process.hrtime.bigint(); // Start the timer

    for (let i = 0; i < numClients; i++) {
      const ws = new WebSocket('ws://localhost:3000');
      let messagesSent = 0;

      ws.on('open', function open() {
        for (let j = 0; j < numMessagesPerClient; j++) {
          ws.send('x'.repeat(messageSize));
        }
      });

      ws.on('message', function incoming(data) {
        messagesSent++;
        if (messagesSent === numMessagesPerClient) {
          ws.close();
        }
      });

      ws.on('close', () => {
        disconnectedClients++;
        if (disconnectedClients === numClients) {
          const end = process.hrtime.bigint();
          const totalTime = Number(end - start) / 1e6; // Convert BigInt to Number
          console.log(`Total time for run ${currentRun + 1}: ${totalTime.toFixed(2)} ms`);
          runTimes.push(totalTime);
          wss.close(() => {
            currentRun++;
            if (currentRun < numRuns) {
              runBenchmark();
            } else {
              printStatistics();
            }
          });
        }
      });
    }
  });

  wss.on('connection', (ws, req) => {
    ws.on('message', (msg) => {
      ws.send(msg); // Echo the message
    });
  });
}

function printStatistics() {
  const total = runTimes.reduce((a, b) => a + b, 0);
  const average = total / runTimes.length;
  const min = Math.min(...runTimes);
  const max = Math.max(...runTimes);
  const stdDev = Math.sqrt(runTimes.map(x => Math.pow(x - average, 2)).reduce((a, b) => a + b) / runTimes.length);

  console.log('\n--- Benchmark Statistics ---');
  console.log(`Runs: ${numRuns}`);
  console.log(`Average Time: ${average.toFixed(2)} ms`);
  console.log(`Min Time: ${min.toFixed(2)} ms`);
  console.log(`Max Time: ${max.toFixed(2)} ms`);
  console.log(`Standard Deviation: ${stdDev.toFixed(2)}`);
}

runBenchmark();
