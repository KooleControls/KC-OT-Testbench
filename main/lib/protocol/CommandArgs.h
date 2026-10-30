#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

// A command's arguments, DECLARED rather than discovered.
//
// Each argument is a `constexpr CommandArg<T>` living beside the handler that reads
// it; the command's entry names the ones it takes. Nothing here is executed to be
// read, which is the whole point: `help` walks these declarations, where it used to
// re-dispatch the handler with a reader that printed instead of filling.
//
//     constexpr CommandArg<const char*> labelArg{
//         "partition", "Label of the partition, as 'partition list' reports it.", 16 };
//     constexpr CommandArg<uint32_t> offsetArg{
//         "offset", "Byte offset to write at.", Presence::Optional };
//
//     ... in the command table:  { ..., { &labelArg, &offsetArg } }
//     ... in the handler:        const char* label = ctx.arg(labelArg);
//
// Arguments are flat, named, typed and finite. A body, if the command takes one, is
// whatever remains on the request stream once these have been decoded -- it is not
// an argument and is never buffered.
//
// This header names no layer and pulls in nothing but the standard library, so the
// decoder below is verified on the host (test/test_command_args.cpp) rather than on
// a board.

// ─────────────────────────────────────────────────────────────────────────────
// How big a request may be. Here rather than beside the line reader, because they
// bound what the decoders below write into and the reader needs a Stream -- which
// on this project means FreeRTOS, and would put a layer under a header that names
// none. They are limits on the REQUEST, and the request's layer-free half is this
// file.
namespace protocol
{
    // The envelope line, bounding the request but NOT the body - bodies stream.
    //
    // ONE constant, because there used to be two. The router had 128 and the
    // argument reader had 512, so a command could declare arguments that made an
    // envelope it could never be routed with: an envelope whose "type" landed past
    // byte 127 was refused with a message about the route, for a fault in the
    // length. Six ordinary arguments were enough to cross it.
    inline constexpr size_t MAX_ENVELOPE = 512;

    // Command names are short by convention; a longer one simply won't match.
    inline constexpr size_t MAX_COMMAND_NAME = 32;
}

/// Ways a REQUEST can be unusable. Closed set, owned by the framework -- anything a
/// command author wants to add is meaning, and belongs in the reply.
enum class CommandResult : uint8_t
{
    Ok = 0,
    UnknownCommand,
    MissingArgument,
    MalformedNumber,
    ArgumentTooLong,
    MalformedRequest,
};

enum class ArgType : uint8_t { String, UInt32, Int32, Float, Bool };

/// Whether the caller must supply this argument. Presence and default value are
/// deliberately different questions: an absent optional argument is absent, and a
/// handler that wants a fallback writes one (`ctx.has(a) ? ctx.arg(a) : 0`), which
/// keeps "the caller said 0" distinguishable from "the caller said nothing".
enum class Presence : uint8_t { Required, Optional };

/// One declared argument, type-erased so that a command's list is an ordinary array
/// of pointers and the decoder is one ordinary function.
struct ArgDesc
{
    const char* name;

    /// What this argument MEANS, for whoever has to compose a call without reading
    /// the source -- a person at `help`, or a model at the other end of the relay's
    /// MCP surface. A name and a type say how to spell a value, never which one:
    /// `address` is a uint32 in both a partition write and a WiFi command. Units,
    /// ranges and defaults belong here.
    const char* description;

    ArgType type;
    bool    required;

    /// Strings only: the length at which a value is refused. Declared rather than
    /// deduced from a buffer, because the decoded value lives in the envelope buffer
    /// the framework already holds -- there is no handler-side array to take a
    /// `sizeof` of any more.
    uint16_t maxLength;
};

template <typename T>
constexpr ArgType ArgTypeOf()
{
    if constexpr (std::is_same_v<T, const char*>) return ArgType::String;
    else if constexpr (std::is_same_v<T, uint32_t>) return ArgType::UInt32;
    else if constexpr (std::is_same_v<T, int32_t>)  return ArgType::Int32;
    else if constexpr (std::is_same_v<T, float>)    return ArgType::Float;
    else if constexpr (std::is_same_v<T, bool>)     return ArgType::Bool;
    else static_assert(sizeof(T) == 0, "CommandArg supports const char*, uint32_t, int32_t, float and bool");
}

/// A declaration, carrying its value type only so that `ctx.arg(a)` knows what to
/// return. It adds no members to ArgDesc -- the template parameter is a compile-time
/// tag, not machinery.
template <typename T>
struct CommandArg : ArgDesc
{
    constexpr CommandArg(const char* name, const char* description,
                         Presence presence = Presence::Required)
        : ArgDesc{ name, description, ArgTypeOf<T>(),
                   presence == Presence::Required, 0 }
    {
        static_assert(ArgTypeOf<T>() != ArgType::String,
                      "a string argument must declare its maximum length");
    }

    constexpr CommandArg(const char* name, const char* description, uint16_t maxLength,
                         Presence presence = Presence::Required)
        : ArgDesc{ name, description, ArgTypeOf<T>(),
                   presence == Presence::Required, maxLength }
    {
        static_assert(ArgTypeOf<T>() == ArgType::String,
                      "only a string argument has a maximum length");
    }
};

/// The type name self-description reports. One place, because `help` and anything
/// that grows beside it must not disagree about what a caller should send.
inline const char* ArgTypeName(ArgType t)
{
    switch (t)   // no default: a new ArgType must be handled here
    {
    case ArgType::String: return "string";
    case ArgType::UInt32: return "uint32";
    case ArgType::Int32:  return "int32";
    case ArgType::Float:  return "float";
    case ArgType::Bool:   return "bool";
    }
    return "unknown";
}

/// How many arguments one command may declare. Today's busiest takes two; the cap is
/// what keeps the decoded values a fixed-size array on the dispatch stack, and a
/// command that exceeds it fails to compile at its own declaration.
inline constexpr size_t MAX_COMMAND_ARGS = 6;

/// The decoded arguments of one request, positionally parallel to the command's
/// declaration list. Lives on the dispatch stack; strings point into the envelope
/// buffer the decoder was given, so nothing here is copied and nothing is owned.
class ArgValues
{
    union Value { const char* s; uint32_t u; int32_t i; float f; bool b; };

public:
    void set(size_t i, const char* v) { v_[i].s = v;  present_[i] = true; }
    void set(size_t i, uint32_t v)    { v_[i].u = v;  present_[i] = true; }
    void set(size_t i, int32_t v)     { v_[i].i = v;  present_[i] = true; }
    void set(size_t i, float v)       { v_[i].f = v;  present_[i] = true; }
    void set(size_t i, bool v)        { v_[i].b = v;  present_[i] = true; }

    bool has(size_t i) const { return i < MAX_COMMAND_ARGS && present_[i]; }

    /// An out-of-range slot reads as absent rather than faulting, so a handler asking
    /// for an argument its command never declared gets a harmless value after the
    /// assert that names the bug -- and a string is "" rather than null, which is what
    /// the handler's own zero-initialised buffer used to give it.
    template <typename T>
    T get(size_t i) const
    {
        static constexpr Value absent{};
        const Value& v = (i < MAX_COMMAND_ARGS) ? v_[i] : absent;

        if constexpr (std::is_same_v<T, const char*>) return v.s != nullptr ? v.s : "";
        else if constexpr (std::is_same_v<T, uint32_t>) return v.u;
        else if constexpr (std::is_same_v<T, int32_t>)  return v.i;
        else if constexpr (std::is_same_v<T, float>)    return v.f;
        else if constexpr (std::is_same_v<T, bool>)     return v.b;
        else static_assert(sizeof(T) == 0, "unsupported argument type");
    }

private:

    Value v_[MAX_COMMAND_ARGS] = {};
    bool  present_[MAX_COMMAND_ARGS] = {};
};

// ──────────────────────────────────────────────────────────────────────────────
// JSON decoding
// ──────────────────────────────────────────────────────────────────────────────

namespace args_detail
{
    inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    /// Which declaration a wire key belongs to, or -1. `args` is null-terminated.
    inline int Match(const ArgDesc* const* args, const char* key, size_t keyLen)
    {
        for (int i = 0; args[i] != nullptr; ++i)
            if (strlen(args[i]->name) == keyLen && memcmp(args[i]->name, key, keyLen) == 0)
                return i;
        return -1;
    }

    /// Unescape [start, end) in place and terminate it. Returns the resulting length.
    ///
    /// Safe to write over the closing quote: the caller has already advanced past it,
    /// and unescaping only ever shrinks, so the terminator lands at or before it.
    ///
    /// The escape rule -- drop the backslash, keep the next character literally -- is
    /// ExtractJsonString's, kept byte for byte so that this decoder decodes exactly
    /// what the old one did. It is wrong for \n and \u, which it turns into 'n' and
    /// 'u'; that is a pre-existing fault of the envelope format and is tracked
    /// separately rather than fixed in passing here.
    inline size_t Unescape(char* start, char* end)
    {
        char* w = start;
        for (char* r = start; r < end; ++r)
        {
            if (*r == '\\' && r + 1 < end) ++r;
            *w++ = *r;
        }
        *w = '\0';
        return static_cast<size_t>(w - start);
    }

    /// Store `text` into slot `i`, converting it to that argument's type.
    inline CommandResult Convert(const ArgDesc& desc, size_t i, char* text, size_t len,
                                ArgValues& out)
    {
        switch (desc.type)   // no default: a new ArgType must be handled here
        {
        case ArgType::String:
            if (len > desc.maxLength) return CommandResult::ArgumentTooLong;
            out.set(i, static_cast<const char*>(text));
            return CommandResult::Ok;

        case ArgType::UInt32:
        {
            char* end = nullptr;
            const unsigned long v = strtoul(text, &end, 0);   // 0 -> accepts 0x...
            if (end == text || *end != '\0') return CommandResult::MalformedNumber;
            out.set(i, static_cast<uint32_t>(v));
            return CommandResult::Ok;
        }

        case ArgType::Int32:
        {
            // strtol, not strtoul: a calibration offset is legitimately negative, and
            // an unsigned argument would have to encode that in the caller, which is
            // where a sign convention goes to be got wrong.
            char* end = nullptr;
            const long v = strtol(text, &end, 0);
            if (end == text || *end != '\0') return CommandResult::MalformedNumber;
            out.set(i, static_cast<int32_t>(v));
            return CommandResult::Ok;
        }

        case ArgType::Float:
        {
            char* end = nullptr;
            const float v = strtof(text, &end);
            if (end == text || *end != '\0') return CommandResult::MalformedNumber;
            out.set(i, v);
            return CommandResult::Ok;
        }

        case ArgType::Bool:
            out.set(i, strcmp(text, "true") == 0 || strcmp(text, "1") == 0);
            return CommandResult::Ok;
        }
        return CommandResult::MalformedRequest;
    }
}

/// Decode a request's declared arguments out of its JSON envelope line.
///
/// ONE forward scan of the object rather than a lookup per argument, which is what
/// lets a string value be unescaped where it already lies: by the time anything is
/// rewritten, the scan has finished reading those bytes. Nothing is copied, so the
/// decoded strings are valid for as long as `line` is.
///
/// `line` is the envelope, NUL-terminated and without its newline, and is modified.
/// `args` is the command's declaration list, null-terminated. An undeclared key is
/// IGNORED, deliberately and unchanged from the reader this replaces: refusing one is
/// the behaviour we want, but it belongs with a format where an undeclared argument is
/// unambiguous. `failed` names the argument a failure was about.
inline CommandResult DecodeJsonArgs(char* line, const ArgDesc* const* args,
                                   ArgValues& out, const char*& failed)
{
    using namespace args_detail;

    failed = nullptr;

    char* p = line;
    while (IsSpace(*p)) ++p;
    if (*p != '{') return CommandResult::MalformedRequest;
    ++p;

    while (*p != '\0')
    {
        while (IsSpace(*p) || *p == ',') ++p;
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') return CommandResult::MalformedRequest;

        char* key = ++p;
        while (*p != '\0' && *p != '"') { if (*p == '\\' && p[1] != '\0') ++p; ++p; }
        if (*p != '"') return CommandResult::MalformedRequest;
        const size_t keyLen = static_cast<size_t>(p - key);
        ++p;

        while (IsSpace(*p)) ++p;
        if (*p != ':') return CommandResult::MalformedRequest;
        ++p;
        while (IsSpace(*p)) ++p;

        const int slot = Match(args, key, keyLen);

        if (*p == '"')
        {
            char* value = ++p;
            while (*p != '\0' && *p != '"') { if (*p == '\\' && p[1] != '\0') ++p; ++p; }
            if (*p != '"') return CommandResult::MalformedRequest;
            char* end = p;
            ++p;

            if (slot >= 0)
            {
                const size_t len = Unescape(value, end);
                const CommandResult e = Convert(*args[slot], static_cast<size_t>(slot),
                                               value, len, out);
                if (e != CommandResult::Ok) { failed = args[slot]->name; return e; }
            }
        }
        else if (*p == '{' || *p == '[')
        {
            // A structured value is never an argument -- arguments are flat -- so it
            // is skipped whether or not its key matched.
            const char open = *p, close = (*p == '{') ? '}' : ']';
            int depth = 1;
            ++p;
            while (*p != '\0' && depth > 0)
            {
                if (*p == '"')
                {
                    ++p;
                    while (*p != '\0' && *p != '"') { if (*p == '\\' && p[1] != '\0') ++p; ++p; }
                    if (*p == '\0') break;
                }
                else if (*p == open) ++depth;
                else if (*p == close) --depth;
                ++p;
            }
        }
        else
        {
            // A bare token -- number, hex, true/false, null -- is terminated where its
            // delimiter was, because a string argument is handed the pointer and needs
            // the terminator. The displaced delimiter therefore decides where the scan
            // resumes, which is why it is read before the write rather than after.
            char* token = p;
            while (*p != '\0' && *p != ',' && *p != '}' && !IsSpace(*p)) ++p;
            const char delimiter = *p;
            *p = '\0';

            // JSON null means the caller said nothing, so the argument stays absent
            // and a required one still fails below.
            if (slot >= 0 && strcmp(token, "null") != 0)
            {
                const CommandResult e = Convert(*args[slot], static_cast<size_t>(slot),
                                               token, strlen(token), out);
                if (e != CommandResult::Ok) { failed = args[slot]->name; return e; }
            }

            if (delimiter == '\0' || delimiter == '}') break;
            ++p;
        }
    }

    for (int i = 0; args[i] != nullptr; ++i)
    {
        if (args[i]->required && !out.has(static_cast<size_t>(i)))
        {
            failed = args[i]->name;
            return CommandResult::MissingArgument;
        }
    }

    return CommandResult::Ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Console decoding
//
// The same declarations, read out of a line a person types:
//
//     led set enabled=true
//     settings set key=device.name value="My Device"
//
// It is the JSON decoder's sibling, not a translation of it: both fill the same
// ArgValues against the same ArgDesc list, and both convert through the same
// args_detail::Convert, so a type is understood identically whichever codec
// carried it. What differs is the grammar above the values, which is all a codec
// should ever differ by.
// ─────────────────────────────────────────────────────────────────────────────

/// Split a console line into the command it names and the arguments after it.
///
/// The name is every leading token that is NOT `key=value`, which is the whole
/// rule: `help` is one word, `system ping` is two, and a three-word command would
/// need no change here. Nothing consults the registry, so this stays a property of
/// the line rather than of what happens to be registered.
///
/// `name` receives the words joined by single spaces -- exactly the form the
/// registry holds and the wire carries. Returns where the arguments start; `line`
/// is not modified.
inline char* SplitConsoleCommand(char* line, char* name, size_t cap)
{
    using namespace args_detail;

    size_t written = 0;
    name[0] = '\0';

    char* p = line;
    while (true)
    {
        while (IsSpace(*p)) ++p;
        if (*p == '\0') break;

        char* start = p;
        while (*p != '\0' && !IsSpace(*p) && *p != '=') ++p;
        if (*p == '=') { p = start; break; }   // an argument: the name ended before it

        const size_t len = static_cast<size_t>(p - start);
        // One space between words, and none before the first.
        const size_t need = (written == 0 ? 0 : 1) + len;
        if (written + need >= cap) { name[0] = '\0'; return line; }   // too long to be a name

        if (written != 0) name[written++] = ' ';
        memcpy(name + written, start, len);
        written += len;
        name[written] = '\0';
    }

    return p;
}

/// Decode `key=value` arguments out of a console line's argument region.
///
/// `p` is what SplitConsoleCommand returned, NUL-terminated and modified in place;
/// decoded strings point into it, so nothing is copied and nothing is owned. An
/// undeclared key is IGNORED, exactly as the JSON decoder ignores one -- the two
/// codecs must not disagree about what an unknown argument means.
inline CommandResult DecodeConsoleArgs(char* p, const ArgDesc* const* args,
                                       ArgValues& out, const char*& failed)
{
    using namespace args_detail;

    failed = nullptr;

    while (*p != '\0')
    {
        while (IsSpace(*p)) ++p;
        if (*p == '\0') break;

        char* key = p;
        while (*p != '\0' && *p != '=' && !IsSpace(*p)) ++p;
        // A bare word here is not an argument. Deliberately refused rather than
        // guessed at: a flag form would have to decide which declaration it meant.
        if (*p != '=') return CommandResult::MalformedRequest;

        const size_t keyLen = static_cast<size_t>(p - key);
        const int slot = Match(args, key, keyLen);
        ++p;   // past '='

        char*  value = p;
        size_t len   = 0;

        if (*p == '"')
        {
            value = ++p;
            while (*p != '\0' && *p != '"') { if (*p == '\\' && p[1] != '\0') ++p; ++p; }
            if (*p != '"') return CommandResult::MalformedRequest;
            char* end = p;
            ++p;                       // past the closing quote
            len = Unescape(value, end);   // terminates at or before it
        }
        else
        {
            while (*p != '\0' && !IsSpace(*p)) ++p;
            len = static_cast<size_t>(p - value);
            // The delimiter is read before it is overwritten, because the value
            // needs a terminator and the scan needs to know where to resume.
            const char delimiter = *p;
            *p = '\0';
            if (delimiter != '\0') ++p;
        }

        if (slot >= 0)
        {
            const CommandResult e = Convert(*args[slot], static_cast<size_t>(slot),
                                            value, len, out);
            if (e != CommandResult::Ok) { failed = args[slot]->name; return e; }
        }
    }

    for (int i = 0; args[i] != nullptr; ++i)
    {
        if (args[i]->required && !out.has(static_cast<size_t>(i)))
        {
            failed = args[i]->name;
            return CommandResult::MissingArgument;
        }
    }

    return CommandResult::Ok;
}
