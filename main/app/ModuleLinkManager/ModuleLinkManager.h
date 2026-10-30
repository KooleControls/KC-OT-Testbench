#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "Task.h"
#include "Mutex.h"
#include "Semaphore.h"
#include "ChannelProtocol.h"
#include "ConnectionState.h"
#include "SerialFraming.h"
#include "Endpoint.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

/// Which line of the PCB1246 a frame is on. The module's own names for them.
enum class OpenThermSide : uint8_t
{
    Thermostat,   // faces the room thermostat; the module plays the boiler on it
    Boiler,       // faces the boiler; the module plays the thermostat on it
};

/// The real device on a line.
inline Endpoint DeviceOn(OpenThermSide side)
{
    return side == OpenThermSide::Thermostat ? Endpoint::Thermostat : Endpoint::Boiler;
}

/// One OpenTherm frame on the bench: who sent it and who it went to. One record per
/// frame, however many hops it took -- a forwarded frame is not recorded again.
struct ModuleFrame
{
    uint32_t seq;           // this testbench's numbering, from 1, gap-free
    uint32_t frame;         // the 32 bits, parity included
    uint32_t testbenchMs;   // this testbench's clock when it was recorded
    uint32_t moduleMs;      // the module's clock when a line received it; 0 otherwise
    Endpoint from;
    Endpoint to;            // None: it went nowhere
};

// ──────────────────────────────────────────────────────────────
// The gateway's end of the link to the PCB1246 modem: the Strux channel protocol over
// the module's UART, the same framing (SerialFraming.h) and handshake as the module's
// LinkManager.
//
// The two ends are not symmetric, and that is the whole shape of this class. The
// module SERVES -- it answers requests and pushes a stream -- and this side only
// ASKS: it opens a channel per request and reads the reply, and it reads the stream
// the module opens. Nothing on the module ever opens a command channel here, so
// there is no CommandManager behind this link and an unexpected OPEN is refused.
//
// One task owns the read. It runs the handshake (protocol::OnHandshake, the rules the
// module's Connection uses), collects the stream into a ring of recent frames, and
// completes whichever request is waiting. A request is made from the caller's task
// and blocks on a semaphore until its reply is in -- the module runs one request at a
// time, so this side sends one at a time too.
//
// The UART is shared with ModuleFirmwareManager, which holds its lock for a whole
// ISP session. Each session restarts the module, so this side starts over after one.
//
// What a frame MEANS is not this class's business either: it is a pipe and a record.
// The ring is what the web UI's OpenTherm page reads (`module frames`).
// ──────────────────────────────────────────────────────────────
class ModuleLinkManager
{
    static constexpr const char* TAG = "ModuleLink";

public:
    enum class SendResult : uint8_t
    {
        Ok,
        Busy,      // the line was still sending the previous frame
        NoLink,    // no settled link to the module
        Failed,    // asked, and no usable answer came back
    };

    explicit ModuleLinkManager(AppProvider& app);

    ModuleLinkManager(const ModuleLinkManager&) = delete;
    ModuleLinkManager& operator=(const ModuleLinkManager&) = delete;
    ModuleLinkManager(ModuleLinkManager&&) = delete;
    ModuleLinkManager& operator=(ModuleLinkManager&&) = delete;

    void Init();

    bool IsReady() const { return phase_.load() == ConnectionState::Phase::Ready; }

    /// Put one frame on a line. Blocks until the module has started it or refused.
    /// Recorded as a frame of its own.
    SendResult Send(OpenThermSide side, uint32_t frame);

    /// Send on a frame a line received. Not recorded again: the received frame's
    /// record already says where it went.
    SendResult Forward(OpenThermSide side, uint32_t frame);

    /// Run any command the module has, as its console line (`opentherm status`), and
    /// copy the reply's text into `reply`. False when no reply came; `refused` when the
    /// module refused the request, with its reason as the reply.
    bool Exec(const char* line, char* reply, size_t cap, bool& refused);

    /// Frames recorded after `after`, oldest first, at most `max`. `latest` is the
    /// newest seq there is; `missed` counts frames after `after` that have already
    /// left the ring. A `latest` below `after` means this gateway restarted.
    size_t ReadFrames(uint32_t after, ModuleFrame* out, size_t max,
                      uint32_t& latest, uint32_t& missed);

    static const char* SideName(OpenThermSide side);

    /// Add a frame to the record the web UI reads. `moduleMs` only for one a line
    /// received, which the link records itself.
    void RecordFrame(Endpoint from, Endpoint to, uint32_t frame, uint32_t moduleMs = 0);

private:
    friend class ModuleTransport;

    static constexpr size_t INBOUND_WINDOW = 512;
    static constexpr size_t FRAME_MAX = channel::HEADER_LEN + INBOUND_WINDOW;
    static constexpr size_t RING = 256;
    static constexpr size_t STREAM_LINE = 96;
    static constexpr uint32_t HANDSHAKE_RETRY_MS = 1000;
    static constexpr int TASK_STACK = 4096;

    AppProvider& app_;
    InitState initState_;
    Task task_;

    // ── Link state: the task's, and requests' under stateMutex_ ──
    Mutex stateMutex_;
    ConnectionState state_;
    std::atomic<ConnectionState::Phase> phase_{ ConnectionState::Phase::Handshake };
    std::atomic<bool> paused_{ false };    // the module is in an ISP session
    uint32_t seenIspSessions_ = 0;
    uint32_t lastHelloMs_ = 0;
    int32_t stream_ = -1;                  // the module's frame stream, -1 when not open
    char streamLine_[STREAM_LINE] = {};
    size_t streamLen_ = 0;

    // ── The one request in flight ──
    Mutex requestMutex_;                   // one request at a time
    Semaphore requestDone_;
    struct Pending
    {
        bool     active = false;
        uint16_t id = 0;
        char*    buf = nullptr;
        size_t   cap = 0;
        size_t   len = 0;
        bool     done = false;
        bool     refused = false;
        bool     failed = false;           // the link went down underneath it
    } pending_;

    // ── Framing: the task decodes, any task encodes under the UART lock ──
    uint8_t decoded_[FRAME_MAX + serial_framing::CRC_LEN];
    SerialFrameDecoder decoder_{ decoded_, sizeof(decoded_) };
    uint8_t encoded_[serial_framing::EncodedMax(channel::HEADER_LEN + 128)];

    struct Counters
    {
        uint32_t framesIn = 0, framesOut = 0, corrupt = 0, tooLong = 0, sendFailed = 0;
        uint32_t requests = 0, requestTimeouts = 0, badRecords = 0;
    } counters_;

    // ── The ring of recent frames ──
    Mutex ringMutex_;
    ModuleFrame ring_[RING] = {};
    uint32_t nextSeq_ = 1;

    void TaskLoop();
    void Restart();
    void SendHello();
    void HandleFrame(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len);
    void StreamBytes(const uint8_t* p, size_t n);
    void StreamRecord(const char* line);
    void FailPending();
    SendResult SendOnLine(OpenThermSide side, uint32_t frame);

    /// Frame and send, under the UART lock. False when the lock is not to be had (an
    /// ISP session) or the UART would not take it.
    bool WriteFrame(const uint8_t* frame, size_t len);

    bool Request(const char* line, char* reply, size_t cap, uint32_t timeoutMs,
                 size_t& len, bool& refused);

    // ── Commands ──
    CommandResult Cmd_Link(CommandContext& ctx);
    CommandResult Cmd_Send(CommandContext& ctx);
    CommandResult Cmd_Exec(CommandContext& ctx);
    CommandResult Cmd_Frames(CommandContext& ctx);
    static CommandEntry linkCommand_;
    static CommandEntry sendCommand_;
    static CommandEntry execCommand_;
    static CommandEntry framesCommand_;
};
