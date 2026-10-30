#include "AuthManager.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "ResumeTokens.h"

#include <esp_log.h>

AuthManager::AuthManager(StruxProvider& strux)
    : strux_(strux)
{
}

namespace {

constexpr CommandArg<const char*> passwordArg{
    "password", "The device's web password (setting 'web.password'). Omit it only to "
                "test whether an empty password is accepted.",
    63, Presence::Optional };

// One less than ResumeTokens::TOKEN_LEN, which is 32 hex characters and a NUL.
constexpr CommandArg<const char*> keyArg{
    "key", "The channel key a previous successful 'auth login' returned.",
    ResumeTokens::TOKEN_LEN - 1 };

} // namespace

CommandEntry AuthManager::helloCommand_{
    "auth hello", &InvokeCommand<&AuthManager::Cmd_AuthHello>,
    "Ask whether this connection has to log in before anything else will be "
    "answered. With no web password set - the default - it never does."
};

CommandEntry AuthManager::loginCommand_{
    "auth login", &InvokeCommand<&AuthManager::Cmd_AuthLogin>,
    "Authenticate this connection with the device's web password. On success "
    "the reply carries a channel key that 'auth resume' takes. Failed attempts "
    "are rate limited, and a refusal says which of the two it was.",
    { &passwordArg }
};

CommandEntry AuthManager::resumeCommand_{
    "auth resume", &InvokeCommand<&AuthManager::Cmd_AuthResume>,
    "Re-authenticate a reconnected client with the channel key a previous "
    "'auth login' handed out, instead of the password again.",
    { &keyArg }
};

void AuthManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    strux_.getSettingsManager().Register({ &webPassword_ });
    auth_.Init();   // snapshot the stored password (after registration)
    strux_.getCommandManager().Register(this, {
        &helloCommand_,
        &loginCommand_,
        &resumeCommand_,
    });

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// ──────────────────────────────────────────────────────────────
// auth — the handshake as ordinary commands. Nothing here frames its own reply or
// parses its own wire format; it is a handler like every other.
// ──────────────────────────────────────────────────────────────

CommandResult AuthManager::Cmd_AuthHello(CommandContext& ctx)
{
    // Per CONNECTION, not per device. A transport whose peer is already proven
    // has nothing left to ask for, while a browser socket on a password-protected
    // device does — and both arrive here. Asking the Authenticator alone told a
    // remote browser riding an authenticated relay pipe to log in with a password
    // it has no way to know.
    const bool alreadyAuthed = ctx.connection && ctx.connection->isAuthed();

    auto resp = ctx.reply.object();
    resp.field("authRequired", !alreadyAuthed && auth_.AuthRequired());
    return CommandResult::Ok;
}

CommandResult AuthManager::Cmd_AuthLogin(CommandContext& ctx)
{
    const char* password = ctx.arg(passwordArg);

    auto resp = ctx.reply.object();

    // A wrong password is MEANING, not form: the request was perfectly well made, the
    // answer is no. So it is a reply, not a refusal. So is being rate limited - and
    // that one says so, because "wrong password" when the password is right is the
    // kind of answer somebody debugs for an hour.
    const auto result = auth_.TryPassword(password);
    if (result == Authenticator::LoginResult::TooManyAttempts)
    {
        resp.field("ok", false);
        resp.field("error", "too many failed attempts - wait and try again");
        resp.field("retryAfter", auth_.LockoutRemainingSeconds());
        return CommandResult::Ok;
    }
    if (result != Authenticator::LoginResult::Ok)
    {
        resp.field("ok", false);
        return CommandResult::Ok;
    }

    char key[ResumeTokens::TOKEN_LEN] = {};
    auth_.MintKey(key);
    if (ctx.connection) ctx.connection->authenticate(key);

    resp.field("ok", true);
    resp.field("key", key);
    return CommandResult::Ok;
}

CommandResult AuthManager::Cmd_AuthResume(CommandContext& ctx)
{
    const char* key = ctx.arg(keyArg);

    auto resp = ctx.reply.object();

    if (!auth_.ValidateKey(key))
    {
        resp.field("ok", false);
        return CommandResult::Ok;
    }

    if (ctx.connection) ctx.connection->authenticate(key);
    resp.field("ok", true);
    return CommandResult::Ok;
}
