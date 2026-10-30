#pragma once

// ──────────────────────────────────────────────────────────────
// Role interface: application vocabulary for "an input that is either active or
// not" — a contact, a button, a card-detect switch. Polarity is the driver's
// business: active means active, whatever level that is on the pin.
// ──────────────────────────────────────────────────────────────

class DigitalInput
{
public:
    virtual bool IsActive() const = 0;
    virtual ~DigitalInput() = default;
};
