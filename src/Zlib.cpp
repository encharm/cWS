#include "Zlib.h"

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

struct Stream {
    ZStream zs;
    bool isDeflate;
};

typedef decltype(ZStream::next_in) InPtr;
typedef decltype(ZStream::next_out) OutPtr;

Stream *createDeflate(int level, int windowBits, int memLevel) {
    Stream *stream = new Stream();
    stream->isDeflate = true;
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
    if (stream->isDeflate) {
        ZFN(deflateReset)(&stream->zs);
    } else {
        ZFN(inflateReset)(&stream->zs);
    }
}

void destroy(Stream *stream) {
    if (!stream) {
        return;
    }
    if (stream->isDeflate) {
        ZFN(deflateEnd)(&stream->zs);
    } else {
        ZFN(inflateEnd)(&stream->zs);
    }
    delete stream;
}

char *deflate(Stream *stream, char *data, size_t &length, char *buffer, size_t bufferSize, std::string &dynamic, bool resetAfter) {
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

const char *backend() {
    return CWS_ZLIB_BACKEND;
}

}
}
