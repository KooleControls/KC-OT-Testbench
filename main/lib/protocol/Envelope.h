#pragma once

#include "CommandArgs.h"   // CommandResult, ArgDesc, and the request's size limits
#include "Stream.h"

// ─────────────────────────────────────────────────────────────────────────────
// A request, in two halves: the LINE, and what a codec makes of it.
//
// A request is one '\n'-terminated line followed by whatever bytes the command's
// body is. That much is the protocol's, and is true of every codec: reading the
// line is what leaves the stream at the first body byte, so a body is never
// buffered and never bounded -- it is simply what is left.
//
// What the line MEANS is a codec's. Two speak it today:
//
//     {"type":"led set","enabled":true}      JsonEnvelope
//     led set enabled=true                   ConsoleEnvelope
//
// The split exists because the sniff requires it: which codec carried a request
// cannot be known until the line has been read, so reading it cannot belong to
// either one. RunCommandChannel reads the line, looks at it, and builds the
// envelope that claims it.
// ─────────────────────────────────────────────────────────────────────────────

/// The request's first line, read once and owned here. Knows no codec.
class RequestLine
{
public:
    /// Consumes the line, leaving `in` positioned at the body.
    explicit RequestLine(Stream& in)
    {
        size_t i = 0;
        char c;
        while (i < sizeof(line_) - 1)
        {
            if (in.read(&c, 1) != 1) { overflowed_ = false; break; }   // stream ended
            if (c == '\n')           { overflowed_ = false; break; }
            line_[i++] = c;
        }
        line_[i] = '\0';
    }

    RequestLine(const RequestLine&) = delete;
    RequestLine& operator=(const RequestLine&) = delete;

    /// The line, NUL-terminated and without its newline. Mutable: a codec decodes
    /// in place, which is what lets a decoded string point into this buffer
    /// instead of being copied somewhere else.
    char* text() { return line_; }

    /// True when the line filled the buffer without ever reaching its newline. That
    /// is a different fault from a line a codec cannot read, and saying which is
    /// which is worth the flag: reporting the route when the fault was the size cost
    /// an afternoon once.
    bool overflowed() const { return overflowed_; }

private:
    char line_[protocol::MAX_ENVELOPE] = {};
    bool overflowed_ = true;   // until the read proves otherwise
};

/// What the dispatcher is handed: a command name, and a way to fill that command's
/// declared arguments. Everything about syntax is behind these two calls, which is
/// the whole of what a second codec has to implement on the request side.
class Envelope
{
public:
    /// The command this request names, exactly as the registry holds it
    /// ("partition write"), or "" when the line names none.
    virtual const char* command() const = 0;

    /// Fill `values` from this request, against the command's declarations.
    /// `failed` names the argument a failure was about.
    virtual CommandResult decode(const ArgDesc* const* args, ArgValues& values,
                                 const char*& failed) = 0;

protected:
    ~Envelope() = default;   // never owned through this type
};
