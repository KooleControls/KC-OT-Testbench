#pragma once

#include "Mutex.h"
#include "ContextLock.h"
#include <esp_transport.h>
#include <cstdint>
#include <cstddef>

// A WebSocket the owner READS, rather than one that calls the owner back.
//
// This is the whole reason the relay can look like the browser socket. The
// high-level esp_websocket_client owns a task and delivers frames through a
// callback, and a callback cannot be the bottom of a streaming handler: the handler
// asks for its next chunk, that chunk is only read when the client's loop runs
// again, and the loop only runs once the callback returns — which it cannot,
// because the handler is inside it. A second task plus a queue of heap-copied
// frames was the way around that, and the queue is what dropped chunks under load.
//
// One floor down, the same handshake and framing are an ordinary blocking read. So
// this is just connect / ReadFrame / SendBinary, and one task does both halves of a
// request. What the client used to do for us and this now does: pick the URL apart,
// and report a dead link so the caller can reconnect. Control frames stay someone
// else's problem — configured not to propagate them, the transport answers a ping
// and completes a close inside the read we are already making.
class RelaySocket
{
    static constexpr const char* TAG = "RelaySocket";

public:
    RelaySocket() = default;
    ~RelaySocket() { Close(); }

    RelaySocket(const RelaySocket&) = delete;
    RelaySocket& operator=(const RelaySocket&) = delete;

    /// Why a Connect() attempt ended. The caller needs the distinction to decide how
    /// hard to retry: a refused upgrade waits on a decision somebody has to make
    /// (approving this device), so hammering it achieves nothing, while an unreachable
    /// server is worth trying again soon.
    enum class ConnectResult
    {
        Ok,
        BadUri,        ///< relay.url is not a ws:// or wss:// URL — retrying cannot fix it
        Unreachable,   ///< no TCP/TLS, or no HTTP response at all
        Refused,       ///< the server answered the upgrade — see LastHttpStatus()
    };

    /// Open `ws://…` or `wss://…` and complete the upgrade. Anything but Ok leaves us
    /// closed. Failures are reported, not logged: how loudly a failed connect is worth
    /// mentioning depends on how often the caller retries, which is the caller's policy.
    ///
    /// `extraHeaders` is raw `Key: Value\r\n` lines added to the upgrade request, or
    /// nullptr. It is NOT copied — the caller keeps it alive, because a reconnect
    /// re-sends it. This is how the relay presents its token: a header goes up with
    /// the handshake, so the server can refuse the upgrade outright and the channel
    /// protocol above learns nothing about authentication.
    ConnectResult Connect(const char* uri, int timeoutMs, const char* extraHeaders = nullptr);
    void Close();

    bool IsConnected() const { return connected_; }

    /// The HTTP status the last refused upgrade carried, or 0 when the last attempt
    /// never got an HTTP response at all. Only meaningful next to `Refused`.
    int LastHttpStatus() const { return httpStatus_; }

    /// A message longer than `buf`: read to its end and discarded, so the PIPE IS
    /// STILL IN SYNC and every other channel on it is unharmed. Distinct from -1
    /// because the two want opposite responses -- reconnect, versus refuse one
    /// channel and carry on. It used to be -1, which let one oversized message
    /// from the relay drop the whole connection.
    static constexpr int READ_TOO_LONG = -2;

    /// The next data message, whole, into `buf`. Returns its length; 0 when nothing
    /// arrived within `timeoutMs`; -1 when the link is dead and wants reconnecting;
    /// READ_TOO_LONG when it did not fit (see above). Fragmented messages are
    /// stitched together, so a WS frame boundary is never a channel chunk boundary.
    ///
    /// `timeoutMs` bounds the wait for a message to START. Once one has started,
    /// STALL_TIMEOUT_MS bounds how long it may make no progress — so a caller free to
    /// wait half a minute for silence to break does not also wait half a minute to
    /// notice a pipe that died halfway through a chunk.
    int ReadFrame(uint8_t* buf, size_t cap, int timeoutMs);

    /// One binary frame. Safe from any task: a channel's reply and a log broadcast
    /// come from different ones, and a frame must not be split down the middle.
    bool SendBinary(const uint8_t* data, size_t len, int timeoutMs);

    /// A keepalive ping. Failing to send one is how a silently dead TCP connection
    /// gets noticed on an otherwise idle pipe — when to send one is the caller's
    /// policy, because that is a question about the relay, not about sockets.
    bool SendPing(int timeoutMs);

private:
    // How long a message that has started arriving may make no progress before the
    // pipe is called dead. Every byte re-arms it, so a slow-but-moving transfer is
    // never cut off; only actual silence mid-message is. It belongs here rather than
    // with the caller's timeout because "a started message must keep moving" is a
    // fact about the socket, not a policy about the relay.
    static constexpr uint32_t STALL_TIMEOUT_MS = 2000;

    esp_transport_handle_t ws_     = nullptr;   // the layer we read and write
    esp_transport_handle_t parent_ = nullptr;   // tcp or ssl underneath it
    bool connected_ = false;
    int  httpStatus_ = 0;                       // of the last upgrade response, if any

    Mutex sendMutex_;

    // Parsed out of the URI. The path carries the query string, which is where the
    // relay puts the device id — so it is not decoration.
    char host_[96]  = {};
    char path_[192] = {};
    int  port_ = 0;
    bool tls_  = false;

    bool ParseUri(const char* uri);
    void Destroy();
};
