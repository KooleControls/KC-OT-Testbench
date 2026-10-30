#pragma once

#include <cstddef>
#include <cstdint>

// How a channel frame crosses a link that has no message boundaries of its own: a UART.
//
// A WebSocket hands Transport whole messages, so a channel frame is one message and
// nothing more is needed. A UART hands it bytes, some of them noise from a board
// powering up, so the frame has to carry its own boundaries and its own check:
//
//     0x00 | COBS( [channel|flags|payload] | crc16 LE ) | 0x00
//
// COBS because it leaves 0x00 as the only delimiter and costs one byte in 254, so a
// receiver that loses sync -- a dropped byte, a peer that reset mid-frame -- is back in
// step at the very next 0x00 without escaping or a length field to distrust. The
// leading 0x00 is what ends whatever half-frame the receiver was holding when this
// one started; an empty frame between two delimiters is not a frame.
//
// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the decoded frame. A frame that
// fails it is dropped and counted, never delivered: on this link a lost frame is a
// channel that times out, and a corrupted one is a command nobody sent.
//
// Pure: no platform, no transport, no allocation. Both ends of the PCB1245 <-> PCB1246
// link carry this file unchanged.
namespace serial_framing
{
    inline constexpr uint8_t DELIMITER = 0x00;
    inline constexpr size_t  CRC_LEN   = 2;

    /// The most bytes Encode() writes for a frame of `len`: both delimiters, the CRC,
    /// and one code byte per 254 bytes of data.
    constexpr size_t EncodedMax(size_t len)
    {
        return 1 + 1 + (len + CRC_LEN) + (len + CRC_LEN) / 254 + 1;
    }

    inline uint16_t Crc16(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF)
    {
        while (n--)
        {
            crc ^= static_cast<uint16_t>(*p++) << 8;
            for (int i = 0; i < 8; ++i)
                crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                     : static_cast<uint16_t>(crc << 1);
        }
        return crc;
    }

    /// Frame `len` bytes into `out`, which must hold EncodedMax(len). Returns the
    /// number of bytes written, delimiters included.
    inline size_t Encode(const uint8_t* frame, size_t len, uint8_t* out)
    {
        size_t  w      = 0;
        out[w++]       = DELIMITER;
        size_t  codeAt = w++;
        uint8_t code   = 1;

        auto put = [&](uint8_t b)
        {
            if (b == 0)
            {
                out[codeAt] = code;
                codeAt = w++;
                code = 1;
                return;
            }
            out[w++] = b;
            if (++code == 0xFF)          // a full block: 254 data bytes, no zero after
            {
                out[codeAt] = code;
                codeAt = w++;
                code = 1;
            }
        };

        for (size_t i = 0; i < len; ++i) put(frame[i]);

        const uint16_t crc = Crc16(frame, len);
        put(static_cast<uint8_t>(crc & 0xFF));
        put(static_cast<uint8_t>(crc >> 8));

        out[codeAt] = code;
        out[w++] = DELIMITER;
        return w;
    }
}

/// The receiving half: fed one byte at a time, as they arrive, and says when a frame
/// is complete. Decodes in place into a buffer the owner provides, so nothing is held
/// twice.
class SerialFrameDecoder
{
public:
    enum class Result : uint8_t
    {
        None,      // nothing complete yet
        Frame,     // Data()/Length() hold a checked frame until the next Push()
        TooLong,   // a frame bigger than the buffer arrived and was discarded whole
        Corrupt,   // a frame arrived and failed its CRC or its COBS; discarded
    };

    /// `buf` holds `cap` bytes; the largest frame it accepts is cap - CRC_LEN.
    SerialFrameDecoder(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    Result Push(uint8_t b)
    {
        if (b == serial_framing::DELIMITER)
            return End();

        inFrame_ = true;
        if (remaining_ == 0)
        {
            // A code byte. The block before it ended in a zero unless it was a full
            // one; the zero after the LAST block is implied and never emitted, which
            // is why it is held back until a next block proves it was real.
            if (zeroPending_) Emit(0);
            remaining_   = static_cast<uint8_t>(b - 1);
            zeroPending_ = (b != 0xFF);
        }
        else
        {
            Emit(b);
            remaining_--;
        }
        return Result::None;
    }

    const uint8_t* Data() const { return buf_; }
    size_t Length() const { return length_; }

    /// Forget a half-received frame: a byte stream that stopped mid-frame is not
    /// going to finish it.
    void Reset()
    {
        len_ = 0; remaining_ = 0;
        zeroPending_ = false; overflow_ = false; inFrame_ = false;
    }

private:
    uint8_t* buf_;
    size_t   cap_;
    size_t   len_ = 0;          // decoded bytes so far, CRC included
    size_t   length_ = 0;       // the last complete frame, CRC excluded
    uint8_t  remaining_ = 0;    // data bytes left in the current COBS block
    bool     zeroPending_ = false;
    bool     overflow_ = false;
    bool     inFrame_ = false;  // any byte since the last delimiter

    void Emit(uint8_t b)
    {
        if (len_ < cap_) buf_[len_++] = b;
        else overflow_ = true;
    }

    Result End()
    {
        if (!inFrame_) return Result::None;     // back-to-back delimiters: no frame

        Result r = Result::Frame;
        if (overflow_)
            r = Result::TooLong;
        else if (remaining_ != 0 || len_ <= serial_framing::CRC_LEN)
            r = Result::Corrupt;                // cut short, or nothing but a CRC
        else
        {
            const size_t n = len_ - serial_framing::CRC_LEN;
            const uint16_t want = static_cast<uint16_t>(buf_[n] | (buf_[n + 1] << 8));
            if (serial_framing::Crc16(buf_, n) != want) r = Result::Corrupt;
            else length_ = n;
        }

        Reset();
        return r;
    }
};
