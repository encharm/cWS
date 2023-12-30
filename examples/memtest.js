const assert = require('assert');
const { WebSocket, secureProtocol } = require('../');
const testMessage = `Hello world from cWS ` + Math.random();

const { WebSocket: WSWebSocket } = require('ws');

const serverPort = 3020;
const secureServerPort = 3021;

const CONNECTION_COUNT = 100;
const MESSAGE_COUNT = 500;

const connectionUrl = `ws://localhost:${serverPort}`;

function countOpenFileDescriptors() {
  return new Promise((resolve, reject) => {
      const fdDir = '/proc/self/fd';

      require('fs').readdir(fdDir, (err, files) => {
          if (err) {
              reject(err);
              return;
          }
          resolve(files.length);
      });
  });
}

async function createWSServer(ssl, server) {
  return new Promise((res) => {
    if (server) {
      return res(new WebSocket.Server({ server }));
    }

    if (ssl) {
      const httpsServer = createServerHttps({
        key: readFileSync('./tests/certs/key.pem'),
        cert: readFileSync('./tests/certs/certificate.pem'),
        secureProtocol
      });

      const wsServer = new WebSocket.Server({ server: httpsServer });

      // NOTE: small workaround to stop external server (for smoother testing)
      // as server provided from outside it should be closed from outside too
      // but to make our testing simple we just overwrite close function on cws
      // to also close this secure server
      (wsServer)._close_ = wsServer.close.bind(wsServer);
      wsServer.close = (cb) => {
        httpsServer.close();
        (wsServer)._close_(cb);
      };

      httpsServer.listen(secureServerPort, () => {
        res(wsServer);
      });
    } else {
      const wsServer = new WebSocket.Server({ port: serverPort }, () => res(wsServer));
    }
  });
}

createWSServer(isSSL = false)
  .then(async (wsServer) => {
    console.log('Server started');
    wsServer.on('connection', (socket) => {
      socket.on('message', (msg) => {
        socket.send(msg);
      });
    });

    const testLoop = async () => {
      const connections = [];

      return new Promise((resolve) => {
        let closeI = 0;
        let closeJ = 0;
        for (let i = 0 ; i < CONNECTION_COUNT;++i) {
          const connection = new WSWebSocket(connectionUrl);
          connections.push(connection);
    
          connection.on('open', async () => {
            for (let j = 0 ; j < MESSAGE_COUNT;++j) {
              try {
                // console.log(`Sending ${i}.${j}`);
                connection.send(`${testMessage} at ${i} at ${j}`, {}, (err) => {
                  if (err) {
                    console.error('Error', err);
                  }
                });
                // await setTimeout((1 * Math.random())|1);
              } catch (err) {
                console.log('Some error', err);
              }
            }
          });
          connection.on('close', () => {
            delete connections[i];
            if (Object.keys(connections).length === 0) {
              resolve();
            }
          });
    
          connection.on('message', async (msg) => {
            if(!(connection).j) { (connection).j = 0; }
            assert(msg.toString() == `${testMessage} at ${i} at ${(connection).j}`);
            (connection).j += 1;
            if (!(connection).messages) {
              (connection).messages = [];
            }
            (connection).messages.push(msg);
    
            if ((connection).j === MESSAGE_COUNT) {
              closeI += 1;
              if (closeI === CONNECTION_COUNT) {
                for (let i = 0; i < CONNECTION_COUNT;++i) {
                  connections[i].close();
                }
              }
            }
          });
        }
      }
    );
  }
  for (let i = 0; i < 500; ++i) {
    await testLoop();
    
    const memory = process.memoryUsage();
    console.log(i.toString().padStart(4),
      (memory.rss / (1024 * 1024)).toFixed(0).padStart(5),
      (memory.heapTotal / (1024 * 1024)).toFixed(0).padStart(5),
      (memory.arrayBuffers / (1024 * 1024)).toFixed(0).padStart(5),
      (memory.external / (1024 * 1024)).toFixed(0).padStart(5),
      (await countOpenFileDescriptors()).toFixed(0).padStart(5),
    );
  }
  wsServer.close();
});
