#pragma once
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "Mutex.h"
#include "esp_log.h"

#define REQUIRE_READY_MAX_WAIT  pdMS_TO_TICKS(10000)

#define WAIT_FOR_READY(obj)                                                      \
    do                                                                           \
    {                                                                            \
        if (!(obj).WaitForReady(REQUIRE_READY_MAX_WAIT))                         \
        {                                                                        \
            ESP_LOGE("InitState", "WaitForReady failed: %s", #obj);              \
            assert(false && "Initialization wait failed");                       \
        }                                                                        \
    } while (0)

#define RETURN_IF_READY(obj) \
    do { if ((obj).IsReady()) return; } while (0)

#define RETURN_IF_NOT_READY(obj) \
    do { if (!(obj).IsReady()) return; } while (0)


class InitState
{
    constexpr static const char* TAG = "InitState";
public:
    InitState()
    {
        _group = xEventGroupCreate();
        assert(_group && "Failed to create event group");
    }

    ~InitState()
    {
        vEventGroupDelete(_group);
    }

    InitState(const InitState&) = delete;
    InitState& operator=(const InitState&) = delete;

    class InitAttempt
    {
        constexpr static const char* TAG = "InitState::InitAttempt";
    public:
        InitAttempt() = default;
        InitAttempt(InitState* g, bool owns) : _guard(g), _ownsInit(owns) {}
        InitAttempt(const InitAttempt&) = delete;
        InitAttempt& operator=(const InitAttempt&) = delete;

        InitAttempt(InitAttempt&& other) noexcept
            : _guard(other._guard), _ownsInit(other._ownsInit), _committed(other._committed)
        {
            other._guard = nullptr;
            other._ownsInit = false;
            other._committed = true; // moved-from must not rollback
        }

        InitAttempt& operator=(InitAttempt&&) = delete;
        ~InitAttempt()
        {
            // If we claimed init but never committed ready, release claim.
            if (_guard && _ownsInit && !_committed)
            {
                _guard->ClearInitializing();
            }
        }

        bool OwnsInit() const noexcept { return _ownsInit; }
        explicit operator bool() const noexcept { return _ownsInit; }

        void SetReady()
        {
            if (!_guard || !_ownsInit)
                return;

            _guard->SetReadyInternal();
            _committed = true;
        }

    private:
        InitState* _guard = nullptr;
        bool _ownsInit = false;
        bool _committed = false;
    };

    // Returns an InitAttempt that is "truthy" only for the single initializer.
    InitAttempt TryBeginInit()
    {
        LOCK(_mutex);

        const EventBits_t bits = xEventGroupGetBits(_group);
        if (bits & (READY_BIT | INITING_BIT))
            return InitAttempt(this, false);

        xEventGroupSetBits(_group, INITING_BIT);
        return InitAttempt(this, true);
    }

    bool IsReady() const
    {
        return (xEventGroupGetBits(_group) & READY_BIT) != 0;
    }

    bool WaitForReady(TickType_t timeout) const
    {
        return (xEventGroupWaitBits(_group, READY_BIT, pdFALSE, pdFALSE, timeout) & READY_BIT) != 0;
    }

private:
    friend class InitAttempt;

    void ClearInitializing()
    {
        LOCK(_mutex);
        xEventGroupClearBits(_group, INITING_BIT);
    }

    void SetReadyInternal()
    {
        LOCK(_mutex);
        xEventGroupClearBits(_group, INITING_BIT);
        xEventGroupSetBits(_group, READY_BIT);
    }

private:
    mutable Mutex _mutex;
    EventGroupHandle_t _group = nullptr;

    static inline constexpr EventBits_t READY_BIT   = (1 << 0);
    static inline constexpr EventBits_t INITING_BIT = (1 << 1);
};
