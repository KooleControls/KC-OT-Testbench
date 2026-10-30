#pragma once
#include "IMutex.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef LOCKINFO_BUILD
#define LOCKINFO_BUILD(mutex) \
    LockInfo{ &(mutex), #mutex, __FILE__, __LINE__, TAG, nullptr, nullptr }
#endif

#ifndef LOCK_CTOR_ARGS
#define LOCK_CTOR_ARGS(mutex) (mutex), LOCKINFO_BUILD(mutex)
#endif

#define LOCK(mutex) ContextLock lock(LOCK_CTOR_ARGS(mutex))

struct LockInfo
{
    const IMutex* mutex;
    const char* mutexVarName;
    const char* file;
    int line;
    const char* callerTag;
    const char* taskName;
    TaskHandle_t taskHandle;
};


class ContextLock
{
    static constexpr TickType_t kAttemptTimeoutTicks = pdMS_TO_TICKS(5000);
    static constexpr int kMaxAttempts = 3;

    const IMutex &mutex;
    LockInfo info;
    bool taken = false;

public:
    ContextLock(const IMutex &_mutex, LockInfo _info) noexcept
        : mutex(_mutex),
          info(_info)
    {
        info.taskHandle = xTaskGetCurrentTaskHandle();
        info.taskName = pcTaskGetName(info.taskHandle);

        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
        {
            taken = mutex.Take(kAttemptTimeoutTicks);
            if (taken)
            {
                return;
            }

            LogLockTimeout(attempt);
        }

        LogDeadlock();
        assert(false && "Deadlock detected, rebooting");
        esp_restart();
    }

    ~ContextLock() noexcept
    {
        if (taken)
        {
            mutex.Give();
        }
    }

private:
    void LogDeadlock() noexcept
    {
        ESP_LOGE(info.callerTag ? info.callerTag : "ContextLock",
                 "DEADLOCK FAILSAFE: mutex '%s' not acquired by task '%s' after (%s:%d). Rebooting.",
                 info.mutexVarName ? info.mutexVarName : "?",
                 info.taskName ? info.taskName : "?",
                 info.file ? info.file : "?",
                 info.line);
    }

    void LogLockTimeout(int attempt) noexcept
    {
        ESP_LOGE(info.callerTag ? info.callerTag : "ContextLock",
                 "Timeout waiting for mutex '%s' in task '%s' (%s:%d) after 5s (attempt %d/%d)",
                 info.mutexVarName ? info.mutexVarName : "?",
                 info.taskName ? info.taskName : "?",
                 info.file ? info.file : "?",
                 info.line,
                 attempt,
                 kMaxAttempts);
    }
};
