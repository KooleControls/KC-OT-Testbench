#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Task.h"
#include "Mutex.h"
#include "ModbusFrame.h"
#include "ModbusSplitter.h"
#include "CoolPlugSimulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

class SerialPort;

/// Who put a frame on the RS485 bus.
enum class BusSender : uint8_t
{
    Bus,         // someone else: the gateway, a real CoolPlug
    Sim,         // one of the simulated CoolPlugs, answering
    Testbench,   // put on the bus by hand (`rs485 send`)
};

/// What a frame on the bus is.
enum class BusFrameKind : uint8_t
{
    Request,
    Reply,       // an exception included
    Noise,       // bytes that made no frame: a collision, a wrong baud rate, a cut-off frame
};

/// One frame on the bus, as recorded. Only the first KEEP bytes are held; `len` is the
/// whole frame's.
struct BusFrame
{
    static constexpr size_t KEEP = 64;

    uint32_t seq;        // this testbench's numbering, from 1, gap-free
    uint32_t ms;         // this testbench's clock when it was recorded
    uint16_t len;
    BusFrameKind kind;
    BusSender from;
    uint8_t bytes[KEEP];
};

// ──────────────────────────────────────────────────────────────
// The RS485 bus: Modbus RTU on the board's RS485 port, a record of everything on it,
// and the simulated CoolPlugs that answer there.
//
// The testbench is a slave on this bus, never its master: a gateway asks, and a
// simulated CoolPlug at the address asked answers -- or a real one does, and this side
// only listens. So it reads every frame, not just its own, and keeps the last 256 in a
// ring for the web UI's RS485 page (`rs485 frames`), the way the OpenTherm frames are
// kept.
//
// Where a frame ends is worked out from its bytes, with the CRC as the judge
// (ModbusSplitter). A request and a reply read differently, so the last request heard
// decides which is tried first: after a request to a unit nobody here plays, the next
// frame is that unit's reply. Bytes that make no frame are recorded as noise.
//
// What this side sends is not heard back: the MAX14830 ignores the line while it
// transmits (echo suppression, set with the port's RS485 mode). So a request sent by
// hand to a simulated unit is dispatched here, not off the bus.
//
// One task owns the port: it reads, answers, sends what `rs485 send` queued once the
// bus is quiet, and keeps the simulators' time. The simulators' own locks are for the
// command handlers.
// ──────────────────────────────────────────────────────────────
class Rs485Manager
{
    static constexpr const char* TAG = "Rs485";

public:
    explicit Rs485Manager(AppProvider& app);

    Rs485Manager(const Rs485Manager&) = delete;
    Rs485Manager& operator=(const Rs485Manager&) = delete;
    Rs485Manager(Rs485Manager&&) = delete;
    Rs485Manager& operator=(Rs485Manager&&) = delete;

    void Init();

    /// Frames recorded after `after`, oldest first, at most `max`. `latest` is the newest
    /// seq there is; `missed` counts frames after `after` that have already left the
    /// ring. A `latest` below `after` means this testbench restarted.
    size_t ReadFrames(uint32_t after, BusFrame* out, size_t max, uint32_t& latest, uint32_t& missed);

private:
    static constexpr size_t RING = 256;
    static constexpr size_t OUTBOX_LEN = 4;
    static constexpr int TASK_STACK = 4096;
    static constexpr uint32_t IDLE_WAIT_MS = 20;
    // A frame's bytes follow each other within a character; a pause this long ends
    // whatever is half-read. Long next to RTU's 3.5 characters, because the bytes come
    // through the MAX14830's service task, not straight off the wire.
    static constexpr uint32_t GAP_MS = 50;
    // How long after a request to someone else its reply is expected first. The
    // gateway's own timeout is at most 5 s; the CRC catches a wrong guess anyway.
    static constexpr uint32_t AWAIT_REPLY_MS = 5000;
    static constexpr uint32_t REFRESH_MS = 1000;

    /// A frame `rs485 send` queued, CRC already on.
    struct Outgoing
    {
        uint16_t len;
        uint8_t bytes[modbus::MAX_ADU];
    };

    AppProvider& app_;
    InitState initState_;
    Task task_;
    SerialPort* port_ = nullptr;

    CoolPlugSimulator units_[CoolPlugSimulator::UNITS];

    // ── The task's alone ──
    ModbusSplitter splitter_;
    uint32_t lastRxMs_ = 0;
    struct Awaiting
    {
        bool active = false;
        uint8_t unit = 0;
        uint32_t sinceMs = 0;
    } awaiting_;
    std::atomic<uint32_t> baud_{ 0 };
    uint32_t nextRefreshMs_ = 0;

    StaticQueue_t outboxBuffer_;
    uint8_t outboxStorage_[OUTBOX_LEN * sizeof(Outgoing)];
    QueueHandle_t outbox_ = nullptr;

    // ── The ring, in PSRAM: 256 frames of 64 bytes is no small thing next to the
    // internal RAM the network needs ──
    Mutex ringMutex_;
    BusFrame* ring_ = nullptr;
    uint32_t nextSeq_ = 1;

    struct Counters
    {
        std::atomic<uint32_t> requests{ 0 }, replies{ 0 }, noiseBytes{ 0 };
        std::atomic<uint32_t> answered{ 0 }, silent{ 0 }, sent{ 0 }, sendFailed{ 0 };
    } counters_;

    void TaskLoop();
    void Refresh(uint32_t nowMs);
    void OnFrame(const uint8_t* adu, size_t len, ModbusSplitter::Kind kind, uint32_t nowMs);
    void OnNoise(const uint8_t* held, size_t heldLen, size_t total);
    void Await(bool active, uint8_t unit = 0, uint32_t nowMs = 0);
    void Dispatch(const uint8_t* adu, size_t len, uint32_t nowMs);
    CoolPlugSimulator* UnitAt(uint8_t address, uint32_t nowMs);
    void Answer(CoolPlugSimulator& unit, const uint8_t* adu, size_t len, uint32_t nowMs);
    bool Transmit(const uint8_t* adu, size_t len, BusSender from, BusFrameKind kind);
    void SendQueued(uint32_t nowMs);
    void Record(BusSender from, BusFrameKind kind, const uint8_t* bytes, size_t len, size_t total);

    // ── Settings (keys at most 15 characters) ──
    inline static UInt32Setting baudSetting_{ "rs485.baud", "RS485 Baud Rate (8N1)", 9600 };

    // ── Commands ──
    CommandResult Cmd_Status(CommandContext& ctx);
    CommandResult Cmd_Frames(CommandContext& ctx);
    CommandResult Cmd_Send(CommandContext& ctx);
    CommandResult Cmd_CoolPlugStatus(CommandContext& ctx);
    CommandResult Cmd_CoolPlugSet(CommandContext& ctx);
    CommandResult Cmd_CoolPlugFilter(CommandContext& ctx);
    CommandResult ApplyToUnit(CommandContext& ctx, uint32_t unit, const CoolPlugSimulator::Change& change);
    static CommandEntry statusCommand_;
    static CommandEntry framesCommand_;
    static CommandEntry sendCommand_;
    static CommandEntry coolPlugStatusCommand_;
    static CommandEntry coolPlugSetCommand_;
    static CommandEntry coolPlugFilterCommand_;
};
