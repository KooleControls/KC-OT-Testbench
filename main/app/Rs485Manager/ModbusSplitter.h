#pragma once

#include "ModbusFrame.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ──────────────────────────────────────────────────────────────
// Cuts the bytes heard on an RS485 bus into Modbus RTU frames, and whatever lies
// between them into noise.
//
// RTU ends a frame with a silence this side cannot see (ModbusFrame.h), so a frame is
// found by its length -- read from its own bytes, once as a request and once as a
// reply -- and confirmed by its CRC. Which reading is tried first is the caller's
// hint: after a request to someone else, the next frame is most likely their reply
// (ExpectReply). Both are always tried, so a wrong hint costs nothing but the order.
//
// Bytes that start no frame are moved to noise one at a time, until a frame starts.
// Noise is handed on when a frame is found after it, or when the caller says the bus
// went quiet -- which also ends a half-read frame: it was never going to be one.
//
// Noise that happens to read as the start of a long frame holds what follows it until
// the bus goes quiet, so a frame straight after it is lost with it. That is the price
// of not looking for frames inside a half-read one, which would now and then cut a long
// frame in two. On a Modbus bus the quiet comes: a master waits out its timeout after a
// request that got no answer.
//
// No timing and no I/O, so it can be tested anywhere; the RS485 task feeds it.
// ──────────────────────────────────────────────────────────────
class ModbusSplitter
{
public:
    enum class Kind : uint8_t { Request, Reply };

    /// How many noise bytes are held for the record; a longer run is counted, not kept.
    static constexpr size_t NOISE_KEEP = 64;

    /// After a request to someone who has not answered yet: try a reply first.
    void ExpectReply(bool expect) { expectReply_ = expect; }

    /// Whether part of a frame is waiting for the rest.
    bool Partial() const { return len_ > 0; }

    /// Add bytes off the bus. Every frame they complete goes to
    /// `onFrame(const uint8_t* adu, size_t len, Kind kind)`, and the noise in front of it
    /// to `onNoise(const uint8_t* held, size_t heldLen, size_t total)` first. Both are
    /// called before Feed returns, and may call ExpectReply.
    template <typename OnFrame, typename OnNoise>
    void Feed(const uint8_t* data, size_t n, OnFrame&& onFrame, OnNoise&& onNoise)
    {
        while (n > 0)
        {
            if (len_ == sizeof(buf_))
            {
                // Full, and no frame in it: the oldest byte was never a frame's.
                AddNoise(buf_, 1);
                Consume(1);
            }
            const size_t k = std::min(n, sizeof(buf_) - len_);
            memcpy(buf_ + len_, data, k);
            len_ += k;
            data += k;
            n -= k;
            Split(onFrame, onNoise);
        }
    }

    /// The bus went quiet: a half-read frame is noise, and the noise is handed on.
    template <typename OnNoise>
    void Quiet(OnNoise&& onNoise)
    {
        if (len_ > 0)
        {
            AddNoise(buf_, len_);
            len_ = 0;
        }
        FlushNoise(onNoise);
    }

private:
    uint8_t buf_[modbus::MAX_ADU];
    size_t len_ = 0;
    uint8_t noise_[NOISE_KEEP];
    size_t noiseLen_ = 0;
    bool expectReply_ = false;

    template <typename OnFrame, typename OnNoise>
    void Split(OnFrame& onFrame, OnNoise& onNoise)
    {
        while (len_ > 0)
        {
            Kind kind = Kind::Request;
            size_t frameLen = 0;
            bool needMore = false;
            if (Match(kind, frameLen, needMore))
            {
                FlushNoise(onNoise);
                onFrame(static_cast<const uint8_t*>(buf_), frameLen, kind);
                Consume(frameLen);
                continue;
            }
            if (needMore)
                return;

            // Starts no frame at all: the first byte is noise, try from the next.
            AddNoise(buf_, 1);
            Consume(1);
        }
    }

    bool Match(Kind& kind, size_t& frameLen, bool& needMore) const
    {
        const Kind first = expectReply_ ? Kind::Reply : Kind::Request;
        const Kind second = expectReply_ ? Kind::Request : Kind::Reply;

        needMore = false;
        for (Kind k : { first, second })
        {
            const size_t n = k == Kind::Request ? modbus::RequestLength(buf_, len_)
                                                : modbus::ReplyLength(buf_, len_);
            if (n == modbus::UNKNOWN || n > modbus::MAX_ADU)
                continue;
            if (n == modbus::NEED_MORE || n > len_)
            {
                needMore = true;
                continue;
            }
            if (modbus::CrcOk(buf_, n))
            {
                kind = k;
                frameLen = n;
                return true;
            }
        }
        return false;
    }

    void Consume(size_t n)
    {
        memmove(buf_, buf_ + n, len_ - n);
        len_ -= n;
    }

    void AddNoise(const uint8_t* data, size_t n)
    {
        for (size_t i = 0; i < n; ++i, ++noiseLen_)
            if (noiseLen_ < NOISE_KEEP)
                noise_[noiseLen_] = data[i];
    }

    template <typename OnNoise>
    void FlushNoise(OnNoise& onNoise)
    {
        if (noiseLen_ == 0)
            return;
        onNoise(static_cast<const uint8_t*>(noise_), noiseLen_ < NOISE_KEEP ? noiseLen_ : NOISE_KEEP, noiseLen_);
        noiseLen_ = 0;
    }
};
