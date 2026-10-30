#pragma once

#include "SettingType.h"
#include "Fatal.h"

class SettingsManager;
class SettingIterator;
struct Int32Setting;
struct UInt32Setting;
struct FloatSetting;
struct BoolSetting;
struct StringSetting;

// ──────────────────────────────────────────────────────────────
// Base setting — the intrusive chain link + schema facts.
//
// Owners declare the typed leaves (TypedSettings.h) as
// `inline static` class members and register them in Init() via
// SettingsManager::Register({&a_, &b_}). The registration pattern:
// entries are the links, the owner provides the memory, misuse
// fails on the first boot. The core knows nothing about JSON or
// any other presentation — converters live at the edge and use the
// type tag + checked downcasts below.
// ──────────────────────────────────────────────────────────────
struct Setting
{
    const char* const key;    // NVS key — a storage detail, not an API
    const char* const label;  // shown in the generated settings UI
    const SettingType type;

    Setting(const Setting&) = delete;
    Setting& operator=(const Setting&) = delete;

    // Checked downcasts for generic consumers (converters). Each leaf
    // overrides exactly its own pair; calling the wrong one is a bug →
    // Die(). The settings UI iterates on every getSettings, so a
    // wrong-type conversion cannot hide.
    virtual Int32Setting&        asInt32()        { Die(SettingType::Int32);  }
    virtual const Int32Setting&  asInt32()  const { Die(SettingType::Int32);  }
    virtual UInt32Setting&       asUInt32()       { Die(SettingType::UInt32); }
    virtual const UInt32Setting& asUInt32() const { Die(SettingType::UInt32); }
    virtual FloatSetting&        asFloat()        { Die(SettingType::Float);  }
    virtual const FloatSetting&  asFloat()  const { Die(SettingType::Float);  }
    virtual BoolSetting&         asBool()         { Die(SettingType::Bool);   }
    virtual const BoolSetting&   asBool()   const { Die(SettingType::Bool);   }
    virtual StringSetting&       asString()       { Die(SettingType::String); }
    virtual const StringSetting& asString() const { Die(SettingType::String); }

    // A registered entry is a live chain link; destroying it would leave
    // a dangling pointer in the chain.
    virtual ~Setting()
    {
        if (registered)
            FATAL("registered setting '%s' destroyed - setting tables must "
                  "live for the whole application", key);
    }

protected:
    Setting(const char* key, const char* label, SettingType type)
        : key(key), label(label), type(type) {}

    // Logs what was asked for AND what it actually is:
    //   "setting 'mqtt.port' is int32, not string"
    [[noreturn]] void Die(SettingType want) const
    {
        FATAL("setting '%s' is %s, not %s",
              key, SettingTypeToString(type), SettingTypeToString(want));
    }

    // Leaves reach NVS through this. Using a setting that was never
    // registered is a code bug (a forgotten Register() entry) — found on
    // the first test run, never shipped as a silent default.
    SettingsManager& Manager() const
    {
        if (mgr == nullptr)
            FATAL("setting '%s' used before registration", key);
        return *mgr;
    }

private:
    friend class SettingsManager;   // links the chain
    friend class SettingIterator;   // walks the chain

    SettingsManager* mgr = nullptr;
    Setting* next = nullptr;
    bool registered = false;
};

// ──────────────────────────────────────────────────────────────
// Iteration — the ONLY public way to walk the chain, which is what
// lets `next` stay private. Minimal forward iterator, enough for
// `for (Setting& s : settingsManager)`. Holds one pointer.
// ──────────────────────────────────────────────────────────────
class SettingIterator
{
    Setting* cur_;

public:
    explicit SettingIterator(Setting* s) : cur_(s) {}

    Setting& operator*() const { return *cur_; }
    SettingIterator& operator++() { cur_ = cur_->next; return *this; }
    bool operator!=(const SettingIterator& o) const { return cur_ != o.cur_; }
};
