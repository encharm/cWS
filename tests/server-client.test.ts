import { expect } from 'chai';
import { connect } from 'net';
import { readFileSync } from 'fs';
import { connect as tlsConnect } from 'tls';
import { Server } from 'http';
import { createServer as createServerHttps, Server as HttpsServer } from 'https';

import { WebSocket, WebSocketServer, secureProtocol } from '../lib';

import { WebSocket as WSWebSocket } from 'ws';


const delay = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));


const serverPort: number = 3000;
const secureServerPort: number = 3001;

async function createWSServer(ssl: boolean, server?: Server | HttpsServer): Promise<WebSocketServer> {
  return new Promise((res: any): void => {
    if (server) {
      return res(new WebSocket.Server({ server }));
    }

    if (ssl) {
      const httpsServer: HttpsServer = createServerHttps({
        key: readFileSync('./tests/certs/key.pem'),
        cert: readFileSync('./tests/certs/certificate.pem'),
        secureProtocol
      });

      const wsServer: WebSocketServer = new WebSocket.Server({ server: httpsServer });

      // NOTE: small workaround to stop external server (for smoother testing)
      // as server provided from outside it should be closed from outside too
      // but to make our testing simple we just overwrite close function on cws
      // to also close this secure server
      (wsServer as any)._close_ = wsServer.close.bind(wsServer);
      wsServer.close = (cb: any): void => {
        httpsServer.close();
        (wsServer as any)._close_(cb);
      };

      httpsServer.listen(secureServerPort, (): void => {
        res(wsServer);
      });
    } else {
      const wsServer: WebSocketServer = new WebSocket.Server({ port: serverPort }, (): void => res(wsServer));
    }
  });
}

['Non-SSL'].forEach((type: string): void => {
  const isSSL: boolean = type === 'SSL';
  const connectionUrl: string = isSSL ? `wss://localhost:${secureServerPort}` : `ws://localhost:${serverPort}`;

  describe(`CWS Server & Client Tests ` + type, (): void => {
    it('Should accept connection', (done: () => void): void => {
      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          wsServer.on('connection', (): void => {
            wsServer.close((): void => {
              done();
            });
          });

          new WebSocket(connectionUrl);
        });
    });

    it('Should receive and send message', (done: () => void): void => {
      const testMessage: string = `Hello world from cWS ` + Math.random();

      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          wsServer.on('connection', (socket: WebSocket): void => {
            socket.on('message', (msg: string): void => {
              expect(msg).to.be.eql(testMessage);
              socket.send(msg);
            });
          });

          const connection: WebSocket = new WebSocket(connectionUrl);

          connection.on('open', (): void => {
            connection.send(testMessage);
          });

          connection.on('message', (msg: string): void => {
            expect(msg).to.be.eql(testMessage);

            wsServer.close((): void => {
              done();
            });
          });
        });
    });

    const CONNECTION_COUNT = 3;
    const MESSAGE_COUNT = 200;

    it.only('Should receive and send message multiple messages', (done: () => void): void => {
      const testMessage: string = `Hello world from cWS ` + Math.random();

      const connections: WebSocket[] = [];

      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          wsServer.on('connection', (socket: WebSocket): void => {
            socket.on('message', (msg: string): void => {
              socket.send(msg);
            });
          });

          let closeI = 0;
          let closeJ = 0;
          for (let i = 0 ; i < CONNECTION_COUNT;++i) {
            const connection: WebSocket = new WSWebSocket(connectionUrl);
            connections.push(connection);

            connection.on('open', async () => {
              for (let j = 0 ; j < MESSAGE_COUNT;++j) {
                try {
                  console.log(`Sending ${i}.${j}`);
                  connection.send(`${testMessage} at ${i} at ${j}`, {}, (err: Error | undefined) => {
                    if (err) {
                      console.error('Error', err);
                    }
                  });
                  await delay((1 * Math.random())|1);
                } catch (err) {
                  console.log('Some error', err);
                }
              }
            });
  
            connection.on('message', (msg: string): void => {
              if(!(connection as any).j) { (connection as any).j = 0; }
              expect(msg.toString()).to.be.eql(`${testMessage} at ${i} at ${(connection as any).j}`);
              (connection as any).j += 1;
              if (!(connection as any).messages) {
                (connection as any).messages = [];
              }
              (connection as any).messages.push(msg);
  
              if ((connection as any).j === MESSAGE_COUNT) {
                closeI += 1;
                if (closeI === CONNECTION_COUNT) {
                  wsServer.close((): void => {
                    done();
                  });
                }
              }
            });
          }
          setTimeout(() => {
            for (let i = 0; i < CONNECTION_COUNT;++i) {
              if ((connections[i] as any).j !== MESSAGE_COUNT) {
                console.log(i, (connections[i] as any).j);
              }
            }
          }, 8000);
        });


      }).timeout(10000);


    it('Should buffer data and report .bufferedAmount', function (done: () => void): void {
      this.timeout(10000);

      const NUMBER_OF_MESSAGES: number = 10;
      const testMessage: string = `Hello world from cWS ` + Math.random();
      const repeatedMessage: string = testMessage.repeat(10000);


      let serverSocket: WebSocket;

      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          wsServer.on('connection', (socket: WebSocket): void => {
            serverSocket = socket;

            let toServerCounter: number = 0;
            socket.on('message', (msg: string): void => {
              expect(msg).to.be.eql(repeatedMessage);
              toServerCounter += 1;

              if (toServerCounter === NUMBER_OF_MESSAGES) {
                for (let i: number = 0; i < NUMBER_OF_MESSAGES;++i) {
                  const str: string = repeatedMessage;
                  socket.send(str);
                }
                expect(socket.bufferedAmount).greaterThan(0);
              }
            });
          });

          const connection: WebSocket = new WebSocket(connectionUrl);

          connection.on('open', (): void => {
            for (let i: number = 0; i < NUMBER_OF_MESSAGES;++i) {
              connection.send(testMessage.repeat(10000));
            }
            expect(connection.bufferedAmount).greaterThan(0);
          });

          let toClientCounter: number = 0;
          connection.on('message', (msg: string): void => {
            expect(msg).to.be.eql(testMessage.repeat(10000));
            toClientCounter += 1;

            if (toClientCounter === NUMBER_OF_MESSAGES) {
              expect(serverSocket.bufferedAmount).equals(0);
              expect(connection.bufferedAmount).equals(0);
              wsServer.close((): void => {
                done();
              });
            }
          });
        });
    });


    it('Should receive and send ping/pong', (done: () => void): void => {
      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          let clientReceivedPing: boolean = false;

          wsServer.on('connection', (socket: WebSocket): void => {
            socket.on('pong', (): void => {
              expect(clientReceivedPing).to.be.true;

              wsServer.close((): void => {
                done();
              });
            });

            socket.ping();
          });

          const connection: WebSocket = new WebSocket(connectionUrl);
          connection.on('ping', (): void => {
            clientReceivedPing = true;
          });
        });
    });


    it('Should close connection with correct code & reason (Server)', (done: () => void): void => {
      // wite close logic
      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          const conditions: any = {
            withCode: {
              code: 3001,
              reason: ''
            },
            withCodeAndReason: {
              code: 3001,
              reason: 'Custom Reason'
            },
            default: {
              code: 1000,
              reason: ''
            }
          };

          wsServer.on('connection', (socket: WebSocket): void => {
            socket.on('message', (msg: string): void => {
              if (msg === 'default') {
                return socket.close();
              }
              socket.close(conditions[msg].code, conditions[msg].reason);
            });
          });

          const allPassed: Promise<any>[] = [];
          for (const key in conditions) {
            allPassed.push(new Promise((res: any): void => {
              const condition: any = conditions[key];
              const connection: WebSocket = new WebSocket(connectionUrl);

              connection.on('close', (code?: number, reason?: string): void => {
                expect(code).to.eql(condition.code);
                expect(reason).to.eql(condition.reason);
                res();
              });

              connection.on('open', (): void => {
                connection.send(key);
              });
            }));
          }

          Promise.all(allPassed).then((): void => {
            wsServer.close((): void => {
              done();
            });
          });
        });
    });


    it('Should close connection with correct code & reason (Client)', (done: () => void): void => {
      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          const conditions: any = {
            withCode: {
              code: 3455,
              reason: ''
            },
            withCodeAndReason: {
              code: 3455,
              reason: 'Custom Reason'
            },
            default: {
              code: 1000,
              reason: ''
            }
          };

          wsServer.on('connection', (socket: WebSocket): void => {
            let expectedCode: number = conditions.default.code;
            let expectedReason: string = conditions.default.reason;

            socket.on('message', (msg: string): void => {
              if (msg) {
                expectedCode = JSON.parse(msg).code;
                expectedReason = JSON.parse(msg).reason;
              }
            });

            socket.on('close', (code?: number, reason?: string): void => {
              expect(code).to.eql(expectedCode);
              expect(reason).to.eql(expectedReason);
            });
          });

          const allPassed: Promise<any>[] = [];
          for (const key in conditions) {
            allPassed.push(new Promise((res: any): void => {
              const condition: any = conditions[key];
              const connection: WebSocket = new WebSocket(connectionUrl);

              connection.on('close', (code?: number, reason?: string): void => {
                setTimeout((): void => res(), 10);
              });

              connection.on('open', (): void => {
                if (key === 'default') {
                  return connection.close();
                }

                connection.send(JSON.stringify(condition));
                setTimeout((): void => connection.close(condition.code, condition.reason), 10);
              });
            }));
          }

          Promise.all(allPassed).then((): void => {
            wsServer.close((): void => {
              done();
            });
          });
        });
    });

    it('Should "broadcast" to all connected users', (done: () => void): void => {
      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          let clientsReceivedMessage: number = 0;
          const messageToBroadcast: string = 'Super message';

          setTimeout((): void => {
            wsServer.broadcast(messageToBroadcast);
            setTimeout((): void => {
              expect(clientsReceivedMessage).to.be.eql(2);
              wsServer.close((): void => {
                done();
              });
            }, 10);
          }, 50);

          const connection1: WebSocket = new WebSocket(connectionUrl);
          const connection2: WebSocket = new WebSocket(connectionUrl);

          connection1.on('message', (msg: string): void => {
            expect(msg).to.be.eql(messageToBroadcast);
            clientsReceivedMessage++;
          });

          connection2.on('message', (msg: string): void => {
            expect(msg).to.be.eql(messageToBroadcast);
            clientsReceivedMessage++;
          });
        });
    });

    it('Should abort request on invalid Sec-WebSocket-Key header', (done: () => void): void => {
      createWSServer(isSSL)
        .then((wsServer: WebSocketServer): void => {
          const host: string = connectionUrl.replace('//', '').split(':')[1];
          const port: number = parseInt(connectionUrl.replace('//', '').split(':')[2], 10);
          const connectMethod: any = isSSL ? tlsConnect : connect;

          let response: string = '';
          const connection: any = connectMethod(port, host, { rejectUnauthorized: false }, (): void => {
            connection.write(`GET / HTTP/1.0\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-Websocket-Key: invalid\r\nSec-Websocket-Version: 13\r\n\r\n`);
          });

          connection.on('data', (data: Buffer): void => {
            response += data.toString();
          });

          connection.on('close', (): void => {
            expect(response).to.be.eql('HTTP/1.1 400 Bad Request\r\n\r\n');
            wsServer.close((): void => {
              done();
            });
          });
        });
    });

    // add more tests
  });
});
