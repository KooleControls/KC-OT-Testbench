#include "RoutingManager.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "esp_log.h"
#include <cstring>

namespace {

const char* MasterName(RoutingManager::Master master)
{
    switch (master)
    {
    case RoutingManager::Master::ThermostatLine: return "thermostat";
    case RoutingManager::Master::ThermostatSim:  return "thermostatSim";
    }
    return "unknown";
}

const char* SlaveName(RoutingManager::Slave slave)
{
    switch (slave)
    {
    case RoutingManager::Slave::Open:       return "open";
    case RoutingManager::Slave::BoilerLine: return "boiler";
    case RoutingManager::Slave::BoilerSim:  return "boilerSim";
    }
    return "unknown";
}

bool ParseMaster(const char* name, RoutingManager::Master& out)
{
    if (strcmp(name, "thermostat") == 0)    { out = RoutingManager::Master::ThermostatLine; return true; }
    if (strcmp(name, "thermostatSim") == 0) { out = RoutingManager::Master::ThermostatSim;  return true; }
    return false;
}

/// "" is open as well as "open": an empty setting is how open is stored.
bool ParseSlave(const char* name, RoutingManager::Slave& out)
{
    if (name[0] == '\0' || strcmp(name, "open") == 0) { out = RoutingManager::Slave::Open;       return true; }
    if (strcmp(name, "boiler") == 0)                  { out = RoutingManager::Slave::BoilerLine; return true; }
    if (strcmp(name, "boilerSim") == 0)               { out = RoutingManager::Slave::BoilerSim;  return true; }
    return false;
}

constexpr RoutingManager::Master MASTERS[] = {
    RoutingManager::Master::ThermostatLine,
    RoutingManager::Master::ThermostatSim,
};

constexpr CommandArg<const char*> masterArg{
    "master", "The thermostat end: 'thermostat' (the real one, on the module's thermostat "
              "line) or 'thermostatSim'.",
    16 };

constexpr CommandArg<const char*> slaveArg{
    "slave", "The boiler end to connect it to: 'boiler' (the real one, on the module's "
             "boiler line), 'boilerSim', or 'open' to leave the thermostat unconnected.",
    16 };

} // namespace

CommandEntry RoutingManager::getCommand_{
    "route get", &InvokeCommand<&RoutingManager::Cmd_Get>,
    "Report which boiler each thermostat is connected to ('open' for none), and "
    "counters of the frames forwarded between the module's lines since boot."
};

CommandEntry RoutingManager::setCommand_{
    "route set", &InvokeCommand<&RoutingManager::Cmd_Set>,
    "Connect a thermostat to a boiler, or leave it open. A boiler has one thermostat at "
    "most, so whichever thermostat had it before is left open. Saved, so it survives a "
    "reboot. Frames routed to a simulator are dropped until simulators exist.",
    { &masterArg, &slaveArg }
};

RoutingManager::RoutingManager(AppProvider& app)
    : app_(app)
{
}

void RoutingManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    StruxProvider& strux = app_.getStrux();
    strux.getSettingsManager().Register({ &thermostatRoute_, &thermostatSimRoute_ });
    strux.getCommandManager().Register(this, { &getCommand_, &setCommand_ });
    Load();

    queue_ = xQueueCreateStatic(QUEUE_LEN, sizeof(LineFrame), queueStorage_, &queueBuffer_);

    // Above the module link's task: a forwarded frame is late the moment it waits, and
    // the thermostat on the other end gives its boiler 800 ms to answer, all included.
    task_.Init("routing", 6, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
        ESP_LOGE(TAG, "Failed to start the routing task");

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized: thermostat -> %s, thermostatSim -> %s",
             SlaveName(PartnerOf(Master::ThermostatLine)), SlaveName(PartnerOf(Master::ThermostatSim)));
}

void RoutingManager::Load()
{
    char value[16];
    StringSetting* stored[MASTER_COUNT] = { &thermostatRoute_, &thermostatSimRoute_ };
    for (Master m : MASTERS)
    {
        stored[Index(m)]->Get(value, sizeof(value));
        Slave slave;
        if (!ParseSlave(value, slave))
        {
            ESP_LOGW(TAG, "Stored route for %s is '%s', which is no boiler; leaving it open",
                     MasterName(m), value);
            slave = Slave::Open;
        }
        routes_[Index(m)] = slave;
    }

    // Two thermostats on one boiler can only come from editing the settings by hand.
    // The real thermostat keeps it.
    const Slave sim = PartnerOf(Master::ThermostatSim);
    if (sim != Slave::Open && sim == PartnerOf(Master::ThermostatLine))
        routes_[Index(Master::ThermostatSim)] = Slave::Open;
}

void RoutingManager::Store()
{
    StringSetting* stored[MASTER_COUNT] = { &thermostatRoute_, &thermostatSimRoute_ };
    for (Master m : MASTERS)
    {
        const Slave slave = PartnerOf(m);
        stored[Index(m)]->Set(slave == Slave::Open ? "" : SlaveName(slave));
    }
    app_.getStrux().getSettingsManager().Save();
}

void RoutingManager::Connect(Master master, Slave slave)
{
    if (slave != Slave::Open)
        for (Master other : MASTERS)
            if (other != master && PartnerOf(other) == slave)
                routes_[Index(other)] = Slave::Open;
    routes_[Index(master)] = slave;
    Store();
    ESP_LOGI(TAG, "%s -> %s", MasterName(master), SlaveName(slave));
}

// ── Forwarding ───────────────────────────────────────────────

void RoutingManager::OnLineFrame(OpenThermSide side, uint32_t frame)
{
    if (queue_ == nullptr)
        return;
    const LineFrame f{ side, frame };
    if (xQueueSend(queue_, &f, 0) != pdTRUE)
        counters_.queueFull++;
}

void RoutingManager::TaskLoop()
{
    for (;;)
    {
        LineFrame f;
        if (xQueueReceive(queue_, &f, portMAX_DELAY) == pdTRUE)
            Forward(f);
    }
}

void RoutingManager::Forward(const LineFrame& f)
{
    switch (f.side)
    {
    case OpenThermSide::Thermostat:
        // The real thermostat asking: to whichever boiler it is connected to.
        switch (PartnerOf(Master::ThermostatLine))
        {
        case Slave::Open:       break;
        case Slave::BoilerLine: SendOn(OpenThermSide::Boiler, f.frame); break;
        case Slave::BoilerSim:  counters_.toSimulator++; break;
        }
        break;

    case OpenThermSide::Boiler:
        // The real boiler answering: back to whichever thermostat has it.
        if (PartnerOf(Master::ThermostatLine) == Slave::BoilerLine)
            SendOn(OpenThermSide::Thermostat, f.frame);
        else if (PartnerOf(Master::ThermostatSim) == Slave::BoilerLine)
            counters_.toSimulator++;
        break;
    }
}

void RoutingManager::SendOn(OpenThermSide side, uint32_t frame)
{
    switch (app_.getModuleLinkManager().Send(side, frame))
    {
    case ModuleLinkManager::SendResult::Ok:     counters_.forwarded++; break;
    case ModuleLinkManager::SendResult::Busy:   counters_.busy++; break;
    case ModuleLinkManager::SendResult::NoLink:
    case ModuleLinkManager::SendResult::Failed: counters_.failed++; break;
    }
}

// ── Commands ─────────────────────────────────────────────────

void RoutingManager::WriteState(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    resp.field("ok", true);
    for (Master m : MASTERS)
        resp.field(MasterName(m), SlaveName(PartnerOf(m)));

    auto c = resp.object("counters");
    c.field("forwarded", counters_.forwarded.load());
    c.field("queueFull", counters_.queueFull.load());
    c.field("busy", counters_.busy.load());
    c.field("failed", counters_.failed.load());
    c.field("toSimulator", counters_.toSimulator.load());
}

CommandResult RoutingManager::Cmd_Get(CommandContext& ctx)
{
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult RoutingManager::Cmd_Set(CommandContext& ctx)
{
    // Which names exist is meaning, not form: to the framework both are strings. So an
    // unknown one is a reply, not a refusal.
    Master master;
    Slave slave;
    if (!ParseMaster(ctx.arg(masterArg), master) || !ParseSlave(ctx.arg(slaveArg), slave))
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "unknown master or slave");
        return CommandResult::Ok;
    }

    Connect(master, slave);
    WriteState(ctx);
    return CommandResult::Ok;
}
