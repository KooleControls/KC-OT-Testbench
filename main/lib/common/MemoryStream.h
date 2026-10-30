#pragma once

#include "Stream.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

// A stream backed by memory: a read-only Stream view over an existing
// byte range. NOT the same thing as BufferStream — that one is about
// buffering/chunking I/O; this one adapts bytes-already-in-RAM (a
// received WebSocket frame, a serial line buffer) to a Stream consumer.
class MemoryStream : public Stream
{
    const uint8_t* buf_;
    size_t len_;
    size_t pos_ = 0;

public:
    MemoryStream(const void* buf, size_t len)
        : buf_(static_cast<const uint8_t*>(buf)), len_(len) {}

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;   // data is already in memory; never blocks
        size_t n = std::min(size, len_ - pos_);
        memcpy(dst, buf_ + pos_, n);
        pos_ += n;
        return n;
    }

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)data; (void)size; (void)timeout;
        return 0;   // read-only view
    }

    size_t available() const override { return len_ - pos_; }
};
