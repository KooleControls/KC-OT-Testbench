#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Task.h"
#include "Endpoint.h"
#include "ThermostatSimulator.h"
#include "BoilerSimulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// Who talks to whom on the bench, and the delivery that makes it so.
//
// A master (a thermostat) is paired with at most one slave (a boiler), and a slave
// with at most one master; either can be left open. The real thermostat and boiler
// are the ones on the PCB1246's two lines; the simulators run here, and this manager
// owns them. Only the masters' choices are stored: a slave's partner is whichever
// master chose it, so the two sides can never disagree.
//
// Delivery: every frame goes from one end to its partner -- a thermostat's request to
// its boiler, the boiler's answer back. Onto a line it is sent through the module;
// into the boiler simulator it is answered, and the answer delivered in turn; into
// the thermostat simulator it is taken as the answer to its request. The thermostat
// simulator asks on its own clock, which this manager's task also keeps.
//
// The module hands frames over on the link task, and sending one blocks on the
// module's reply, which that same task reads. So a frame from a line is queued here
// and delivered from this manager's own task -- the one task that ever touches the
// simulators, so they need no locking beyond their reports.
//
// Every frame is recorded once, with where it came from and where it went
// (ModuleLinkManager::RecordFrame): frames from a line by the link, the simulators'
// frames here.
// ──────────────────────────────────────────────────────────────
class RoutingManager
{
    static constexpr const char* TAG = "Routing";

public:
    enum class Master : uint8_t { ThermostatLine, ThermostatSim };
    enum class Slave : uint8_t { Open, BoilerLine, BoilerSim };

    explicit RoutingManager(AppProvider& app);

    RoutingManager(const RoutingManager&) = delete;
    RoutingManager& operator=(const RoutingManager&) = delete;
    RoutingManager(RoutingManager&&) = delete;
    RoutingManager& operator=(RoutingManager&&) = delete;

    void Init();

    /// A frame one of the module's lines received, from `from` (the real thermostat
    /// or boiler): decide where it goes and queue it. Called from the module link's
    /// task, so it never blocks. Returns where it is going, for the link's record;
    /// None when nowhere, a full queue included (counted).
    Endpoint OnLineFrame(Endpoint from, uint32_t frame);

    /// Connect `master` to `slave` (Open leaves it unconnected). Whichever master had
    /// `slave` before is left open. Saved, so it survives a reboot.
    void Connect(Master master, Slave slave);

    Slave PartnerOf(Master master) const { return routes_[Index(master)].load(); }

private:
    static constexpr size_t QUEUE_LEN = 8;
    static constexpr int TASK_STACK = 4096;
    static constexpr size_t MASTER_COUNT = 2;
    static constexpr uint32_t IDLE_WAIT_MS = 20;

    /// A frame on its way, with the destination decided when it came in.
    struct Delivery
    {
        Endpoint to;
        uint32_t frame;
    };

    AppProvider& app_;
    InitState initState_;
    Task task_;

    std::atomic<Slave> routes_[MASTER_COUNT] = { Slave::BoilerLine, Slave::Open };

    ThermostatSimulator thermostatSim_;
    BoilerSimulator boilerSim_;

    StaticQueue_t queueBuffer_;
    uint8_t queueStorage_[QUEUE_LEN * sizeof(Delivery)];
    QueueHandle_t queue_ = nullptr;

    struct Counters
    {
        std::atomic<uint32_t> forwarded{ 0 };   // sent out on a line
        std::atomic<uint32_t> simulated{ 0 };   // taken by a simulator
        std::atomic<uint32_t> queueFull{ 0 };
        std::atomic<uint32_t> busy{ 0 };
        std::atomic<uint32_t> failed{ 0 };
    } counters_;

    static size_t Index(Master master) { return static_cast<size_t>(master); }

    void TaskLoop();
    Endpoint Destination(Endpoint from) const;
    void Deliver(const Delivery& d);
    void Emit(Endpoint from, uint32_t frame);
    void Load();
    void Store();

    // ── Settings. Keys are at most 15 characters (NVS). The value is the slave's
    // name as the commands spell it, empty for open. ──
    inline static StringSetting thermostatRoute_{ "route.thr", "Thermostat Line Routes To", "boiler" };
    inline static StringSetting thermostatSimRoute_{ "route.thrsim", "Thermostat Sim Routes To", "" };

    // ── Commands ──
    CommandResult Cmd_Get(CommandContext& ctx);
    CommandResult Cmd_Set(CommandContext& ctx);
    CommandResult Cmd_SimStatus(CommandContext& ctx);
    void WriteState(CommandContext& ctx);
    static CommandEntry getCommand_;
    static CommandEntry setCommand_;
    static CommandEntry simStatusCommand_;
};
