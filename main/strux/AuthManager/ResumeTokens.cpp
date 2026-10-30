#include "ResumeTokens.h"
#include "ContextLock.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>

static constexpr const char* TAG = "ResumeTokens";

void ResumeTokens::Create(char* tokenOut)
{
    uint32_t r[4] = { esp_random(), esp_random(), esp_random(), esp_random() };
    snprintf(tokenOut, TOKEN_LEN, "%08lx%08lx%08lx%08lx",
             static_cast<unsigned long>(r[0]), static_cast<unsigned long>(r[1]),
             static_cast<unsigned long>(r[2]), static_cast<unsigned long>(r[3]));

    int64_t now = esp_timer_get_time();

    LOCK(mutex_);
    Channel* slot = nullptr;
    for (auto& s : sessions_)   // prefer an empty or expired slot
    {
        if (s.token[0] == 0 || now - s.lastActivity > IDLE_TIMEOUT_US)
        {
            slot = &s;
            break;
        }
    }
    if (!slot)                  // all live: evict the stalest
    {
        slot = &sessions_[0];
        for (auto& s : sessions_)
            if (s.lastActivity < slot->lastActivity) slot = &s;
    }
    strlcpy(slot->token, tokenOut, TOKEN_LEN);
    slot->lastActivity = now;
}

bool ResumeTokens::Touch(const char* token)
{
    if (!token || token[0] == 0) return false;
    int64_t now = esp_timer_get_time();

    LOCK(mutex_);
    for (auto& s : sessions_)
    {
        if (s.token[0] == 0 || strcmp(s.token, token) != 0) continue;
        if (now - s.lastActivity > IDLE_TIMEOUT_US)   // lazy expiry
        {
            s.token[0] = 0;
            return false;
        }
        s.lastActivity = now;
        return true;
    }
    return false;
}

void ResumeTokens::Clear()
{
    LOCK(mutex_);
    for (auto& s : sessions_)
    {
        s.token[0] = 0;
        s.lastActivity = 0;
    }
}
