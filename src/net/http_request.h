#pragma once
#include <stdint.h>

// Streaming HTTP/1.x header reader for a small stack.
//
// Browsers send headers the server never reads, such as Cookie,
// User-Agent and Accept. The reader validates every header line as it
// streams but keeps only the request line plus Content-Length and
// Transfer-Encoding. The whole header section may reach
// MAX_HEADER_BYTES; only the kept text must fit in KEPT_CAP.
class HttpRequestReader {
public:
    static constexpr uint16_t KEPT_CAP = 512;
    static constexpr uint16_t MAX_HEADER_BYTES = 8192;
    static constexpr uint8_t  NAME_CAP = 32;

    static constexpr uint16_t NEED_MORE = 0;
    static constexpr uint16_t DONE = 1;

    HttpRequestReader() { reset(); }
    void reset();

    // Returns NEED_MORE, DONE, or an HTTP error status (400 or 431).
    // After DONE, kept() holds the request line and kept header lines,
    // each CRLF-terminated, followed by the blank CRLF line.
    uint16_t feed(char c);

    char*    kept() { return _kept; }
    uint16_t total() const { return _total; }

private:
    enum Phase : uint8_t { REQUEST_LINE, HEADER_NAME, HEADER_VALUE, FINISHED };

    bool     append(char c);
    bool     appendText(const char* s, uint16_t n);
    uint16_t endLine();

    char     _kept[KEPT_CAP];
    uint16_t _kept_len;
    uint16_t _total;
    Phase    _phase;
    bool     _pending_cr;
    bool     _keep_line;
    char     _name[NAME_CAP];
    uint8_t  _name_len;
    bool     _name_long;
};

// Parses the kept text from HttpRequestReader. Returns 0 on success or an
// HTTP error status: 400 for bad framing, 413 for a body over 511 bytes.
uint16_t parseRequestHeaders(char* headers, char* method, char* path,
                             uint16_t* body_length, bool* has_length);
