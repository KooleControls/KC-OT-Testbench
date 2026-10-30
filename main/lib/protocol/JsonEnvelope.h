#pragma once

#include "Envelope.h"
#include "JsonHelpers.h"

// The JSON codec's request half: the envelope every client spoke before there was
// a second codec, and still the one every browser page and the relay speak.
//
//     {"type":"partition write","partition":"ota_1"}\n<firmware bytes...>
//
// It reads the line the caller already has, ONCE: the command's name comes out of
// it, and so do the command's arguments, without a second pass over the stream and
// without a second copy of the line. Decoded string arguments point INTO that
// buffer, unescaped where they lie, which is why the RequestLine has to outlive
// this object rather than being a temporary beside it.
class JsonEnvelope final : public Envelope
{
public:
    /// `line` is the request line, owned by the caller and modified in place.
    explicit JsonEnvelope(char* line) : line_(line)
    {
        // Absent or unparseable leaves this empty, which is how the caller tells a
        // request it cannot route from one it can.
        ExtractJsonString(line_, "type", command_, sizeof(command_));
    }

    const char* command() const override { return command_; }

    CommandResult decode(const ArgDesc* const* args, ArgValues& values,
                         const char*& failed) override
    {
        return DecodeJsonArgs(line_, args, values, failed);
    }

private:
    char* line_;
    char  command_[protocol::MAX_COMMAND_NAME] = {};
};
