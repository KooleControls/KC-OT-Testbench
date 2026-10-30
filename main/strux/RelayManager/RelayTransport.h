#pragma once

#include "Transport.h"
#include "ChannelProtocol.h"
#include "RelaySocket.h"

// Transport over the outbound relay WebSocket (the second implementation — see
// Transport.h).
//
// There is nothing to it, and that is the whole point of the change that produced
// it: the socket underneath is one the owning task READS, so "get me the next
// chunk" is a read, exactly as it is on the browser socket. It used to be a queue
// pop, because frames arrived on a task that was not ours — see RelaySocket for why
// that is gone, along with the per-frame allocation and the dropped chunks.
class RelayTransport : public Transport
{
    static constexpr int SEND_TIMEOUT_MS = 5000;

    // A body chunk that never arrives must not wedge the task forever; EOF the
    // channel instead and let the server retry.
    //
    // Paired with the server's gate idle timeout (DeviceConnection.IdleTimeout in
    // the strux-relay server), which is deliberately longer. Whichever fires first
    // decides how a stalled channel ends, and it has to be this one: the device EOFs
    // its own request, the handler writes a reply, and that reply releases the
    // server's gate in the ordinary way. The other order releases the pipe while this
    // side still believes the channel is open — which is the interleaving the gate is
    // there to prevent. Neither timeout limits how LONG a healthy channel may run;
    // both measure silence.
    static constexpr int RECV_TIMEOUT_MS = 10000;

    RelaySocket& socket_;
    size_t inboundLimit_;

public:
    // `inboundLimit` is the owner's receive buffer, reported back through
    // Transport::InboundLimit for diagnostics. ReadFrame enforces the capacity it
    // is handed, so this never becomes a second source of truth about the size.
    explicit RelayTransport(RelaySocket& socket, size_t inboundLimit = 0)
        : socket_(socket), inboundLimit_(inboundLimit) {}

    size_t InboundLimit() const override { return inboundLimit_; }

    bool SendRaw(const uint8_t* frame, size_t len) override
    {
        return socket_.SendBinary(frame, len, SEND_TIMEOUT_MS);
    }

    int RecvChunk(uint8_t* buf, size_t cap, uint16_t* sid, uint8_t* flags) override
    {
        // Idle, dead and too-short all end the request the same way: mid-request
        // there is no such thing as "nothing arrived, carry on".
        //
        // A message over this link's window is the one exception, and it is why
        // ReadFrame has a return value of its own for it: the relay has framed
        // more than fits, ReadFrame has already read it to the end and thrown it
        // away, and the PIPE IS STILL GOOD. Failing it like a dead socket is what
        // used to drop every channel on the connection over one bad chunk.
        const int n = socket_.ReadFrame(buf, cap, RECV_TIMEOUT_MS);
        if (n == RelaySocket::READ_TOO_LONG) return RECV_TOO_LONG;
        if (n < static_cast<int>(channel::HEADER_LEN)) return RECV_EOF;

        *sid   = channel::readU16(buf);
        *flags = buf[2];
        return n - static_cast<int>(channel::HEADER_LEN);
    }
};
