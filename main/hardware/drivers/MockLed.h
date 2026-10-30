#pragma once

#include "interfaces/Led.h"

// ──────────────────────────────────────────────────────────────
// Led role with no hardware behind it — remembers state only.
// For boards without an LED fitted.
// ──────────────────────────────────────────────────────────────

class MockLed : public Led
{
public:
    void Set(bool on) override { state_ = on; }
    bool IsOn() const override { return state_; }

private:
    bool state_ = false;
};
