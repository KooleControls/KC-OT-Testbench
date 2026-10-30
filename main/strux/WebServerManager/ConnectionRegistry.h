#pragma once
#include "WsConnection.h"
#include "Mutex.h"
#include <cstdint>

// The fixed slot table of live connections. Owns allocation, lookup, removal,
// and the pre-auth reaper. Thread-safe (its own mutex). Snapshot via forEach
// by the log pump (the caller sends outside any lock it holds).
class ConnectionRegistry {
    static constexpr const char* TAG = "ConnectionRegistry";
public:
    static constexpr int MAX = 4;
    static constexpr int64_t PRE_AUTH_TIMEOUT_US = 10LL * 1000 * 1000;

    WsConnection* find(int fd);
    // Allocates a slot for fd; when full, reaps a stale UN-authenticated slot.
    // Returns nullptr if no slot could be freed. `now` = esp_timer_get_time().
    WsConnection* add(int fd, bool authed, int64_t now);
    void remove(int fd);

    // Invoke fn(const WsConnection&) for each active slot, under the lock.
    template <class F> void forEach(F&& fn)
    {
        LOCK(mutex_);
        for (auto& c : conns_) if (c.active()) fn(c);
    }

private:
    WsConnection conns_[MAX];
    Mutex mutex_;
};
