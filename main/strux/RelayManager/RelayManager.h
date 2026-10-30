#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "TypedSettings.h"
#include "Task.h"
#include "CommandChannel.h"
#include "ChannelProtocol.h"
#include "WsConnection.h"
#include "RelaySocket.h"
#include "RelayTransport.h"
#include "Connection.h"

class Authenticator;

// The device's outbound connection to a relay server, so the device is reachable
// from outside its LAN without a VPN or a port forward.
//
// It is *only* a transport: a second Transport feeding the same Channel type the
// local browser socket uses, so AuthGate, CommandManager and every command
// handler are reached unchanged and know nothing about the relay. The server asks
// for frontend files with the ordinary `getWebFile` command and relays browser
// traffic as opaque channel chunks.
//
// Why it reads its own socket: docs/reasoning/2026-08-05-13h55-owning-the-read-removes-the-buffer.md
class RelayManager
{
    static constexpr const char* TAG = "RelayManager";

    // Reply window. NOT payload-proportional - a file of any size streams window by
    // window, because Channel splits at this boundary and the reader reassembles
    // until FINAL.
    //
    // PURELY LOCAL, and this transport is free to pick differently from the browser
    // socket: every chunk here is a WAN round trip's worth of framing and, over
    // wss://, one TLS record with its own header, tag and send. Frontend bundles are
    // the common payload, so the count is what costs. Bulk AES-GCM is not the
    // expense on an ESP32 - doing it three hundred times with a syscall each is.
    static constexpr size_t CHANNEL_WINDOW = 4096;

    // THIS CONNECTION'S OWN LIMIT for one inbound chunk. It is not a shared
    // constant and nothing at the other end has to equal it: a peer that frames
    // more than this now loses that one channel, and the pipe and every other
    // channel on it carry on. The relay keeps its own, smaller, send-side number
    // for the same reason we keep this one - see strux-relay's DeviceChunkLimit.
    static constexpr size_t INBOUND_WINDOW = 4096;

    // This task reads the socket AND runs the command, so its stack has to cover the
    // heaviest handler and, on a wss:// pipe, a TLS handshake â€” never at the same
    // time, so it is the larger of the two rather than the sum.
    static constexpr int TASK_STACK = 10240;

    // 16 random bytes as hex. Long enough that guessing is not a strategy, short
    // enough to read off a screen when pairing a board by hand.
    static constexpr size_t TOKEN_BYTES   = 16;
    static constexpr size_t TOKEN_HEX_LEN = TOKEN_BYTES * 2;

    static constexpr int CONNECT_TIMEOUT_MS  = 10000;


    // Retry pacing, and two different kinds of waiting. An unreachable server is
    // usually transient â€” WiFi, DNS, a restart â€” so the first retry is quick and then
    // doubles, because the tenth attempt is no more likely than the ninth and costs a
    // TLS handshake. A refused upgrade is not transient at all: it waits on a person
    // approving this device, so it retries slowly, which is still often enough to keep
    // the device in the server's pending list and to pick up an approval promptly.
    static constexpr int RECONNECT_DELAY_MS     = 5000;
    static constexpr int RECONNECT_DELAY_MAX_MS = 60000;
    static constexpr int REFUSED_DELAY_MS       = 30000;

    // Short, because this is just waiting for WiFi to finish coming up.
    static constexpr int NO_NETWORK_DELAY_MS = 1000;

    // Nothing on an idle pipe means nothing to notice a dead TCP connection by, so we
    // make traffic: a ping that cannot be sent is the signal to reconnect. This is
    // both the keepalive interval and how long an idle read blocks, because they are
    // the same question â€” the read returns exactly when the next ping comes due, so an
    // idle pipe wakes this task once per interval rather than thirty times. Expressing
    // it as a 1 s poll and a count of polls pinned the wake rate at 1 Hz for no reason
    // beyond arithmetic, and that rate is the ceiling on how long a fork enabling light
    // sleep could keep the chip asleep.
    static constexpr int PING_INTERVAL_MS = 30000;
    static constexpr int PING_TIMEOUT_MS = 2000;

    // A log line is worth less than the task that emits it: never wait long.
    static constexpr int BROADCAST_TIMEOUT_MS = 200;

public:
    explicit RelayManager(StruxProvider& strux);

    RelayManager(const RelayManager&) = delete;
    RelayManager& operator=(const RelayManager&) = delete;
    RelayManager(RelayManager&&) = delete;
    RelayManager& operator=(RelayManager&&) = delete;

    void Init();

    bool IsConnected() const { return linkUp_; }

    /// The id this device registers under. Telemetry tags points with it so a
    /// measurement says which board it came from.
    const char* GetDeviceId() const { return deviceId_; }

private:
    StruxProvider& strux_;
    InitState initState_;

    RelaySocket socket_;
    Task task_;
    volatile bool linkUp_ = false;

    Authenticator* auth_ = nullptr;

    // Auth state for the pipe. One WsConnection because the pipe IS one
    // connection: every browser the server relays shares it, so a login by one
    // remote user authenticates the pipe for all of them. Acceptable for a single
    // trusted operator; splitting per-browser needs the server to carry a client
    // identity alongside the channel id.
    WsConnection conn_;

    // Sized for the worst case BuildUri() can produce, which is now only the
    // configured url plus the device id - everything else is an ordinary command.
    char uri_[256] = {};
    char deviceId_[48] = {};

    // 32 hex + NUL. Proves to the server that this device is the id it claims.
    char token_[TOKEN_HEX_LEN + 1] = {};

    // The upgrade request's extra headers, built once in Init and pointed at by
    // RelaySocket on every reconnect â€” so it must outlive Connect().
    char headers_[96] = {};

    // Framing buffers, members rather than stack: this task's stack is sized for
    // the heaviest command handler and must not also carry these.
    //
    // channelInbound_ is the ONLY inbound buffer. A frame is read straight into it
    // and the channel's first chunk is read out of it in place; further chunks of
    // the same request refill it, which is safe because a channel only ever refills
    // once the current chunk is drained. It used to be one of three â€” a reassembly
    // buffer, a heap copy per frame, and this â€” because a frame had to survive being
    // handed between two tasks. One task, one buffer.
    uint8_t channelFrame_[channel::HEADER_LEN + CHANNEL_WINDOW];
    uint8_t channelInbound_[channel::HEADER_LEN + INBOUND_WINDOW];

    /// How much telemetry goes in one frame. Comfortably inside the 4096 window and
    /// enough for a dozen points, which is more than a drain ever finds at once.
    static constexpr size_t TELEMETRY_BATCH = 1024;

    /// Open the two streams this device pushes on, once the handshake has settled.
    /// Idempotent: a channel the peer RESET is simply reopened next time round.
    void OpenStreams();

    /// Ship whatever the console and telemetry rings are holding. Called from the
    /// read loop between requests, never from a producer's task.
    void DrainConsole();
    void DrainTelemetry();

    void BuildUri();

    void ResolveDeviceId();
    void ResolveToken();
    void TaskLoop();
    void HandleFrame(const uint8_t* frame, size_t len);

    /// Log this task's stack headroom when it reaches a new low. Called after every
    /// command, because a command handler runs on this task and is the deepest thing
    /// that ever will â€” as is, on a wss:// pipe, the TLS handshake.
    void CheckStackHeadroom();
    size_t stackLow_ = SIZE_MAX;

    void OnConnected();
    void OnDisconnected();

    /// Report a failed connect attempt and return how long to wait before the next
    /// one. Logs at most once per distinct reason â€” see the definition for why that
    /// matters more here than the usual "log every failure" instinct.
    int ReportConnectFailure(RelaySocket::ConnectResult result);

    // Connect-failure bookkeeping for the above: what went wrong last time, and how
    // many identical failures have gone unmentioned since.
    RelaySocket::ConnectResult lastFailure_ = RelaySocket::ConnectResult::Ok;
    int      lastFailureStatus_  = 0;
    uint32_t suppressedFailures_ = 0;
    int      reconnectDelayMs_   = RECONNECT_DELAY_MS;

    // â”€â”€ Settings (registered with SettingsManager in Init) â”€â”€
    inline static BoolSetting   enabled_  { "relay.enabled",  "Relay Enabled",   false };
    inline static StringSetting url_      { "relay.url",      "Relay Server URL", "" };
    // Empty â†’ derived from the WiFi MAC, so a fresh device registers without being
    // told who it is. The MAC is the *technical* identity; what a human reads in the
    // relay's device list is device.name, which travels alongside it for display only.
    // Set this only to pin an id that should outlive the board it started on.
    inline static StringSetting deviceId_setting_{ "relay.deviceId", "Relay Device ID", "" };

    // The device's proof that it is the id it claims. Empty â†’ generated on first
    // Init and stored, so the secret is created here and never travels except
    // inside TLS. There is no serverâ†’device message that can set it.
    inline static StringSetting token_setting_{ "relay.token", "Relay Device Token", "" };
};
