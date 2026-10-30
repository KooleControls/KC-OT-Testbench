#include "WebSocketHandler.h"
#include "CommandManager.h"
#include "Authenticator.h"
#include "AuthGate.h"
#include "WsTransport.h"   // the concrete Transport for this socket
#include "Connection.h"
#include "ConsoleManager.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cstring>

static constexpr const char* TAG = "WebSocketHandler";

// The inbound frame-drain primitive (the private httpd_ws_get_frame_type wart)
// now lives in WsTransport::RecvChunk; Channel::read() pulls streamed request
// bodies through it. See WsTransport.h.

void WebSocketHandler::SetCommandManager(CommandManager& commandManager)
{
    commandManager_ = &commandManager;
}

void WebSocketHandler::SetAuth(Authenticator& auth)
{
    auth_ = &auth;
}

void WebSocketHandler::SetConsole(ConsoleManager& console)
{
    console_ = &console;
}

void WebSocketHandler::RegisterRoute(httpd_handle_t server)
{
    const httpd_uri_t ws_route = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = HandleWs,
        .user_ctx = this,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &ws_route);
}

// ──────────────────────────────────────────────────────────────
// Client tracking
// ──────────────────────────────────────────────────────────────

bool WebSocketHandler::AddWsClient(httpd_req_t* req, ConsoleManager& console)
{
    const int fd = httpd_req_to_sockfd(req);
    bool authed = !(auth_ && auth_->AuthRequired());   // empty password ⇒ authed at connect
    WsConnection* c = registry_.add(fd, authed, esp_timer_get_time());
    if (!c) return false;

    c->conn.logCursor = console.Tip();

    // Unprompted, the moment the socket exists. esp_http_server has already sent
    // the 101 by the time this handler runs, so the frame is legal here, and
    // sending rather than answering is what keeps the handshake symmetric: neither
    // peer leads, and the same code works on a transport with no dialer at all.
    WsTransport link(req, sendMutex_, INBOUND_WINDOW);
    protocol::SendHandshake(c->conn, link);
    return true;
}

void WebSocketHandler::RemoveWsClient(int fd)
{
    registry_.remove(fd);
}

void WebSocketHandler::TouchClient(int fd)
{
    if (auto* c = registry_.find(fd); c && c->authed)
        auth_->TouchKey(c->key);   // TouchKey locks its own table
}

void WebSocketHandler::OnClientDisconnected(int fd)
{
    RemoveWsClient(fd);
}

void WebSocketHandler::PumpLogs(httpd_handle_t server, ConsoleManager& console)
{
    // Snapshot fds under the registry lock, then work outside it. Holding the lock
    // across a send would deadlock the moment httpd internals called back into us.
    int fds[ConnectionRegistry::MAX];
    int count = 0;
    registry_.forEach([&](const WsConnection& c) {
        if (c.authed && count < ConnectionRegistry::MAX) fds[count++] = c.fd;
    });
    if (count == 0) return;

    // Everything logged from here until this returns is a consequence of shipping
    // logs, so it is stored but never shipped. Without this a dead socket feeds
    // itself: the send fails, the failure logs, the log is sent, it fails.
    ConsoleManager::DrainScope guard(console);

    char json[ConsoleManager::JSON_CAP];
    uint8_t frame[channel::HEADER_LEN + ConsoleManager::JSON_CAP];

    for (int i = 0; i < count; i++)
    {
        WsConnection* c = registry_.find(fds[i]);
        if (!c) continue;

        // READY *and* authed. READY is the protocol saying the id spaces are
        // settled; authed is this transport's own policy, and on the LAN they are
        // not the same question -- web.password may be set, and a browser that has
        // not logged in has no business being handed the device's log stream. On
        // the relay pipe authed is true from connect, so the two coincide there
        // and the difference only shows up here.
        if (c->conn.phase != ConnectionState::Phase::Ready) continue;

        WsPumpTransport link(server, fds[i], sendMutex_);

        if (c->conn.logChannel < 0)
        {
            if (!protocol::OpenPassiveChannel(c->conn, link, protocol::LOG_STREAM_ENVELOPE,
                                              c->conn.logChannel))
                continue;
        }

        const uint16_t id = static_cast<uint16_t>(c->conn.logChannel);

        while (size_t n = console.ReadJson(c->conn.logCursor, json, sizeof(json)))
        {
            // No FINAL: this direction stays open for the life of the connection.
            // The peer ends it with RESET when it stops wanting logs.
            channel::writeHeader(frame, id, 0);
            memcpy(frame + channel::HEADER_LEN, json, n);

            if (!link.SendRaw(frame, channel::HEADER_LEN + n))
            {
                // DEBUG, not WARN. A browser that closes a tab takes its socket
                // with it without a close frame, so the next send fails - every
                // page close produced two scary lines about a device that was
                // working perfectly. Removing the client IS the handling.
                ESP_LOGD(TAG, "log pump failed to fd=%d, removing", fds[i]);
                registry_.remove(fds[i]);
                break;
            }
        }
    }
}

// ──────────────────────────────────────────────────────────────
// WebSocket frame handling
// ──────────────────────────────────────────────────────────────

// Take an over-long frame off the socket and refuse it on its own channel.
//
// The channel id is in the first three bytes, which are the first three bytes of
// the payload we are about to throw away -- so the drain keeps the first gulp
// long enough to read them, and the sender learns which request died and why.
// A frame too short to even carry a header has no channel to refuse, so it is
// simply discarded.
//
// Nothing here touches the client slot: an oversized frame is a fault in one
// request, not a reason to drop a browser.
void WebSocketHandler::RefuseOverlongFrame(httpd_req_t* req, size_t len, size_t cap)
{
    ESP_LOGW(TAG, "inbound frame %u > %u-byte window - refusing the channel, "
             "keeping the socket", static_cast<unsigned>(len),
             static_cast<unsigned>(cap));

    size_t remaining = len;
    bool   haveHeader = false;
    uint16_t sid = 0;

    while (remaining > 0)
    {
        httpd_ws_frame_t g = {};
        g.len = remaining < cap ? remaining : cap;
        g.payload = inboundFrame_;
        // `len` already set, so this skips the header parse and reads exactly that
        // many bytes. Every gulp after the first is unmasked from the wrong offset
        // because IDF restarts the mask cursor per call and does not expose it --
        // which is fine for bytes on their way to the bin, and is why the header is
        // taken from the FIRST gulp only.
        if (httpd_ws_recv_frame(req, &g, g.len) != ESP_OK)
        {
            ESP_LOGD(TAG, "drain failed - dropping the client");
            RemoveWsClient(httpd_req_to_sockfd(req));
            return;
        }
        if (!haveHeader && g.len >= channel::HEADER_LEN)
        {
            sid = channel::readU16(inboundFrame_);
            haveHeader = true;
        }
        remaining -= g.len;
    }

    if (!haveHeader) return;

    WsTransport link(req, sendMutex_, INBOUND_WINDOW);
    protocol::SendReset(link, sid, "chunk over the inbound window");
}

esp_err_t WebSocketHandler::HandleWs(httpd_req_t* req)
{
    auto* self = static_cast<WebSocketHandler*>(req->user_ctx);

    if (req->method == HTTP_GET)
    {
        // The WS now opens UNAUTHENTICATED — auth is an in-band handshake
        // (see AuthGate). esp_http_server has already sent the 101; we
        // only need a client slot. A full table (after reaping stale un-authed
        // sockets) refuses the upgrade so the client hits its reconnect loop.
        if (!self->AddWsClient(req, *self->console_))
            return ESP_FAIL;
        return ESP_OK;
    }

    // Two steps, not one, and the reason is the whole of issue #39. A single
    // httpd_ws_recv_frame with a max_len refuses an over-long frame WITHOUT
    // consuming its payload, so the next read takes body bytes for a frame header
    // and the connection is finished -- which is how one oversized frame from one
    // browser used to drop the socket with no reply and no reason. Reading the
    // header first lets an over-long frame be drained and refused on its own
    // channel, leaving every other channel on this socket alone.
    const size_t cap = sizeof(self->inboundFrame_) - 1;

    httpd_ws_frame_t frame = {};                   // len == 0 -> header-only read
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK)
    {
        // Also DEBUG: the common cause is the peer vanishing, which is not this
        // device's problem and not something a reader can act on.
        ESP_LOGD(TAG, "WS recv failed: %s", esp_err_to_name(ret));
        self->RemoveWsClient(httpd_req_to_sockfd(req));
        return ret;
    }

    if (frame.len > cap)
    {
        self->RefuseOverlongFrame(req, frame.len, cap);
        return ESP_OK;
    }

    if (frame.len > 0)
    {
        frame.payload = self->inboundFrame_;
        ret = httpd_ws_recv_frame(req, &frame, cap);
        if (ret != ESP_OK)
        {
            ESP_LOGD(TAG, "WS payload recv failed: %s", esp_err_to_name(ret));
            self->RemoveWsClient(httpd_req_to_sockfd(req));
            return ret;
        }
    }

    // Any inbound frame (heartbeat included) keeps the channel alive —
    // an open tab never logs out; see spec.
    self->TouchClient(httpd_req_to_sockfd(req));

    if (frame.type == HTTPD_WS_TYPE_CLOSE)
    {
        self->RemoveWsClient(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    if (frame.type == HTTPD_WS_TYPE_BINARY)
    {
        if (frame.len >= channel::HEADER_LEN)
            self->HandleBinary(req, self->inboundFrame_, frame.len);
        return ESP_OK;
    }

    // Inbound TEXT frames are no longer used: requests are binary channel
    // chunks and no client sends text. Ignore any stray text frame.
    return ESP_OK;
}

// ──────────────────────────────────────────────────────────────
// Binary channel transport. One inbound binary frame = one
// channel chunk; step-1 requests are a single chunk dispatched synchronously.
// ──────────────────────────────────────────────────────────────

void WebSocketHandler::HandleBinary(httpd_req_t* req, const uint8_t* frame, size_t len)
{
    uint16_t sid   = channel::readU16(frame);
    uint8_t  flags = frame[2];
    const uint8_t* payload = frame + channel::HEADER_LEN;
    size_t plen = len - channel::HEADER_LEN;
    int fd = httpd_req_to_sockfd(req);

    // All inbound frames are processed single-threaded on the httpd task, so the
    // AuthGate below is the only writer of this connection's state (authed/key).
    // The log pump runs on another task and may reset (remove) a slot concurrently,
    // but it only ever *clears* a slot — it never sets `authed` — so the auth gate
    // can't be defeated by that race, and a cleared slot reads as empty (self-
    // healing). If a worker task ever consumes these pointers (step 6), this needs
    // real locking (copy-under-lock, as the pre-refactor code did).
    WsConnection* conn = registry_.find(fd);
    if (!conn)
    {
        // The socket is live as far as httpd is concerned, but it has no slot. Two
        // ways in: the broadcast path evicts a slot when a send fails while httpd
        // keeps the connection, and a refused upgrade (table full) is returned after
        // esp_http_server has already sent the 101. Returning silently left the
        // client holding an established socket, waiting out its timeout for a reply
        // that was never coming — every command swallowed, nothing logged. Refuse the
        // channel instead, so the failure lands at the caller rather than in a
        // timeout.
        ESP_LOGW(TAG, "frame on fd=%d with no client slot - refusing channel %u",
                 fd, (unsigned)sid);
        WsTransport link(req, sendMutex_, INBOUND_WINDOW);
        Channel s(sid, link, channelFrame_, CHANNEL_WINDOW,
                  channelInbound_, sizeof(channelInbound_));
        s.feedRequest(payload, plen, (flags & channel::FLAG_FINAL) != 0);
        s.reject("connection has no client slot");
        return;
    }

    // Both transports now enter the protocol at the same point. The Connection is
    // built on this stack because the two things it wraps are per frame here --
    // WsTransport holds this call's httpd_req_t, AuthGate holds this connection's
    // slot -- while the state that has to outlive the frame, the channel table,
    // lives in the slot itself.
    //
    // The gate decides what may run before this connection has authenticated, and
    // lends its auth state to the `auth` handlers. No handshake parsing here any more
    // — the handshake is three ordinary commands.
    WsTransport link(req, sendMutex_, INBOUND_WINDOW);
    AuthGate gate(*conn, *auth_);

    protocol::Connection<CommandManager, AuthGate> connection(
        conn->conn, link, *commandManager_, gate,
        channelFrame_, CHANNEL_WINDOW,
        channelInbound_, sizeof(channelInbound_));
    connection.OnFrame(sid, flags, payload, plen);

    if (conn->conn.phase == ConnectionState::Phase::Failed)
    {
        // Version mismatch, or a handshake that never settled. Nothing on this
        // socket can be understood, so end it rather than leave it half-open.
        ESP_LOGW(TAG, "connection on fd=%d failed its handshake - dropping", fd);
        registry_.remove(fd);
    }
}
