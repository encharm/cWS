import { native } from './shared';

// Releases the native payload once the JS handle is collected. Native sends hold their
// own references, so an in-flight payload outlives its handle.
const preparedRegistry: any = typeof (global as any).FinalizationRegistry === 'function'
  ? new (global as any).FinalizationRegistry((external: any): void => native.server.releaseShared(external))
  : undefined;

/**
 * A payload prepared once and passed to `send` for many sockets (`send(prepared, { prefix })`). The bytes are copied into native
 * memory at construction; the first compressed send also caches their deflate blocks, so a
 * fan-out costs no serialization, copy or compression per recipient.
 */
export class PreparedMessage {
  public readonly byteLength: number;
  /** @internal native handle */
  public readonly external: any;

  constructor(data: Buffer | ArrayBufferView | ArrayBuffer) {
    this.byteLength = data.byteLength;
    this.external = native.server.prepareShared(data);
    if (preparedRegistry) {
      preparedRegistry.register(this, this.external);
    }
  }
}
