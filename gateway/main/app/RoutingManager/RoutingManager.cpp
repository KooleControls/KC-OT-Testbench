#include "RoutingManager.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "ModuleLinkManager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>

namespace {

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

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

Endpoint EndpointOf(RoutingManager::Master master)
{
    switch (master)
    {
    case RoutingManager::Master::ThermostatLine: return Endpoint::Thermostat;
    case RoutingManager::Master::ThermostatSim:  return Endpoint::ThermostatSim;
    }
    return Endpoint::None;
}

Endpoint EndpointOf(RoutingManager::Slave slave)
{
    switch (slave)
    {
    case RoutingManager::Slave::Open:       return Endpoint::None;
    case RoutingManager::Slave::BoilerLine: return Endpoint::Boiler;
    case RoutingManager::Slave::BoilerSim:  return Endpoint::BoilerSim;
    }
    return Endpoint::None;
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
    "counters of the frames delivered since boot."
};

CommandEntry RoutingManager::setCommand_{
    "route set", &InvokeCommand<&RoutingManager::Cmd_Set>,
    "Connect a thermostat to a boiler, or leave it open. A boiler has one thermostat at "
    "most, so whichever thermostat had it before is left open. Saved, so it survives a "
    "reboot.",
    { &masterArg, &slaveArg }
};

CommandEntry RoutingManager::simStatusCommand_{
    "sim status", &InvokeCommand<&RoutingManager::Cmd_SimStatus>,
    "Report the two simulators: whether each is connected (running), to whom, and what "
    "it has heard from the other end. Their behaviour is in the settings tsim.* and "
    "bsim.*, which apply with the next frame."
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
    thermostatSim_.RegisterSettings(strux.getSettingsManager());
    boilerSim_.RegisterSettings(strux.getSettingsManager());
    strux.getCommandManager().Register(this, { &getCommand_, &setCommand_, &simStatusCommand_ });
    Load();

    queue_ = xQueueCreateStatic(QUEUE_LEN, sizeof(Delivery), queueStorage_, &queueBuffer_);

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

// ── Delivery ─────────────────────────────────────────────────

Endpoint RoutingManager::Destination(Endpoint from) const
{
    switch (from)
    {
    case Endpoint::Thermostat:    return EndpointOf(PartnerOf(Master::ThermostatLine));
    case Endpoint::ThermostatSim: return EndpointOf(PartnerOf(Master::ThermostatSim));
    case Endpoint::Boiler:
    case Endpoint::BoilerSim: {
        // A boiler answers whichever thermostat chose it.
        const Slave self = from == Endpoint::Boiler ? Slave::BoilerLine : Slave::BoilerSim;
        for (Master m : MASTERS)
            if (PartnerOf(m) == self)
                return EndpointOf(m);
        return Endpoint::None;
    }
    case Endpoint::None:
    case Endpoint::Testbench:
        break;
    }
    return Endpoint::None;
}

Endpoint RoutingManager::OnLineFrame(Endpoint from, uint32_t frame)
{
    if (queue_ == nullptr)
        return Endpoint::None;

    const Endpoint to = Destination(from);
    if (to == Endpoint::None)
        return Endpoint::None;

    const Delivery d{ to, frame };
    if (xQueueSend(queue_, &d, 0) != pdTRUE)
    {
        counters_.queueFull++;
        return Endpoint::None;
    }
    return to;
}

void RoutingManager::TaskLoop()
{
    for (;;)
    {
        // A short wait, so the thermostat simulator keeps its interval to within it.
        Delivery d;
        if (xQueueReceive(queue_, &d, pdMS_TO_TICKS(IDLE_WAIT_MS)) == pdTRUE)
            Deliver(d);

        const uint32_t now = NowMs();
        if (PartnerOf(Master::ThermostatSim) != Slave::Open && thermostatSim_.Due(now))
            Emit(Endpoint::ThermostatSim, thermostatSim_.NextRequest(now));
    }
}

void RoutingManager::Deliver(const Delivery& d)
{
    switch (d.to)
    {
    case Endpoint::Thermostat:
    case Endpoint::Boiler: {
        const OpenThermSide line = d.to == Endpoint::Thermostat ? OpenThermSide::Thermostat : OpenThermSide::Boiler;
        switch (app_.getModuleLinkManager().Forward(line, d.frame))
        {
        case ModuleLinkManager::SendResult::Ok:     counters_.forwarded++; break;
        case ModuleLinkManager::SendResult::Busy:   counters_.busy++; break;
        case ModuleLinkManager::SendResult::NoLink:
        case ModuleLinkManager::SendResult::Failed: counters_.failed++; break;
        }
        break;
    }

    case Endpoint::BoilerSim:
        counters_.simulated++;
        Emit(Endpoint::BoilerSim, boilerSim_.Answer(d.frame, NowMs()));
        break;

    case Endpoint::ThermostatSim:
        counters_.simulated++;
        thermostatSim_.OnResponse(d.frame, NowMs());
        break;

    case Endpoint::None:
    case Endpoint::Testbench:
        break;
    }
}

void RoutingManager::Emit(Endpoint from, uint32_t frame)
{
    const Endpoint to = Destination(from);
    app_.getModuleLinkManager().RecordFrame(from, to, frame);
    if (to != Endpoint::None)
        Deliver(Delivery{ to, frame });
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
    c.field("simulated", counters_.simulated.load());
    c.field("queueFull", counters_.queueFull.load());
    c.field("busy", counters_.busy.load());
    c.field("failed", counters_.failed.load());
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

CommandResult RoutingManager::Cmd_SimStatus(CommandContext& ctx)
{
    const uint32_t now = NowMs();
    auto resp = ctx.reply.object();
    {
        const Endpoint partner = Destination(Endpoint::ThermostatSim);
        auto o = resp.object("thermostatSim");
        o.field("running", partner != Endpoint::None);
        if (partner != Endpoint::None)
            o.field("partner", EndpointName(partner));
        thermostatSim_.Report(o, now);
    }
    {
        const Endpoint partner = Destination(Endpoint::BoilerSim);
        auto o = resp.object("boilerSim");
        o.field("running", partner != Endpoint::None);
        if (partner != Endpoint::None)
            o.field("partner", EndpointName(partner));
        boilerSim_.Report(o, now);
    }
    return CommandResult::Ok;
}
