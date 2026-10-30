#pragma once

#include "Mutex.h"
#include "Transport.h"
#include "ChannelProtocol.h"
#include <esp_http_server.h>
#include "esp_log.h"
#include <cstdint>
#include <cstddef>

// ESP-IDF internal (declared in the private esp_httpd_priv.h, NOT the public
// esp_http_server.h): parses the next WS frame's first byte (FIN + opcode) into
// req->aux, which httpd_ws_recv_frame then consumes. httpd's own loop calls it
// once per handler invocation before dispatch, so to read frames *beyond* the
// first within a single handler call we must call it ourselves. This is a
// deliberate, documented wart — the price of draining an inbound stream on the
// httpd task without a worker. A future IDF dropping the symbol fails as a clean
// link error, not silent breakage.
extern "C" esp_err_t httpd_ws_get_frame_type(httpd_req_t* req);

// Transport link for the local browser WebSocket (one of two Transport
// implementations — see Transport.h). Outbound: sends one already-framed
// channel chunk ([channel|flags|payload], assembled by Channel) as one WS binary
// frame. Inbound: pulls the NEXT WS binary frame off the socket (RecvChunk), so a
// streamed request body — arriving as several channel chunks that share one id —
// can be drained within a single httpd handler call. The shared send mutex
// serializes whole outbound frames so the log pump can't split a reply.
class WsTransport : public Transport
{
    static constexpr const char* TAG = "WsTransport";   // referenced by the LOCK macro

    httpd_req_t* req_;
    Mutex& sendMutex_;
    size_t inboundLimit_;

public:
    // `inboundLimit` is what the owner's receive buffer can hold, reported back
    // through Transport::InboundLimit for diagnostics. RecvChunk enforces the
    // capacity it is actually handed, so this never becomes a second source of
    // truth about the size.
    WsTransport(httpd_req_t* req, Mutex& sendMutex, size_t inboundLimit = 0)
        : req_(req), sendMutex_(sendMutex), inboundLimit_(inboundLimit) {}

    // `frame` is [channel|flags|payload]; `len` is the total (header + payload).
    bool SendRaw(const uint8_t* frame, size_t len) override
    {
        httpd_ws_frame_t f = {};
        f.type = HTTPD_WS_TYPE_BINARY;
        f.payload = const_cast<uint8_t*>(frame);
        f.len = len;

        LOCK(sendMutex_);
        return httpd_ws_send_frame(req_, &f) == ESP_OK;
    }

private:
    // Read and throw away a frame whose header has already been taken off the
    // socket, `remaining` bytes of payload at a time, using `buf` as a sink.
    //
    // This is what keeps an oversized frame from killing the connection.
    // httpd_ws_recv_frame refuses outright when the frame does not fit, WITHOUT
    // consuming the payload, so the next read takes body bytes for a frame header
    // and the socket is finished. Draining puts the stream back in sync and costs
    // only the bytes themselves.
    //
    // It works because a frame passed in with `len` already set skips the header
    // parse and reads exactly that many bytes (see httpd_ws.c) - the same two-step
    // this class already relies on. The payload comes back unmasked from the wrong
    // offset for every gulp after the first, because the mask cursor restarts each
    // call and IDF does not expose it. That is irrelevant to a reader that is
    // discarding, and it is the reason this drains rather than reassembles.
    bool DrainFrame(size_t remaining, uint8_t* buf, size_t cap)
    {
        while (remaining > 0)
        {
            httpd_ws_frame_t g = {};
            g.len = remaining < cap ? remaining : cap;
            g.payload = buf;
            if (httpd_ws_recv_frame(req_, &g, g.len) != ESP_OK) return false;
            remaining -= g.len;
        }
        return true;
    }

public:
    size_t InboundLimit() const override { return inboundLimit_; }

    // Receive the next inbound WS frame into `buf` (capacity `cap`) as a channel
    // chunk. On success returns the payload length (>= 0), fills *sid / *flags
    // from the 3-byte header, and leaves the payload at buf + HEADER_LEN.
    //
    // RECV_EOF on a broken link or a non-data frame (CLOSE/PING). RECV_TOO_LONG
    // when the peer framed more than fits: the frame is drained first, so the
    // socket survives and only the one channel is lost.
    int RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags) override
    {
        if (httpd_ws_get_frame_type(req_) != ESP_OK) return RECV_EOF;

        httpd_ws_frame_t f = {};                       // len==0 → header-only read
        if (httpd_ws_recv_frame(req_, &f, 0) != ESP_OK) return RECV_EOF;

        if (f.type != HTTPD_WS_TYPE_BINARY && f.type != HTTPD_WS_TYPE_CONTINUE)
            return RECV_EOF;                           // CLOSE/PING/TEXT → end of stream

        if (f.len > cap)
        {
            ESP_LOGW(TAG, "inbound frame %u > %u-byte window - dropping the frame, "
                     "keeping the socket", static_cast<unsigned>(f.len),
                     static_cast<unsigned>(cap));
            if (!DrainFrame(f.len, buf, cap)) return RECV_EOF;
            return RECV_TOO_LONG;
        }
        if (f.len < channel::HEADER_LEN) return RECV_EOF;

        f.payload = buf;
        if (httpd_ws_recv_frame(req_, &f, cap) != ESP_OK) return RECV_EOF;

        *sid   = channel::readU16(buf);
        *flags = buf[2];
        return static_cast<int>(f.len - channel::HEADER_LEN);
    }
};

// The same wire, addressed by fd instead of by request.
//
// WsTransport wraps the httpd_req_t of the call it was built in, which is what a
// handler has. The log pump has no request -- it runs on its own task and walks
// the connection registry -- so it addresses each socket the way httpd allows from
// outside a handler, with the async send. Receiving is not part of this: the pump
// only ever pushes.
class WsPumpTransport : public Transport
{
    static constexpr const char* TAG = "WsPumpTransport";

    httpd_handle_t server_;
    int fd_;
    IMutex& sendMutex_;

public:
    WsPumpTransport(httpd_handle_t server, int fd, IMutex& sendMutex)
        : server_(server), fd_(fd), sendMutex_(sendMutex) {}

    bool SendRaw(const uint8_t* frame, size_t len) override
    {
        httpd_ws_frame_t f = {};
        f.type = HTTPD_WS_TYPE_BINARY;
        f.payload = const_cast<uint8_t*>(frame);
        f.len = len;

        LOCK(sendMutex_);
        return httpd_ws_send_frame_async(server_, fd_, &f) == ESP_OK;
    }

    size_t InboundLimit() const override { return 0; }   // push only

    int RecvChunk(uint8_t*, size_t, uint16_t*, uint8_t*) override
    {
        return RECV_EOF;   // push only
    }
};
