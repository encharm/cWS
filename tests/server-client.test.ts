import { expect } from 'chai';
import { connect } from 'net';
import { readFileSync } from 'fs';
import { connect as tlsConnect } from 'tls';
import { Server } from 'http';
import { createServer as createServerHttps, Server as HttpsServer } from 'https';

import { WebSocket, WebSocketServer, PreparedMessage, secureProtocol } from '../lib';

import { WebSocket as WSWebSocket } from 'ws';
import { constants as zlibConstants, deflateRawSync, inflateRawSync } from 'zlib';
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

describe('CWS prepared messages (shared payload fan-out)', (): void => {
  const port: number = 3005;
  const payloadText: string = JSON.stringify({ jams: Array.from({ length: 40 }, (_: unknown, i: number) => ({ shortId: `jam${i}`, users: i * 3, team: 'team-a' })) });
  const payload: Buffer = Buffer.from(payloadText);

  // Connects `count` ws clients, lets the server call `send` for each, resolves with what every client received.
  function fanOut(deflate: any, clientDeflate: boolean, count: number, send: (ws: WebSocket) => void, expectedPerClient: number): Promise<Buffer[][]> {
    return new Promise((resolve: (v: Buffer[][]) => void, reject: (e: any) => void): void => {
      const received: Buffer[][] = [];
      const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: deflate }, (): void => {
        for (let i: number = 0; i < count; i++) {
          const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: clientDeflate });
          const mine: Buffer[] = [];
          received.push(mine);
          client.on('message', (data: Buffer, isBinary: boolean): void => {
            mine.push(isBinary ? data : Buffer.from(`text:${data.toString()}`));
            if (received.every((r: Buffer[]) => r.length >= expectedPerClient)) {
              wsServer.close((): void => resolve(received));
            }
          });
          client.on('error', reject);
        }
      });
      wsServer.on('connection', (ws: WebSocket): void => send(ws));
    });
  }

  it('Should send prefix + prepared payload to many clients without deflate', async (): Promise<void> => {
    const prepared: PreparedMessage = new PreparedMessage(payload);
    let callbacks: number = 0;
    const got: Buffer[][] = await fanOut(false, false, 3, (ws: WebSocket): void => {
      ws.send(prepared, { prefix: Buffer.from('prefix:') }, (): void => { callbacks++; });
      ws.send(prepared, { prefix: Buffer.from('again:') });
    }, 2);
    expect(prepared.byteLength).to.equal(payload.length);
    for (let i: number = 0; i < 3; i++) {
      expect(got[i][0].equals(Buffer.concat([Buffer.from('prefix:'), payload]))).to.equal(true);
      expect(got[i][1].equals(Buffer.concat([Buffer.from('again:'), payload]))).to.equal(true);
    }
    expect(callbacks).to.equal(3);
  });

  it('Should send text frames when binary is false', async (): Promise<void> => {
    const prepared: PreparedMessage = new PreparedMessage(Buffer.from('shared text'));
    const got: Buffer[][] = await fanOut(false, false, 1, (ws: WebSocket): void => {
      ws.send(prepared, { binary: false, prefix: 'hello ' });
    }, 1);
    expect(got[0][0].toString()).to.equal('text:hello shared text');
  });

  it('Should splice the prefix ahead of cached deflate blocks in shared mode', async (): Promise<void> => {
    const prepared: PreparedMessage = new PreparedMessage(payload);
    const got: Buffer[][] = await fanOut({ threshold: 16 }, true, 3, (ws: WebSocket): void => {
      ws.send(prepared, { prefix: Buffer.from('compressed:') });
      ws.send(prepared);
      ws.send(prepared, { compress: false, prefix: Buffer.from('tiny') });
    }, 3);
    for (let i: number = 0; i < 3; i++) {
      expect(got[i][0].equals(Buffer.concat([Buffer.from('compressed:'), payload]))).to.equal(true);
      expect(got[i][1].equals(payload)).to.equal(true);
      expect(got[i][2].equals(Buffer.concat([Buffer.from('tiny'), payload]))).to.equal(true);
    }
  });

  it('Should fall back to per-socket compression with context takeover', async (): Promise<void> => {
    const prepared: PreparedMessage = new PreparedMessage(payload);
    const got: Buffer[][] = await fanOut({ serverNoContextTakeover: false, threshold: 16 }, true, 2, (ws: WebSocket): void => {
      ws.send(prepared, { prefix: Buffer.from('takeover:') });
      ws.send(payload);
      ws.send(prepared, { prefix: Buffer.from('takeover-again:') });
    }, 3);
    for (let i: number = 0; i < 2; i++) {
      expect(got[i][0].equals(Buffer.concat([Buffer.from('takeover:'), payload]))).to.equal(true);
      expect(got[i][1].equals(payload)).to.equal(true);
      expect(got[i][2].equals(Buffer.concat([Buffer.from('takeover-again:'), payload]))).to.equal(true);
    }
  });

  it('Should emit one RSV1 frame whose payload is a stored prefix block followed by the shared blocks', (done: (err?: any) => void): void => {
    const prefix: Buffer = Buffer.from('raw-prefix:');
    const prepared: PreparedMessage = new PreparedMessage(payload);
    const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: { threshold: 16 } }, (): void => {
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
        }
        if (buffer.length < 4) { return; }
        const lengthByte: number = buffer[1] & 0x7f;
        const headerLength: number = lengthByte === 126 ? 4 : lengthByte === 127 ? 10 : 2;
        const frameLength: number = lengthByte === 126 ? buffer.readUInt16BE(2) : lengthByte === 127 ? Number(buffer.readBigUInt64BE(2)) : lengthByte;
        if (buffer.length < headerLength + frameLength) { return; }
        const body: Buffer = buffer.subarray(headerLength, headerLength + frameLength);
        try {
          expect(buffer[0]).to.equal(0x80 | 0x40 | 0x2); // FIN, RSV1, binary
          expect(body[0]).to.equal(0); // stored block: BFINAL=0, BTYPE=00
          expect(body.readUInt16LE(1)).to.equal(prefix.length);
          expect(body.readUInt16LE(3)).to.equal(~prefix.length & 0xffff);
          expect(body.subarray(5, 5 + prefix.length).equals(prefix)).to.equal(true);
          const inflated: Buffer = inflateRawSync(Buffer.concat([body, Buffer.from([0, 0, 0xff, 0xff])]), { finishFlush: zlibConstants.Z_SYNC_FLUSH });
          expect(inflated.equals(Buffer.concat([prefix, payload]))).to.equal(true);
          expect(frameLength).to.be.lessThan(payload.length / 2);
        } catch (e) {
          return done(e);
        }
        socket.destroy();
        wsServer.close((): void => done());
      });
    });
    wsServer.on('connection', (ws: WebSocket): void => ws.send(prepared, { prefix: prefix }));
  });
});

describe('CWS send queue under pressure', (): void => {
  const port: number = 3006;

  // Connects one `ws` client, runs `act` on the server side once the client is open, and resolves
  // with everything the client received (in order) once `expected` messages arrived or the socket closed.
  function collect(act: (ws: WebSocket) => void, expected: number, options: any = {}): Promise<{ messages: Buffer[], closed: boolean, code?: number }> {
    return new Promise((resolve: (v: any) => void, reject: (e: any) => void): void => {
      const messages: Buffer[] = [];
      let closed: boolean = false;
      let code: number | undefined;
      const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: false }, (): void => {
        const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: false });
        const finish = (): void => wsServer.close((): void => resolve({ messages, closed, code }));
        client.on('message', (data: Buffer): void => {
          messages.push(data);
          if (options.slowReader) { client.pause(); setTimeout((): void => client.resume(), 2); }
          if (messages.length === expected && !options.waitForClose) { client.close(); finish(); }
        });
        client.on('close', (c: number): void => { closed = true; code = c; finish(); });
        client.on('error', reject);
      });
      wsServer.on('connection', (ws: WebSocket): void => act(ws));
    });
  }

  it('Should deliver a burst larger than the kernel buffer, in order, to a slow reader', async (): Promise<void> => {
    const count: number = 3000;
    const got: any = await collect((ws: WebSocket): void => {
      for (let i: number = 0; i < count; i++) {
        const buf: Buffer = Buffer.alloc(4096, i & 0xff);
        buf.writeUInt32LE(i, 0);
        ws.send(buf);
      }
    }, count, { slowReader: true });
    expect(got.messages.length).to.equal(count);
    for (let i: number = 0; i < count; i++) {
      expect(got.messages[i].readUInt32LE(0)).to.equal(i);
      expect(got.messages[i][4095]).to.equal(i & 0xff);
    }
  });

  it('Should deliver every message sent before close(), then the close frame', async (): Promise<void> => {
    const count: number = 2000;
    const got: any = await collect((ws: WebSocket): void => {
      for (let i: number = 0; i < count; i++) {
        const buf: Buffer = Buffer.alloc(2048, 7);
        buf.writeUInt32LE(i, 0);
        ws.send(buf);
      }
      ws.close(4001, 'done');
    }, count, { waitForClose: true, slowReader: true });
    expect(got.messages.length).to.equal(count);
    expect(got.messages[count - 1].readUInt32LE(0)).to.equal(count - 1);
    expect(got.closed).to.equal(true);
    expect(got.code).to.equal(4001);
  });

  it('Should survive terminate() with a burst in flight and keep serving', async (): Promise<void> => {
    const got: any = await collect((ws: WebSocket): void => {
      for (let i: number = 0; i < 2000; i++) { ws.send(Buffer.alloc(4096, 1)); }
      setImmediate((): void => ws.terminate());
    }, 1e9, { waitForClose: true, slowReader: true });
    expect(got.closed).to.equal(true);
    // the server is still usable afterwards
    const again: any = await collect((ws: WebSocket): void => ws.send('after'), 1);
    expect(again.messages[0].toString()).to.equal('after');
  });

  it('Should deliver messages queued behind an in-flight send when the loop then idles', async (): Promise<void> => {
    const got: any = await collect((ws: WebSocket): void => {
      for (let i: number = 0; i < 500; i++) { ws.send(Buffer.alloc(4096, 1)); }   // in flight on the worker after this tick
      setImmediate((): void => { ws.send('late-1'); setImmediate((): void => ws.send('late-2')); });
    }, 502, { slowReader: true });
    expect(got.messages.length).to.equal(502);
    expect(got.messages[500].toString()).to.equal('late-1');
    expect(got.messages[501].toString()).to.equal('late-2');
  });

  it('Should close the fd of a socket terminated with a send in flight, then idle', async (): Promise<void> => {
    const got: any = await collect((ws: WebSocket): void => {
      for (let i: number = 0; i < 200; i++) { ws.send(Buffer.alloc(4096, 2)); }
      setImmediate((): void => ws.terminate());
    }, 1e9, { waitForClose: true });
    expect(got.closed).to.equal(true);
  });

  it('Should deliver a compressed burst, in order, to a slow reader (shared compressor)', async (): Promise<void> => {
    const count: number = 1500;
    const got: any = await new Promise((resolve: (v: any) => void, reject: (e: any) => void): void => {
      const messages: Buffer[] = [];
      const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: { threshold: 16 } }, (): void => {
        const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: true });
        client.on('message', (data: Buffer): void => {
          messages.push(data);
          client.pause(); setTimeout((): void => client.resume(), 2);
          if (messages.length === count) { client.close(); wsServer.close((): void => resolve(messages)); }
        });
        client.on('error', reject);
      });
      wsServer.on('connection', (ws: WebSocket): void => {
        for (let i: number = 0; i < count; i++) {
          const buf: Buffer = Buffer.alloc(4096, 65 + (i % 26));
          buf.writeUInt32LE(i, 0);
          ws.send(buf);
        }
      });
    });
    expect(got.length).to.equal(count);
    for (let i: number = 0; i < count; i++) {
      expect(got[i].readUInt32LE(0)).to.equal(i);
      expect(got[i][4095]).to.equal(65 + (i % 26));
    }
  });

  it('Should round-trip context takeover with the microdeflate window (dedicated, level 1)', async (): Promise<void> => {
    // 400 similar messages (history references), a 300 KB message (spans several window slides),
    // then more small ones, then a burst to a slow reader; every byte must inflate correctly.
    const small: Buffer[] = [];
    for (let i: number = 0; i < 400; i++) { small.push(Buffer.from(JSON.stringify({ shortId: `jam${i % 40}`, connectedUsers: i % 17, thumbTimestamp: 1725300000000 + i, team: '66d5a1b2c3d4e5f6a7b8c9d0', shareType: i % 2 }))); }
    const big: Buffer = Buffer.alloc(300 * 1024);
    for (let i: number = 0; i < big.length; i++) { big[i] = (i * 7 + (i >> 9)) & 0xff; }
    // Prepared messages on a takeover connection go out independently compressed (cached once)
    // with a history-sync entry behind them; the window-compressed messages around them must
    // still decode, so interleave both, and reuse the prepared payload with different prefixes.
    const sharedPayload: Buffer = Buffer.from(JSON.stringify({ jams: Array.from({ length: 30 }, (_: unknown, i: number) => ({ shortId: `jam${i}`, connectedUsers: i * 3, team: 'team-a' })) }));
    const prepared: PreparedMessage = new PreparedMessage(sharedPayload);
    const mixed: { prefix?: Buffer, plain?: Buffer }[] = [];
    for (let i: number = 0; i < 60; i++) { mixed.push(i % 3 === 0 ? { prefix: Buffer.from(`p${i}:`) } : { plain: small[i] }); }
    const expected: Buffer[] = [...small, big, ...small.slice(0, 50), ...mixed.map((m: any) => m.prefix ? Buffer.concat([m.prefix, sharedPayload]) : m.plain)];
    const got: any = await new Promise((resolve: (v: any) => void, reject: (e: any) => void): void => {
      const messages: Buffer[] = [];
      const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: { serverNoContextTakeover: false, level: 1, threshold: 0 } }, (): void => {
        const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: { serverNoContextTakeover: false } });
        client.on('message', (data: Buffer): void => {
          messages.push(data);
          if (messages.length % 50 === 0) { client.pause(); setTimeout((): void => client.resume(), 2); }
          if (messages.length === expected.length) { client.close(); wsServer.close((): void => resolve(messages)); }
        });
        client.on('error', reject);
      });
      wsServer.on('connection', (ws: WebSocket): void => {
        for (const m of [...small, big, ...small.slice(0, 50)]) { ws.send(m); }
        for (const m of mixed) { if (m.prefix) { ws.send(prepared, { prefix: m.prefix }); } else { ws.send(m.plain as Buffer); } }
      });
    });
    expect(got.length).to.equal(expected.length);
    for (let i: number = 0; i < expected.length; i++) {
      expect(got[i].equals(expected[i])).to.equal(true, `message ${i}`);
    }
  });

  it('Should echo a compressed binary message unchanged (shared and takeover, main-thread and worker paths)', async (): Promise<void> => {
    // The inflated bytes live in the hub's scratch buffer and the echo is compressed from there;
    // the deflater must not write its output over its own input.
    for (const pmd of [{ threshold: 16 }, { serverNoContextTakeover: false, threshold: 16, level: 1 }, { serverNoContextTakeover: false, threshold: 16, level: 2 }]) {
      const payload: Buffer = Buffer.alloc(20000);
      for (let i: number = 0; i < payload.length; i++) { payload[i] = i % 251; }
      const got: Buffer = await new Promise((resolve: (v: Buffer) => void, reject: (e: any) => void): void => {
        const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: pmd }, (): void => {
          const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: true });
          client.on('open', (): void => client.send(payload));
          client.on('message', (data: Buffer): void => { client.close(); wsServer.close((): void => resolve(Buffer.from(data))); });
          client.on('error', reject);
        });
        wsServer.on('connection', (ws: WebSocket): void => ws.on('message', (m: ArrayBuffer): void => ws.send(Buffer.from(m))));
      });
      expect(got.equals(payload)).to.equal(true, JSON.stringify(pmd));
    }
  });

  it('Should accept a compressed frame whose length header arrives split across two reads', async (): Promise<void> => {
    // A 126/127-length header spilled across a TCP read boundary is re-parsed on the next read; the
    // compressed flag must not be recorded twice (that used to close the connection with 1006).
    const raw: Buffer = randomBytes(400);                                    // incompressible: deflated length >= 126
    let deflated: Buffer = deflateRawSync(raw, { finishFlush: zlibConstants.Z_SYNC_FLUSH });
    deflated = deflated.subarray(0, deflated.length - 4);
    const mask: Buffer = Buffer.from([1, 2, 3, 4]);
    const header: Buffer = Buffer.alloc(4); header[0] = 0x82 | 0x40; header[1] = 126 | 0x80; header.writeUInt16BE(deflated.length, 2);
    const frame: Buffer = Buffer.concat([header, mask, Buffer.from(deflated.map((b: number, i: number) => b ^ mask[i & 3]))]);
    for (const split of [1, 2, 3, 5, 6, 7, 8, 20]) {
      const got: number = await new Promise((resolve: (v: number) => void, reject: (e: any) => void): void => {
        const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: { threshold: 0 } }, (): void => {
          const socket: any = connect(port, '127.0.0.1');
          let upgraded: boolean = false;
          socket.on('connect', (): void => { socket.setNoDelay(true); socket.write(`GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover\r\n\r\n`); });
          socket.on('data', (): void => { if (upgraded) { return; } upgraded = true; socket.write(frame.subarray(0, split)); setTimeout((): void => socket.write(frame.subarray(split)), 30); });
          socket.on('error', reject);
          wsServer.on('connection', (ws: WebSocket): void => {
            ws.on('message', (m: ArrayBuffer): void => { const n: number = m.byteLength; socket.destroy(); wsServer.close((): void => resolve(n)); });   // read before the view is detached
            ws.on('close', (code: number): void => { if (code !== 1000) { socket.destroy(); wsServer.close((): void => reject(new Error(`closed with ${code} at split ${split}`))); } });
          });
        });
      });
      expect(got).to.equal(raw.length, `split ${split}`);
    }
  });

  it('Should keep binary views alive across a read and detach them after its microtask drain', async (): Promise<void> => {
    // Two binary messages in one TCP write arrive in one read. With the per-read callback scope the
    // first message's view is still readable while the second is handled and inside a promise
    // continuation, and detached once the read is over; with CWS_READ_SCOPE=0 only the
    // continuation guarantee holds (the view is detached before the next message).
    const readScope: boolean = !['0', 'false', 'off', 'no'].includes(String(process.env.CWS_READ_SCOPE));
    const frame = (payload: Buffer): Buffer => { const mask: Buffer = Buffer.from([1, 2, 3, 4]); return Buffer.concat([Buffer.from([0x82, 0x80 | payload.length]), mask, Buffer.from(payload.map((b: number, i: number) => b ^ mask[i & 3]))]); };
    const result: any = await new Promise((resolve: (v: any) => void, reject: (e: any) => void): void => {
      const wsServer: WebSocketServer = new WebSocket.Server({ port }, (): void => {
        const socket: any = connect(port, '127.0.0.1');
        let upgraded: boolean = false;
        socket.on('connect', (): void => socket.write(`GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
        socket.on('data', (): void => { if (upgraded) { return; } upgraded = true; socket.write(Buffer.concat([frame(Buffer.from('first')), frame(Buffer.from('second'))])); });
        socket.on('error', reject);
        wsServer.on('connection', (ws: WebSocket): void => {
          let first: ArrayBuffer | undefined; const out: any = {};
          ws.on('message', (m: ArrayBuffer): void => {
            if (!first) {
              first = m;
              Promise.resolve().then((): void => { out.inContinuation = first!.byteLength; });   // runs in the drain
            } else {
              out.duringSecond = first!.byteLength;
              setImmediate((): void => { out.afterRead = first!.byteLength; socket.destroy(); wsServer.close((): void => resolve(out)); });
            }
          });
        });
      });
    });
    expect(result.inContinuation).to.equal(5);                        // both modes: continuation in the drain sees the bytes
    expect(result.duringSecond).to.equal(readScope ? 5 : 0);           // read scope: still attached during the next message
    expect(result.afterRead).to.equal(0);                              // detached once the read is over
  });

  it('Should report a throwing handler as uncaughtException and keep delivering the rest of the read', async (): Promise<void> => {
    const { execFile } = await import('child_process');
    const out: string = await new Promise((resolve: (v: string) => void): void => {
      execFile(process.execPath, [`${__dirname}/read-scope-throw.child.js`], { env: process.env }, (_err: any, stdout: string): void => resolve(stdout.trim().split('\n').pop() || ''));
    });
    const r: any = JSON.parse(out);
    expect(r.seen).to.deep.equal(['one', 'two', 'three']);
    expect(r.uncaught).to.equal(1);
  });

  it('Should announce server_no_context_takeover only in shared mode', async (): Promise<void> => {
    const negotiate = (pmd: any, offer: string): Promise<string> => new Promise((resolve: (v: string) => void, reject: (e: any) => void): void => {
      const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: pmd }, (): void => {
        const socket: any = connect(port, '127.0.0.1');
        let buffer: string = '';
        socket.on('connect', (): void => socket.write(`GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Extensions: ${offer}\r\n\r\n`));
        socket.on('data', (d: Buffer): void => {
          buffer += d.toString('latin1');
          if (buffer.includes('\r\n\r\n')) { const m: RegExpExecArray | null = /Sec-WebSocket-Extensions: ([^\r]*)/.exec(buffer); socket.destroy(); wsServer.close((): void => resolve(m ? m[1] : '')); }
        });
        socket.on('error', reject);
      });
    });
    const browser: string = 'permessage-deflate; client_max_window_bits';
    const asksNoServer: string = 'permessage-deflate; server_no_context_takeover; client_max_window_bits';
    expect(await negotiate({ threshold: 128 }, browser)).to.contain('server_no_context_takeover');
    expect(await negotiate({ serverNoContextTakeover: false, threshold: 128 }, browser)).to.not.contain('server_no_context_takeover');
    expect(await negotiate({ serverNoContextTakeover: false, threshold: 128 }, asksNoServer)).to.contain('server_no_context_takeover');
  });

  it('Should count raw and wire bytes per server (stats)', async (): Promise<void> => {
    const text: string = 'a compressible message '.repeat(50);   // 1150 B, compressed
    const tiny: string = 'x';                                    // below threshold, plain
    const prepared: PreparedMessage = new PreparedMessage(Buffer.from(text));
    let statsBefore: any, statsAfter: any;
    const got: any = await new Promise((resolve: (v: any) => void, reject: (e: any) => void): void => {
      const messages: Buffer[] = [];
      const wsServer: WebSocketServer = new WebSocket.Server({ port, perMessageDeflate: { threshold: 64 } }, (): void => {
        statsBefore = wsServer.stats;
        const client: WSWebSocket = new WSWebSocket(`ws://localhost:${port}`, { perMessageDeflate: true });
        client.on('message', (data: Buffer): void => {
          messages.push(data);
          if (messages.length === 4) { statsAfter = wsServer.stats; client.close(); wsServer.close((): void => resolve(messages)); }
        });
        client.on('error', reject);
      });
      wsServer.on('connection', (ws: WebSocket): void => {
        ws.send(text);
        ws.send(tiny);
        ws.send(prepared, { prefix: 'p:', binary: false });
        ws.send(Buffer.from(text));
      });
    });
    expect(got.length).to.equal(4);
    const d: any = { messages: statsAfter.messages - statsBefore.messages, rawBytes: statsAfter.rawBytes - statsBefore.rawBytes, wireBytes: statsAfter.wireBytes - statsBefore.wireBytes, compressedMessages: statsAfter.compressedMessages - statsBefore.compressedMessages };
    expect(d.messages).to.equal(4);
    expect(d.rawBytes).to.equal(text.length * 3 + 1 + 2);
    expect(d.compressedMessages).to.equal(3);
    expect(d.wireBytes).to.be.greaterThan(0);
    expect(d.wireBytes).to.be.lessThan(d.rawBytes / 3);   // three of four messages compress ~10x
    // performance counters: three compressed messages, timed, and the worker did some work
    // Two of the four are compressed per-socket (text + Buffer); the prepared message is compressed
    // once at prepare time, not on this socket, so it does not add a compress call here.
    // The two per-socket compressible messages (text + Buffer) are compressed here in every mode;
    // the prepared blob is compressed at prepare time (worker) or first send (no worker).
    const p: any = { compressCalls: statsAfter.compressCalls - statsBefore.compressCalls, compressNanos: statsAfter.compressNanos - statsBefore.compressNanos };
    expect(p.compressCalls).to.be.greaterThanOrEqual(2);
    expect(p.compressNanos).to.be.greaterThan(0);
    expect(statsAfter.workerOps).to.be.greaterThanOrEqual(statsBefore.workerOps);
  });

  it('Should interleave callbacks, prepared payloads and plain sends in order', async (): Promise<void> => {
    const prepared: PreparedMessage = new PreparedMessage(Buffer.from('shared-payload'));
    const order: string[] = [];
    const got: any = await collect((ws: WebSocket): void => {
      ws.send('a');
      ws.send('b', undefined, (): void => { order.push('cb-b'); });
      ws.send(prepared, { binary: false, prefix: 'c:' });
      ws.send('d');
      ws.send('e', undefined, (): void => { order.push('cb-e'); });
      ws.send(prepared, { binary: false });
      ws.send('g');
    }, 7);
    expect(got.messages.map((m: Buffer) => m.toString())).to.deep.equal(['a', 'b', 'c:shared-payload', 'd', 'e', 'shared-payload', 'g']);
    expect(order).to.deep.equal(['cb-b', 'cb-e']);
  });
});
