#pragma once

// ──────────────────────────────────────────────────────────────
// Role interface: application vocabulary for "the LED".
// Boards bind a real driver (GpioLed) or a mock (MockLed).
// Role interfaces stay small (1-3 pure methods) and speak
// application vocabulary — never chip or GPIO vocabulary.
// ──────────────────────────────────────────────────────────────

class Led
{
public:
    virtual void Set(bool on) = 0;
    virtual bool IsOn() const = 0;
    virtual ~Led() = default;

    void On() { Set(true); }
    void Off() { Set(false); }
    void Toggle() { Set(!IsOn()); }
};
