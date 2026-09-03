import * as HTTP from 'http';
import * as HTTPS from 'https';

export type VerifyClientNext = (verified: boolean, code?: number, message?: string) => void;

export type SocketAddress = {
  remotePort?: number,
  remoteAddress?: string,
  remoteFamily?: string
};

export type ConnectionInfo = {
  req: HTTP.IncomingMessage,
  secure: boolean
  origin?: string,
};

export type ServerConfigs = {
  path?: string,
  port?: number,
  host?: string,
  server?: HTTP.Server | HTTPS.Server,
  noDelay?: boolean,
  noServer?: boolean,
  maxPayload?: number,
  /**
   * `true` enables permessage-deflate with a shared compressor (no context takeover).
   * `serverNoContextTakeover: false` keeps a per-socket sliding window (streaming compression,
   * much better on small messages); `windowBits` (9..15) and `memLevel` (1..9) pick its memory
   * tier: 15/8 = ~256 KB per socket, 12/5 = ~32 KB, 10/3 = ~8 KB. Messages of at least `threshold`
   * bytes are compressed (default 0 = all); `send(..., { compress })` overrides per message. `level` (1..9,
   * default 2) is the deflate level of the per-socket compressor: with the bundled zlib-ng, 1 is the very fast
   * "quick" strategy (~8% worse ratio than zlib level 1), 2 matches zlib level 1's ratio at higher speed.
   */
  perMessageDeflate?: boolean | { serverNoContextTakeover?: boolean, windowBits?: number, memLevel?: number, level?: number, threshold?: number },
  verifyClient?: (info: ConnectionInfo, next: VerifyClientNext) => void
};

export { WebSocket } from './client';
export { PreparedMessage } from './prepared';
export { WebSocketServer } from './server';

export const secureProtocol: string = 'TLSv1_2_method';

/** DEFLATE implementation compiled into the native binding, e.g. 'zlib-ng 2.2.4'. */
export { zlibBackend, sendThread } from './shared';