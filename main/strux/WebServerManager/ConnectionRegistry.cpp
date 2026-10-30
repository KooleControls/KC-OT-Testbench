#include "ConnectionRegistry.h"
#include <esp_log.h>

WsConnection* ConnectionRegistry::find(int fd)
{
    LOCK(mutex_);
    for (auto& c : conns_) if (c.fd == fd) return &c;
    return nullptr;
}

WsConnection* ConnectionRegistry::add(int fd, bool authed, int64_t now)
{
    LOCK(mutex_);
    for (auto& c : conns_) if (c.fd == fd) return &c;   // already present

    WsConnection* slot = nullptr;
    for (auto& c : conns_) if (!c.active()) { slot = &c; break; }
    if (!slot)   // full: reap a stale un-authenticated squatter
        for (auto& c : conns_)
            if (!c.authed && c.age(now) > PRE_AUTH_TIMEOUT_US) { slot = &c; break; }
    if (!slot) { ESP_LOGW(TAG, "table full: fd=%d refused", fd); return nullptr; }

    slot->reset();
    slot->fd = fd;
    slot->authed = authed;
    slot->connectedAt = now;
    return slot;
}

void ConnectionRegistry::remove(int fd)
{
    LOCK(mutex_);
    for (auto& c : conns_) if (c.fd == fd) { c.reset(); return; }
}
