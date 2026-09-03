import { WebSocketServer } from './server';
import { SocketAddress, ServerConfigs } from './index';
import { native, setupNative, noop, DEFAULT_PAYLOAD_LIMIT, OPCODE_PING, OPCODE_BINARY, OPCODE_TEXT } from './shared';
import { PreparedMessage } from './prepared';

const clientGroup: any = native.client.group.create(0, DEFAULT_PAYLOAD_LIMIT);

function messageByteLength(message: any): number {
  if (typeof message === 'string') {
    return Buffer.byteLength(message);
  }
  return message && typeof message.byteLength === 'number' ? message.byteLength : 0;
}
setupNative(clientGroup, 'client');

export class WebSocket {
  public static OPEN: number = 1;
  public static CLOSED: number = 3;
  public static Server: new (options: ServerConfigs, cb?: () => void) => WebSocketServer = WebSocketServer;

  public OPEN: number = WebSocket.OPEN;
  public CLOSED: number = WebSocket.OPEN;
  public registeredEvents: any = {
    open: noop,
    ping: noop,
    pong: noop,
    error: noop,
    close: noop,
    message: noop
  };

  private external: any;
  private socketType: string = 'client';
  private compressThreshold: number | undefined;
  // native.server / native.client, resolved once instead of per call
  private nativeApi: any = native.client;

  constructor(public url: string, private options: any = {}) {
    if (!this.url && (this.options as any).external) {
      this.socketType = 'server';
      this.nativeApi = native.server;
      this.external = (this.options as any).external;
      this.compressThreshold = (this.options as any).compressThreshold;
    } else {
      native.connect(clientGroup, url, this);
    }
  }

  public get bufferedAmount(): number {
    return this.external ? native.getBufferedAmount(this.external) : 0;
  }

  public get _socket(): SocketAddress {
    const address: any[] = this.external ? native.getAddress(this.external) : new Array(3);
    return {
      remotePort: address[0],
      remoteAddress: address[1],
      remoteFamily: address[2]
    };
  }

  public get readyState(): number {
    return this.external ? this.OPEN : this.CLOSED;
  }

  public set onopen(listener: () => void) {
    this.on('open', listener);
  }

  public set onclose(listener: (code?: number, reason?: string) => void) {
    this.on('close', listener);
  }

  public set onerror(listener: (err: Error) => void) {
    this.on('error', listener);
  }

  public set onmessage(listener: (message: string | any) => void) {
    this.on('message', listener);
  }

  public on(event: 'open', listener: () => void): void;
  public on(event: 'ping', listener: () => void): void;
  public on(event: 'pong', listener: () => void): void;
  public on(event: 'error', listener: (err: Error) => void): void;
  public on(event: 'message', listener: (message: string | any) => void): void;
  public on(event: 'close', listener: (code?: number, reason?: string) => void): void;
  public on(event: string, listener: (...args: any[]) => void): void {
    if (this.registeredEvents[event] === undefined) {
      console.warn(`cWS does not support '${event}' event`);
      return;
    }

    if (typeof listener !== 'function') {
      throw new Error(`Listener for '${event}' event must be a function`);
    }

    if (this.registeredEvents[event] !== noop) {
      console.warn(`cWS does not support multiple listeners for the same event. Old listener for '${event}' event will be overwritten`);
    }

    this.registeredEvents[event] = listener;
  }

  /**
   * `message` may be a `PreparedMessage`: a payload prepared once for many sockets. It is then
   * neither copied nor compressed per socket; `options.prefix` (a per-socket header) is spliced
   * in front of it inside the same frame. Prepared messages are binary unless `binary: false`.
   */
  public send(
    message: string | Buffer | ArrayBufferView | PreparedMessage,
    options?: { binary?: boolean, compress?: boolean, prefix?: string | Buffer | ArrayBufferView },
    cb?: (err?: Error) => void
  ): void {
    if (this.external) {
      const prepared: boolean = message instanceof PreparedMessage;
      let opCode: number = typeof message === 'string' ? OPCODE_TEXT : OPCODE_BINARY;

      // provided options should always overwrite default
      if (options && options.binary === false) {
        opCode = OPCODE_TEXT;
      }

      if (options && options.binary === true) {
        opCode = OPCODE_BINARY;
      }

      // Explicit `compress` wins; otherwise compress when permessage-deflate was negotiated
      // for this server and the message meets the configured size threshold.
      let compress: boolean = false;
      if (options && options.compress !== undefined) {
        compress = !!options.compress;
      } else if (this.compressThreshold !== undefined) {
        compress = messageByteLength(message) + (options && options.prefix ? messageByteLength(options.prefix) : 0) >= this.compressThreshold;
      }

      const callback: (() => void) | null = cb ? (): void => process.nextTick(cb) : null;
      if (prepared) {
        this.nativeApi.sendShared(this.external, options && options.prefix, (message as PreparedMessage).external, opCode, callback, compress);
      } else {
        this.nativeApi.send(this.external, message, opCode, callback, compress);
      }
    } else if (cb) {
      cb(new Error('Socket not connected'));
    }
  }

  public ping(message?: string | Buffer): void {
    if (this.external) {
      this.nativeApi.send(this.external, message, OPCODE_PING);
    }
  }

  public close(code: number = 1000, reason?: string): void {
    if (this.external) {
      this.nativeApi.close(this.external, code, reason);
      this.external = null;
    }
  }

  public terminate(): void {
    if (this.external) {
      this.nativeApi.terminate(this.external);
      this.external = null;
    }
  }
}
