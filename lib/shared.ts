import { WebSocket } from './client';
import { WebSocketServer } from './server';

export const noop: () => void = (): void => { /** ignore */ };

export const OPCODE_TEXT: number = 1;
export const OPCODE_PING: number = 9;
export const OPCODE_BINARY: number = 2;

export const APP_PING_CODE: Buffer = Buffer.from('9');
export const PERMESSAGE_DEFLATE: number = 1;
export const SLIDING_DEFLATE_WINDOW: number = 16;
export const DEFAULT_PAYLOAD_LIMIT: number = 16777216;

export const native: any = ((): NodeRequire => {
  try {
    return require(`../dist/bindings/cws_${process.platform}_${process.arch}_node${process.versions.modules}`);
  } catch (err) {
    err.message = err.message + ` check './node_modules/@encharm/cws/build_log.txt' for post install build logs`;
    throw err;
  }
})();

/**
 * cWS links against Node's *private* internal headers (it pulls the raw `SSL*` out of TLSWrap and
 * the libuv handle out of TCPWrap). Those struct layouts change between minor releases of the same
 * Node major while `process.versions.modules` — the ABI number in the binding's filename — stays
 * the same, so npm installs a binding that loads cleanly and then reads the wrong offsets:
 * connections misbehave or crash instead of failing loudly. Node 24.7 -> 24.13 is a real example
 * (an Environment member changed from v8::LocalVector to std::deque, and two members were added).
 *
 * So compare the running Node against the one the binding was built for. A differing patch level
 * is fine; a differing minor is the dangerous case. Set CWS_IGNORE_NODE_MISMATCH=1 to silence.
 */
function checkNodeVersion(): void {
  const builtFor: string = String(native.builtForNode || '').replace(/^v/, '');
  if (!builtFor || process.env.CWS_IGNORE_NODE_MISMATCH) {
    return;   // binding predates the check, or the user opted out
  }
  const running: string = process.versions.node;
  if (builtFor === running) {
    return;
  }
  const [bMajor, bMinor]: string[] = builtFor.split('.');
  const [rMajor, rMinor]: string[] = running.split('.');
  if (bMajor === rMajor && bMinor === rMinor) {
    return;   // same minor: private layouts are stable within a patch line
  }
  // tslint:disable-next-line
  console.error(
    `@encharm/cws: this prebuilt binding was compiled against Node v${builtFor} but you are running ` +
    `Node v${running}. Node's internal struct layouts change between minor versions even though the ` +
    `addon ABI (${process.versions.modules}) does not, and cWS uses those internals, so sockets can ` +
    `fail in confusing ways. Install Node v${builtFor}, or rebuild cWS for your Node with ` +
    `CWS_FORCE_BUILD=1 npm rebuild @encharm/cws. Set CWS_IGNORE_NODE_MISMATCH=1 to silence this.`);
}
checkNodeVersion();

export const zlibBackend: string = native.zlibBackend;
/** Send worker thread status: 'active', or the reason it is not. */
export const sendThread: string = native.sendThread;
/** Receive worker thread status: 'active' once a server enabled it, 'not started', or why it cannot run. */
export function recvThread(): string {
  return native.recvThread;
}

export function setupNative(group: any, type: string, wsServer?: WebSocketServer): void {
  native.setNoop(noop);

  native[type].group.onConnection(group, (external: any): void => {
    if (type === 'server' && wsServer) {
      const socket: WebSocket = new WebSocket(null, { external, compressThreshold: wsServer.compressThreshold });
      native.setUserData(external, socket);

      if (wsServer.upgradeCb) {
        wsServer.upgradeCb(socket);
      } else {
        wsServer.registeredEvents['connection'](socket, wsServer.upgradeReq);
      }

      wsServer.upgradeCb = null;
      wsServer.upgradeReq = null;
      return;
    }

    const webSocket: WebSocket = native.getUserData(external);
    (webSocket as any).external = external;
    webSocket.registeredEvents['open']();
  });

  native[type].group.onPing(group, (message: string | Buffer, webSocket: WebSocket): void => {
    webSocket.registeredEvents['ping'](message);
  });

  native[type].group.onPong(group, (message: string | Buffer, webSocket: WebSocket): void => {
    webSocket.registeredEvents['pong'](message);
  });

  native[type].group.onMessage(group, (message: string | Buffer, webSocket: WebSocket): void => {
    webSocket.registeredEvents['message'](message);
  });

  native[type].group.onDisconnection(group, (newExternal: any, code: number, message: any, webSocket: WebSocket): void => {
    (webSocket as any).external = null;

    process.nextTick((): void => {
      webSocket.registeredEvents['close'](code || 1005, message || '');
    });

    native.clearUserData(newExternal);
  });

  if (type === 'client') {
    native[type].group.onError(group, (webSocket: WebSocket): void => {
      process.nextTick((): void => {
        webSocket.registeredEvents['error']({
          message: 'cWs client connection error',
          stack: 'cWs client connection error'
        });
      });
    });
  }
}
