#pragma once
#include "ResumeTokens.h"
#include "TypedSettings.h"
#include "Mutex.h"
#include <cstddef>
#include <cstdint>

// The credential authority: checks the configured password, rate-limits failed
// attempts, owns the RAM channel-key table, and detects a password change.
// Transport-neutral; no connection state. Owned by AuthManager (a plain class,
// not a StruxProvider manager).
//
// It does not DECLARE the password setting — settings belong to the manager that
// owns them, and this is not a manager. It holds a reference to AuthManager's
// setting and reads it live on every check: a copy taken at Init would make
// change detection blind, since nothing notifies on a setting write.
class Authenticator {
public:
    explicit Authenticator(StringSetting& password) : password_(password) {}

    /// Snapshot the stored password. Must run AFTER the owning manager has
    /// registered the setting, or the first check would see NVS-value-vs-default
    /// and report a spurious password change.
    void Init();

    /// What a login attempt came to. `Wrong` and `TooManyAttempts` are both
    /// "no", but a caller that cannot tell them apart cannot say why.
    enum class LoginResult { Ok, Wrong, TooManyAttempts };

    bool AuthRequired();                 // password non-empty

    /// One login attempt, rate limit included. This is what a login handler
    /// calls; CheckPassword is the bare comparison underneath it.
    LoginResult TryPassword(const char* pw);

    /// Seconds left on the current lock-out, or 0 when there is none.
    uint32_t LockoutRemainingSeconds();

    bool CheckPassword(const char* pw);  // epoch-check, then constant-time compare
    void MintKey(char* out);             // ResumeTokens::Create (out >= ResumeTokens::TOKEN_LEN)
    bool ValidateKey(const char* key);   // epoch-check, then ResumeTokens::Touch
    void TouchKey(const char* key);      // ResumeTokens::Touch (refresh only)

private:
    static constexpr const char* TAG = "Authenticator";

    // ── Rate limiting ──
    //
    // A wrong password used to cost an attacker nothing but a round trip, so a
    // short password was guessable at whatever rate the socket allowed. Past
    // MAX_FAILURES, attempts are refused outright until LOCKOUT_US has passed
    // since the last one.
    //
    // Refused IMMEDIATELY, never answered slowly. Every command runs on the one
    // web-server task, so sleeping here to slow an attacker down would hand
    // that attacker a way to stall the whole device with wrong passwords - a
    // worse bug than the one being fixed. The counter is global rather than
    // per-connection for the same reason it is not per-IP: at this layer a new
    // connection is free, so anything keyed on one counts nothing.
    //
    // RAM only, so a power cycle clears it. That is the right trade rather than
    // an omission: persisting the counter would write NVS on every wrong guess,
    // which is a flash-wear attack available to anyone who can reach the socket,
    // and clearing it needs physical access to a device whose attacker would by
    // then have better options than the login form.
    static constexpr int     MAX_FAILURES = 5;
    static constexpr int64_t LOCKOUT_US   = 60LL * 1000 * 1000;

    StringSetting& password_;            // declared and registered by AuthManager
    ResumeTokens tokens_;
    char passwordSnapshot_[64] = {};
    Mutex authMutex_;
    int     failures_    = 0;            // guarded by authMutex_
    int64_t lastFailure_ = 0;            // guarded by authMutex_
    void CheckPasswordEpoch();           // clears tokens_ when the password changed

    /// Read the configured password into a zero-padded buffer. The padding is
    /// what makes the comparison constant time: NVS leaves whatever was in the
    /// tail of `out` alone.
    void ReadPasswordPadded(char* out, size_t size);
};
