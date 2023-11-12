const { WebSocket: { Server } } = require('../dist');
const WebSocket = require('ws');

const wss = new Server({ port: 3000 }, () => {
  console.log('Server is running on port 3000');

  // setTimeout(() => {
    const ws = new WebSocket('ws://localhost:3000');

    ws.on('open', function open() {
      console.log('Connected to the server');
      ws.send('Hello, server!');
    });
    
    ws.on('message', function incoming(data) {
      console.log(`Received echoed message: ${data}`);
      // Close the connection after receiving the echoed message
      ws.close();
    });
    
    ws.on('close', () => {
      console.log('Disconnected from the server');
    });
  }, 0);
// });

wss.on('connection', (ws, req) => {
  ws.on('message', (msg) => {
    ws.send(msg);
  });

  ws.on('close', () => {
    setTimeout(() => {
      console.log('Client disconncted, quitting');
      wss.close();
    }, 10);
  });
});