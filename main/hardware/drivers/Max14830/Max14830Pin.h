#pragma once

#include "Max14830.h"
#include "interfaces/DigitalInput.h"

// ──────────────────────────────────────────────────────────────
// One MAX14830 GPIO as a role: a pin number, a polarity and a parent chip. The same
// idea as ESP-MAX14830's MAX14830GPIOPin, without ESPHome.
//
// Output is a template over the role so one class serves every output role with the
// same shape (Led, Relay, DigitalOutput: Set / IsOn) — the role is what the
// application sees, the pin is what the board wires.
// ──────────────────────────────────────────────────────────────

template <class Role>
class Max14830Output final : public Role
{
public:
    Max14830Output(Max14830& chip, uint8_t pin, bool activeHigh, bool initiallyOn = false)
        : chip_(chip), pin_(pin), activeHigh_(activeHigh), state_(initiallyOn) {}

    /// After the chip's Init().
    void Init() { chip_.PinModeOutput(pin_, Level(state_)); }

    void Set(bool on) override
    {
        state_ = on;
        chip_.WritePin(pin_, Level(on));
    }

    bool IsOn() const override { return state_; }

private:
    Max14830& chip_;
    const uint8_t pin_;
    const bool activeHigh_;
    bool state_;

    bool Level(bool on) const { return on == activeHigh_; }
};

class Max14830Input final : public DigitalInput
{
public:
    Max14830Input(Max14830& chip, uint8_t pin, bool activeHigh)
        : chip_(chip), pin_(pin), activeHigh_(activeHigh) {}

    /// After the chip's Init().
    void Init() { chip_.PinModeInput(pin_); }

    bool IsActive() const override { return chip_.ReadPin(pin_) == activeHigh_; }

private:
    Max14830& chip_;
    const uint8_t pin_;
    const bool activeHigh_;
};
