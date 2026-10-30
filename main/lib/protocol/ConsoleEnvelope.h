#pragma once

#include "Envelope.h"

// The console codec's request half: a line a person types.
//
//     system ping
//     led set enabled=true
//     settings set key=device.name value="My Device"
//
// The command's name is every leading word that is not `key=value`; the rest are
// its declared arguments. Nothing here consults the registry, so the grammar is a
// property of the line rather than of what happens to be registered -- and a
// three-word command, if one is ever named, needs no change.
//
// It exists to prove the boundary rather than to be clever: it fills the same
// ArgValues, against the same declarations, through the same converters as the
// JSON codec. Everything below Execute() -- dispatch, CommandContext, every
// handler -- cannot tell which one carried the request, and that is the point.
class ConsoleEnvelope final : public Envelope
{
public:
    /// `line` is the request line, owned by the caller and modified in place.
    explicit ConsoleEnvelope(char* line)
    {
        args_ = SplitConsoleCommand(line, command_, sizeof(command_));
    }

    const char* command() const override { return command_; }

    CommandResult decode(const ArgDesc* const* args, ArgValues& values,
                         const char*& failed) override
    {
        return DecodeConsoleArgs(args_, args, values, failed);
    }

private:
    char  command_[protocol::MAX_COMMAND_NAME] = {};
    char* args_;
};
