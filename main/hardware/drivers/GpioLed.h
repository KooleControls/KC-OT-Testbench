#pragma once

#include "interfaces/Led.h"
#include "driver/gpio.h"

// ──────────────────────────────────────────────────────────────
// GPIO LED driver. Pin and polarity are injected by the board
// (from its BoardConfig constants) — this driver assumes a valid
// pin. A board without an LED binds MockLed instead.
// ──────────────────────────────────────────────────────────────

class GpioLed : public Led
{
public:
    GpioLed(int pin, bool activeHigh)
        : pin_(static_cast<gpio_num_t>(pin)), activeHigh_(activeHigh)
    {
    }

    void Init()
    {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin_;
        cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);

        Set(false);
    }

    void Set(bool on) override
    {
        state_ = on;
        gpio_set_level(pin_, on == activeHigh_ ? 1 : 0);
    }

    bool IsOn() const override { return state_; }

private:
    gpio_num_t pin_;
    bool activeHigh_;
    bool state_ = false;
};
