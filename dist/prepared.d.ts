/// <reference types="node" />
export declare class PreparedMessage {
    readonly byteLength: number;
    readonly external: any;
    constructor(data: Buffer | ArrayBufferView | ArrayBuffer);
}
