#include "HvacSimManager.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "ModbusFrame.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>

namespace {

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

int32_t Code(uint32_t value) { return static_cast<int32_t>(std::min<uint32_t>(value, INT32_MAX)); }

constexpr CommandArg<uint32_t> unitArg{
    "unit", "Which simulated unit: 1 to 10, as in the gateway's HVAC list (not its Modbus "
            "address)." };

constexpr CommandArg<bool> powerArg{
    "power", "Switch the indoor unit on (true) or off (false).", Presence::Optional };

constexpr CommandArg<uint32_t> modeArg{
    "mode", "The mode in the unit's type's own code, as its mode register takes it: a "
            "CoolPlug's 0 cool, 1 heat, 2 auto; a Hisense's 0 fan, 1 heat, 2 cool, 3 dry, "
            "4 auto; an Intesis box's 0 auto, 1 heat, 2 dry, 3 fan, 4 cool; a Daikin's 1 "
            "heat, 2 auto, 3 cool.",
    Presence::Optional };

constexpr CommandArg<uint32_t> setpointArg{
    "setpoint", "The set temperature in tenths of a °C: 225 is 22.5 °C. 100 to 350; a "
                "Hisense takes whole degrees, 180 to 320.",
    Presence::Optional };

constexpr CommandArg<uint32_t> fanArg{
    "fan", "The fan speed in the type's own code, as its fan register takes it.",
    Presence::Optional };

constexpr CommandArg<uint32_t> swingArg{
    "swing", "The louvres in the type's own code, as its louvre register takes it (a "
             "Hisense's control codes, not the ones it reports).",
    Presence::Optional };

constexpr CommandArg<bool> filterArg{
    "on", "Raise the filter sign (true) or clear it (false)." };

} // namespace

// ── Commands ─────────────────────────────────────────────────

CommandEntry HvacSimManager::statusCommand_{
    "hvac status", &InvokeCommand<&HvacSimManager::Cmd_Status>,
    "Report the ten simulated HVAC units: each one's type (the gateway's numbers) and "
    "whether it is simulated; for those that are, the address, the indoor unit's state, "
    "the registers the gateway reads as they read now, and what the gateway asked. "
    "Their simulation settings are hvac1.* to hvac10.*, which apply within a second."
};

CommandEntry HvacSimManager::setCommand_{
    "hvac set", &InvokeCommand<&HvacSimManager::Cmd_Set>,
    "Change a simulated indoor unit as its own remote would: power, mode, setpoint, "
    "fan, louvres. Codes are the unit's type's own. The gateway reads the change at its "
    "next poll, and can overwrite it; a remote the gateway locked takes nothing. Not "
    "saved: every unit starts the same after a reboot.",
    { &unitArg, &powerArg, &modeArg, &setpointArg, &fanArg, &swingArg }
};

// Apart from `hvac set`, which is at Strux's limit of six arguments -- and it is the
// unit's doing, not its remote's.
CommandEntry HvacSimManager::filterCommand_{
    "hvac filter", &InvokeCommand<&HvacSimManager::Cmd_Filter>,
    "Raise or clear a simulated indoor unit's filter sign, as the unit does when its "
    "filter needs cleaning. Only a type with one (the CoolPlug). The gateway can clear it "
    "too.",
    { &unitArg, &filterArg }
};

HvacSimManager::HvacSimManager(AppProvider& app)
    : app_(app)
{
}

void HvacSimManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    StruxProvider& strux = app_.getStrux();
    SettingsManager& settings = strux.getSettingsManager();
    HvacSimulator::RegisterSettings(settings);
    for (uint8_t i = 0; i < HvacSimulator::UNITS; ++i)
        units_[i].Init(i, settings);

    strux.getCommandManager().Register(this, { &statusCommand_, &setCommand_, &filterCommand_ });

    // Below the RS485 task: this only keeps time, and a request waits for nothing here.
    task_.Init("hvacsim", 4, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
        ESP_LOGE(TAG, "Failed to start the HVAC simulator task");

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

void HvacSimManager::TaskLoop()
{
    uint32_t nextRefreshMs = NowMs() + REFRESH_MS;
    for (;;)
    {
        const uint32_t now = NowMs();
        if (Due(now, nextRefreshMs))
        {
            nextRefreshMs = now + REFRESH_MS;
            for (HvacSimulator& unit : units_)
                unit.Refresh();
        }
        for (HvacSimulator& unit : units_)
            unit.Tick(now);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ── For the transports ───────────────────────────────────────

HvacSimulator* HvacSimManager::Find(HvacTransport transport, uint8_t unit)
{
    for (HvacSimulator& u : units_)
        if (u.Serves(transport, unit))
            return &u;
    return nullptr;
}

void HvacSimManager::Broadcast(HvacTransport transport, const uint8_t* pdu, size_t len, uint32_t nowMs)
{
    uint8_t reply[modbus::MAX_PDU];
    for (HvacSimulator& u : units_)
        if (u.Serves(transport, modbus::BROADCAST) && u.Answers())
            u.Handle(pdu, len, true, reply, nowMs);
}

// ── Command handlers ─────────────────────────────────────────

CommandResult HvacSimManager::Cmd_Status(CommandContext& ctx)
{
    const uint32_t now = NowMs();
    auto resp = ctx.reply.object();
    auto list = resp.array("units");
    for (HvacSimulator& unit : units_)
    {
        auto o = list.object();
        unit.Report(o, now);
    }
    return CommandResult::Ok;
}

CommandResult HvacSimManager::Cmd_Set(CommandContext& ctx)
{
    HvacSimulator::Change change;
    if (ctx.has(powerArg))    change.power = ctx.arg(powerArg) ? 1 : 0;
    if (ctx.has(modeArg))     change.mode = Code(ctx.arg(modeArg));
    if (ctx.has(setpointArg)) change.setpoint = Code(ctx.arg(setpointArg));
    if (ctx.has(fanArg))      change.fan = Code(ctx.arg(fanArg));
    if (ctx.has(swingArg))    change.swing = Code(ctx.arg(swingArg));
    return ApplyToUnit(ctx, ctx.arg(unitArg), change);
}

CommandResult HvacSimManager::Cmd_Filter(CommandContext& ctx)
{
    HvacSimulator::Change change;
    change.filter = ctx.arg(filterArg) ? 1 : 0;
    return ApplyToUnit(ctx, ctx.arg(unitArg), change);
}

CommandResult HvacSimManager::ApplyToUnit(CommandContext& ctx, uint32_t n, const HvacSimulator::Change& change)
{
    auto resp = ctx.reply.object();
    if (n < 1 || n > HvacSimulator::UNITS)
    {
        resp.field("ok", false);
        resp.field("error", "no such unit: 1 to 10");
        return CommandResult::Ok;
    }

    const char* error = nullptr;
    HvacSimulator& unit = units_[n - 1];
    if (!unit.Apply(change, error))
    {
        resp.field("ok", false);
        resp.field("error", error);
        return CommandResult::Ok;
    }

    resp.field("ok", true);
    auto o = resp.object("unit");
    unit.Report(o, NowMs());
    return CommandResult::Ok;
}
