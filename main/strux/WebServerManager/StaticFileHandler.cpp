#include "StaticFileHandler.h"
#include "WebAssets.h"

#include <cstring>
#include <esp_log.h>

static constexpr const char* TAG = "StaticFileHandler";

void StaticFileHandler::RegisterRoute(httpd_handle_t server)
{
    const httpd_uri_t route = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = Handle,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server, &route);
}

const char* StaticFileHandler::GetContentType(const char* ext)
{
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".woff2") == 0) return "font/woff2";
    return "application/octet-stream";
}

bool StaticFileHandler::Resolve(const char* uri, Resolved& out)
{
    // Strip query string
    char clean[256];
    if (const char* query = strchr(uri, '?'))
    {
        size_t len = static_cast<size_t>(query - uri);
        if (len >= sizeof(clean)) len = sizeof(clean) - 1;
        memcpy(clean, uri, len);
        clean[len] = '\0';
        uri = clean;
    }

    if (uri[0] == '\0' || strcmp(uri, "/") == 0) uri = "/index.html";

    // Blob names are relative to www/ ("index.html", "assets/index-….js"), while
    // a URI arrives with a leading slash and the relay may omit it. One form
    // reaches the table.
    if (uri[0] == '/') ++uri;

    out.contentType = "application/octet-stream";
    if (const char* ext = strrchr(uri, '.')) out.contentType = GetContentType(ext);

    // The packer gzips whatever shrinks under it, per file. `gzipped` must reach
    // the client as Content-Encoding, or it receives gzip bytes labelled as
    // JavaScript.
    WebFile file;
    if (!WebAssets().Find(uri, file)) return false;

    out.data = file.data;
    out.size = file.size;
    out.gzipped = file.gzipped;
    return true;
}

esp_err_t StaticFileHandler::Handle(httpd_req_t* req)
{
    Resolved file;
    if (!Resolve(req->uri, file))
    {
        // SPA fallback lives here, in the route layer — not in Resolve(), which
        // stays "give me this exact file or nothing".
        if (!Resolve("/index.html", file))
        {
            ESP_LOGW(TAG, "no index.html in the embedded bundle");
            httpd_resp_send_404(req);
            return ESP_OK;
        }
        file.contentType = "text/html";
    }

    httpd_resp_set_type(req, file.contentType);
    if (file.gzipped)
    {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    // Caching, and it has to be told rather than left to the browser.
    //
    // This server sent no Cache-Control, no ETag and no Last-Modified, which does
    // not mean "do not cache" — it means the browser may guess, and Chrome guesses
    // yes. That is wrong in the worst way for the URLs here whose names are
    // deliberately STABLE: `/index.html` does not change when its contents do, so a
    // frontend updated by OTA kept being served from disk cache and the new UI
    // simply did not appear. Nothing was broken and nothing said so.
    //
    // Two rules, decided by whether the name identifies the bytes:
    //   /assets/<name>-<hash>.js  content-hashed by the build, so a change is a new
    //                             URL and the old one can be kept forever.
    //   everything else           index.html above all, whose name is stable, so it
    //                             must be revalidated every load.
    //
    // `no-cache` rather than `no-store`: the browser may still keep the bytes, it
    // just may not use them without asking. There is nothing to revalidate WITH yet
    // — no ETag — so today that is a plain refetch, which is what an ESP32 serving a
    // page a handful of times a day should do. An ETag is the optimisation, not the
    // fix, and the blob now carries a per-file hash to build one from.
    const bool hashedAsset = strncmp(req->uri, "/assets/", 8) == 0;
    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        hashedAsset ? "public, max-age=31536000, immutable" : "no-cache");

    // One send, no read buffer and no chunking: the bytes are flash-mapped rodata
    // that httpd copies straight to the socket, and a known length means a real
    // Content-Length instead of a chunked response.
    httpd_resp_send(req, reinterpret_cast<const char*>(file.data),
                    static_cast<ssize_t>(file.size));
    return ESP_OK;
}
