#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Task.h"
#include "ModbusFrame.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The Modbus TCP server: a CoolMasterNet as a gateway reaches it over the network.
//
// A CoolMasterNet is one slave for every indoor unit on it. The gateway connects to it
// (CSHWMIP, CSHWMPN; port 502) once per poll, asks for each indoor unit's registers at
// unit id 1 -- a block of ten from the unit's address -- and disconnects again. So this
// listens on `mbtcp.port`, takes a few connections at a time (two gateways on one
// CoolMasterNet is a testplan case), and answers each request from the simulated unit
// whose block holds the registers asked for (HvacSimManager::FindByRegisters). Which
// units those are, and as what, is the HVAC simulators' business: a type with a block
// (HvacProfile::Span) is served here, the rest on RS485.
//
// Registers no simulated unit owns are refused (Illegal Data Address), as a real
// CoolMasterNet does for an indoor unit it does not have (measured 2026-11-12); a unit
// taken off the bus (hvacN.answer) answers the same. A real one answers any unit id, so
// `mbtcp.unit` is 0 (any) by default; set, it refuses other unit ids with Gateway Target
// Failed, to catch a gateway asking the wrong one. `mbtcp.answer` false closes the port
// and every connection: a CoolMasterNet that is switched off, which the gateway sees as
// Disconnected.
//
// One task owns the sockets: it accepts, reads, and answers, a frame at a time.
// ──────────────────────────────────────────────────────────────
class ModbusTcpManager
{
    static constexpr const char* TAG = "ModbusTcp";

public:
    explicit ModbusTcpManager(AppProvider& app);

    ModbusTcpManager(const ModbusTcpManager&) = delete;
    ModbusTcpManager& operator=(const ModbusTcpManager&) = delete;
    ModbusTcpManager(ModbusTcpManager&&) = delete;
    ModbusTcpManager& operator=(ModbusTcpManager&&) = delete;

    void Init();

private:
    static constexpr int TASK_STACK = 4096;
    static constexpr size_t MAX_CLIENTS = 4;
    static constexpr uint32_t REFRESH_MS = 1000;
    static constexpr uint32_t SELECT_MS = 200;

    // A connection that says nothing for this long is dropped: a gateway disconnects
    // after every poll, so a quiet one is gone.
    static constexpr uint32_t IDLE_CLOSE_MS = 5 * 60 * 1000;

    /// The MBAP header: transaction id, protocol id, length, unit id.
    static constexpr size_t MBAP = 7;

    struct Client
    {
        int fd = -1;
        size_t len = 0;        // bytes in buf not yet made into a frame
        uint32_t lastMs = 0;
        uint8_t buf[MBAP + modbus::MAX_PDU];
    };

    struct Counters
    {
        std::atomic<uint32_t> accepted{ 0 }, refused{ 0 }, requests{ 0 }, answered{ 0 }, exceptions{ 0 },
            noUnit{ 0 }, absent{ 0 }, otherUnit{ 0 }, dropped{ 0 };
    };

    AppProvider& app_;
    InitState initState_;
    Task task_;

    // ── The task's ──
    int listener_ = -1;
    Client clients_[MAX_CLIENTS];

    // ── Read by the status command too ──
    std::atomic<uint16_t> port_{ 0 };     // listening on it; 0 when not
    std::atomic<uint8_t> unit_{ 0 };
    std::atomic<bool> answers_{ true };
    std::atomic<uint32_t> clientCount_{ 0 };
    Counters counters_;

    void TaskLoop();
    void Refresh();
    void Listen(uint16_t port);
    void CloseListener();
    void Accept(uint32_t nowMs);
    void Receive(Client& c, uint32_t nowMs);
    void Drop(Client& c);
    bool AnyClient() const;

    /// One frame (an MBAP header and its PDU) from `c`: answered, or nothing to say.
    /// False when the connection is to be dropped.
    bool Answer(Client& c, const uint8_t* adu, size_t len, uint32_t nowMs);

    // ── Settings. Keys are at most 15 characters (NVS). ──
    inline static UInt32Setting portSetting_{ "mbtcp.port", "Modbus TCP Port (0 off)", 502 };
    inline static UInt32Setting unitSetting_{ "mbtcp.unit", "Modbus TCP Unit Id (0 any)", 0 };
    inline static BoolSetting answerSetting_{ "mbtcp.answer", "Modbus TCP Answers (CoolMasterNet on)", true };

    // ── Commands ──
    CommandResult Cmd_Status(CommandContext& ctx);
    static CommandEntry statusCommand_;
};
