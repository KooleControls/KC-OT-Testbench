#pragma once

#include <esp_http_server.h>
#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "StaticFileHandler.h"
#include "WebSocketHandler.h"
#include "Task.h"

class Stream;

class WebServerManager {
    static constexpr const char* TAG = "WebServerManager";

public:
    explicit WebServerManager(StruxProvider& strux);

    WebServerManager(const WebServerManager&) = delete;
    WebServerManager& operator=(const WebServerManager&) = delete;
    WebServerManager(WebServerManager&&) = delete;
    WebServerManager& operator=(WebServerManager&&) = delete;

    void Init();

private:
    /// How often the pump looks for new log lines. The LAN transport is purely
    /// reactive -- esp_http_server calls into it only when a frame arrives -- so
    /// unlike the relay, which drains inside its own read loop, this side needs
    /// something to do the walking. Short enough that a console feels live,
    /// long enough that an idle device is not waking ten times a second.
    static constexpr int PUMP_INTERVAL_MS = 100;

    Task consolePump_;
    void ConsolePumpLoop();

    StruxProvider& strux_;

    InitState initState;
    httpd_handle_t server_ = nullptr;

    StaticFileHandler staticFileHandler_;
    WebSocketHandler wsHandler_;

    // No settings and no credentials of its own: the password, the Authenticator
    // and the `auth` commands are AuthManager's, and this manager borrows the
    // authority through the provider like any other peer. No HTTP auth surface
    // remains either — the WebSocket carries all device interaction.

    void StartServer();
    void RegisterRoutes();

    // ── Commands (registered with CommandManager in Init) ──

    // Serve one frontend file by logical path. This is the whole of the relay's
    // access to the device's frontend: the server asks for "/index.html" and
    // never learns that it lives gzipped in a blob embedded in the app image.
    // Reply is a header line then the raw bytes:
    //
    //   {"ok":true,"status":200,"contentType":"...","contentEncoding":"gzip"}\n<bytes>
    CommandResult Cmd_GetWebFile(CommandContext& ctx);

    // Defined in WebServerManager.cpp, beside the handler and the argument it reads.
    static CommandEntry readCommand_;
};
