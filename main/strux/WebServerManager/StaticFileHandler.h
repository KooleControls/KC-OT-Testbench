#pragma once

#include <esp_http_server.h>
#include <cstddef>
#include <cstdint>

class StaticFileHandler {
public:
    // A resolved static file: the bytes as they are stored in the app image,
    // plus the two HTTP facts a route layer needs to serve them.
    struct Resolved {
        const uint8_t* data;
        uint32_t       size;
        const char*    contentType;
        bool           gzipped;
    };

    // Logical path → embedded file. Shared by the local HTTP route and the
    // `web read` command that serves the relay, so both agree on gzip flag and
    // MIME type. Accepts an optional query string and a path with or without a
    // leading '/'.
    //
    // Deliberately does NOT do SPA fallback — a missing path returns false.
    // Falling back to index.html is an HTTP decision that belongs to each route
    // layer, which keeps a mistyped asset a real 404 instead of HTML with status
    // 200 (which a browser rejects as a MIME error).
    //
    // There is no path-traversal check any more, and none is missing: the lookup
    // is an exact name match against the blob's directory (WebAssets), so "../"
    // in a request simply matches nothing. It was a filesystem hazard, and there
    // is no filesystem.
    static bool Resolve(const char* uri, Resolved& out);

    void RegisterRoute(httpd_handle_t server);

private:
    static esp_err_t Handle(httpd_req_t* req);
    static const char* GetContentType(const char* ext);
};
