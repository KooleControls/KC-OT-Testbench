#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "Task.h"
#include "HvacSimulator.h"
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The simulated HVAC units: ten, set up as the gateway's HVAC list is -- a type and an
// address each (HvacSimulator) -- with their commands and the task that keeps their
// time and reads their settings.
//
// It does not touch a bus. Each transport asks it which unit a request is for: the
// RS485 bus (Rs485Manager) for the types on Modbus RTU, and a Modbus TCP server, when
// there is one, for the CoolMasterNets. So a type is added here, in its profile, and
// not in a transport.
// ──────────────────────────────────────────────────────────────
class HvacSimManager
{
    static constexpr const char* TAG = "HvacSims";

public:
    explicit HvacSimManager(AppProvider& app);

    HvacSimManager(const HvacSimManager&) = delete;
    HvacSimManager& operator=(const HvacSimManager&) = delete;
    HvacSimManager(HvacSimManager&&) = delete;
    HvacSimManager& operator=(HvacSimManager&&) = delete;

    void Init();

    /// The unit a request to `unit` over `transport` is for, or nullptr when none here
    /// plays that address. Two at one address is a test setup mistake; the first has it.
    HvacSimulator* Find(HvacTransport transport, uint8_t unit);

    /// A broadcast over `transport`: every unit that is up and answering carries it out.
    /// None replies.
    void Broadcast(HvacTransport transport, const uint8_t* pdu, size_t len, uint32_t nowMs);

private:
    static constexpr int TASK_STACK = 4096;
    static constexpr uint32_t TICK_MS = 100;
    static constexpr uint32_t REFRESH_MS = 1000;

    AppProvider& app_;
    InitState initState_;
    Task task_;
    HvacSimulator units_[HvacSimulator::UNITS];

    void TaskLoop();

    // ── Commands ──
    CommandResult Cmd_Status(CommandContext& ctx);
    CommandResult Cmd_Set(CommandContext& ctx);
    CommandResult Cmd_Filter(CommandContext& ctx);
    CommandResult ApplyToUnit(CommandContext& ctx, uint32_t n, const HvacSimulator::Change& change);
    static CommandEntry statusCommand_;
    static CommandEntry setCommand_;
    static CommandEntry filterCommand_;
};
