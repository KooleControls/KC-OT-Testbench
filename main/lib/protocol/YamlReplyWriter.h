#pragma once

#include "Fatal.h"
#include "ReplyWriter.h"
#include "Stream.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>

// The console codec's reply half: the same records, drawn for a person.
//
//     ok: true
//     partitions:
//       - label: nvs
//         size: 24576
//
// It is the second implementation of ReplyWriter, and writing it is what the
// interface was for: no handler changed, and nothing above knows a reply left the
// device as YAML rather than as JSON.
//
// ── Records, and why the separator is not a newline here ─────────────────────
//
// ReplyBody.h says a reply is a sequence of records and the writer owns what
// divides them. For JSON that is '\n', which is safe because a JSON record can
// never contain one. A YAML record is nothing BUT newlines, so this codec's
// separator is a line reading `---` -- YAML's own document separator, and
// unambiguous for the same reason the newline is unambiguous over there: every
// line this writer emits inside a record is either indented or ends in ':', and a
// block scalar's lines are indented too, so a bare `---` can only be the divider.
//
// A declared body follows the separator exactly as it follows the newline in JSON,
// so the two codecs differ in the bytes of the rule and not in the rule.
class YamlReplyWriter final : public ReplyWriter
{
public:
    explicit YamlReplyWriter(Stream& out) : out_(out) {}

protected:
    void beginObject() override { OpenScope(false); }
    void beginArray()  override { OpenScope(true); }

    void endObject() override { CloseScope("{}"); }
    void endArray()  override { CloseScope("[]"); }

    /// YAML divides members by line, and every member writes its own line, so there
    /// is nothing left for this to do. The interface asks because JSON needs it.
    void separator() override {}

    void key(const char* name) override
    {
        Member();
        WriteRaw(name);
        WriteRaw(":");
    }

    void endRecord() override
    {
        WriteRaw("\n---\n");
        atStart_ = true;
    }

    Stream& stream() override { return out_; }

    void value(const char* v) override
    {
        if (strchr(v, '\n') != nullptr) { WriteBlock(v); return; }
        BeforeValue();
        WriteQuoted(v);
    }

    void value(bool v) override { BeforeValue(); WriteRaw(v ? "true" : "false"); }

    void value(int32_t v) override
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRId32, v);
        BeforeValue();
        WriteRaw(buf);
    }

    void value(uint32_t v) override
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRIu32, v);
        BeforeValue();
        WriteRaw(buf);
    }

    void value(float v) override
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        BeforeValue();
        WriteRaw(buf);
    }

private:
    // Indentation is the only state a YAML emitter needs that a JSON one does not,
    // and it is bounded: a reply nested deeper than this is a handler bug, not a
    // buffer to grow.
    static constexpr size_t MAX_DEPTH = 8;

    struct Frame { bool isArray; bool empty; };

    Stream& out_;
    Frame   frames_[MAX_DEPTH] = {};
    size_t  depth_ = 0;
    bool    atStart_ = true;      // nothing written in this record yet
    bool    onDashLine_ = false;  // the next key rides an array item's dash

    void OpenScope(bool isArray)
    {
        // An object or array that is an array's ITEM starts on the dash, so its
        // first member needs no line of its own.
        if (depth_ > 0 && frames_[depth_ - 1].isArray)
        {
            ItemLine();
            onDashLine_ = true;
        }
        if (depth_ == MAX_DEPTH)
            FATAL("reply nested deeper than %d", (int)MAX_DEPTH);
        frames_[depth_++] = { isArray, true };
    }

    void CloseScope(const char* emptyForm)
    {
        if (frames_[depth_ - 1].empty)
        {
            // Nothing was written, so say so where the value would have gone.
            if (onDashLine_) onDashLine_ = false;
            else if (!atStart_) WriteRaw(" ");
            WriteRaw(emptyForm);
            atStart_ = false;
        }
        --depth_;
    }

    /// Start the line a key or an array item is written on.
    void Line()
    {
        if (!atStart_) WriteRaw("\n");
        atStart_ = false;
        for (size_t i = 1; i < depth_; ++i) WriteRaw("  ");
    }

    void ItemLine()
    {
        Line();
        WriteRaw("- ");
        frames_[depth_ - 1].empty = false;
    }

    /// A named member of an object: its own line, unless it is riding a dash.
    void Member()
    {
        if (onDashLine_) onDashLine_ = false;
        else Line();
        frames_[depth_ - 1].empty = false;
    }

    /// A value about to be written: an array's item gets a dash and a line, an
    /// object's field follows the colon its key just wrote.
    void BeforeValue()
    {
        if (depth_ > 0 && frames_[depth_ - 1].isArray) ItemLine();
        else WriteRaw(" ");
    }

    /// A multi-line string, as a block scalar. This is most of why the console
    /// codec is worth reading at all: `system describe` answers with paragraphs,
    /// which JSON renders as one long line of `\n`.
    void WriteBlock(const char* v)
    {
        WriteRaw(" |");
        const size_t indent = depth_;   // one level deeper than the key
        for (const char* p = v; *p != '\0';)
        {
            WriteRaw("\n");
            for (size_t i = 0; i < indent; ++i) WriteRaw("  ");
            while (*p != '\0' && *p != '\n')
            {
                if (static_cast<uint8_t>(*p) >= 0x20) out_.write(p, 1);
                ++p;
            }
            if (*p == '\n') ++p;
        }
        atStart_ = false;
    }

    void WriteRaw(const char* s) { out_.write(s, strlen(s)); }

    /// Quoted only where a reader could take the text for something else. Being
    /// unambiguous is the whole job -- nothing parses this back.
    void WriteQuoted(const char* s)
    {
        if (!NeedsQuotes(s))
        {
            for (const char* p = s; *p; ++p)
                if (static_cast<uint8_t>(*p) >= 0x20) out_.write(p, 1);
            return;
        }

        out_.write("\"", 1);
        for (const char* p = s; *p; ++p)
        {
            switch (*p)
            {
            case '"':  out_.write("\\\"", 2); break;
            case '\\': out_.write("\\\\", 2); break;
            default:
                if (static_cast<uint8_t>(*p) >= 0x20) out_.write(p, 1);
                break;
            }
        }
        out_.write("\"", 1);
    }

    static bool NeedsQuotes(const char* s)
    {
        if (s[0] == '\0') return true;
        if (s[0] == ' ' || strchr("-?:,[]{}#&*!|>'\"%@`", s[0]) != nullptr) return true;
        if (s[strlen(s) - 1] == ' ') return true;
        if (strstr(s, ": ") != nullptr || strstr(s, " #") != nullptr) return true;
        if (LooksNumeric(s)) return true;
        return strcmp(s, "true") == 0 || strcmp(s, "false") == 0 ||
               strcmp(s, "null") == 0 || strcmp(s, "~") == 0;
    }

    /// Whether YAML would read this text as a number. Written out rather than
    /// approximated by "starts with a digit", because a firmware version is
    /// "0.0.8" and quoting that would be noise on every `system info`.
    static bool LooksNumeric(const char* s)
    {
        const char* p = s;
        if (*p == '+' || *p == '-') ++p;

        bool digits = false, dot = false;
        for (; *p != '\0'; ++p)
        {
            if (*p >= '0' && *p <= '9') { digits = true; continue; }
            if (*p == '.' && !dot)      { dot = true;    continue; }
            if ((*p == 'e' || *p == 'E') && digits)
            {
                ++p;
                if (*p == '+' || *p == '-') ++p;
                if (*p == '\0') return false;
                for (; *p != '\0'; ++p)
                    if (*p < '0' || *p > '9') return false;
                return true;
            }
            return false;
        }
        return digits;
    }
};
