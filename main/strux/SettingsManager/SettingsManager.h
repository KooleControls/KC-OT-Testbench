#pragma once

#include "StruxProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "Setting.h"
#include "TypedSettings.h"
#include "Mutex.h"
#include <nvs_handle.hpp>

class Stream;

// ──────────────────────────────────────────────────────────────
// Schema registry + NVS storage. Managers own their settings as
// typed inline static members (TypedSettings.h) and register them
// in Init(). Nothing about JSON, UI, or any presentation format
// lives here — converters (e.g. the getSettings/setSetting command
// handlers below) walk the chain via begin()/end().
// ──────────────────────────────────────────────────────────────
class SettingsManager {
    static constexpr const char* TAG = "SettingsManager";
    static constexpr const char* NVS_NAMESPACE = "settings";

public:
    explicit SettingsManager(StruxProvider& strux);

    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    void Init();

    // ── Schema registration ──────────────────────────────────
    // Called from a manager's Init(). Entries MUST have static storage
    // duration (see ~Setting). Heterogeneous leaves register through
    // base pointers; the initializer_list lives on the caller's stack.
    //
    // SettingsManager is the NVS link, so registration enforces NVS
    // rules — boot-deterministically.
    void Register(std::initializer_list<Setting*> settings);

    // ── Iteration ────────────────────────────────────────────
    // begin() takes the lock only to read the head; the links behind it
    // are write-once and the entries immortal, so the walk itself needs
    // no lock.
    //
    //   for (Setting& s : settingsManager) { ... }
    SettingIterator begin();
    SettingIterator end() { return SettingIterator(nullptr); }

    // ── Persistence ──────────────────────────────────────────
    bool Save();
    /// Erase the NVS namespace and commit. That's all — defaults resolve
    /// at read, so nothing needs to be written back.
    bool ResetToDefaults();

    // ── NVS primitives (used by the typed leaves via Manager()) ──
    // Return false when the key has no stored value (caller falls back
    // to the entry's default) or when NVS is unavailable.
    bool ReadI32(const char* key, int32_t& out) const;
    bool WriteI32(const char* key, int32_t v);
    bool ReadU32(const char* key, uint32_t& out) const;
    bool WriteU32(const char* key, uint32_t v);
    bool ReadU8(const char* key, uint8_t& out) const;
    bool WriteU8(const char* key, uint8_t v);
    bool ReadString(const char* key, char* out, size_t maxLen) const;
    bool WriteString(const char* key, const char* v);

private:
    StruxProvider& strux_;
    InitState initState_;
    std::unique_ptr<nvs::NVSHandle> handle_;

    Mutex mutex_;              // guards the chain (Register/begin)
    Setting* head_ = nullptr;

    const Setting* FindLocked(const char* key) const;

    // ── WebSocket commands (the JSON converter lives HERE, at the
    //    edge — not in the schema/storage core above) ──────────
    CommandResult Cmd_GetSettings(CommandContext& ctx);
    CommandResult Cmd_SetSetting(CommandContext& ctx);
    CommandResult Cmd_SaveSettings(CommandContext& ctx);

    // Defined in SettingsManager.cpp, beside the handlers and the arguments they read.
    static CommandEntry listCommand_;
    static CommandEntry setCommand_;
    static CommandEntry saveCommand_;
};
