#pragma once

#include "StruxProvider.h"
#include "CommandEntry.h"
#include "TypedSettings.h"

class Stream;

// ──────────────────────────────────────────────────────────────
// SystemManager owns device identity and lifecycle — and nothing
// else (no timers, no watchdogs; those belong elsewhere):
//   - device.name: exposed to other managers via GetDeviceName()
//     (settings are private to their owner — nobody else reads the
//     key)
//   - the generic system commands: ping / info / reboot
// ──────────────────────────────────────────────────────────────
class SystemManager
{
    static constexpr const char* TAG = "SystemManager";

public:
    explicit SystemManager(StruxProvider& strux);

    SystemManager(const SystemManager&) = delete;
    SystemManager& operator=(const SystemManager&) = delete;
    SystemManager(SystemManager&&) = delete;
    SystemManager& operator=(SystemManager&&) = delete;

    void Init();

    /// Copies the device name into `out`; falls back to the project name if the
    /// stored value is empty.
    void GetDeviceName(char* out, size_t maxLen);

    /// What this product IS, and how it is meant to be driven.
    ///
    /// Registered by the application from its own Init() — the framework has no idea
    /// what the product does, and a Strux manager reaching into the app for it would
    /// be the design error CLAUDE.md names. Both are string literals with static
    /// storage duration: nothing is copied, nothing is freed.
    ///
    ///   `description`  one line, the answer to "which board is this". Keep it to a
    ///                  sentence.
    ///   `instructions` free-form, as long as it needs to be, served only when asked
    ///                  for (`system describe`). This is where relationships between
    ///                  commands, workflows, units, conventions and limitations go.
    ///                  Deliberately prose and not a schema: the commands describe
    ///                  their own shape, and what they do not describe is the part
    ///                  that does not fit in one.
    ///
    /// Both may be null, which is a device that says nothing rather than a device
    /// that says "unknown".
    void SetDocumentation(const char* description, const char* instructions);

    /// The one-line description, or "" when the application registered none.
    const char* GetDescription() const { return description_ ? description_ : ""; }

    /// How this chip is clocked, e.g. "48 MHz". Reported by `system info` and logged
    /// at boot.
    void DescribeCpu(char* out, size_t maxLen);

private:
    StruxProvider& strux_;

    // ── Settings (registered with SettingsManager in Init) ──
    inline static StringSetting name_{ "device.name", "Device Name", "KC-OT-PCB1246" };

    // What the application registered about itself. Pointers to literals, set once
    // during startup and read afterwards — no copy, no lock, nothing to free.
    const char* description_  = nullptr;
    const char* instructions_ = nullptr;

    // ── Commands (registered with CommandManager in Init) ──
    CommandResult Cmd_Ping(CommandContext& ctx);
    CommandResult Cmd_Info(CommandContext& ctx);
    CommandResult Cmd_Reboot(CommandContext& ctx);
    CommandResult Cmd_Describe(CommandContext& ctx);

    // Defined in SystemManager.cpp, beside the handlers.
    static CommandEntry pingCommand_;
    static CommandEntry infoCommand_;
    static CommandEntry rebootCommand_;
    static CommandEntry describeCommand_;
};
