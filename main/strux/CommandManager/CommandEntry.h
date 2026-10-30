#pragma once

#include "Fatal.h"
#include "CommandContext.h"
#include <type_traits>

class Stream;

// ──────────────────────────────────────────────────────────────
// One command in the CommandManager registry.
//
// Entries are the links of an intrusive chain. A manager declares one
// named object per command (static storage duration) and hands the ones
// it wants registered to CommandManager::Register(), which stamps ctx
// and links them. Owners never touch ctx/next/registered.
//
// Handlers are plain function pointers — no heap, no std::function.
// The usual shape is a static member "trampoline" that casts ctx
// back to the owning manager and calls a private method.
// ──────────────────────────────────────────────────────────────
struct CommandEntry
{
    // The command, exactly as the wire carries it: `partition write`, `system ping`.
    // ONE identity, because the wire only ever had one — it used to be split into a
    // category and a name at dispatch and joined again everywhere it was shown.
    //
    // The first word still groups: `help` derives a category from it and AuthGate
    // gates on it. Both derive, neither stores, because grouping for display and
    // grouping for permission are questions ABOUT a command rather than part of what
    // it is called — and a command that had two identities had to keep them in step.
    const char* name;
    CommandResult (*handler)(void* ctx, CommandContext& c);

    /// One line saying what this command DOES, in the vocabulary of whoever calls it.
    ///
    /// Optional and defaulted, so an existing table compiles unchanged — but a
    /// command without one is a command an operator, or a model reaching this device
    /// through the relay, can only guess at. The arguments describe themselves (see
    /// ArgDesc::description); this is the sentence the arguments are arguments TO.
    ///
    /// A string literal, never composed: the table is static storage and nothing
    /// frees it. Keep it short — a second sentence for a caveat is fine, a paragraph
    /// belongs in the device's instructions (`system describe`).
    const char* help = nullptr;

    /// The arguments this command takes, in the order `help` reports them, ending at
    /// the first null. Each entry is the address of the very `CommandArg<T>` object
    /// the handler reads, so ctx.arg() matches by identity and there is no second
    /// place for an argument's name or type to live.
    ///
    /// Empty means the command takes no arguments, and `help` says so without
    /// running anything.
    ///
    /// One slot longer than a command may declare, so the terminator always fits;
    /// Register() refuses a table that fills it.
    const ArgDesc* args[MAX_COMMAND_ARGS + 1] = {};

    // Managed by CommandManager::Register() — owners never touch these.
    void* ctx = nullptr;
    CommandEntry* next = nullptr;
    bool registered = false;

    // A registered entry is a live link in the dispatch chain; letting it
    // die would leave a dangling pointer in the chain. There is no
    // compile-time way to forbid this (a deleted dtor would propagate up
    // through the owning manager to the global AppContext), so:
    // abort. The device resets with a clear message on the very first run
    // of the offending code.
    ~CommandEntry()
    {
        if (registered)
            FATAL("registered command '%s' destroyed - command tables must "
                  "live for the whole application", name);
    }
};

// ──────────────────────────────────────────────────────────────
// Handler trampoline.
//
// Handlers are ordinary functions with no ctx in sight — either a
// (usually private, non-static) member of the owning manager:
//
//     CommandResult Cmd_Ping(CommandContext& ctx);
//     { "system", "ping", &InvokeCommand<&SystemManager::Cmd_Ping> },
//
// or a free/static function (e.g. quick hacking in main.cpp —
// register with ctx = nullptr).
//
// Arguments arrive already parsed and validated; `in` is positioned at the body
// (empty for most commands), and the handler writes its reply to `out`. Returning
// anything but Ok makes the framework refuse the request — a handler never writes
// error text and never names a framework error.
//
// The trampoline is instantiated at compile time; for members the
// owning class is deduced from the method pointer itself, so the
// ctx cast can never target the wrong type. The if constexpr branch
// resolves during instantiation — there is no runtime check.
//
// The void* plumbing still exists (it is the type erasure that lets
// one chain hold commands of many classes) but it lives only here.
// ──────────────────────────────────────────────────────────────
template <typename T> struct CommandOwner;
template <typename C> struct CommandOwner<CommandResult (C::*)(CommandContext&)>       { using type = C; };
template <typename C> struct CommandOwner<CommandResult (C::*)(CommandContext&) const> { using type = const C; };

template <auto Handler>
CommandResult InvokeCommand(void* ctx, CommandContext& c)
{
    if constexpr (std::is_member_function_pointer_v<decltype(Handler)>)
    {
        using C = typename CommandOwner<decltype(Handler)>::type;
        return (static_cast<C*>(ctx)->*Handler)(c);
    }
    else
    {
        return Handler(c);
    }
}
