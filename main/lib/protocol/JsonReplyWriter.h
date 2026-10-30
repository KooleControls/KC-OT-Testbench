#pragma once

#include "ReplyWriter.h"
#include "Stream.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>

// ReplyWriter over today's JSON replies. The syntax half only: braces, commas, quoting
// and escaping. All ordering and lifetime logic lives in ReplyScope, so this class holds
// no state beyond the stream — every byte goes straight out, and a reply of any size
// costs the same nothing that JsonScope already cost.
//
// It is the mirror of DecodeJsonArgs, and the reason a handler can be written without
// naming a wire format. Unlike that one it holds no buffer at all: the reply side was
// never the place the RAM went.
class JsonReplyWriter final : public ReplyWriter
{
public:
    explicit JsonReplyWriter(Stream& out) : out_(out) {}

protected:
    void beginObject() override { out_.write("{", 1); }
    void endObject()   override { out_.write("}", 1); }
    void beginArray()  override { out_.write("[", 1); }
    void endArray()    override { out_.write("]", 1); }
    void separator()   override { out_.write(",", 1); }

    void key(const char* name) override { WriteEscaped(name); out_.write(":", 1); }

    // A record it produced can never contain a raw newline: strings are escaped and
    // every control character below 0x20 is dropped. See ReplyBody.h.
    void endRecord() override { out_.write("\n", 1); }

    Stream& stream() override { return out_; }

    void value(const char* v) override { WriteEscaped(v); }
    void value(bool v)        override { WriteRaw(v ? "true" : "false"); }

    void value(int32_t v) override
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, v);
        WriteRaw(buf);
    }

    void value(uint32_t v) override
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRIu32, v);
        WriteRaw(buf);
    }

    void value(float v) override
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        WriteRaw(buf);
    }

private:
    Stream& out_;

    void WriteRaw(const char* s) { out_.write(s, strlen(s)); }

    void WriteEscaped(const char* s)
    {
        out_.write("\"", 1);
        for (const char* p = s; *p; p++)
        {
            switch (*p)
            {
            case '"':  out_.write("\\\"", 2); break;
            case '\\': out_.write("\\\\", 2); break;
            case '\n': out_.write("\\n", 2); break;
            case '\r': out_.write("\\r", 2); break;
            case '\t': out_.write("\\t", 2); break;
            default:
                if (static_cast<uint8_t>(*p) >= 0x20)
                    out_.write(p, 1);
                break;
            }
        }
        out_.write("\"", 1);
    }
};
