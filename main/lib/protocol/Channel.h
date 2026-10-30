#pragma once

#include "Stream.h"
#include "Transport.h"
#include "ChannelProtocol.h"
#include "esp_log.h"
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cstring>

// A channel's stream, and the only thing that crosses the transport/dispatch
// boundary: a transport turns wire bytes into one of these, the dispatcher turns
// it into a handler call. There is no layer in between — a transport constructs a
// Channel on its own stack, feeds it the first chunk, and hands it to
// CommandManager::Execute.
//
// NEITHER DIRECTION IS SIZED BY THE TRANSPORT. write() splits the reply at
// whatever payload capacity it was handed, emitting non-final DATA chunks; read()
// pulls further chunks until one carries FLAG_FINAL. So a Channel of any length
// works over a link that frames in 256 bytes or 4096, and two peers on one link
// need not have chosen the same number. The only rule is the one a sender cannot
// break silently: a single chunk must fit the RECEIVER's buffer, which is why
// RecvChunk reports overrunning it as a channel fault rather than a dead link.
//
// read() = the request bytes; write() = the reply, accumulated and flushed as
// binary DATA chunks, closed by finish() with FLAG_FINAL. The reply is assembled
// directly into an EXTERNAL framing buffer (owned by the transport, off the
// task stack): payload goes after the 3-byte header slot, so a flush is one
// SendRaw with no extra copy.
//
// The request is a run of channel chunks that share this id: the first is fed
// up front (feedRequest); once it's drained, read() pulls further chunks off
// the link (RecvChunk) until a chunk carries FLAG_FINAL. A small no-body command
// is a single FLAG_FINAL chunk, so read() never blocks; a streamed upload is many
// chunks ending in FLAG_FINAL.
//
// Both buffers are also lent out as-is (Stream::canLend and friends), which is what
// lets a handler stream a firmware image to flash holding no buffer of its own.
// Where a frame goes when it is not ours.
//
// A handler reads its own request off the wire (see ensureInput below), so it is
// the thing holding the transport while other channels' frames arrive. It cannot
// run them -- a Connection serves one operation at a time -- but it must not
// destroy them either, which is what this interface is for: hand the frame to the
// Connection, let it update its table and refuse what it cannot serve, and carry
// on waiting for our own next chunk.
//
// Implemented by Connection. Optional only so that Channel stays usable without
// one; both transports pass one.
class ForeignFrameSink
{
public:
    virtual ~ForeignFrameSink() = default;
    virtual void OnForeignFrame(uint16_t id, uint8_t flags,
                                const uint8_t* payload, size_t len) = 0;
};

class Channel : public Stream
{
    uint16_t id_;
    Transport& link_;
    ForeignFrameSink* foreign_;

    const uint8_t* req_ = nullptr;   // current chunk's payload
    size_t reqLen_ = 0;
    size_t reqPos_ = 0;
    size_t consumed_ = 0;            // request bytes handed to the reader, for diagnostics
    bool   reqFinal_ = false;        // current chunk was FLAG_FINAL → no more after it

    uint8_t* inBuf_;      // buffer for pulled continuation chunks [ header | payload ]
    size_t   inCap_;      // capacity of inBuf_ (total, header included)

    uint8_t* buf_;        // external [ header | payload ] buffer (reply)
    size_t   cap_;        // payload capacity (buf_ size minus HEADER_LEN)
    size_t   outLen_ = 0; // payload bytes buffered so far
    bool     failed_ = false;
    bool     reset_ = false;  // the peer terminated this channel mid-request

    // Frame [id|flags|payload] into buf_ and send it; reset the payload cursor.
    void emitChunk(uint8_t flags)
    {
        // A channel the peer has RESET is over in both directions. Writing to it
        // would put frames on the wire for an id that no longer means anything,
        // and on a relay those get mapped to whoever holds that id next.
        if (reset_) { outLen_ = 0; return; }

        channel::writeHeader(buf_, id_, flags);
        if (!link_.SendRaw(buf_, channel::HEADER_LEN + outLen_)) failed_ = true;
        outLen_ = 0;
    }

    // Leave req_ holding at least one unread byte, pulling the next chunk off the
    // link when the current one is drained. False = end of the request, either
    // because it ended (reqFinal_) or because the transport broke (failed_).
    bool ensureInput()
    {
        while (reqPos_ >= reqLen_)                      // current chunk drained
        {
            if (reqFinal_ || failed_) return false;    // EOF
            uint16_t sid = 0; uint8_t flags = 0;
            int n = link_.RecvChunk(inBuf_, inCap_, &sid, &flags);
            if (n == Transport::RECV_TOO_LONG)
            {
                // The peer framed a chunk bigger than this link can receive. The
                // transport has already discarded it, so the link is fine and every
                // other channel on it is untouched -- this request alone is lost.
                //
                // failed_ rather than a RESET from in here: the handler is mid-read
                // and owns the reply direction, and everything that needs all of its
                // input already asks failed(). A RESET raised underneath it would
                // race the reply it is about to write.
                ESP_LOGE("Channel",
                         "channel %u: peer sent a chunk over this link's %u-byte "
                         "inbound limit after %u bytes - request lost, link kept",
                         static_cast<unsigned>(id_),
                         static_cast<unsigned>(link_.InboundLimit()),
                         static_cast<unsigned>(consumed_));
                failed_ = true;
                return false;
            }
            if (n < 0)
            {
                // Logged because the caller sees 0, the same as a clean end of
                // stream: without a line here a transport failure is silent, and a
                // reader that trusts 0 to mean "complete" acts on a truncated
                // request.
                ESP_LOGE("Channel", "read failed after %u bytes: n=%d (channel %u)",
                         static_cast<unsigned>(consumed_), n,
                         static_cast<unsigned>(id_));
                failed_ = true;
                return false;
            }
            if (sid == id_ && (flags & channel::FLAG_RESET))
            {
                // The peer gave up on this request: a cancelled upload, an aborted
                // command. It is NOT an end of stream -- a handler that reads 0 and
                // concludes "complete" would commit a truncated image -- so this
                // sets failed_, which is the flag anything needing all of its input
                // already has to ask about.
                //
                // Reading it as body bytes instead is what a half-duplex reader
                // would do, and it wedges: the handler waits for a FINAL that by
                // definition is not coming, and the channel stays Active until the
                // transport itself gives up.
                ESP_LOGI("Channel", "channel %u reset by the peer after %u bytes",
                         static_cast<unsigned>(id_), static_cast<unsigned>(consumed_));
                reset_ = true;
                failed_ = true;
                return false;
            }
            if (sid != id_)
            {
                // Someone else's frame, arriving while we hold the transport. It
                // used to fail THIS request and drop that frame on the floor, which
                // broke two channels with one frame and left the other peer waiting
                // out a timeout for a reply that had been discarded. Hand it over
                // and keep waiting for ours.
                //
                // Without a sink there is nothing that could be done with it except
                // what used to happen, so do that rather than loop forever.
                if (!foreign_)
                {
                    ESP_LOGE("Channel", "frame for channel %u while reading %u, and "
                             "no connection to hand it to",
                             static_cast<unsigned>(sid), static_cast<unsigned>(id_));
                    failed_ = true;
                    return false;
                }
                foreign_->OnForeignFrame(sid, flags, inBuf_ + channel::HEADER_LEN,
                                         static_cast<size_t>(n));
                continue;
            }
            req_ = inBuf_ + channel::HEADER_LEN;
            reqLen_ = static_cast<size_t>(n);
            reqPos_ = 0;
            reqFinal_ = (flags & channel::FLAG_FINAL) != 0;
        }
        return true;
    }

public:
    Channel(uint16_t id, Transport& link, uint8_t* buf, size_t payloadCap,
            uint8_t* inBuf, size_t inCap, ForeignFrameSink* foreign = nullptr)
        : id_(id), link_(link), foreign_(foreign),
          inBuf_(inBuf), inCap_(inCap), buf_(buf), cap_(payloadCap) {}

    void feedRequest(const uint8_t* data, size_t len, bool final)
    {
        req_ = data; reqLen_ = len; reqPos_ = 0; reqFinal_ = final;
    }

    size_t read(void* dst, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        if (!ensureInput()) return 0;
        size_t n = std::min(size, reqLen_ - reqPos_);
        if (n) { memcpy(dst, req_ + reqPos_, n); reqPos_ += n; consumed_ += n; }
        return n;
    }

    // ── Zero-copy handoff (Stream) ────────────────────────────────────────────
    // The request already sits in the transport's inbound buffer and the reply is
    // already assembled in its framing buffer, so a handler moving kilobytes can
    // work in both directly and hold no buffer at all. See Stream.h.

    bool canLend() const override { return true; }

    size_t lendInput(const uint8_t*& data) override
    {
        if (!ensureInput()) { data = nullptr; return 0; }
        const size_t n = reqLen_ - reqPos_;
        data = req_ + reqPos_;
        reqPos_ = reqLen_;
        consumed_ += n;
        return n;
    }

    uint8_t* lendOutput(size_t& avail) override
    {
        if (outLen_ == cap_) emitChunk(0);   // full buffer → send it, then lend it
        if (failed_) { avail = 0; return nullptr; }
        avail = cap_ - outLen_;
        return buf_ + channel::HEADER_LEN + outLen_;
    }

    void commitOutput(size_t n) override
    {
        outLen_ += n;
        if (outLen_ == cap_) emitChunk(0);   // full buffer → non-final DATA chunk
    }

    size_t write(const void* data, size_t size, TickType_t timeout = portMAX_DELAY) override
    {
        (void)timeout;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (remaining > 0)
        {
            size_t n = std::min(cap_ - outLen_, remaining);
            memcpy(buf_ + channel::HEADER_LEN + outLen_, p, n);
            outLen_ += n; p += n; remaining -= n;
            if (outLen_ == cap_) emitChunk(0);   // full buffer → non-final DATA chunk
        }
        return size;
    }

    // Emit whatever reply bytes are buffered as a NON-final chunk, now. Lets a
    // handler push incremental output (e.g. upload progress) mid-stream instead
    // of it sitting in the buffer until finish(). No-op when nothing is buffered.
    bool flush() override
    {
        if (outLen_ > 0) emitChunk(0);
        return !failed_;
    }

    // Emit the final chunk, closing the reply direction.
    void finish() { emitChunk(channel::FLAG_FINAL); }

    // Transport/framework refusal (unknown command, bad route): one RESET chunk
    // whose payload is the reason text.
    void reject(const char* reason)
    {
        size_t n = std::min(cap_, strlen(reason));
        memcpy(buf_ + channel::HEADER_LEN, reason, n);
        outLen_ = n;
        emitChunk(channel::FLAG_RESET);
    }

    /// A read or write that stopped short because the transport broke, not because
    /// the request ended. Both look like read() == 0 to the caller, so anything that
    /// needs *all* of its input has to ask. An override, so a handler holding only a
    /// `Stream&` can ask too.
    bool failed() const override { return failed_; }

    /// Did the peer RESET this channel, as opposed to the transport breaking? The
    /// difference matters to the caller that has to decide whether to log a fault.
    bool wasReset() const { return reset_; }
    uint16_t id() const { return id_; }
};
