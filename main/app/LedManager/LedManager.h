#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Timer.h"

// ──────────────────────────────────────────────────────────────
// The worked example of an application manager. It owns the LED and uses it to say one
// thing: whether this device is connected to its relay server — lit while the pipe is
// up, dark while it is not. In doing so it touches every edge the layering allows and
// none that it forbids:
//
//   • the board, for the hardware        — app_.getBoard().GetLed()
//   • the framework, for the link state  — app_.getStrux().getRelayManager()
//   • the framework, for settings        — led.enabled, in the settings UI with no UI
//                                          code written
//   • the framework, for commands        — `led get` / `led set`, in `help` and
//                                          reachable over WebSocket and the relay alike
//   • the framework, for telemetry       — a point when the indication is switched
//
// Its browser half is the frontend's home page (frontend/src/pages/HomePage.tsx over
// the use-led hook), which is ordinary SPA code and touches no firmware.
//
// Note what it does NOT do: nothing in Strux knows this class exists. It is not named in
// StruxContext, not in StruxProvider, and not in main.cpp. It announces itself by
// registering, from its own Init(), into managers it found through AppProvider — and
// reading the link state needed no accessor added to RelayManager, because IsConnected()
// was already the framework's own idea of the answer. Copy this file to start a feature;
// delete it when the product has real ones.
// ──────────────────────────────────────────────────────────────
class LedManager
{
    static constexpr const char* TAG = "LedManager";

    // How often the link state is sampled. A poll rather than a callback registered with
    // RelayManager: reading a bool costs nothing, and it keeps the framework untouched,
    // which is the whole point of this file. Quarter-second is below what anyone reads
    // as a delay on an indicator.
    static constexpr uint32_t POLL_MS = 250;

public:
    explicit LedManager(AppProvider& app);

    LedManager(const LedManager&) = delete;
    LedManager& operator=(const LedManager&) = delete;
    LedManager(LedManager&&) = delete;
    LedManager& operator=(LedManager&&) = delete;

    void Init();

    /// Whether the LED shows the link at all. Persisted, so a product that wants a dark
    /// board stays dark across a reboot. Public because it is this manager's domain API —
    /// another app manager would call this, not the command.
    void SetEnabled(bool enabled);

    bool IsEnabled() const { return enabled_.Get(); }

private:
    AppProvider& app_;
    InitState initState_;
    Timer timer_;

    /// Drive the LED to what the link says, and only when that differs from where the
    /// LED already is. There is no cached state to go stale: the pin is the state, so
    /// this both applies the indication and repairs it if anything else moved the LED.
    void Apply();

    /// Runs on the FreeRTOS timer service task, whose stack is small — so this reads a
    /// bool and maybe writes a GPIO, and nothing more. A telemetry Point does not fit
    /// here (see TelemetryManager), which is why the point is taken in the command
    /// handler. No loss: the only link transition a point could ever *reach* the server
    /// is the one where the link came up, because a point travels down the very pipe
    /// whose death it would be reporting.
    void OnTick() { Apply(); }

    // ── Settings. Keys are at most 15 characters — NVS's limit, asserted at RUNTIME in
    // Register(), so an over-long key compiles fine and then boot-loops the device.
    // Read on every tick rather than once at Init, so a change in the settings UI shows
    // on the board a quarter-second later instead of after a reboot.
    inline static BoolSetting enabled_{ "led.enabled", "LED Shows Relay Link", true };

    // ── Commands ──
    CommandResult Cmd_Get(CommandContext& ctx);
    CommandResult Cmd_Set(CommandContext& ctx);

    // Defined in LedManager.cpp, beside the handlers and the argument they read.
    static CommandEntry getCommand_;
    static CommandEntry setCommand_;
};
