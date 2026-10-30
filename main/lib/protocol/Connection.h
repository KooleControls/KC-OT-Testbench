#pragma once

#include "Channel.h"
#include "ChannelProtocol.h"
#include "ChannelTable.h"
#include "ConnectionState.h"
#include "CommandChannel.h"
#include "Transport.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace protocol
{

inline constexpr const char* CONNECTION_TAG = "Connection";

/// The envelopes naming the two streams a device opens for itself. Ordinary
/// envelope lines, read by the peer exactly as a command's is -- what used to be
/// three reserved channel ids is now three ordinary channels with names.
inline constexpr const char* LOG_STREAM_ENVELOPE = "{\"type\":\"log stream\"}\n";
inline constexpr const char* TELEMETRY_STREAM_ENVELOPE = "{\"type\":\"telemetry stream\"}\n";

/// One frame, framed on the caller's stack. Used for anything that is not a
/// Channel's own reply: the handshake, a RESET, the OPEN of a passive stream.
inline bool SendFrame(Transport& link, uint16_t id, uint8_t flags,
                      const uint8_t* payload, size_t len)
{
    uint8_t frame[channel::HEADER_LEN + 96];
    const size_t cap = sizeof(frame) - channel::HEADER_LEN;
    if (len > cap) len = cap;

    channel::writeHeader(frame, id, flags);
    if (len) memcpy(frame + channel::HEADER_LEN, payload, len);
    return link.SendRaw(frame, channel::HEADER_LEN + len);
}

/// Terminate a channel, in both directions, with a reason the peer can act on.
///
/// The reason strings are a small fixed set on purpose. "busy" in particular has to
/// be distinguishable from a handler's own error, because a peer that one day runs
/// handlers concurrently must retry rather than surface it -- which is what lets
/// this wire format survive that change.
inline void SendReset(Transport& link, uint16_t id, const char* reason)
{
    SendFrame(link, id, channel::FLAG_RESET,
              reinterpret_cast<const uint8_t*>(reason), strlen(reason));
    ESP_LOGD(CONNECTION_TAG, "channel %u reset: %s", static_cast<unsigned>(id), reason);
}

/// Draw a nonce. Mixed with the timer because esp_random() is only properly random
/// with the RF subsystem up, and the case this protocol exists for -- two identical
/// boards on a UART link, radios off, booted from the same image at the same
/// moment -- is exactly where that assumption is weakest.
inline uint64_t DrawNonce(uint64_t mixIn = 0)
{
    const uint64_t hi = esp_random();
    const uint64_t lo = esp_random();
    return ((hi << 32) ^ lo) ^ static_cast<uint64_t>(esp_timer_get_time()) ^ mixIn;
}

/// Send our half of the handshake. Called the moment the transport is up, without
/// waiting for the peer, so neither side leads.
inline bool SendHandshake(ConnectionState& state, Transport& link, uint64_t mixIn = 0)
{
    state.nonce = DrawNonce(mixIn);
    state.attempts++;

    uint8_t payload[channel::HANDSHAKE_LEN];
    payload[0] = channel::PROTOCOL_VERSION;
    channel::writeU64(payload + 1, state.nonce);

    // Channel 0 by convention and ignored on receipt, so a hex dump reads cleanly
    // and 0 stays usable as an ordinary id.
    return SendFrame(link, 0, channel::FLAG_CONTROL, payload, sizeof(payload));
}

/// The peer's handshake: a CONTROL frame, in whatever phase this side is in.
///
/// A free function rather than Connection's alone because not every peer serves
/// channels. Connection answers channels the peer OPENs; a client that only opens
/// its own -- the gateway's side of the PCB1246 link -- still has to settle the id
/// space, and must do it by exactly these rules or the two sides disagree.
inline void OnHandshake(ConnectionState& state, Transport& link,
                        const uint8_t* payload, size_t len)
{
    if (len < channel::HANDSHAKE_LEN)
    {
        ESP_LOGW(CONNECTION_TAG, "short CONTROL frame (%u) - ignored",
                 static_cast<unsigned>(len));
        return;
    }

    if (state.phase == ConnectionState::Phase::Ready)
    {
        // The peer restarted underneath us. On a socket the transport would
        // have told us; on UART there is no such event, so this frame is the
        // strongest signal there is. Drop everything and handshake again.
        ESP_LOGW(CONNECTION_TAG, "peer restarted - re-handshaking");
        state.Reset();
        SendHandshake(state, link);
    }

    const uint8_t version = payload[0];
    if (version != channel::PROTOCOL_VERSION)
    {
        ESP_LOGE(CONNECTION_TAG,
                 "protocol version %u, this firmware speaks %u - closing",
                 static_cast<unsigned>(version),
                 static_cast<unsigned>(channel::PROTOCOL_VERSION));
        state.phase = ConnectionState::Phase::Failed;
        return;
    }

    const uint64_t peer = channel::readU64(payload + 1);
    if (!state.Settle(peer))
    {
        if (state.attempts >= channel::MAX_HANDSHAKE_ATTEMPTS)
        {
            ESP_LOGE(CONNECTION_TAG, "nonce collision %u times - giving up",
                     static_cast<unsigned>(state.attempts));
            state.phase = ConnectionState::Phase::Failed;
            return;
        }
        // Mix the peer's nonce in, so two boards with identical RNG state
        // diverge instead of colliding identically again.
        ESP_LOGW(CONNECTION_TAG, "nonce collision - redrawing");
        SendHandshake(state, link, peer);
        return;
    }

    state.phase = ConnectionState::Phase::Ready;
    ESP_LOGI(CONNECTION_TAG, "ready, channels %s half",
             state.lowHalf ? "low" : "high");
}

/// Open a device-initiated stream: OPEN carrying the envelope that names it, then
/// nothing until a drain has something to push. No handler and no task -- the entry
/// is Passive, which is all "channel != execution context" means in practice.
inline bool OpenPassiveChannel(ConnectionState& state, Transport& link,
                               const char* envelope, int32_t& out)
{
    if (state.phase != ConnectionState::Phase::Ready) return false;

    uint16_t id = 0;
    if (!state.AllocateId(id)) return false;
    if (!state.channels.Open(id, ChannelTable::State::Passive)) return false;

    if (!SendFrame(link, id, channel::FLAG_OPEN,
                   reinterpret_cast<const uint8_t*>(envelope), strlen(envelope)))
    {
        state.channels.Close(id);
        return false;
    }

    out = static_cast<int32_t>(id);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// The per-frame facade. Built on the caller's stack because the two things it
// wraps are per frame on the local socket: WsTransport holds that call's
// httpd_req_t, AuthGate holds the connection slot it belongs to.
// ──────────────────────────────────────────────────────────────────────────────
template <class Dispatcher, class Gate>
class Connection final : public ForeignFrameSink
{
public:
    Connection(ConnectionState& state, Transport& link,
               Dispatcher& dispatcher, Gate& gate,
               uint8_t* outBuf, size_t outCap,
               uint8_t* inBuf, size_t inCap)
        : state_(state), link_(link), dispatcher_(dispatcher), gate_(gate),
          outBuf_(outBuf), outCap_(outCap), inBuf_(inBuf), inCap_(inCap) {}

    /// One frame at the top of the transport's read loop. Nothing is executing
    /// here -- a running handler owns the read, and its frames arrive through
    /// OnForeignFrame below.
    void OnFrame(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len)
    {
        if (flags & channel::FLAG_CONTROL)
        {
            OnControl(payload, len);
            return;
        }

        if (state_.phase != ConnectionState::Phase::Ready)
        {
            // On an ordered transport this cannot happen from a correct peer: it
            // only sends channel traffic once READY, which means it has already
            // sent the CONTROL that precedes this frame.
            ESP_LOGW(CONNECTION_TAG, "channel %u before READY - dropped",
                     static_cast<unsigned>(id));
            return;
        }

        ChannelTable::Entry* e = state_.channels.Find(id);

        if (flags & channel::FLAG_RESET)
        {
            // Terminal, both directions, and never answered with another RESET.
            if (e) Forget(id);
            return;
        }

        if (e)
        {
            if (e->state == ChannelTable::State::Active)
            {
                // Active at the top of the read loop is a contradiction: its
                // handler would have had to return to get here.
                SendReset(link_, id, "channel already active");
                return;
            }
            // A Passive stream is ours to push on; the peer has nothing to say on
            // it except RESET, handled above.
            SendReset(link_, id, "not active");
            Forget(id);
            return;
        }

        // Unknown id. WITHOUT open this is residue from a channel that has already
        // finished, or a stale frame from before a reconnect. Dropping it is the
        // whole handling, and needing no memory of the dead channel to do it is
        // what OPEN is for.
        if (!(flags & channel::FLAG_OPEN)) return;

        if (state_.channels.Busy())
        {
            SendReset(link_, id, "busy");
            return;
        }

        if (!state_.channels.Open(id, ChannelTable::State::Active))
        {
            SendReset(link_, id, "too many channels");
            return;
        }

        Channel ch(id, link_, outBuf_, outCap_, inBuf_, inCap_, this);
        ch.feedRequest(payload, len, (flags & channel::FLAG_FINAL) != 0);
        protocol::RunCommandChannel(ch, dispatcher_, gate_);

        // Both directions are done: the handler finalled or reset its reply, and
        // anything still arriving for this id is residue the rule above drops.
        state_.channels.Close(id);
    }

private:
    void OnControl(const uint8_t* payload, size_t len)
    {
        OnHandshake(state_, link_, payload, len);
    }

    /// A frame for another channel, arriving while a handler holds the transport.
    ///
    /// Runs INSIDE that handler, so it must not dispatch: the one execution context
    /// is in use. It can update the table and tell the peer what it could not do,
    /// which is the whole set of answers v1 owes.
    void OnForeignFrame(uint16_t id, uint8_t flags,
                        const uint8_t* payload, size_t len) override
    {
        if (flags & channel::FLAG_CONTROL)
        {
            // Mid-request is not a legal place for this. Handling it would mean
            // tearing down the channel we are reading, from inside it.
            (void)payload; (void)len;
            ESP_LOGW(CONNECTION_TAG, "CONTROL mid-request - ignored");
            return;
        }

        ChannelTable::Entry* e = state_.channels.Find(id);

        if (flags & channel::FLAG_RESET)
        {
            if (e) Forget(id);
            return;
        }

        if (e) return;                          // a Passive stream: nothing to do
        if (!(flags & channel::FLAG_OPEN)) return;   // residue: drop

        SendReset(link_, id, "busy");
    }

    /// Drop a channel, and any role the connection had pinned to it.
    void Forget(uint16_t id)
    {
        state_.channels.Close(id);
        if (state_.logChannel == static_cast<int32_t>(id)) state_.logChannel = -1;
        if (state_.telemetryChannel == static_cast<int32_t>(id)) state_.telemetryChannel = -1;
    }

    ConnectionState& state_;
    Transport&       link_;
    Dispatcher&      dispatcher_;
    Gate&            gate_;
    uint8_t*         outBuf_;
    size_t           outCap_;
    uint8_t*         inBuf_;
    size_t           inCap_;
};

}   // namespace protocol
