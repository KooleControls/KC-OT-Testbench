#pragma once

#include <cstdint>
#include <cstddef>
#include "Setting.h"

// ──────────────────────────────────────────────────────────────
// Typed setting leaves — what owners declare. Typed const defaults,
// typed Get/Set; the string key never leaks into calling code.
//
//   inline static UInt32Setting port_{ "mqtt.port", "MQTT Port", 1883 };
//   ...
//   settings.Register({ &port_ });
//   uint32_t p = port_.Get();
//
// Get() returns the NVS value, or the default when nothing is
// stored. Set() writes NVS (commit via SettingsManager::Save()).
// Bodies live in Setting.cpp (they need the full SettingsManager).
// ──────────────────────────────────────────────────────────────

struct Int32Setting : Setting
{
    const int32_t def;

    Int32Setting(const char* key, const char* label, int32_t def)
        : Setting(key, label, SettingType::Int32), def(def) {}

    int32_t Get() const;
    bool Set(int32_t v);

    Int32Setting&       asInt32()       override { return *this; }
    const Int32Setting& asInt32() const override { return *this; }
};

struct UInt32Setting : Setting
{
    const uint32_t def;

    UInt32Setting(const char* key, const char* label, uint32_t def)
        : Setting(key, label, SettingType::UInt32), def(def) {}

    uint32_t Get() const;
    bool Set(uint32_t v);

    UInt32Setting&       asUInt32()       override { return *this; }
    const UInt32Setting& asUInt32() const override { return *this; }
};

struct FloatSetting : Setting
{
    const float def;

    FloatSetting(const char* key, const char* label, float def)
        : Setting(key, label, SettingType::Float), def(def) {}

    // NVS has no float type — stored bit-cast through u32.
    float Get() const;
    bool Set(float v);

    FloatSetting&       asFloat()       override { return *this; }
    const FloatSetting& asFloat() const override { return *this; }
};

struct BoolSetting : Setting
{
    const bool def;

    BoolSetting(const char* key, const char* label, bool def)
        : Setting(key, label, SettingType::Bool), def(def) {}

    // Stored as u8 (same as the previous settings system).
    bool Get() const;
    bool Set(bool v);

    BoolSetting&       asBool()       override { return *this; }
    const BoolSetting& asBool() const override { return *this; }
};

struct StringSetting : Setting
{
    const char* const def;   // must be a string literal (checked at Register)

    StringSetting(const char* key, const char* label, const char* def)
        : Setting(key, label, SettingType::String), def(def) {}

    /// Copies the NVS value into `out`, or the default when nothing is
    /// stored. Returns true if a stored value was found.
    bool Get(char* out, size_t maxLen) const;
    bool Set(const char* v);

    StringSetting&       asString()       override { return *this; }
    const StringSetting& asString() const override { return *this; }
};
