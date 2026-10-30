#pragma once

#include "ChannelProtocol.h"
#include "ChannelTable.h"

#include <cstdint>

// Everything a Connection has to remember between frames.
//
// A struct rather than a class owning a task, because the two transports do not
// have the same shape: the relay is one connection on its own task, while
// esp_http_server serves up to four browser sockets from one. So channel state is
// per Connection -- this lives in WsConnection and in RelayManager -- while the
// large frame buffers stay per TASK.
struct ConnectionState
{
    enum class Phase : uint8_t
    {
        // Our handshake is out; the peer's has not arrived. Nothing but CONTROL
        // may be sent or received.
        Handshake,

        // Both handshakes exchanged, the id spaces are decided, channels may open.
        Ready,

        // Version mismatch, or too many nonce collisions. The transport closes.
        Failed,
    };

    Phase phase = Phase::Handshake;

    ChannelTable channels;

    // ── Handshake ─────────────────────────────────────────────────────────────
    uint64_t nonce = 0;
    uint8_t  attempts = 0;

    /// Which half we allocate from, once the nonces have decided. Higher nonce
    /// takes the low half; the rule only has to be the same on both sides.
    bool lowHalf = false;

    /// Monotonic-with-wrap allocation cursor inside our half.
    uint16_t nextId = 0;

    // ── Device-initiated passive streams; -1 when not open ───────────────────
    //
    // Signed because 0 is a perfectly ordinary channel id now that nothing is
    // reserved, so it cannot double as "none".
    int32_t logChannel = -1;
    int32_t telemetryChannel = -1;

    /// Where this connection has got to in the console ring.
    uint32_t logCursor = 0;

    void Reset()
    {
        phase = Phase::Handshake;
        channels.Reset();
        nonce = 0;
        attempts = 0;
        lowHalf = false;
        nextId = 0;
        logChannel = -1;
        telemetryChannel = -1;
        logCursor = 0;
    }

    /// Settle the id space from the two nonces. False when they are equal, which
    /// the caller answers by redrawing and sending again.
    bool Settle(uint64_t peerNonce)
    {
        if (peerNonce == nonce) return false;
        lowHalf = nonce > peerNonce;
        nextId = lowHalf ? channel::LOW_BASE : channel::HIGH_BASE;
        return true;
    }

    /// Next free id in our half, skipping anything the table still holds. False
    /// only when our whole half is somehow in use, which the table's size makes
    /// impossible in practice -- it is checked because silently reusing a live id
    /// presents as the peer answering the wrong request.
    bool AllocateId(uint16_t& out)
    {
        const uint32_t base  = lowHalf ? channel::LOW_BASE : channel::HIGH_BASE;
        const uint32_t limit = lowHalf ? channel::LOW_LIMIT : channel::HIGH_LIMIT;

        for (uint32_t tries = 0; tries < (limit - base); ++tries)
        {
            const uint16_t id = nextId;
            nextId = static_cast<uint16_t>((id + 1u >= limit) ? base : id + 1u);
            if (!channels.Holds(id))
            {
                out = id;
                return true;
            }
        }
        return false;
    }
};
