#pragma once

#include <cstdint>
#include <cstddef>

// On-wire channel frame: [ channel:u16 LE ][ flags:u8 ][ payload ].
//
// A Connection is one persistent transport link between two peers, and a Channel is
// one logical stream multiplexed over it. Neither peer is a client: both allocate
// channels, from disjoint halves of the id space that the handshake below assigns.
//
// There are no reserved channel ids. Logs, telemetry and the device's own
// description used to have one each -- 0, 0xFFFF and 0xFFFE -- and all three existed
// for a single reason: a device with no allocator can only be addressed by
// constants. Give it half the space and they are ordinary channels.
// See docs/reasoning/2026-09-21-11h30-three-reserved-ids-are-one-missing-capability.md.
namespace channel
{
    // ── Flags ─────────────────────────────────────────────────────────────────
    //
    // FINAL closes ONE direction. A channel disappears when both directions have
    // finalled, so a one-shot request is OPEN|FINAL out and FINAL back.
    //
    // RESET terminates the whole channel in both directions, from either peer, at
    // any time. It is what refuses an OPEN, cancels an upload, aborts a command and
    // unsubscribes from a stream -- one flag rather than four mechanisms, and the
    // reason it replaced REJECT rather than joining it. A RESET is never answered
    // with a RESET; two peers that have both forgotten a channel would otherwise
    // trade them forever.
    //
    // OPEN marks the first frame of a channel. It is not negotiation -- there is no
    // OPEN_OK and silence is acceptance -- it is the discriminator that lets a
    // receiver tell a new channel from the residue of a dead one without keeping
    // state about the dead one.
    //
    // CONTROL is connection-level rather than channel-level. The sender MUST write
    // channel 0 and the receiver MUST ignore it, which is why 0 stays an ordinary
    // usable id. CONTROL never combines with the other three.
    inline constexpr uint8_t FLAG_FINAL   = 0x01;
    inline constexpr uint8_t FLAG_RESET   = 0x02;
    inline constexpr uint8_t FLAG_OPEN    = 0x04;
    inline constexpr uint8_t FLAG_CONTROL = 0x08;

    inline constexpr size_t HEADER_LEN = 3;

    // ── Connection handshake ──────────────────────────────────────────────────
    //
    // The only CONTROL frame v1 defines. Both peers send it unprompted the moment
    // the transport is up; neither waits for the other, so it costs one exchange
    // and no round-trip dependency.
    //
    // Payload: [ version:u8 ][ nonce:u64 LE ].
    //
    // The nonces decide who allocates from which half. Higher nonce takes the low
    // half. That is the whole mechanism, and it is a nonce rather than "the dialer
    // wins" because UART and ESP-NOW have no dialer -- two boards powering up
    // together is exactly the case this protocol exists to serve.
    //
    // Firmware supports EXACTLY this version. A mismatch closes the connection
    // rather than negotiating down: old protocol implementations accumulating in
    // flash is the cost, and the relay is where tolerance belongs because it is the
    // hub and the only participant that is easy to redeploy.
    inline constexpr uint8_t PROTOCOL_VERSION = 1;
    inline constexpr size_t  HANDSHAKE_LEN    = 1 + 8;

    // Equal nonces are not impossible: esp_random() has reduced entropy with the
    // radios off, which is the UART-linked-pair case. Both peers redraw and resend,
    // a bounded number of times, then give up and let the transport reconnect.
    inline constexpr uint8_t MAX_HANDSHAKE_ATTEMPTS = 3;

    // ── Id spaces ─────────────────────────────────────────────────────────────
    //
    // Two contiguous halves rather than odd/even: identical in cost, and the relay
    // already allocates this way, so its code carries over.
    inline constexpr uint16_t LOW_BASE   = 0x0000;
    inline constexpr uint16_t LOW_LIMIT  = 0x8000;   // exclusive
    inline constexpr uint16_t HIGH_BASE  = 0x8000;
    inline constexpr uint32_t HIGH_LIMIT = 0x10000;  // exclusive

    inline uint16_t readU16(const uint8_t* p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    /// Writes the 3-byte header into `out`; returns HEADER_LEN.
    inline size_t writeHeader(uint8_t* out, uint16_t id, uint8_t flags)
    {
        out[0] = static_cast<uint8_t>(id & 0xFF);
        out[1] = static_cast<uint8_t>((id >> 8) & 0xFF);
        out[2] = flags;
        return HEADER_LEN;
    }

    inline uint64_t readU64(const uint8_t* p)
    {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
        return v;
    }

    inline void writeU64(uint8_t* out, uint64_t v)
    {
        for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}
