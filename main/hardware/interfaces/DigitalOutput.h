#pragma once

// ──────────────────────────────────────────────────────────────
// Role interface: a control line that is asserted or not — a reset, an enable,
// a boot-select. Polarity is the driver's business: asserted means asserted,
// whatever level that is on the pin.
// ──────────────────────────────────────────────────────────────

class DigitalOutput
{
public:
    virtual void Set(bool asserted) = 0;
    virtual bool IsOn() const = 0;
    virtual ~DigitalOutput() = default;
};
