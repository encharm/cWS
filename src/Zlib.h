#ifndef CWS_ZLIB_H
#define CWS_ZLIB_H

#include <cstddef>
#include <string>

// Thin backend-neutral wrapper around the DEFLATE library. Built with
// -DCWS_ZLIB_NG (the prebuilt bindings) it uses the vendored zlib-ng through
// its native zng_ API, so nothing collides with the zlib inside Node; without
// the define (node-gyp fallback) it uses zlib.h from the Node headers. Zlib.cpp
// is the only translation unit that includes either header.
namespace cWS {
namespace zlib {

struct Stream;

Stream *createDeflate(int level, int windowBits, int memLevel);
Stream *createInflate(int windowBits = 15);
void reset(Stream *stream);
void destroy(Stream *stream);

// Deflates one message with a sync flush. Output lands in `buffer` (bufferSize
// bytes) or, when it does not fit, is accumulated in `dynamic`. Returns the output
// pointer and sets `length` to its size minus the 4-byte sync-flush tail
// (RFC 7692). With `resetAfter` the stream forgets the message (no context takeover).
char *deflate(Stream *stream, char *data, size_t &length, char *buffer, size_t bufferSize, std::string &dynamic, bool resetAfter);

// Inflates one message (the 4-byte tail must already be appended). Returns nullptr
// and length 0 on error or when the output would exceed maxPayload. Always resets.
char *inflate(Stream *stream, char *data, size_t &length, size_t maxPayload, char *buffer, size_t bufferSize, std::string &dynamic);

// Compresses one independent message (no context takeover). Uses the built-in
// microdeflate encoder unless CWS_MICRO_DEFLATE=0, in which case `fallback` (a
// shared zlib stream, reset after) is used. Same output contract as deflate().
char *deflateIndependent(Stream *fallback, char *data, size_t &length, char *buffer, size_t bufferSize, std::string &dynamic);
bool microDeflateEnabled();

// e.g. "zlib-ng 2.2.4" or "zlib 1.3.1"
const char *backend();

}
}

#endif // CWS_ZLIB_H
