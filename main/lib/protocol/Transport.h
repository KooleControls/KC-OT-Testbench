#pragma once

#include <cstdint>
#include <cstddef>

// The transport seam under Channel: turn an already-framed channel chunk into wire
// bytes, and pull inbound wire bytes back as chunks. This is the ONLY
// per-transport code — Channel and every command handler are written against this
// interface and are identical across transports.
//
// This header, ChannelProtocol.h, Channel.h and CommandChannel.h are the whole of
// the protocol layer, depending on nothing but Stream and the JSON helpers. They
// live in lib/ rather than under a transport because the transports depend on them,
// not the reverse.
//
// Implementations, each owned by the manager that owns its transport:
//   WsTransport    — the local browser socket. One chunk = one WS binary frame;
//                      inbound frames are read synchronously on the httpd task.
//   RelayTransport — the outbound socket to the relay server. Also a synchronous
//                      read, on the task that runs the command. See
//                      docs/reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md.
//
// Both are now the same shape, and that is worth stating because it was not always
// true: the relay used to receive frames on a WebSocket library's own task and hand
// them across on a queue, which cost a heap allocation per frame and silently
// dropped chunks when the queue filled. RecvChunk being an actual read on the
// calling task is what a streaming handler needs, and now both have it.
//
// SendRaw takes a WHOLE pre-framed chunk rather than (channel, flags, payload):
// Channel assembles the 3-byte header and the payload into one external buffer,
// so a flush is a single send with no extra copy. A (channel, flags, payload)
// signature would reintroduce that copy on every chunk — including every chunk
// of a multi-MB firmware image.
class Transport
{
public:
    virtual ~Transport() = default;

    // `frame` is [channel|flags|payload]; `len` is the total (header + payload).
    virtual bool SendRaw(const uint8_t* frame, size_t len) = 0;

    // How big a chunk this transport can receive in one piece, for a sender that
    // is framing on this same link. It is a PROPERTY OF THIS CONNECTION and
    // nothing else: a UART link may answer 256, a WebSocket 4096. Nothing above
    // Transport reads it to decide correctness -- Channel splits its reply at
    // whatever capacity it was handed and reassembles inbound chunks until FINAL,
    // whatever their sizes -- so this exists for diagnostics and for a peer on the
    // same board, never as a number the wire carries.
    virtual size_t InboundLimit() const = 0;

    // Receive the next inbound chunk into `buf` (capacity `cap`), blocking until
    // one is available. On success returns the payload length (>= 0), fills
    // *sid / *flags from the 3-byte header, and leaves the payload at
    // buf + HEADER_LEN.
    //
    // Two distinct failures, and telling them apart is the whole reason this
    // returns an int rather than a bool:
    //
    //   RECV_EOF (-1)       the link is gone or unusable. Nothing more will come.
    //   RECV_TOO_LONG (-2)  the peer sent a chunk larger than `cap`. The chunk has
    //                       been CONSUMED AND DISCARDED, so the link is still in
    //                       sync and every other channel on it is unaffected --
    //                       only this one request is lost.
    //
    // The second used to be the first, which made one oversized frame from one
    // peer kill the whole connection and every channel on it. A receive buffer is
    // this Connection's own business; overrunning it is a fault on ONE channel and
    // has to be reported as one.
    static constexpr int RECV_EOF      = -1;
    static constexpr int RECV_TOO_LONG = -2;

    virtual int RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags) = 0;
};
