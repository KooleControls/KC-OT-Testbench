#include "TypedSettings.h"
#include "SettingsManager.h"
#include <cstring>
#include <cstdio>

// Typed leaf Get/Set bodies. They live here (not in the header) because
// they need the full SettingsManager, which includes Setting.h itself.

int32_t Int32Setting::Get() const
{
    int32_t v = def;
    Manager().ReadI32(key, v);
    return v;
}

bool Int32Setting::Set(int32_t v)
{
    return Manager().WriteI32(key, v);
}

uint32_t UInt32Setting::Get() const
{
    uint32_t v = def;
    Manager().ReadU32(key, v);
    return v;
}

bool UInt32Setting::Set(uint32_t v)
{
    return Manager().WriteU32(key, v);
}

float FloatSetting::Get() const
{
    uint32_t bits = 0;
    if (!Manager().ReadU32(key, bits))
        return def;
    float v;
    static_assert(sizeof(v) == sizeof(bits), "float/u32 size mismatch");
    memcpy(&v, &bits, sizeof(v));
    return v;
}

bool FloatSetting::Set(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return Manager().WriteU32(key, bits);
}

bool BoolSetting::Get() const
{
    uint8_t v = def ? 1 : 0;
    Manager().ReadU8(key, v);
    return v != 0;
}

bool BoolSetting::Set(bool v)
{
    return Manager().WriteU8(key, v ? 1 : 0);
}

bool StringSetting::Get(char* out, size_t maxLen) const
{
    if (Manager().ReadString(key, out, maxLen))
        return true;

    snprintf(out, maxLen, "%s", def);
    return false;
}

bool StringSetting::Set(const char* v)
{
    return Manager().WriteString(key, v);
}
