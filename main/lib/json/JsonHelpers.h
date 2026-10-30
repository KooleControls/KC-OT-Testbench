#pragma once

#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// ──────────────────────────────────────────────────────────────
// Fixed-capacity buffer for building JSON responses (no heap)
// ──────────────────────────────────────────────────────────────

struct FixBuf {
    char* data;
    int32_t len = 0;
    int32_t cap;

    FixBuf(char* buf, int32_t capacity) : data(buf), cap(capacity) { data[0] = '\0'; }

    FixBuf(const FixBuf&) = delete;
    FixBuf& operator=(const FixBuf&) = delete;

    int32_t remaining() const { return cap - 1 - len; }

    void append(const char* s, int32_t n = -1)
    {
        if (n < 0) n = static_cast<int32_t>(strlen(s));
        int32_t avail = remaining();
        if (n > avail) n = avail;
        if (n <= 0) return;
        memcpy(data + len, s, n);
        len += n;
        data[len] = '\0';
    }

    void appendf(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        int32_t avail = remaining();
        if (avail <= 0) return;
        va_list args;
        va_start(args, fmt);
        int32_t written = vsnprintf(data + len, avail + 1, fmt, args);
        va_end(args);
        if (written > 0)
            len += (written > avail) ? avail : written;
    }
};

// ──────────────────────────────────────────────────────────────
// JSON parsing helpers
// ──────────────────────────────────────────────────────────────

// Find the value position for a top-level JSON field by name.
// Properly skips over string values so field names inside strings don't match.
// Returns pointer to the first non-whitespace character after the colon, or nullptr.
inline const char* FindJsonField(const char* json, const char* field)
{
    if (!json || !field) return nullptr;

    size_t fieldLen = strlen(field);
    const char* p = json;

    while (*p)
    {
        // Skip whitespace and structural characters
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',' || *p == '{' || *p == '[')) p++;
        if (!*p) break;

        if (*p == '"')
        {
            // Read a quoted string — check if it's our field name
            p++; // skip opening quote
            const char* keyStart = p;
            while (*p && *p != '"')
            {
                if (*p == '\\' && *(p + 1)) p++;
                p++;
            }
            size_t keyLen = p - keyStart;
            if (*p == '"') p++;

            while (*p == ' ' || *p == '\t') p++;
            if (*p == ':')
            {
                p++;
                while (*p == ' ' || *p == '\t') p++;

                if (keyLen == fieldLen && memcmp(keyStart, field, fieldLen) == 0)
                    return p;

                // Skip the value
                if (*p == '"')
                {
                    p++;
                    while (*p && *p != '"')
                    {
                        if (*p == '\\' && *(p + 1)) p++;
                        p++;
                    }
                    if (*p == '"') p++;
                }
                else if (*p == '{' || *p == '[')
                {
                    char open = *p, close = (*p == '{') ? '}' : ']';
                    int depth = 1;
                    p++;
                    while (*p && depth > 0)
                    {
                        if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && *(p+1)) p++; p++; } if (*p) p++; continue; }
                        if (*p == open) depth++;
                        else if (*p == close) depth--;
                        p++;
                    }
                }
                else
                {
                    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
                }
            }
        }
        else
        {
            p++;
        }
    }
    return nullptr;
}

inline bool ExtractJsonString(const char* json, const char* field, char* out, size_t outSize)
{
    const char* val = FindJsonField(json, field);
    if (!val || *val != '"') return false;

    val++; // skip opening quote
    size_t i = 0;
    while (*val && *val != '"' && i < outSize - 1)
    {
        if (*val == '\\' && *(val + 1)) val++;
        out[i++] = *val++;
    }
    out[i] = '\0';
    return true;
}

inline int32_t ExtractJsonInt(const char* json, const char* field, int32_t defaultVal = 0)
{
    const char* val = FindJsonField(json, field);
    if (!val) return defaultVal;
    if (*val == 'n') return defaultVal; // null
    return static_cast<int32_t>(atoi(val));
}
