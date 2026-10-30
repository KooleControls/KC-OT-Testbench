#pragma once

#include "Stream.h"
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstring>

class JsonWriter
{
    Stream& stream_;

    static constexpr int32_t MAX_DEPTH = 16;
    bool needsComma_[MAX_DEPTH] = {};
    int32_t depth_ = 0;

    void raw(const char* s, int32_t n = -1)
    {
        if (n < 0) n = static_cast<int32_t>(strlen(s));
        stream_.write(s, n);
    }

    void comma()
    {
        if (depth_ > 0 && needsComma_[depth_])
            raw(",");
        if (depth_ > 0)
            needsComma_[depth_] = true;
    }

    void writeKey(const char* name)
    {
        comma();
        raw("\"");
        raw(name);
        raw("\":");
    }

    void writeString(const char* s)
    {
        raw("\"");
        for (const char* p = s; *p; p++)
        {
            switch (*p)
            {
            case '"':  raw("\\\""); break;
            case '\\': raw("\\\\"); break;
            case '\n': raw("\\n"); break;
            case '\r': raw("\\r"); break;
            case '\t': raw("\\t"); break;
            default:
                if (static_cast<uint8_t>(*p) >= 0x20)
                    stream_.write(p, 1);
                break;
            }
        }
        raw("\"");
    }

public:
    explicit JsonWriter(Stream& stream) : stream_(stream) {}

    JsonWriter& beginObject()
    {
        comma();
        raw("{");
        depth_++;
        needsComma_[depth_] = false;
        return *this;
    }

    JsonWriter& endObject()
    {
        raw("}");
        if (depth_ > 0) depth_--;
        return *this;
    }

    JsonWriter& beginArray()
    {
        comma();
        raw("[");
        depth_++;
        needsComma_[depth_] = false;
        return *this;
    }

    JsonWriter& endArray()
    {
        raw("]");
        if (depth_ > 0) depth_--;
        return *this;
    }

    // ── Field methods (key:value inside an object) ──────────

    JsonWriter& field(const char* name, const char* value)
    {
        writeKey(name);
        writeString(value);
        return *this;
    }

    JsonWriter& field(const char* name, int32_t value)
    {
        writeKey(name);
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, value);
        raw(buf);
        return *this;
    }

    JsonWriter& field(const char* name, uint32_t value)
    {
        writeKey(name);
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRIu32, value);
        raw(buf);
        return *this;
    }

    JsonWriter& field(const char* name, float value)
    {
        writeKey(name);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", value);
        raw(buf);
        return *this;
    }

    JsonWriter& field(const char* name, bool value)
    {
        writeKey(name);
        raw(value ? "true" : "false");
        return *this;
    }

    JsonWriter& nullField(const char* name)
    {
        writeKey(name);
        raw("null");
        return *this;
    }

    JsonWriter& fieldObject(const char* name)
    {
        writeKey(name);
        raw("{");
        depth_++;
        needsComma_[depth_] = false;
        return *this;
    }

    JsonWriter& fieldArray(const char* name)
    {
        writeKey(name);
        raw("[");
        depth_++;
        needsComma_[depth_] = false;
        return *this;
    }

    // ── Value methods (for array elements) ──────────────────

    JsonWriter& value(const char* v)
    {
        comma();
        writeString(v);
        return *this;
    }

    JsonWriter& value(int32_t v)
    {
        comma();
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, v);
        raw(buf);
        return *this;
    }

    JsonWriter& value(bool v)
    {
        comma();
        raw(v ? "true" : "false");
        return *this;
    }
};
