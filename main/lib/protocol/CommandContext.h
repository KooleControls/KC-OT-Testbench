#pragma once

#include "Stream.h"
#include "ReplyWriter.h"
#include "CommandArgs.h"
#include <cassert>
#include <cstddef>
#include <cstdint>

// Everything a command handler gets: its arguments, its request body, its reply.
//
//     CommandResult PartitionManager::Cmd_ClearPartition(CommandContext& ctx)
//     {
//         const char* label = ctx.arg(partitionArg);
//         ...
//     }
//
// Arguments are already parsed and validated by the time a handler runs, and `in` is
// already positioned at the body. A handler therefore has no prologue: it cannot
// forget to read its arguments, and there is nothing it can do first that `help`
// depends on. Commands declare what they take in their CommandEntry (see
// CommandArgs.h), which is also where `help` reads it from.
//
// The framework validates FORM — is a required argument present, is that number a
// number. The handler validates MEANING — is that address inside this partition.
// Form failures become a REJECT; meaning goes in the reply, where it can carry data.

/// The authentication state of the connection a request arrived on, lent by the
/// transport. Per-connection state is transport-specific — a socket has one shape, an
/// outbound pipe another — so a handler that needs it (the `auth` commands, and
/// nothing else) receives it through the context rather than reaching for it.
///
/// Null for a transport that does no gating.
class ConnectionAuth
{
public:
    virtual ~ConnectionAuth() = default;

    /// Mark this connection authenticated, remembering the resume key so a
    /// reconnect can resume.
    virtual void authenticate(const char* key) = 0;

    virtual bool isAuthed() const = 0;
};

class CommandContext
{
public:
    /// `args` is the command's declaration list, null-terminated; `values` is what a
    /// decoder made of the request against it. Both come from the dispatcher and
    /// outlive the handler.
    CommandContext(ReplyWriter& writer, Stream& request,
                   const ArgDesc* const* args, const ArgValues& values,
                   ConnectionAuth* connection = nullptr)
        : in(request), reply(writer),
          connection(connection), args_(args), values_(values) {}

    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    Stream& in;    ///< request body, the stream already positioned there

    /// The reply: `auto resp = ctx.reply.object();`. Writes through in the
    /// connection's format, so a handler need not name one. It is the ONLY way out —
    /// raw bytes are reached through `resp.body("...")`, which declares what they are
    /// first, so no handler can write to the wire without saying what it is writing.
    ReplyWriter& reply;

    /// Auth state of the connection this arrived on; null when the transport gates
    /// nothing. Only the `auth` commands have any business touching it.
    ConnectionAuth* const connection;

    /// One declared argument's value, already parsed and validated. Reading an
    /// argument the command did not declare is a firmware bug and nothing here is
    /// built to prevent it — the assert names it the first time the command runs.
    template <typename T>
    T arg(const CommandArg<T>& a) const
    {
        const int i = indexOf(a);
        assert(i >= 0 && "command reads an argument it did not declare");
        return values_.get<T>(static_cast<size_t>(i));
    }

    /// Did the caller supply it? Only ever interesting for an optional argument, and
    /// only where absence means something a default cannot express.
    bool has(const ArgDesc& a) const
    {
        const int i = indexOf(a);
        return i >= 0 && values_.has(static_cast<size_t>(i));
    }

private:
    const ArgDesc* const* args_;
    const ArgValues&      values_;

    /// Which slot this declaration occupies, by identity rather than by name: the
    /// entry stores the address of the very object the handler reads.
    int indexOf(const ArgDesc& a) const
    {
        for (int i = 0; args_[i] != nullptr; ++i)
            if (args_[i] == &a) return i;
        return -1;
    }
};

/// Human-readable form of a request failure, for the REJECT payload. Written by the
/// framework — handlers never compose error text.
const char* DescribeCommandResult(CommandResult e, const char* arg, char* buf, size_t cap);
