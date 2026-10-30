#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "RecursiveMutex.h"
#include "ContextLock.h"
#include <cstring>
#include <cassert>
#include <cstddef>
#include <initializer_list>

class Stream;
class Envelope;
class ReplyWriter;

// Pure dispatcher — knows no other managers, and no commands but the
// registry's own (`help`, which is the registry describing itself and
// could not live anywhere else). Every other command lives in the
// manager that owns its domain and is registered from that manager's
// Init().
//
// Deliberately knows nothing about channels, transports or the wire format: give
// it an envelope, a reply writer and a stream and it runs the handler. It asks the
// envelope for a name and for a decode against the command's declarations, so no
// codec's syntax is visible from here — JSON and the console line reach this same
// function and leave through the same ReplyWriter.
//
// Reading a request's envelope is the protocol layer's job — see
// protocol::RunCommandChannel, which every transport calls.
class CommandManager {
    static constexpr const char* TAG = "CommandManager";

public:
    explicit CommandManager(StruxProvider& strux);

    CommandManager(const CommandManager&) = delete;
    CommandManager& operator=(const CommandManager&) = delete;
    CommandManager(CommandManager&&) = delete;
    CommandManager& operator=(CommandManager&&) = delete;

    void Init();

    /// Register this manager's commands. Each one is an object the manager owns
    /// and names; nothing is generated, nothing registers itself, and the list is
    /// the manager saying which of its commands exist:
    ///
    ///     strux_.getCommandManager().Register(this, { &pingCommand_, &infoCommand_ });
    ///
    /// Every entry MUST have static storage duration (see ~CommandEntry). `ctx` is
    /// the owner's `this`, stamped into each entry and handed back to its handler at
    /// dispatch — it cannot come from the entry itself, because an entry is
    /// initialised before any manager exists.
    ///
    /// Thread-safe, and usable from construction — managers whose Init()
    /// runs before CommandManager's may register safely.
    void Register(void* ctx, std::initializer_list<CommandEntry*> commands)
    {
        LOCK(mutex_);
        for (CommandEntry* c : commands)
        {
            // Re-registering would re-link an entry already in the chain
            // and cycle it → Execute() would hang. Chain-corruption class,
            // so FATAL (survives NDEBUG), not assert.
            if (c->registered)
                FATAL("command '%s' registered twice", c->name);
            assert(Find(c->name) == nullptr && "duplicate command name");
            // The declaration list is null-terminated and the last slot is the
            // terminator's. A list that fills it would be walked off the end.
            if (c->args[MAX_COMMAND_ARGS] != nullptr)
                FATAL("command '%s' declares more than %d arguments",
                      c->name, (int)MAX_COMMAND_ARGS);

            c->ctx = ctx;
            c->registered = true;
            c->next = head_;
            head_ = c;
        }
    }

    /// Run the command the envelope names. `in` is positioned at the request body
    /// (empty for most commands) and the handler writes its reply through `reply`.
    ///
    /// BOTH halves arrive already chosen, because both belong to the codec that
    /// carried the request and this class has no way to know which that was. The
    /// envelope was read by the interface — reading it is what leaves `in` at the
    /// body — and is asked to decode this command's declared arguments; the writer
    /// is that same codec's. Nothing here names a wire format, and adding a second
    /// one changed neither this signature's meaning nor a single handler.
    ///
    /// Returns Ok, UnknownCommand, or whatever the decode reported. `failedArg` (when
    /// non-null) receives the argument name a failure was about, so the caller can
    /// compose the refusal text.
    CommandResult Execute(Envelope& envelope, ReplyWriter& reply, Stream& in,
                          ConnectionAuth* connection = nullptr,
                          const char** failedArg = nullptr);

private:
    StruxProvider& strux_;
    InitState initState_;

    RecursiveMutex mutex_;
    CommandEntry* head_ = nullptr;

    // Locks internally (recursive, so Register may call it under its own
    // lock). Handing the pointer out after unlock is safe because entries
    // are immortal and name/handler/ctx are written before linking.
    const CommandEntry* Find(const char* name);

    // ── help: the registry describing itself ────────────────
    //
    // ONE command and ONE reply: every command this firmware has, with its
    // description and its full argument declarations. A caller that has to compose
    // a call - a person exploring, the command console in the browser, a model at
    // the other end of the relay - knows everything after one round trip.
    //
    // Nothing here is a second copy of anything. The names come off the chain; a
    // command's arguments come off its own entry, where the handler reads them from.
    // So help cannot go stale - there is nothing to update - and nothing is executed
    // to find out, which is what makes `system reboot` safe to ask about.
    //
    // The list is FLAT, and the reply says nothing about categories. The first word
    // of a name still groups, for a reader and for AuthGate, but that is a question
    // asked ABOUT a command rather than part of what it is called - so the registry
    // reports what a command IS and leaves the grouping to whoever wants one.
    CommandResult Cmd_Help(CommandContext& ctx);

    /// Writes one command's declared arguments into `args`, straight off its entry.
    void DescribeArguments(const CommandEntry& entry, ReplyArray& args);

    // Defined in CommandManager.cpp, beside the handler and the chain it walks.
    static CommandEntry helpCommand_;
};
