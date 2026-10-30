#include "CommandManager.h"
#include "Envelope.h"
#include "ReplyWriter.h"
#include "Stream.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

CommandManager::CommandManager(StruxProvider& strux)
    : strux_(strux)
{
}

void CommandManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    Register(this, { &helpCommand_ });

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

CommandResult CommandManager::Execute(Envelope& envelope, ReplyWriter& reply, Stream& in,
                                      ConnectionAuth* connection,
                                      const char** failedArg)
{
    const CommandEntry* e = Find(envelope.command());
    if (e == nullptr)
        return CommandResult::UnknownCommand;

    // Handler runs OUTSIDE the lock: entries are immortal, so the pointer
    // stays valid, and a handler may register commands or dispatch nested
    // commands without deadlocking.
    //
    // Arguments are decoded before the handler runs, which is why a handler has no
    // prologue and receives them already validated. A command that declares none
    // skips the decode entirely — there is nothing for it to look for.
    ArgValues values;
    if (e->args[0] != nullptr)
    {
        const char* failed = nullptr;
        const CommandResult err = envelope.decode(e->args, values, failed);
        if (err != CommandResult::Ok)
        {
            if (failedArg) *failedArg = failed;
            return err;
        }
    }

    CommandContext ctx(reply, in, e->args, values, connection);
    return e->handler(e->ctx, ctx);
}

const char* DescribeCommandResult(CommandResult e, const char* arg, char* buf, size_t cap)
{
    switch (e)   // no default: a new CommandResult must be handled here
    {
    case CommandResult::Ok:              return "ok";
    case CommandResult::UnknownCommand:  return "unknown command";
    case CommandResult::MissingArgument:
        snprintf(buf, cap, "missing required argument: %s", arg ? arg : "?");
        return buf;
    case CommandResult::MalformedRequest:  return "malformed request";
    case CommandResult::MalformedNumber:
        snprintf(buf, cap, "malformed number: %s", arg ? arg : "?");
        return buf;
    case CommandResult::ArgumentTooLong:
        snprintf(buf, cap, "argument too long: %s", arg ? arg : "?");
        return buf;
    }
    return "bad request";
}

// ──────────────────────────────────────────────────────────────
// help
// ──────────────────────────────────────────────────────────────

CommandEntry CommandManager::helpCommand_{
    "help", &InvokeCommand<&CommandManager::Cmd_Help>,
    "Describe every command this firmware offers - name, description and full "
    "argument declarations - in one reply."
};

CommandResult CommandManager::Cmd_Help(CommandContext& ctx)
{
    // Held across the whole reply: a manager registering from another task must not
    // relink the chain half way through the answer. Nothing is dispatched from in
    // here - the names come off the chain and the arguments off each entry, which is
    // why describing `system reboot` does not reboot anything.
    LOCK(mutex_);

    auto resp = ctx.reply.object();
    resp.field("ok", true);

    auto commands = resp.array("commands");
    for (const CommandEntry* e = head_; e != nullptr; e = e->next)
    {
        auto cmd = commands.object();
        cmd.field("name", e->name);
        if (e->help != nullptr && e->help[0] != '\0')
            cmd.field("description", e->help);

        auto args = cmd.array("arguments");
        DescribeArguments(*e, args);
    }

    return CommandResult::Ok;
}

void CommandManager::DescribeArguments(const CommandEntry& entry, ReplyArray& args)
{
    // Read, never run. Every command declares its arguments statically, so nothing
    // here dispatches and a handler's body can no longer execute under `help`.
    for (int i = 0; entry.args[i] != nullptr; ++i)
    {
        const ArgDesc& d = *entry.args[i];
        auto arg = args.object();
        arg.field("name", d.name);
        arg.field("type", ArgTypeName(d.type));
        arg.field("required", d.required);
        if (d.type == ArgType::String)
            arg.field("maxLength", static_cast<uint32_t>(d.maxLength));
        // Absent rather than empty when the command did not describe it, so a
        // reader can tell "nothing was said" from "said to be nothing".
        if (d.description != nullptr && d.description[0] != '\0')
            arg.field("description", d.description);
    }
}

const CommandEntry* CommandManager::Find(const char* name)
{
    LOCK(mutex_);
    for (CommandEntry* e = head_; e != nullptr; e = e->next)
        if (strcmp(name, e->name) == 0)
            return e;
    return nullptr;
}
