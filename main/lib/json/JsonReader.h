#pragma once

#include "Stream.h"
#include "JsonHelpers.h"
#include "esp_log.h"
#include <cstddef>
#include <cstdint>

// Buffered JSON request reader. Consumes the request stream into an
// internal bounded buffer at construction, then serves typed getters
// (the JsonHelpers free functions as methods). Deliberately NOT a
// streaming parser — command payloads are a few hundred bytes; a
// streaming implementation can replace the internals later without
// touching any handler.
template <size_t N = 1024>
class JsonReader
{
    char buf_[N];
    size_t len_ = 0;

public:
    explicit JsonReader(Stream& in)
    {
        // Timeout 0: in phase 1 `in` is always a MemoryStream (data
        // already in RAM). Revisit for socket-backed streams in phase 2.
        while (len_ < N - 1)
        {
            size_t n = in.read(buf_ + len_, N - 1 - len_, 0);
            if (n == 0) break;
            len_ += n;
        }
        buf_[len_] = '\0';

        if (in.available() > 0)
            ESP_LOGE("JsonReader", "Request truncated: capacity %u exhausted, %u bytes left unread",
                     static_cast<unsigned>(N), static_cast<unsigned>(in.available()));
    }

    JsonReader(const JsonReader&) = delete;
    JsonReader& operator=(const JsonReader&) = delete;

    bool GetString(const char* key, char* out, size_t maxLen) const
    {
        return ExtractJsonString(buf_, key, out, maxLen);
    }

    int32_t GetInt(const char* key, int32_t def = 0) const
    {
        return ExtractJsonInt(buf_, key, def);
    }

    bool GetBool(const char* key, bool def = false) const
    {
        const char* v = FindJsonField(buf_, key);
        if (!v) return def;
        return *v == 't';   // JSON literals: true / false / null
    }
};
