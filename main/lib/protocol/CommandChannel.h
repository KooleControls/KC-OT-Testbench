#pragma once

#include "Channel.h"
#include "CommandContext.h"
#include "ConsoleEnvelope.h"
#include "Envelope.h"
#include "JsonEnvelope.h"
#include "JsonReplyWriter.h"
#include "YamlReplyWriter.h"

// The sequence that runs one opened channel against a dispatcher. This is protocol,
// not dispatch: reading the request's line, deciding which codec carried it, and
// deciding what a malformed one is called is the wire format's business, and the
// dispatcher never sees a Channel.
//
// A request is a single '\n'-terminated line; the body — if any — is whatever bytes
// follow it in the same channel. Two codecs speak that line today:
//
//     {"type":"partition write","partition":"ota_1"}\n<firmware bytes…>
//     partition write partition=ota_1\n<firmware bytes…>
//
// WHICH ONE is decided by the request itself — a JSON envelope opens with '{' and a
// console line cannot — so nothing is negotiated, nothing is configured, and a
// client that has never heard of the second one is unaffected. It is the same rule
// the relay uses to tell an old device from a new one, and it works for the same
// reason: both sides are obliged to speak first, and they say different things.
//
// A codec owns BOTH directions. Picking one picks the reply writer with it, which is
// why the writer is built here and handed down rather than being chosen by the
// dispatcher — the dispatcher has no way to know, and no business knowing.
namespace protocol
{
    /// Does this line belong to the JSON codec? Leading space is skipped for the
    /// same reason the decoder skips it: a client that pretty-prints its envelope
    /// is not speaking a different language.
    inline bool IsJsonRequest(const char* line)
    {
        while (*line == ' ' || *line == '\t') ++line;
        return *line == '{';
    }

    /// Everything after the codec is chosen, which is all of it: name the command,
    /// gate it, dispatch it, close or refuse the reply.
    template <class Dispatcher, class Gate>
    void DispatchEnvelope(Channel& channel, Dispatcher& dispatcher, Gate& gate,
                          Envelope& envelope, ReplyWriter& reply)
    {
        if (envelope.command()[0] == '\0')
        {
            channel.reject("expected a command, e.g. 'system ping' or "
                           "{\"type\":\"system ping\"}");
            return;
        }

        if (!gate.Allows(envelope.command()))
        {
            channel.reject("unauthorized");
            return;
        }

        // The handler reads any body from the same channel the reply is written to.
        const char* failedArg = nullptr;
        const CommandResult err =
            dispatcher.Execute(envelope, reply, channel, &gate, &failedArg);
        if (err != CommandResult::Ok)
        {
            // Form failures refuse the request. REJECT ends the channel like FINAL
            // does, so this composes with anything the handler already wrote — a
            // refusal can always be last. The reason is prose, not a codec's
            // syntax, so it reads the same whichever one asked.
            char buf[96];
            channel.reject(DescribeCommandResult(err, failedArg, buf, sizeof(buf)));
            return;
        }

        channel.finish();   // FINAL — end of reply
    }

    /// Run one opened channel.
    ///
    /// `dispatcher` is duck-typed on
    /// `CommandResult Execute(Envelope&, ReplyWriter&, Stream&, ConnectionAuth*, const char**)`
    /// — a template rather than an interface so the protocol layer never depends
    /// upward on the dispatcher, and no callback inversion comes back.
    /// `gate` is duck-typed on `bool Allows(const char* command) const` plus
    /// `ConnectionAuth&`-conversion — the transport decides what may run before a
    /// connection has authenticated, and lends the auth state to the handlers that
    /// need it. Checked AFTER the line is read, because the decision is about which
    /// command was asked for.
    template <class Dispatcher, class Gate>
    void RunCommandChannel(Channel& channel, Dispatcher& dispatcher, Gate& gate)
    {
        RequestLine line(channel);

        // A line with no room left for its newline is a LENGTH problem, and saying
        // so is worth a branch of its own: it is not a request either codec failed
        // to read, and the two used to be indistinguishable.
        if (line.overflowed())
        {
            channel.reject("request line too long: it must be one line under "
                           "512 bytes, and the body goes AFTER the newline");
            return;
        }

        if (IsJsonRequest(line.text()))
        {
            JsonEnvelope   envelope(line.text());
            JsonReplyWriter reply(channel);
            DispatchEnvelope(channel, dispatcher, gate, envelope, reply);
        }
        else
        {
            ConsoleEnvelope envelope(line.text());
            YamlReplyWriter reply(channel);
            DispatchEnvelope(channel, dispatcher, gate, envelope, reply);
        }
    }
}
