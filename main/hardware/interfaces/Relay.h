#pragma once

// ──────────────────────────────────────────────────────────────
// Role interface: application vocabulary for "a relay".
// ──────────────────────────────────────────────────────────────

class Relay
{
public:
    virtual void Set(bool on) = 0;
    virtual bool IsOn() const = 0;
    virtual ~Relay() = default;
};
