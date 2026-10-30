#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>

class Stream
{
public:
    virtual ~Stream() = default;

    virtual size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) = 0;
    virtual size_t read(void* buffer, size_t size, TickType_t timeout = portMAX_DELAY) = 0;

    virtual size_t available() const { return 0; }
    virtual bool flush() { return true; }

    // ── Zero-copy handoff ─────────────────────────────────────────────────────
    //
    // For the handlers that move kilobytes — a firmware image to flash, a partition
    // back out — where owning a buffer to copy through is the whole cost. A stream
    // that already holds its bytes in a buffer can lend that buffer instead, so the
    // handler needs no buffer of its own: not on its task's stack (too big) and not
    // on the heap (fragmentation, and an allocation that can fail mid-write).
    //
    // Both directions are one capability, because the one stream that has it — a
    // channel over a transport's framing buffers — has it both ways.

    /// Does this stream lend its buffers? Asked once, up front: afterwards "nothing
    /// to lend" and "nothing left" both come back as zero, and a handler must not
    /// mistake a stream that cannot do this for an empty one.
    virtual bool canLend() const { return false; }

    /// Take the next run of already-received bytes and consume it. Returns 0 at the
    /// end of the input — ask failed() to tell that from a broken transport.
    virtual size_t lendInput(const uint8_t*& data) { data = nullptr; return 0; }

    /// A writable run of the output buffer, to be filled in place. `avail` is how
    /// much of it there is (never 0 unless the stream broke, in which case the
    /// return is null); commitOutput() publishes however much was actually filled.
    virtual uint8_t* lendOutput(size_t& avail) { avail = 0; return nullptr; }
    virtual void commitOutput(size_t n) { (void)n; }

    /// True when the stream stopped early because something went wrong, rather
    /// than because it ended. read() returns 0 for both, so a reader that cares
    /// about receiving *all* of the input — a firmware image, say — has to ask;
    /// otherwise a transport failure is indistinguishable from a complete stream.
    virtual bool failed() const { return false; }
};
