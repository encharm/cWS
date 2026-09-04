#include "Zlib.h"
#include "MicroDeflate.h"
#include <cstdlib>
#include <cstring>

#ifdef CWS_ZLIB_NG
#include "zlib-ng.h"
typedef zng_stream ZStream;
#define ZFN(name) zng_##name
#define CWS_ZLIB_BACKEND "zlib-ng " ZLIBNG_VERSION
#else
#include <zlib.h>
typedef z_stream ZStream;
#define ZFN(name) name
#define CWS_ZLIB_BACKEND "zlib " ZLIB_VERSION
#endif

namespace cWS {
namespace zlib {

bool microDeflateEnabled();

struct Stream {
    ZStream zs;
    bool isDeflate;
    // Level-1 deflate streams with context takeover use the microdeflate window instead of
    // the library (same fixed-tree output family, ~2x faster, 64 KB per stream); zs is unused then.
    microdeflate::Window *window = nullptr;
};

typedef decltype(ZStream::next_in) InPtr;
typedef decltype(ZStream::next_out) OutPtr;

Stream *createDeflate(int level, int windowBits, int memLevel) {
    Stream *stream = new Stream();
    stream->isDeflate = true;
    if (level <= 1 && microDeflateEnabled()) {
        stream->window = new microdeflate::Window(windowBits);
        return stream;
    }
    ZFN(deflateInit2)(&stream->zs, level, Z_DEFLATED, -windowBits, memLevel, Z_DEFAULT_STRATEGY);
    return stream;
}

Stream *createInflate(int windowBits) {
    Stream *stream = new Stream();
    stream->isDeflate = false;
    ZFN(inflateInit2)(&stream->zs, -windowBits);
    return stream;
}

void reset(Stream *stream) {
    if (stream->window) {
        stream->window->reset();
    } else if (stream->isDeflate) {
        ZFN(deflateReset)(&stream->zs);
    } else {
        ZFN(inflateReset)(&stream->zs);
    }
}

void destroy(Stream *stream) {
    if (!stream) {
        return;
    }
    if (stream->window) {
        delete stream->window;
    } else if (stream->isDeflate) {
        ZFN(deflateEnd)(&stream->zs);
    } else {
        ZFN(inflateEnd)(&stream->zs);
    }
    delete stream;
}

// The input may live inside the output scratch buffer: a message inflated into the hub's
// buffer and echoed from the JS handler is compressed on the main thread from exactly there.
// Compressing in place would clobber it, so such input is copied first (only that case pays).
static const char *unalias(const char *data, size_t length, const char *buffer, size_t bufferSize, std::string &copy) {
    if (data < buffer + bufferSize && data + length > buffer) {
        copy.assign(data, length);
        return copy.data();
    }
    return data;
}

char *deflate(Stream *stream, char *data, size_t &length, char *buffer, size_t bufferSize, std::string &dynamic, bool resetAfter) {
    std::string aliased;
    data = (char *) unalias(data, length, buffer, bufferSize, aliased);
    if (stream->window) {
        size_t need = microdeflate::bound(length);
        uint8_t *out;
        if (need <= bufferSize) {
            out = (uint8_t *) buffer;
        } else {
            dynamic.resize(need);
            out = (uint8_t *) &dynamic[0];
        }
        length = stream->window->compress((const uint8_t *) data, length, out);
        if (resetAfter) {
            stream->window->reset();
        }
        return (char *) out;
    }
    ZStream &zs = stream->zs;
    dynamic.clear();

    zs.next_in = (InPtr) data;
    zs.avail_in = (unsigned int) length;

    // note: zlib requires more than 6 bytes of output space with Z_SYNC_FLUSH
    int err;
    do {
        zs.next_out = (OutPtr) buffer;
        zs.avail_out = (unsigned int) bufferSize;

        err = ZFN(deflate)(&zs, Z_SYNC_FLUSH);
        if (Z_OK == err && zs.avail_out == 0) {
            dynamic.append(buffer, bufferSize - zs.avail_out);
            continue;
        } else {
            break;
        }
    } while (true);

    // note: must not change avail_out
    if (resetAfter) {
        ZFN(deflateReset)(&zs);
    }

    if (dynamic.length()) {
        dynamic.append(buffer, bufferSize - zs.avail_out);
        length = dynamic.length() - 4;
        return (char *) dynamic.data();
    }

    length = bufferSize - zs.avail_out - 4;
    return buffer;
}

bool supportsHistoryAppend(Stream *stream) {
    return stream && stream->window;
}

void appendHistory(Stream *stream, const char *data, size_t length) {
    if (stream && stream->window) {
        stream->window->append((const uint8_t *) data, length);
    }
}

char *inflate(Stream *stream, char *data, size_t &length, size_t maxPayload, char *buffer, size_t bufferSize, std::string &dynamic) {
    ZStream &zs = stream->zs;
    dynamic.clear();

    zs.next_in = (InPtr) data;
    zs.avail_in = (unsigned int) length;

    int err;
    do {
        zs.next_out = (OutPtr) buffer;
        zs.avail_out = (unsigned int) bufferSize;
        err = ZFN(inflate)(&zs, Z_FINISH);
        // Z_STREAM_END: the sender ended the message with a BFINAL=1 block, which
        // RFC 7692 section 7.2.3.4 permits (libdeflate and some non-zlib clients do
        // this); anything left in the input is the 4-byte tail and is ignored.
        if (!zs.avail_in || err == Z_STREAM_END) {
            break;
        }

        dynamic.append(buffer, bufferSize - zs.avail_out);
    } while (err == Z_BUF_ERROR && dynamic.length() <= maxPayload);

    ZFN(inflateReset)(&zs);

    if ((err != Z_BUF_ERROR && err != Z_OK && err != Z_STREAM_END) || dynamic.length() > maxPayload) {
        length = 0;
        return nullptr;
    }

    if (dynamic.length()) {
        dynamic.append(buffer, bufferSize - zs.avail_out);
        length = dynamic.length();
        return (char *) dynamic.data();
    }

    length = bufferSize - zs.avail_out;
    return buffer;
}

bool microDeflateEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("CWS_MICRO_DEFLATE");
        enabled = !(v && (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "no")));
    }
    return enabled == 1;
}

char *deflateIndependent(Stream *fallback, char *data, size_t &length, char *buffer, size_t bufferSize, std::string &dynamic) {
    std::string aliased;
    data = (char *) unalias(data, length, buffer, bufferSize, aliased);
    if (!microDeflateEnabled()) {
        return deflate(fallback, data, length, buffer, bufferSize, dynamic, true);
    }
    static thread_local microdeflate::Encoder *encoder = nullptr;
    if (!encoder) {
        encoder = new microdeflate::Encoder();
    }
    size_t need = microdeflate::bound(length);
    uint8_t *out;
    if (need <= bufferSize) {
        out = (uint8_t *) buffer;
    } else {
        dynamic.resize(need);
        out = (uint8_t *) &dynamic[0];
    }
    length = encoder->compress((const uint8_t *) data, length, out);
    return (char *) out;
}

const char *backend() {
    return microDeflateEnabled() ? CWS_ZLIB_BACKEND " + microdeflate" : CWS_ZLIB_BACKEND;
}

}
}
