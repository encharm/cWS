import { expect } from 'chai';
import { connect } from 'net';
import { readFileSync } from 'fs';
import { connect as tlsConnect } from 'tls';
import { Server } from 'http';
import { createServer as createServerHttps, Server as HttpsServer } from 'https';

import { WebSocket, WebSocketServer, secureProtocol } from '../lib';

import { WebSocket as WSWebSocket } from 'ws';
import { deflateRawSync } from 'zlib';
import { randomBytes } from 'crypto';


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

    it('Should receive and send message multiple messages', (done: () => void): void => {
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


describe('CWS permessage-deflate', (): void => {
  const port: number = 3002;

  it('Should round-trip text and binary through a per-socket sliding window with a size threshold', (done: (err?: any) => void): void => {
    const wsServer: WebSocketServer = new WebSocket.Server({
      port,
      perMessageDeflate: { serverNoContextTakeover: false, windowBits: 12, memLevel: 5, threshold: 64 },
    }, (): void => {
      const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: true });
      const small: string = 'tiny';
      const big: string = 'The quick brown fox jumps over the lazy dog. '.repeat(400);
      const binary: Buffer = Buffer.alloc(20000);
      for (let i: number = 0; i < binary.length; i++) { binary[i] = i % 251; }
      const received: any[] = [];

      client.on('open', (): void => {
        client.send(small);
        client.send(big);
        client.send(binary);
      });
      client.on('message', (data: any, isBinary: boolean): void => {
        received.push(isBinary ? Buffer.from(data) : data.toString());
        if (received.length === 3) {
          try {
            expect(received[0]).to.equal(small);
            expect(received[1]).to.equal(big);
            expect(Buffer.compare(received[2], binary)).to.equal(0);
          } catch (e) {
            return done(e);
          }
          client.close();
          wsServer.close((): void => done());
        }
      });
      client.on('error', done);
    });

    wsServer.on('connection', (ws: WebSocket): void => {
      ws.on('message', (message: any): void => {
        ws.send(typeof message === 'string' ? message : Buffer.from(message));
      });
    });
  });
});

describe('CWS permessage-deflate BFINAL=1 messages', (): void => {
  const port: number = 3003;

  it('Should accept a compressed message ending with a BFINAL=1 block (RFC 7692 7.2.3.4)', (done: (err?: any) => void): void => {
    const text: string = 'hello from a client that does not use sync flush, '.repeat(20);
    const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: { threshold: 128 } }, (): void => {
      const socket: any = connect(port, '127.0.0.1');
      let buffer: Buffer = Buffer.alloc(0);
      let handshakeDone: boolean = false;
      socket.on('connect', (): void => {
        socket.write(`GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n`);
      });
      socket.on('data', (data: Buffer): void => {
        buffer = Buffer.concat([buffer, data]);
        if (!handshakeDone) {
          const end: number = buffer.indexOf('\r\n\r\n');
          if (end < 0) { return; }
          handshakeDone = true;
          buffer = buffer.subarray(end + 4);
          const payload: Buffer = deflateRawSync(Buffer.from(text)); // ends with BFINAL=1, no trailing empty block
          const mask: Buffer = randomBytes(4);
          const masked: Buffer = Buffer.from(payload.map((b: number, i: number) => b ^ mask[i % 4]));
          const header: Buffer = Buffer.from([0x80 | 0x40 | 0x1, 0x80 | 126, payload.length >> 8, payload.length & 0xff]);
          socket.write(Buffer.concat([header, mask, masked]));
        } else if (buffer.length) {
          try {
            expect(buffer[0] & 0x0f).to.equal(1); // text echo frame, not a close
          } catch (e) {
            return done(e);
          }
          socket.destroy();
          wsServer.close((): void => done());
        }
      });
      socket.on('close', (): void => {
        if (handshakeDone && !socket.destroyed) { done(new Error('server closed the connection')); }
      });
    });

    wsServer.on('connection', (ws: WebSocket): void => {
      ws.on('message', (message: any): void => {
        try {
          expect(message).to.equal(text);
        } catch (e) {
          return done(e);
        }
        ws.send('ok');
      });
    });
  });
});

describe('CWS permessage-deflate shared mode (independent messages)', (): void => {
  const port: number = 3004;

  it('Should round-trip text, binary and a large message through the shared compressor', (done: (err?: any) => void): void => {
    const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: { threshold: 32 } }, (): void => {
      const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: true });
      const items: any[] = ['tiny', 'x'.repeat(40), JSON.stringify({ users: Array.from({ length: 500 }, (_: any, i: number) => ({ id: i, name: `user-${i}`, online: i % 3 === 0 })) })];
      const binary: Buffer = Buffer.alloc(300000);
      for (let i: number = 0; i < binary.length; i++) { binary[i] = (i * 7 + (i >> 8)) & 0xff; }
      items.push(binary);
      const received: any[] = [];
      client.on('open', (): void => { for (const item of items) { client.send(item); } });
      client.on('message', (data: any, isBinary: boolean): void => {
        received.push(isBinary ? Buffer.from(data) : data.toString());
        if (received.length === items.length) {
          try {
            for (let i: number = 0; i < 3; i++) { expect(received[i]).to.equal(items[i]); }
            expect(Buffer.compare(received[3], binary)).to.equal(0);
          } catch (e) { return done(e); }
          client.close();
          wsServer.close((): void => done());
        }
      });
      client.on('error', done);
    });
    wsServer.on('connection', (ws: WebSocket): void => {
      ws.on('message', (message: any): void => { ws.send(typeof message === 'string' ? message : Buffer.from(message)); });
    });
  });
});
