#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Task.h"
#include "ModuleLinkManager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// Who talks to whom on the bench, and the forwarding that makes it so.
//
// A master (a thermostat) is paired with at most one slave (a boiler), and a slave
// with at most one master; either can be left open. The real thermostat and boiler
// are the ones on the PCB1246's two lines; the simulators are to run on this
// testbench. Only the masters' choices are stored: a slave's partner is whichever
// master chose it, so the two sides can never disagree. The web UI's Connections
// page keeps the same rule (frontend/src/lib/routing.ts).
//
// Forwarding: a frame one line receives goes out on the line its device is paired
// with -- a thermostat's request to its boiler, the boiler's answer back. The module
// hands frames over on the link task, and sending one blocks on the module's reply,
// which that same task reads. So a frame is queued here and sent from this manager's
// own task; the queue never blocks the link.
//
// The simulators do not exist yet. A frame routed to one is counted and dropped.
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

    /// A frame one of the module's lines received. Called from the module link's
    /// task, so it only queues; a full queue drops the frame and counts it.
    void OnLineFrame(OpenThermSide side, uint32_t frame);

    /// Connect `master` to `slave` (Open leaves it unconnected). Whichever master had
    /// `slave` before is left open. Saved, so it survives a reboot.
    void Connect(Master master, Slave slave);

    Slave PartnerOf(Master master) const { return routes_[Index(master)].load(); }

private:
    static constexpr size_t QUEUE_LEN = 8;
    static constexpr int TASK_STACK = 4096;
    static constexpr size_t MASTER_COUNT = 2;

    struct LineFrame
    {
        OpenThermSide side;
        uint32_t frame;
    };

    AppProvider& app_;
    InitState initState_;
    Task task_;

    std::atomic<Slave> routes_[MASTER_COUNT] = { Slave::BoilerLine, Slave::Open };

    StaticQueue_t queueBuffer_;
    uint8_t queueStorage_[QUEUE_LEN * sizeof(LineFrame)];
    QueueHandle_t queue_ = nullptr;

    struct Counters
    {
        std::atomic<uint32_t> forwarded{ 0 };
        std::atomic<uint32_t> queueFull{ 0 };
        std::atomic<uint32_t> busy{ 0 };
        std::atomic<uint32_t> failed{ 0 };
        std::atomic<uint32_t> toSimulator{ 0 };
    } counters_;

    static size_t Index(Master master) { return static_cast<size_t>(master); }

    void TaskLoop();
    void Forward(const LineFrame& f);
    void SendOn(OpenThermSide side, uint32_t frame);
    void Load();
    void Store();

    // ── Settings. Keys are at most 15 characters (NVS). The value is the slave's
    // name as the commands spell it, empty for open. ──
    inline static StringSetting thermostatRoute_{ "route.thr", "Thermostat Line Routes To", "boiler" };
    inline static StringSetting thermostatSimRoute_{ "route.thrsim", "Thermostat Sim Routes To", "" };

    // ── Commands ──
    CommandResult Cmd_Get(CommandContext& ctx);
    CommandResult Cmd_Set(CommandContext& ctx);
    void WriteState(CommandContext& ctx);
    static CommandEntry getCommand_;
    static CommandEntry setCommand_;
};
