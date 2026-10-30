#include "RelaySocket.h"

#include <esp_transport_tcp.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace {

inline uint32_t NowMs() { return pdTICKS_TO_MS(xTaskGetTickCount()); }

} // namespace

// ──────────────────────────────────────────────────────────────
// Connect / close
// ──────────────────────────────────────────────────────────────

bool RelaySocket::ParseUri(const char* uri)
{
    const char* p = uri;
    if (strncmp(p, "wss://", 6) == 0)      { tls_ = true;  p += 6; }
    else if (strncmp(p, "ws://", 5) == 0)  { tls_ = false; p += 5; }
    else
    {
        ESP_LOGE(TAG, "relay.url must start with ws:// or wss:// - got '%s'", uri);
        return false;
    }

    const char* slash   = strchr(p, '/');
    const char* hostEnd = slash ? slash : p + strlen(p);

    // Last colon before the path, so a port is optional.
    const char* colon = nullptr;
    for (const char* q = p; q < hostEnd; ++q)
        if (*q == ':') colon = q;

    const size_t hostLen = static_cast<size_t>((colon ? colon : hostEnd) - p);
    if (hostLen == 0 || hostLen >= sizeof(host_))
    {
        ESP_LOGE(TAG, "no usable host in '%s'", uri);
        return false;
    }
    memcpy(host_, p, hostLen);
    host_[hostLen] = '\0';

    port_ = tls_ ? 443 : 80;
    if (colon)
    {
        port_ = atoi(colon + 1);
        if (port_ <= 0 || port_ > 65535)
        {
            ESP_LOGE(TAG, "no usable port in '%s'", uri);
            return false;
        }
    }

    snprintf(path_, sizeof(path_), "%s", slash ? slash : "/");
    return true;
}

RelaySocket::ConnectResult RelaySocket::Connect(const char* uri, int timeoutMs,
                                               const char* extraHeaders)
{
    Close();
    httpStatus_ = 0;

    if (!ParseUri(uri)) return ConnectResult::BadUri;

    parent_ = tls_ ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!parent_)
    {
        ESP_LOGE(TAG, "no %s transport", tls_ ? "TLS" : "TCP");
        return ConnectResult::Unreachable;
    }
    if (tls_)
        esp_transport_ssl_crt_bundle_attach(parent_, esp_crt_bundle_attach);

    ws_ = esp_transport_ws_init(parent_);
    if (!ws_) { Destroy(); return ConnectResult::Unreachable; }

    esp_transport_ws_config_t cfg = {};
    cfg.ws_path = path_;
    // Not copied by the transport either, which is the other half of why the caller
    // has to own this string.
    cfg.headers = extraHeaders;
    // False = the transport answers pings and completes closes itself, inside
    // whatever read is in progress. Those frames never reach the channel layer,
    // which is the only reason this class stays as short as it is.
    cfg.propagate_control_frames = false;
    if (esp_transport_ws_set_config(ws_, &cfg) != ESP_OK)
    {
        Destroy();
        return ConnectResult::Unreachable;
    }

    const int rc = esp_transport_connect(ws_, host_, port_, timeoutMs);

    // Read the status whether connect succeeded or not, because a refusal reports
    // FAILURE here: the transport records the response's status line and only then
    // looks for Sec-WebSocket-Accept, which a refusal has no reason to carry — so it
    // returns -1 having already parsed the very number that explains why. Asking only
    // on the success path is what made a device the relay had refused, in as many
    // words, look to its owner like a host that could not be reached.
    // 0 = no response at all; -1 = a response whose status line did not parse.
    httpStatus_ = esp_transport_ws_get_upgrade_request_status(ws_);
    const bool answered = (httpStatus_ > 0 && httpStatus_ != 101);
    if (httpStatus_ < 0) httpStatus_ = 0;

    if (rc < 0)
    {
        // An answered upgrade means TCP/TLS did come up, so there is a socket to close.
        if (answered) esp_transport_close(ws_);
        Destroy();
        return answered ? ConnectResult::Refused : ConnectResult::Unreachable;
    }

    if (httpStatus_ != 101)
    {
        esp_transport_close(ws_);
        Destroy();
        return ConnectResult::Refused;
    }

    connected_ = true;
    ESP_LOGI(TAG, "connected to %s:%d%s", host_, port_, tls_ ? " (TLS)" : "");
    return ConnectResult::Ok;
}

void RelaySocket::Close()
{
    // Under the send lock: a broadcast from another task must not be writing to a
    // handle while this destroys it.
    LOCK(sendMutex_);

    connected_ = false;
    if (ws_) esp_transport_close(ws_);
    Destroy();
}

void RelaySocket::Destroy()
{
    // Two handles, destroyed separately: the websocket layer frees only its own
    // state, never the transport it was stacked on.
    if (ws_)     { esp_transport_destroy(ws_);     ws_ = nullptr; }
    if (parent_) { esp_transport_destroy(parent_); parent_ = nullptr; }
    connected_ = false;
}

// ──────────────────────────────────────────────────────────────
// Read
// ──────────────────────────────────────────────────────────────

int RelaySocket::ReadFrame(uint8_t* buf, size_t cap, int timeoutMs)
{
    if (!connected_) return -1;

    // Two different waits in one variable. Until the first byte lands, `deadline` is
    // the caller's idle timeout — tens of seconds, because for RelayManager it is the
    // keepalive interval. From the first byte on it becomes a rolling no-progress
    // window, re-armed by every read that returns data.
    const uint32_t idleDeadline = NowMs() + static_cast<uint32_t>(timeoutMs);
    uint32_t deadline = idleDeadline;

    size_t total    = 0;   // bytes of this message assembled so far
    size_t frameGot = 0;   // bytes of the current frame

    // Set the moment the message outgrows `buf`. From there the loop keeps reading
    // exactly as it would otherwise -- over the top of the buffer, which is the
    // point, because the bytes are going in the bin -- and stops at the same FIN.
    // Reading it to the end is what keeps the pipe usable; returning early would
    // leave the tail to be parsed as the next message.
    bool overlong = false;

    for (;;)
    {
        const int32_t left = static_cast<int32_t>(deadline - NowMs());
        if (left <= 0)
        {
            // Half a message that stopped arriving is a broken pipe, not an idle
            // one — the rest is never coming and the channel cannot be completed.
            // `overlong` counts as half-arrived even though `total` has been wound
            // back to zero: the bytes are being discarded, but the message is still
            // in progress and returning 0 here would resume mid-message next time.
            if (total > 0 || overlong)
            {
                ESP_LOGW(TAG, "message stalled after %u bytes", static_cast<unsigned>(total));
                return -1;
            }
            // Nothing at all: silence, or a control frame answered along the way. A
            // peer that closed cleanly also looks like silence, so ask.
            if (esp_transport_ws_poll_connection_closed(ws_, 0) == 1)
            {
                ESP_LOGW(TAG, "peer closed the connection");
                return -1;
            }
            return 0;
        }

        // Once over the window, every further read lands back at the start of the
        // buffer and is overwritten: the message is being discarded, not assembled.
        if (total >= cap)
        {
            if (!overlong)
            {
                ESP_LOGW(TAG, "inbound message over the %u-byte window - discarding "
                         "it, keeping the pipe", static_cast<unsigned>(cap));
                overlong = true;
            }
            total = 0;
        }

        const int n = esp_transport_read(ws_, reinterpret_cast<char*>(buf + total),
                                        static_cast<int>(cap - total), left);
        if (n < 0)
        {
            ESP_LOGW(TAG, "read failed (%d)", n);
            return -1;
        }
        if (n == 0)
            continue;   // nothing yet, or a control frame handled for us

        total    += static_cast<size_t>(n);
        frameGot += static_cast<size_t>(n);
        deadline = NowMs() + STALL_TIMEOUT_MS;   // progress: the message is still moving

        // One frame can take several reads, and one message can take several
        // frames. Only a complete frame carrying the FIN flag ends the message.
        const int payload = esp_transport_ws_get_read_payload_len(ws_);
        if (payload > 0 && frameGot < static_cast<size_t>(payload))
            continue;

        const bool fin = esp_transport_ws_get_fin_flag(ws_);
        const ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(ws_);
        frameGot = 0;

        // The pipe carries binary only, same as the browser socket. Anything else
        // is discarded whole rather than handed up as a channel chunk.
        if (op != WS_TRANSPORT_OPCODES_BINARY && op != WS_TRANSPORT_OPCODES_CONT)
        {
            // Nothing is half-arrived any more, so go back to waiting for a message to
            // start — discarding one must not shorten the caller's idle window.
            total = 0;
            overlong = false;
            deadline = idleDeadline;
            continue;
        }
        if (!fin) continue;

        if (overlong) return READ_TOO_LONG;
        return static_cast<int>(total);
    }
}

// ──────────────────────────────────────────────────────────────
// Write
// ──────────────────────────────────────────────────────────────

bool RelaySocket::SendBinary(const uint8_t* data, size_t len, int timeoutMs)
{
    LOCK(sendMutex_);
    if (!connected_) return false;

    const int sent = esp_transport_ws_send_raw(
        ws_, static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_BINARY |
                                                WS_TRANSPORT_OPCODES_FIN),
        reinterpret_cast<const char*>(data), static_cast<int>(len), timeoutMs);

    return sent == static_cast<int>(len);
}

bool RelaySocket::SendPing(int timeoutMs)
{
    LOCK(sendMutex_);
    if (!connected_) return false;

    return esp_transport_ws_send_raw(
        ws_, static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_PING |
                                                WS_TRANSPORT_OPCODES_FIN),
        nullptr, 0, timeoutMs) >= 0;
}
