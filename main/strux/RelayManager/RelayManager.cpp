#include "RelayManager.h"
#include "SettingsManager.h"
#include "NetworkManager.h"
#include "ConsoleManager.h"
#include "TelemetryManager.h"
#include "CommandManager.h"
#include "AuthManager.h"
#include "AuthGate.h"
#include "Authenticator.h"

#include "SystemManager.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {

inline uint32_t NowMs() { return pdTICKS_TO_MS(xTaskGetTickCount()); }

} // namespace

RelayManager::RelayManager(StruxProvider& strux)
    : strux_(strux)
{
}

void RelayManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    strux_.getSettingsManager().Register(
        { &enabled_, &url_, &deviceId_setting_, &token_setting_ });

    auth_ = &strux_.getAuthManager().GetAuthenticator();

    if (!enabled_.Get())
    {
        ESP_LOGI(TAG, "Disabled (set relay.enabled to connect)");
        initAttempt.SetReady();
        return;
    }

    char url[128] = {};
    url_.Get(url, sizeof(url));
    if (url[0] == '\0')
    {
        ESP_LOGW(TAG, "relay.enabled is set but relay.url is empty - not connecting");
        initAttempt.SetReady();
        return;
    }

    ResolveDeviceId();
    ResolveToken();
    BuildUri();

    // Built once: RelaySocket keeps the pointer and re-sends these on every
    // reconnect, so a stack buffer here would be a dangling one.
    snprintf(headers_, sizeof(headers_), "X-Strux-Token: %s\r\n", token_);

    // Two IDF components narrate every single connect attempt, and this task
    // reconnects for the lifetime of the device. The certificate bundle announces each
    // successful validation at INFO, which is only news the first time. transport_ws
    // reports a refused upgrade as an ERROR about a missing handshake header, which
    // describes the symptom one layer below the cause — ReportConnectFailure() says
    // the same thing as "the relay refused this device", with the status and the id
    // needed to act on it. Raise either one when debugging the transport itself.
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("transport_ws", ESP_LOG_NONE);

    // One task connects, reads the socket, and runs the commands it reads. That is
    // the whole transport: there is no second task and no queue between them, because
    // the socket underneath is one this task reads rather than one that calls it back
    // (see RelaySocket). Reconnecting is part of the same loop.
    task_.Init("relay", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
    {
        ESP_LOGE(TAG, "Failed to start relay task");
        return;
    }

    ESP_LOGI(TAG, "Connecting to %s", uri_);
    initAttempt.SetReady();
}

void RelayManager::ResolveDeviceId()
{
    char configured[sizeof(deviceId_)] = {};
    deviceId_setting_.Get(configured, sizeof(configured));
    if (configured[0] != '\0')
    {
        strlcpy(deviceId_, configured, sizeof(deviceId_));
        return;
    }

    // No configured id → derive one from the MAC so a fresh device registers
    // without being told who it is. esp_read_mac works before WiFi starts.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(deviceId_, sizeof(deviceId_), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void RelayManager::ResolveToken()
{
    char stored[sizeof(token_)] = {};
    token_setting_.Get(stored, sizeof(stored));
    if (stored[0] != '\0')
    {
        strlcpy(token_, stored, sizeof(token_));
        return;
    }

    // First boot with the relay enabled: mint one and keep it. esp_fill_random is
    // the hardware RNG, seeded well enough for this once WiFi/BT is up — and it is,
    // because Init runs after NetworkManager.
    uint8_t raw[TOKEN_BYTES] = {};
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < TOKEN_BYTES; ++i)
        snprintf(token_ + i * 2, 3, "%02x", raw[i]);

    if (!token_setting_.Set(token_))
    {
        // Not fatal, but say so plainly: a token that is not stored is a new identity
        // on every boot, which means re-approving the device after every reboot.
        ESP_LOGE(TAG, "failed to store relay.token - it will change on reboot");
        return;
    }
    ESP_LOGI(TAG, "generated a relay token for this device");
}

void RelayManager::BuildUri()
{
    char url[128] = {};
    url_.Get(url, sizeof(url));

    // IDENTITY ONLY. `id` is technical, is what the token proves, and is the address
    // in every relay URL — nothing else belongs in a string that is logged, proxied
    // and cached on the way. What this device is CALLED, what it runs and what it was
    // built from are answered by `system info` on an ordinary channel, where adding a
    // fact costs a key rather than a query parameter, a percent-encoder and a bigger
    // buffer on both sides.
    const char* sep = strchr(url, '?') ? "&" : "?";
    snprintf(uri_, sizeof(uri_), "%s%sid=%s", url, sep, deviceId_);
}

// How many bytes `value` occupies once escaped. Only the two characters JSON
// requires, and control characters dropped: a device name is typed by a human and
// may hold a quote or a backslash; it may not hold a newline, because nothing that
// sets one allows it.
static size_t EscapedLen(const char* value)
{
    size_t n = 0;
    for (const char* v = value; *v; ++v)
    {
        const unsigned char c = static_cast<unsigned char>(*v);
        if (c < 0x20) continue;
        n += (c == '"' || c == '\\') ? 2 : 1;
    }
    return n;
}

// Appends a JSON "key":"value" pair, whole or not at all. False means it did not
// fit, and the caller decides what that is worth — this function cannot know which
// key mattered. Whole-or-nothing because the alternative is a truncated value that
// still looks like a value: a device list showing half a commit sha is worse than
// one showing none, and a cut in the middle of an escape would not even parse.
static bool AppendPair(char* out, size_t cap, const char* key, const char* value)
{
    const size_t n = strlen(out);

    //          ,     "key"            :     "value"
    const size_t need = 1 + 1 + strlen(key) + 1 + 1 + 1 + EscapedLen(value) + 1;
    if (n + need + 1 > cap) return false;   // +1 for the NUL

    size_t w = n;
    out[w++] = ',';
    out[w++] = '"';
    for (const char* k = key; *k; ++k) out[w++] = *k;
    out[w++] = '"';
    out[w++] = ':';
    out[w++] = '"';

    for (const char* v = value; *v; ++v)
    {
        const unsigned char c = static_cast<unsigned char>(*v);
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[w++] = '\\';
        out[w++] = static_cast<char>(c);
    }

    out[w++] = '"';
    out[w] = '\0';
    return true;
}


// ──────────────────────────────────────────────────────────
// The pipe: connect, read, dispatch, repeat
// ──────────────────────────────────────────────────────────

void RelayManager::TaskLoop()
{
    uint32_t nextPingAt = 0;

    for (;;)
    {
        if (!socket_.IsConnected())
        {
            // Nothing to dial with yet. This task starts during Init, while WiFi is
            // still associating, so without this every boot spends a connect attempt
            // it cannot win and logs three ERROR lines from the TLS and transport
            // layers on the way out. Same check covers WiFi dropping later.
            if (!strux_.getNetworkManager().HasUpstream())
            {
                vTaskDelay(pdMS_TO_TICKS(NO_NETWORK_DELAY_MS));
                continue;
            }

            const auto result = socket_.Connect(uri_, CONNECT_TIMEOUT_MS, headers_);
            if (result == RelaySocket::ConnectResult::BadUri)
            {
                // uri_ is built once in Init and cannot change without a reboot, so
                // retrying a URL the parser already rejected would only reprint its
                // complaint forever. Stop the task instead; the settings UI is where
                // this gets fixed, and the fix takes effect on the next boot.
                ESP_LOGE(TAG, "relay.url is not usable - not retrying until reboot");
                return;
            }
            if (result != RelaySocket::ConnectResult::Ok)
            {
                vTaskDelay(pdMS_TO_TICKS(ReportConnectFailure(result)));
                continue;
            }

            if (suppressedFailures_ > 0)
                ESP_LOGI(TAG, "connected after %u further failed attempt%s",
                         static_cast<unsigned>(suppressedFailures_),
                         suppressedFailures_ == 1 ? "" : "s");
            lastFailure_        = RelaySocket::ConnectResult::Ok;
            lastFailureStatus_  = 0;
            suppressedFailures_ = 0;
            reconnectDelayMs_   = RECONNECT_DELAY_MS;

            OnConnected();
            // Before anything else on this pipe: both peers send their handshake
            // unprompted, so neither leads and the same code works on a transport
            // with no dialer. What this device IS used to go out here as a hello on
            // a reserved channel; it is an ordinary `system info` command now, which
            // the relay calls once the connection is READY.
            {
                RelayTransport link(socket_, INBOUND_WINDOW);
                protocol::SendHandshake(conn_.conn, link);
            }
            // Right after connect, because on a wss:// pipe the TLS handshake just
            // ran on this stack and is one of the two things it has to fit.
            CheckStackHeadroom();
            nextPingAt = NowMs() + PING_INTERVAL_MS;
        }

        // Between requests this is where the task sits — blocked until a frame arrives
        // or the next keepalive comes due, whichever happens first. Mid-request the
        // same read happens under the channel, one layer down (RelayTransport) —
        // same socket, same task, which is the property that removed the queue.
        // Between requests is the only safe point: mid-request this task is inside
        // a handler that owns the socket, which is v1's accepted head-of-line
        // blocking. A long upload delays logs; it does not lose them.
        OpenStreams();
        DrainConsole();
        DrainTelemetry();

        int32_t untilPing = static_cast<int32_t>(nextPingAt - NowMs());
        if (untilPing < 0) untilPing = 0;

        const int n = socket_.ReadFrame(channelInbound_, sizeof(channelInbound_),
                                        untilPing);
        if (n == RelaySocket::READ_TOO_LONG)
        {
            // The relay framed more than this link takes in one chunk. The message
            // has been read to its end and discarded, so the pipe is still in sync
            // -- and the channel id went with the bytes, so there is nothing to
            // refuse by name. Saying so and carrying on beats dropping every other
            // channel on the pipe, which is what this used to do.
            ESP_LOGW(TAG, "oversized inbound chunk discarded - pipe kept");
            continue;
        }
        if (n < 0)
        {
            OnDisconnected();
            continue;
        }

        if (n == 0)
        {
            // The read ran its whole deadline out with nothing to show, which is what
            // being idle looks like — so the ping is due. A ping that will not go out
            // is how an otherwise idle pipe finds out its TCP connection is gone.
            nextPingAt = NowMs() + PING_INTERVAL_MS;
            if (!socket_.SendPing(PING_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "keepalive ping failed");
                OnDisconnected();
            }
            continue;
        }

        HandleFrame(channelInbound_, static_cast<size_t>(n));

        // After the request rather than before it: a channel that took a while has
        // just proven the pipe alive, and the next keepalive is owed from here.
        nextPingAt = NowMs() + PING_INTERVAL_MS;
    }
}

void RelayManager::OnConnected()
{
    // A reconnect is a fresh pipe: drop the old channel state.
    conn_.reset();
    conn_.fd = -1;   // "slot in use" — there is no socket fd on this transport

    // Authentication belongs to the INTERFACE, and this one authenticates by
    // existing: the device dialled OUT, over TLS, to a URL its owner configured,
    // presenting a token it generated itself. That is proof of peer at the link
    // layer — the same basis as a bonded Bluetooth transport, which AuthGate
    // already describes as "a policy difference rather than a structural one" —
    // so the pipe is authed the moment it is up and never sees `auth`.
    //
    // This line used to read `!(auth_ && auth_->AuthRequired())`, which is the
    // WEB interface's policy. Setting web.password — a LAN concern — therefore
    // locked the relay out of `web read`, leaving the asset proxy unable to fetch
    // even the login page that would have unlocked it.
    conn_.authed = true;

    // A fresh pipe knows no channels and has not handshaken. The cursor starts at
    // the tip because a reconnect is not a reason to replay.
    conn_.conn.Reset();
    conn_.conn.logCursor = strux_.getConsoleManager().Tip();
    linkUp_ = true;

    // Says WHY the pipe is open, which is no longer "nobody set a password": this
    // interface authenticates by its own dial-out, so web.password never gated it.
    ESP_LOGI(TAG, "Connected as '%s' (relay interface - authenticated by dialling out)",
             deviceId_);
}

int RelayManager::ReportConnectFailure(RelaySocket::ConnectResult result)
{
    // Log a REASON, not an attempt. This loop retries forever, so a line per attempt
    // is a line per attempt forever: a device left un-approved used to emit three of
    // them every five seconds — one here plus two from the TLS and websocket layers —
    // and a console that scrolls that is a console nobody reads. What is worth saying
    // is said once, when it changes, and the count of what went unsaid is reported by
    // the connect that eventually succeeds.
    const int status = socket_.LastHttpStatus();
    if (result == lastFailure_ && status == lastFailureStatus_)
    {
        suppressedFailures_++;
    }
    else
    {
        lastFailure_        = result;
        lastFailureStatus_  = status;
        suppressedFailures_ = 0;

        if (result == RelaySocket::ConnectResult::Refused && status == 403)
            ESP_LOGW(TAG, "relay refused this device - approve '%s' on the relay "
                          "server; retrying every %ds",
                     deviceId_, REFUSED_DELAY_MS / 1000);
        else if (result == RelaySocket::ConnectResult::Refused)
            ESP_LOGW(TAG, "relay refused the upgrade with HTTP %d", status);
        else
            ESP_LOGW(TAG, "cannot reach the relay at %s - retrying, backing off to %ds",
                     uri_, RECONNECT_DELAY_MAX_MS / 1000);
    }

    if (result != RelaySocket::ConnectResult::Unreachable)
        return REFUSED_DELAY_MS;

    // Double up to the cap, and hand back the delay this attempt earned.
    const int delay = reconnectDelayMs_;
    reconnectDelayMs_ = (reconnectDelayMs_ >= RECONNECT_DELAY_MAX_MS / 2)
                          ? RECONNECT_DELAY_MAX_MS
                          : reconnectDelayMs_ * 2;
    return delay;
}

void RelayManager::OnDisconnected()
{
    linkUp_ = false;
    socket_.Close();

    // Nothing to unblock: a handler waiting for its next chunk is waiting on a read
    // of this same socket, on this same task, so it has already returned by now.
    ESP_LOGW(TAG, "Disconnected - will retry");
    vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
}

void RelayManager::HandleFrame(const uint8_t* frame, size_t len)
{
    if (len < channel::HEADER_LEN) return;

    uint16_t id    = channel::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + channel::HEADER_LEN;
    size_t plen = len - channel::HEADER_LEN;

    // Identical to the local transport's frame path (WebSocketHandler::HandleBinary),
    // because everything above Transport is shared: the Connection decides what this
    // frame is, the gate says what may run yet, and CommandManager runs the command.
    //
    // The residue skip that used to live here is gone with it. It was this transport's
    // own workaround for having no channel table -- the tail of a request whose handler
    // returned early, which read as a fresh chunk would be taken for a request header
    // and invent a command out of firmware bytes. The table knows the id is draining,
    // so the frames are dropped by lookup rather than by a mode flag, and the local
    // WebSocket -- which never had the workaround, and therefore had the bug -- gets
    // the same handling from the same code.
    RelayTransport link(socket_, INBOUND_WINDOW);
    AuthGate gate(conn_, *auth_);

    protocol::Connection<CommandManager, AuthGate> connection(
        conn_.conn, link, strux_.getCommandManager(), gate,
        channelFrame_, CHANNEL_WINDOW,
        channelInbound_, sizeof(channelInbound_));
    connection.OnFrame(id, flags, payload, plen);

    if (conn_.conn.phase == ConnectionState::Phase::Failed)
    {
        // A version the relay does not share, or nonces that would not settle.
        // Nothing on this pipe can be understood, so drop it and let the reconnect
        // loop try again.
        ESP_LOGE(TAG, "handshake failed - dropping the pipe");
        OnDisconnected();
    }

    // The handler just ran on this stack; if it was the deepest one yet, say so.
    CheckStackHeadroom();
}

void RelayManager::CheckStackHeadroom()
{
    // ESP-IDF returns bytes here, not words as vanilla FreeRTOS does.
    const size_t free = uxTaskGetStackHighWaterMark(nullptr);
    if (free >= stackLow_) return;
    stackLow_ = free;

    // A quarter left is the point where the next slightly deeper handler, or a TLS
    // handshake against a server with a longer certificate chain, stops fitting.
    if (free < TASK_STACK / 4)
        ESP_LOGW(TAG, "stack headroom down to %u of %d bytes",
                 static_cast<unsigned>(free), TASK_STACK);
    else
        ESP_LOGI(TAG, "stack headroom %u of %d bytes",
                 static_cast<unsigned>(free), TASK_STACK);
}

// ──────────────────────────────────────────────────────────────
// Passive streams
//
// Both are drained from the read loop, between requests, which is the whole change:
// the console task and whichever task took a measurement used to call in here and
// block on RelaySocket::sendMutex_. Every LOCK() is under ContextLock, so a stalled
// TLS write turned three producers into a reboot. Now one task writes this socket.
//
// Neither drain logs on failure. A line raised here would be stored, drained, sent,
// fail, and log again; ConsoleManager::DrainScope covers the lines this code causes
// further down the stack, and saying nothing ourselves covers the rest.
// ──────────────────────────────────────────────────────────────

void RelayManager::OpenStreams()
{
    if (!linkUp_ || conn_.conn.phase != ConnectionState::Phase::Ready) return;

    RelayTransport link(socket_, INBOUND_WINDOW);

    if (conn_.conn.logChannel < 0)
        protocol::OpenPassiveChannel(conn_.conn, link, protocol::LOG_STREAM_ENVELOPE,
                                     conn_.conn.logChannel);

    if (conn_.conn.telemetryChannel < 0 && strux_.getTelemetryManager().IsEnabled())
        protocol::OpenPassiveChannel(conn_.conn, link, protocol::TELEMETRY_STREAM_ENVELOPE,
                                     conn_.conn.telemetryChannel);
}

void RelayManager::DrainConsole()
{
    if (!linkUp_ || conn_.conn.logChannel < 0) return;

    const uint16_t id = static_cast<uint16_t>(conn_.conn.logChannel);
    auto& console = strux_.getConsoleManager();
    ConsoleManager::DrainScope guard(console);

    char json[ConsoleManager::JSON_CAP];
    uint8_t frame[channel::HEADER_LEN + ConsoleManager::JSON_CAP];

    // No FINAL: this direction stays open for the life of the connection, and the
    // peer ends it with RESET when it stops wanting logs.
    while (size_t n = console.ReadJson(conn_.conn.logCursor, json, sizeof(json)))
    {
        channel::writeHeader(frame, id, 0);
        memcpy(frame + channel::HEADER_LEN, json, n);
        if (!socket_.SendBinary(frame, channel::HEADER_LEN + n, BROADCAST_TIMEOUT_MS))
            return;
    }
}

void RelayManager::DrainTelemetry()
{
    if (!linkUp_ || conn_.conn.telemetryChannel < 0) return;

    const uint16_t id = static_cast<uint16_t>(conn_.conn.telemetryChannel);
    auto& telemetry = strux_.getTelemetryManager();
    if (!telemetry.IsEnabled()) return;

    char batch[TELEMETRY_BATCH];
    uint8_t frame[channel::HEADER_LEN + TELEMETRY_BATCH];

    for (;;)
    {
        uint32_t lines = 0;
        const size_t n = telemetry.Drain(batch, sizeof(batch), lines);
        if (n == 0) return;

        channel::writeHeader(frame, id, 0);
        memcpy(frame + channel::HEADER_LEN, batch, n);

        if (socket_.SendBinary(frame, channel::HEADER_LEN + n, BROADCAST_TIMEOUT_MS))
            telemetry.ReportSent(lines);
        else
        {
            // Drain is destructive, so a failed batch is gone. Counted rather than
            // requeued: telemetry is explicitly lossy, and a retry queue is the
            // thing this ring exists to avoid.
            telemetry.ReportDropped(lines);
            return;
        }
    }
}
