"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.PreparedMessage = void 0;
const shared_1 = require("./shared");
const preparedRegistry = typeof global.FinalizationRegistry === 'function'
    ? new global.FinalizationRegistry((external) => shared_1.native.server.releaseShared(external))
    : undefined;
class PreparedMessage {
    constructor(data) {
        this.byteLength = data.byteLength;
        this.external = shared_1.native.server.prepareShared(data);
        if (preparedRegistry) {
            preparedRegistry.register(this, this.external);
        }
    }
}
exports.PreparedMessage = PreparedMessage;
