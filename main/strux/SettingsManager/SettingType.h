#pragma once

#include <cstdint>

// Grows rarely. Every switch over it has NO default case, so (with
// -Werror=switch) adding a type here breaks the build at every converter
// that hasn't been updated — exactly what we want.
enum class SettingType : uint8_t { String, Int32, UInt32, Float, Bool };

// One place for the names; used by Setting::Die(), the UI converter, logs.
// The trailing return is NOT a default case — -Wswitch still flags a missing
// enum value; the trailing return only satisfies -Wreturn-type.
constexpr const char* SettingTypeToString(SettingType type)
{
    switch (type)
    {
    case SettingType::String: return "string";
    case SettingType::Int32:  return "int32";
    case SettingType::UInt32: return "uint32";
    case SettingType::Float:  return "float";
    case SettingType::Bool:   return "bool";
    }
    return "?";
}
