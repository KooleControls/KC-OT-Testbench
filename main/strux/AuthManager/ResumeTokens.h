#pragma once

#include "Mutex.h"
#include <cstdint>
#include <cstddef>

// ──────────────────────────────────────────────────────────────
// Fixed-slot bearer-token table — pure token bookkeeping.
// Password checking and password-change detection live in
// Authenticator (AuthManager declares the setting itself);
// this class only mints, refreshes, and expires opaque tokens.
//
// The idle timeout is a garbage collector, not a UX rule: any WS
// frame or authenticated HTTP request refreshes a token, so an
// open browser tab (with its heartbeat) never expires. Tokens die
// ~30 minutes after their tab closes.
// ──────────────────────────────────────────────────────────────
class ResumeTokens {
public:
    static constexpr int     MAX_TOKENS    = 4;    // matches ConnectionRegistry::MAX
    static constexpr size_t  TOKEN_LEN       = 33;   // 32 hex chars + NUL
    static constexpr int64_t IDLE_TIMEOUT_US = 30LL * 60 * 1000 * 1000;

    /// Mint a new token (128 bits of esp_random, hex-encoded) and
    /// write its token into `tokenOut` (must hold TOKEN_LEN bytes).
    /// Prefers an empty/expired slot; evicts the stalest live one if full.
    void Create(char* tokenOut);

    /// True if `token` names a live entry; refreshes its activity.
    /// Expired entries are reclaimed lazily here — no timer task.
    bool Touch(const char* token);

    /// Drop all sessions (reboot does this implicitly — RAM only).
    void Clear();

private:
    struct Channel {
        char    token[TOKEN_LEN] = {};   // token[0] == 0 → empty slot
        int64_t lastActivity = 0;
    };
    Channel sessions_[MAX_TOKENS];
    Mutex mutex_;
};
