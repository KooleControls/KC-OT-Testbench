#include "LedManager.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "RelayManager.h"
#include "TelemetryManager.h"
#include "esp_log.h"

LedManager::LedManager(AppProvider& app)
    : app_(app)
{
}

void LedManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    // Reaching DOWN into the framework, which is the only direction allowed. Nothing in
    // Strux was edited to make these two lines work.
    StruxProvider& strux = app_.getStrux();
    strux.getSettingsManager().Register({ &enabled_ });
    strux.getCommandManager().Register(this, { &getCommand_, &setCommand_ });

    timer_.SetHandler([this] { OnTick(); });
    timer_.Init("led", pdMS_TO_TICKS(POLL_MS));

    // The LED leaves Init() saying what is true right now — dark, because the relay
    // cannot be up this early — rather than in whatever state the driver left behind.
    Apply();
    timer_.Start();

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void LedManager::SetEnabled(bool enabled)
{
    enabled_.Set(enabled);
    app_.getStrux().getSettingsManager().Save();

    // Don't wait for the next tick: a switch in the dashboard that takes a visible
    // moment to reach the board reads as a switch that did not work.
    Apply();
}

void LedManager::Apply()
{
    const bool want = enabled_.Get() && app_.getStrux().getRelayManager().IsConnected();

    Led& led = app_.getBoard().GetLed();
    if (want != led.IsOn())
        led.Set(want);
}

// ──────────────────────────────────────────────────────────────
// Commands. These appear in `help` and work over the local WebSocket and the relay
// alike, because a handler serves neither — it serves a CommandContext.
// ──────────────────────────────────────────────────────────────

namespace {

constexpr CommandArg<bool> enabledArg{
    "enabled", "true lights the LED while the relay link is up; false keeps the "
               "board dark. Absent leaves the current setting alone.",
    Presence::Optional };

} // namespace

CommandEntry LedManager::getCommand_{
    "led get", &InvokeCommand<&LedManager::Cmd_Get>,
    "Report the indicator: whether it is enabled, whether the relay link is up, "
    "and whether the LED is lit right now."
};

CommandEntry LedManager::setCommand_{
    "led set", &InvokeCommand<&LedManager::Cmd_Set>,
    "Turn the relay-link indicator on or off. Persisted, so it survives a "
    "reboot. Omitting 'enabled' leaves it as it is and just reports the state.",
    { &enabledArg }
};

CommandResult LedManager::Cmd_Get(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    resp.field("enabled", enabled_.Get());
    resp.field("connected", app_.getStrux().getRelayManager().IsConnected());
    resp.field("on", app_.getBoard().GetLed().IsOn());
    return CommandResult::Ok;
}

CommandResult LedManager::Cmd_Set(CommandContext& ctx)
{
    // "Absent means leave it alone" is the one place has() earns itself: an omitted
    // optional bool decodes as false, which would turn the indicator OFF rather than
    // leave it. The destination no longer belongs to the handler, so the fallback has
    // to be written rather than left standing in an initialiser.
    const bool enabled = ctx.has(enabledArg) ? ctx.arg(enabledArg) : enabled_.Get();

    SetEnabled(enabled);

    // Telemetry from a command handler rather than from OnTick(): a Point is a few
    // hundred bytes of stack, which the timer service task has not got.
    auto point = app_.getStrux().getTelemetryManager().Measure("led");
    point.Tag("mode", enabled ? "link" : "off");
    point.Field("on", app_.getBoard().GetLed().IsOn());
    point.Commit();

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("enabled", enabled_.Get());
    resp.field("connected", app_.getStrux().getRelayManager().IsConnected());
    resp.field("on", app_.getBoard().GetLed().IsOn());
    return CommandResult::Ok;
}
