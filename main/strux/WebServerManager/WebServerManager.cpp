#include "WebServerManager.h"
#include "ConsoleManager.h"
#include "CommandManager.h"
#include "AuthManager.h"
#include "JsonHelpers.h"

#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <esp_log.h>

static constexpr const char* TAG = "WebServerManager";

// The manager, reachable from a C callback that carries no context of its own.
//
// `httpd_config_t::close_fn` is a plain function pointer with signature
// `(httpd_handle_t, int fd)` — no user pointer, unlike every other callback here,
// which is why this one alone needs a static. Every other callback here takes a
// user pointer; a capturing lambda cannot convert to this one, so the `this` has to
// come from somewhere outside the call.
//
// Safe in practice for the reason a singleton usually is not: there is exactly one
// WebServerManager, it is owned by StruxContext for the life of the process, and
// `Init()` is guarded by an InitState — so the pointer is written once, before the
// server that would call it exists, and never dangles. Checked for null anyway,
// because the httpd task can outlive a failed Init.
static WebServerManager* s_instance_ = nullptr;

namespace {

// The old destination was char[192], so 191 characters is the longest path this
// command has ever accepted -- declared now instead of deduced from a buffer.
constexpr CommandArg<const char*> pathArg{
    "path", "Path of the file within the device's web assets, e.g. 'index.html' or "
            "'assets/index.js'. An empty path or a directory resolves to index.html.",
    191 };

} // namespace

CommandEntry WebServerManager::readCommand_{
    "web read", &InvokeCommand<&WebServerManager::Cmd_GetWebFile>,
    "Read one file of the device's own web UI. The reply is a JSON header "
    "line (status, content type, encoding), a newline, then the raw bytes - "
    "which may be gzipped. It serves the device's browser page; it is not a "
    "general filesystem.",
    { &pathArg }
};

WebServerManager::WebServerManager(StruxProvider& strux)
    : strux_(strux)
{
}

void WebServerManager::Init()
{
    auto initAttempt = initState.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    s_instance_ = this;

    wsHandler_.SetCommandManager(strux_.getCommandManager());
    wsHandler_.SetConsole(strux_.getConsoleManager());

    // Borrowed, not owned: the credential authority is the DEVICE's, and this
    // manager is one of two transports that put a gate in front of it.
    wsHandler_.SetAuth(strux_.getAuthManager().GetAuthenticator());

    StartServer();
    RegisterRoutes();

    strux_.getCommandManager().Register(this, { &readCommand_ });

    // Log delivery is a PULL now: every connection holds a cursor into the console
    // ring and this task walks them. What that removes is not the callback so much
    // as the fan-out that hung off it -- this manager used to hand each line to its
    // own clients AND to RelayManager, which is a web server reaching sideways into
    // a peer for no reason except that ConsoleManager had room for one subscriber.
    // The relay now drains the same ring from its own read loop and neither knows
    // the other exists.
    consolePump_.Init("ConsolePump", 4, 3072);
    consolePump_.SetHandler([this]() { ConsolePumpLoop(); });
    consolePump_.Run();

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void WebServerManager::ConsolePumpLoop()
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(PUMP_INTERVAL_MS));
        if (server_) wsHandler_.PumpLogs(server_, strux_.getConsoleManager());
    }
}

void WebServerManager::StartServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 20;
    config.close_fn = [](httpd_handle_t, int fd) {
        if (s_instance_)
            s_instance_->wsHandler_.OnClientDisconnected(fd);
        close(fd);
    };
    config.lru_purge_enable = true;

    // esp_http_server narrates a client disappearing as three warnings — a recv
    // errno, an unmasked frame read from the corpse of the connection, and a failed
    // send — none of which a reader can act on, and all of which a browser produces
    // every time a tab closes. At ERROR these components still report faults that
    // are this device's own. Raise them when debugging the transport itself.
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_ws", ESP_LOG_ERROR);

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}

void WebServerManager::RegisterRoutes()
{
    if (!server_) return;

    // HTTP serves two things only: the WebSocket upgrade (which carries ALL
    // device interaction — commands, uploads, downloads, auth) and the static
    // app that bootstraps the page. No /api command route, no CORS: every
    // device interaction is a channel on the one socket.
    wsHandler_.RegisterRoute(server_);
    staticFileHandler_.RegisterRoute(server_);
}

// ──────────────────────────────────────────────────────────────
// Commands
// ──────────────────────────────────────────────────────────────

CommandResult WebServerManager::Cmd_GetWebFile(CommandContext& ctx)
{
    const char* path = ctx.arg(pathArg);

    StaticFileHandler::Resolved file;
    const bool found = StaticFileHandler::Resolve(path, file);

    if (!found)
    {
        // A real 404 — SPA fallback is the asking route layer's decision, not
        // ours (see StaticFileHandler::Resolve).
        //
        // The record is ENDED even though no body follows, because this command's
        // reply is a header line and then a file, and a 404's file is empty rather
        // than absent. The relay's asset proxy reads it that way and refuses a reply
        // with no header line at all, so the separator is part of the contract here
        // rather than a separator between two records.
        {
            auto head = ctx.reply.object();
            head.field("ok", true);
            head.field("status", static_cast<uint32_t>(404));
        }
        ctx.reply.flush();
        return CommandResult::Ok;   // the request was fine; the file simply is not there
    }

    auto head = ctx.reply.object();
    head.field("ok", true);
    head.field("status", static_cast<uint32_t>(200));
    if (file.gzipped)
        head.field("contentEncoding", "gzip");

    // Declaring the body seals the header record and hands back the stream; the
    // separator between them is the writer's business, not this handler's.
    Stream& body = head.body(file.contentType);

    // One write of the whole file, straight from flash-mapped rodata: Channel::write
    // splits it across the channel window itself, so a 200 KB bundle still needs no
    // 200 KB buffer here or on the transport — and no read buffer at all now that
    // there is no file to read.
    body.write(file.data, file.size);
    return CommandResult::Ok;
}
