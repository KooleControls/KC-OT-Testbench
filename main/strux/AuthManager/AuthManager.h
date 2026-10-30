#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Authenticator.h"

// ──────────────────────────────────────────────────────────────
// The device's credential authority, as a manager of its own.
//
// It owns the password setting, the Authenticator (password check, rate limit,
// resume-key table) and the `auth` command category. Nothing here is a transport:
// a transport borrows the Authenticator through the provider and puts an AuthGate
// in front of its own connection state.
//
// It lived under WebServerManager/ because that is where the HTTP/WS login was
// extracted from, which made RelayManager include WebServerManager.h to borrow a
// credential authority that was never the web server's — the last sideways edge
// between two transports, and the reason StruxContext::Init() carried an ordering
// constraint. Both are gone: a transport asks the provider, and this manager only
// has to come up after SettingsManager, like every other.
// ──────────────────────────────────────────────────────────────
class AuthManager
{
    static constexpr const char* TAG = "AuthManager";

public:
    explicit AuthManager(StruxProvider& strux);

    AuthManager(const AuthManager&) = delete;
    AuthManager& operator=(const AuthManager&) = delete;
    AuthManager(AuthManager&&) = delete;
    AuthManager& operator=(AuthManager&&) = delete;

    void Init();

    /// The credential authority, for a transport that carries the auth handshake
    /// (the browser socket today) or has to ask whether one is required at all.
    Authenticator& GetAuthenticator() { return auth_; }

private:
    StruxProvider& strux_;
    InitState initState_;

    // ── Settings (registered with SettingsManager in Init) ──
    //
    // Empty means no login is required at all, which is the default.
    //
    // The KEY stays "web.password" although this is no longer the web server's
    // setting: the key is the NVS address of a password somebody already set, and
    // renaming it would silently unlock every device that has one on the next
    // firmware update. The name is wrong and harmless; the rename is not.
    inline static StringSetting webPassword_{ "web.password", "Web Password", "" };

    // Reads webPassword_ live by reference — see Authenticator.h.
    Authenticator auth_{ webPassword_ };

    // ── auth: the handshake, as ordinary commands ──
    //
    // These are the only commands a connection may run before authenticating
    // (AuthGate whitelists the category), and the only ones that touch
    // ctx.connection.

    /// Does this device want a password at all? Answered before login, so a client
    /// knows whether to prompt.
    CommandResult Cmd_AuthHello(CommandContext& ctx);

    /// Password in, channel key out. The key lets a reconnect resume without
    /// re-prompting.
    CommandResult Cmd_AuthLogin(CommandContext& ctx);

    /// Resume with a key minted earlier.
    CommandResult Cmd_AuthResume(CommandContext& ctx);

    // Defined in AuthManager.cpp, beside the handlers and the arguments they read.
    static CommandEntry helloCommand_;
    static CommandEntry loginCommand_;
    static CommandEntry resumeCommand_;
};
