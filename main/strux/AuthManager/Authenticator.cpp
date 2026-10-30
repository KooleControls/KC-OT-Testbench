#include "Authenticator.h"
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>

// ──────────────────────────────────────────────────────────────
// Auth — the credential authority. Nothing above the transport
// edge (commands, streams) ever sees a token or password.
// ──────────────────────────────────────────────────────────────

void Authenticator::Init()
{
    password_.Get(passwordSnapshot_, sizeof(passwordSnapshot_));
}

void Authenticator::CheckPasswordEpoch()
{
    LOCK(authMutex_);
    char current[64] = {};
    password_.Get(current, sizeof(current));
    if (strcmp(current, passwordSnapshot_) != 0)
    {
        ESP_LOGI(TAG, "password changed - clearing all channels");
        tokens_.Clear();
        strlcpy(passwordSnapshot_, current, sizeof(passwordSnapshot_));
    }
}

bool Authenticator::ValidateKey(const char* key)
{
    CheckPasswordEpoch();
    return tokens_.Touch(key);
}

void Authenticator::TouchKey(const char* key)
{
    tokens_.Touch(key);
}

bool Authenticator::AuthRequired()
{
    char pw[64] = {};
    password_.Get(pw, sizeof(pw));
    return pw[0] != '\0';
}

void Authenticator::ReadPasswordPadded(char* out, size_t size)
{
    memset(out, 0, size);
    password_.Get(out, size);
    // NVS fills up to the terminator and leaves the rest as it found it, so the
    // tail is zeroed again here rather than trusted.
    const size_t n = strnlen(out, size);
    if (n < size) memset(out + n, 0, size - n);
}

bool Authenticator::CheckPassword(const char* pw)
{
    CheckPasswordEpoch();

    // Both sides copied into zero-padded buffers of the same fixed size and
    // compared all the way to the end. strcmp stopped at the first wrong byte,
    // so how long it took said how much of the password was right.
    char expected[64];
    char given[64] = {};
    ReadPasswordPadded(expected, sizeof(expected));
    if (pw) strlcpy(given, pw, sizeof(given));

    unsigned char diff = 0;
    for (size_t i = 0; i < sizeof(expected); ++i)
        diff |= static_cast<unsigned char>(expected[i] ^ given[i]);
    return diff == 0;
}

uint32_t Authenticator::LockoutRemainingSeconds()
{
    LOCK(authMutex_);
    if (failures_ < MAX_FAILURES) return 0;
    const int64_t elapsed = esp_timer_get_time() - lastFailure_;
    if (elapsed >= LOCKOUT_US) return 0;
    return static_cast<uint32_t>((LOCKOUT_US - elapsed + 999999) / 1000000);
}

Authenticator::LoginResult Authenticator::TryPassword(const char* pw)
{
    // Each lock is taken and dropped on its own: CheckPassword takes the same
    // mutex by way of CheckPasswordEpoch, and it is not recursive.
    {
        LOCK(authMutex_);
        if (failures_ >= MAX_FAILURES)
        {
            if (esp_timer_get_time() - lastFailure_ < LOCKOUT_US)
                return LoginResult::TooManyAttempts;
            failures_ = 0;               // the cool-off elapsed; start counting again
        }
    }

    if (CheckPassword(pw))
    {
        LOCK(authMutex_);
        failures_ = 0;
        return LoginResult::Ok;
    }

    LOCK(authMutex_);
    ++failures_;
    lastFailure_ = esp_timer_get_time();
    if (failures_ == MAX_FAILURES)
        ESP_LOGW(TAG, "%d failed logins - refusing further attempts for %d s",
                 failures_, static_cast<int>(LOCKOUT_US / 1000000));
    return LoginResult::Wrong;
}

void Authenticator::MintKey(char* out)
{
    tokens_.Create(out);
}
