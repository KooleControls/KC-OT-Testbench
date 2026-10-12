#include "ModuleLinkManager.h"
#include "ModuleFirmwareManager.h"
#include "RoutingManager.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "CommandManager.h"
#include "Connection.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

constexpr const char* STREAM_ENVELOPE = "{\"type\":\"opentherm stream\"}";

constexpr uint32_t SEND_TIMEOUT_MS = 500;
constexpr uint32_t EXEC_TIMEOUT_MS = 3000;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool ParseSide(const char* name, OpenThermSide& out)
{
    if (strcmp(name, "thermostat") == 0) { out = OpenThermSide::Thermostat; return true; }
    if (strcmp(name, "boiler") == 0)     { out = OpenThermSide::Boiler;     return true; }
    return false;
}

/// The unsigned number after `"key":` in a flat JSON record.
bool FindU32(const char* line, const char* key, uint32_t& out)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(line, pattern);
    if (p == nullptr) return false;
    p += strlen(pattern);
    char* end = nullptr;
    const unsigned long v = strtoul(p, &end, 10);
    if (end == p) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

constexpr CommandArg<const char*> sideArg{
    "side", "Which line: 'thermostat' (the module plays the boiler towards the room "
            "thermostat) or 'boiler' (the module plays the thermostat towards the boiler).",
    10 };

constexpr CommandArg<uint32_t> frameArg{
    "frame", "The 32 frame bits exactly as they go on the line, parity bit included "
             "- nothing is added or checked. Decimal or 0x-prefixed hex." };

constexpr CommandArg<const char*> lineArg{
    "line", "The module's command as its console line, e.g. 'opentherm status', "
            "'link status' or 'help'.",
    120 };

constexpr CommandArg<uint32_t> afterArg{
    "after", "Only frames with a seq above this. 0 or absent: everything still held.",
    Presence::Optional };

constexpr CommandArg<uint32_t> maxArg{
    "max", "At most this many frames, oldest first (default and limit 64). Ask again "
           "from the last seq for the rest.",
    Presence::Optional };

} // namespace

// ── The transport: SendRaw is the only half anything uses ───

class ModuleTransport final : public Transport
{
public:
    explicit ModuleTransport(ModuleLinkManager& link) : link_(link) {}

    bool SendRaw(const uint8_t* frame, size_t len) override { return link_.WriteFrame(frame, len); }
    size_t InboundLimit() const override { return ModuleLinkManager::INBOUND_WINDOW; }

    // Never called: nothing on this side runs a Channel, which is the only reader of
    // this. Replies and the stream are collected by the link task.
    int RecvChunk(uint8_t*, size_t, uint16_t*, uint8_t*) override { return RECV_EOF; }

private:
    ModuleLinkManager& link_;
};

// ── Commands ─────────────────────────────────────────────────

CommandEntry ModuleLinkManager::linkCommand_{
    "module link", &InvokeCommand<&ModuleLinkManager::Cmd_Link>,
    "Report the serial link to the PCB1246 modem: handshake phase, whether its frame "
    "stream is open, and frame and request counters since boot."
};

CommandEntry ModuleLinkManager::sendCommand_{
    "module send", &InvokeCommand<&ModuleLinkManager::Cmd_Send>,
    "Put one OpenTherm frame on one of the module's lines. Answers once the module has "
    "started it (34 ms on the line); ok=false with 'busy' while the previous one is "
    "still going out.",
    { &sideArg, &frameArg }
};

CommandEntry ModuleLinkManager::execCommand_{
    "module exec", &InvokeCommand<&ModuleLinkManager::Cmd_Exec>,
    "Run one of the PCB1246's own commands and return its reply as text. For "
    "diagnostics: 'opentherm status', 'link status', 'help', 'system info'.",
    { &lineArg }
};

CommandEntry ModuleLinkManager::framesCommand_{
    "module frames", &InvokeCommand<&ModuleLinkManager::Cmd_Frames>,
    "The OpenTherm frames that crossed the module, received and sent, from a ring of "
    "the last 256. Poll with after=<last seq seen> to follow them.",
    { &afterArg, &maxArg }
};

ModuleLinkManager::ModuleLinkManager(AppProvider& app)
    : app_(app)
{
}

const char* ModuleLinkManager::SideName(OpenThermSide side)
{
    switch (side)
    {
    case OpenThermSide::Thermostat: return "thermostat";
    case OpenThermSide::Boiler:     return "boiler";
    }
    return "unknown";
}

void ModuleLinkManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    app_.getStrux().getCommandManager().Register(
        this, { &linkCommand_, &sendCommand_, &execCommand_, &framesCommand_ });

    task_.Init("module-link", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
        ESP_LOGE(TAG, "Failed to start the module link task");

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// ── The task: owns the read ──────────────────────────────────

void ModuleLinkManager::TaskLoop()
{
    ModuleFirmwareManager& firmware = app_.getModuleFirmwareManager();
    SerialPort& serial = app_.getBoard().GetOpenThermModule().Serial();
    seenIspSessions_ = firmware.IspSessions();
    lastHelloMs_ = NowMs() - HANDSHAKE_RETRY_MS;   // the first pass says hello

    for (;;)
    {
        // An ISP session holds the UART lock for its whole length, during which the
        // module is its boot ROM and nothing here may touch the line. Taken only to
        // ask, not held: holding it across the read below would starve every request
        // waiting to send, because this task would take it straight back.
        if (firmware.IsBusy() || !firmware.UartMutex().Take(pdMS_TO_TICKS(100)))
        {
            paused_ = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const uint32_t sessions = firmware.IspSessions();
        firmware.UartMutex().Give();

        if (sessions != seenIspSessions_)
        {
            // The module was reset through its bootloader: whatever was settled with
            // the firmware that ran before is gone, ids and stream included.
            seenIspSessions_ = sessions;
            Restart();
        }
        paused_ = false;

        if (phase_.load() == ConnectionState::Phase::Handshake
            && NowMs() - lastHelloMs_ >= HANDSHAKE_RETRY_MS)
            SendHello();   // same rule as the module: only while waiting

        // Outside the lock. This task is the only reader, and a session that starts
        // during this read spends its first 150 ms holding the module in reset, so
        // the boot ROM has nothing to say before this read is over.
        uint8_t buf[64];
        const size_t n = serial.read(buf, sizeof(buf), pdMS_TO_TICKS(10));
        for (size_t i = 0; i < n; ++i)
        {
            switch (decoder_.Push(buf[i]))
            {
            case SerialFrameDecoder::Result::None:
                break;
            case SerialFrameDecoder::Result::Corrupt:
                counters_.corrupt++;
                break;
            case SerialFrameDecoder::Result::TooLong:
                counters_.tooLong++;
                break;
            case SerialFrameDecoder::Result::Frame:
                if (decoder_.Length() < channel::HEADER_LEN)
                {
                    counters_.corrupt++;
                    break;
                }
                counters_.framesIn++;
                HandleFrame(channel::readU16(decoder_.Data()), decoder_.Data()[2],
                            decoder_.Data() + channel::HEADER_LEN,
                            decoder_.Length() - channel::HEADER_LEN);
                break;
            }
        }
    }
}

void ModuleLinkManager::SendHello()
{
    LOCK(stateMutex_);
    ModuleTransport link(*this);
    protocol::SendHandshake(state_, link);
    phase_ = state_.phase;
    lastHelloMs_ = NowMs();
}

void ModuleLinkManager::Restart()
{
    {
        LOCK(stateMutex_);
        state_.Reset();
        phase_ = state_.phase;
        stream_ = -1;
        streamLen_ = 0;
        FailPending();
    }
    decoder_.Reset();
    SendHello();
}

void ModuleLinkManager::FailPending()
{
    if (!pending_.active || pending_.done) return;
    pending_.failed = true;
    pending_.done = true;
    requestDone_.Give();
}

void ModuleLinkManager::HandleFrame(uint16_t id, uint8_t flags, const uint8_t* payload, size_t len)
{
    LOCK(stateMutex_);
    ModuleTransport link(*this);

    if (flags & channel::FLAG_CONTROL)
    {
        // The module's hello. In Ready it means the module restarted, which takes
        // its stream and any request it was serving with it.
        stream_ = -1;
        streamLen_ = 0;
        FailPending();
        protocol::OnHandshake(state_, link, payload, len);
        phase_ = state_.phase;
        return;
    }

    if (state_.phase != ConnectionState::Phase::Ready)
        return;

    // The reply to the request in flight.
    if (pending_.active && !pending_.done && id == pending_.id)
    {
        const size_t room = pending_.cap - 1 - pending_.len;
        const size_t n = std::min(room, len);
        memcpy(pending_.buf + pending_.len, payload, n);
        pending_.len += n;
        pending_.buf[pending_.len] = '\0';

        if (flags & (channel::FLAG_FINAL | channel::FLAG_RESET))
        {
            pending_.refused = (flags & channel::FLAG_RESET) != 0;
            pending_.done = true;
            requestDone_.Give();
        }
        return;
    }

    if (flags & channel::FLAG_RESET)
    {
        if (static_cast<int32_t>(id) == stream_)
        {
            stream_ = -1;
            streamLen_ = 0;
        }
        return;
    }

    if (flags & channel::FLAG_OPEN)
    {
        // The only channel the module opens is its frame stream. Its first frame
        // carries the envelope line; anything after that line is already data.
        const size_t envLen = strlen(STREAM_ENVELOPE);
        if (len >= envLen && memcmp(payload, STREAM_ENVELOPE, envLen) == 0)
        {
            stream_ = id;
            streamLen_ = 0;
            const uint8_t* nl = static_cast<const uint8_t*>(memchr(payload, '\n', len));
            if (nl != nullptr)
                StreamBytes(nl + 1, len - static_cast<size_t>(nl + 1 - payload));
            ESP_LOGI(TAG, "module frame stream open on channel %u", static_cast<unsigned>(id));
            return;
        }
        protocol::SendReset(link, id, "not served here");
        return;
    }

    if (static_cast<int32_t>(id) == stream_)
        StreamBytes(payload, len);
    // Anything else is residue of a channel that has finished.
}

void ModuleLinkManager::StreamBytes(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        const char c = static_cast<char>(p[i]);
        if (c == '\n')
        {
            streamLine_[streamLen_] = '\0';
            if (streamLen_ > 0)
                StreamRecord(streamLine_);
            streamLen_ = 0;
        }
        else if (streamLen_ + 1 < sizeof(streamLine_))
            streamLine_[streamLen_++] = c;
    }
}

void ModuleLinkManager::StreamRecord(const char* line)
{
    // {"side":"boiler","frame":3221291008,"ms":123456} -- the module's ModemManager.
    OpenThermSide side;
    uint32_t frame = 0, ms = 0;
    const char* s = strstr(line, "\"side\":\"");
    char name[12] = {};
    if (s != nullptr)
        sscanf(s + 8, "%11[a-z]", name);
    if (!ParseSide(name, side) || !FindU32(line, "frame", frame) || !FindU32(line, "ms", ms))
    {
        counters_.badRecords++;
        return;
    }
    // Routed first, so the record says where it went.
    const FrameRoute to = app_.getRoutingManager().OnLineFrame(side, frame);
    Record(side, frame, ms, false, to);
}

bool ModuleLinkManager::WriteFrame(const uint8_t* frame, size_t len)
{
    ModuleFirmwareManager& firmware = app_.getModuleFirmwareManager();
    if (serial_framing::EncodedMax(len) > sizeof(encoded_)
        || !firmware.UartMutex().Take(pdMS_TO_TICKS(200)))
    {
        counters_.sendFailed++;
        return false;
    }

    bool ok = false;
    if (!firmware.IsBusy())
    {
        const size_t n = serial_framing::Encode(frame, len, encoded_);
        ok = app_.getBoard().GetOpenThermModule().Serial()
                 .write(encoded_, n, pdMS_TO_TICKS(500)) == n;
    }
    firmware.UartMutex().Give();

    if (ok) counters_.framesOut++;
    else counters_.sendFailed++;
    return ok;
}

// ── Requests ─────────────────────────────────────────────────
//
// Lock order, everywhere: the state lock before the UART lock (handling a frame
// sends under the state lock), and never the other way round. A request releases the
// state lock before it waits, because the reply it waits for is handled under it.

bool ModuleLinkManager::Request(const char* line, char* reply, size_t cap, uint32_t timeoutMs,
                                size_t& len, bool& refused)
{
    LOCK(requestMutex_);
    if (cap < 2) return false;

    uint16_t id = 0;
    {
        LOCK(stateMutex_);
        if (state_.phase != ConnectionState::Phase::Ready || !state_.AllocateId(id))
            return false;
        pending_ = Pending{};
        pending_.active = true;
        pending_.id = id;
        pending_.buf = reply;
        pending_.cap = cap;
        reply[0] = '\0';
        counters_.requests++;
    }
    requestDone_.Take(0);   // nothing stale from a request that timed out

    ModuleTransport link(*this);
    const bool sent = protocol::SendFrame(link, id, channel::FLAG_OPEN | channel::FLAG_FINAL,
                                          reinterpret_cast<const uint8_t*>(line), strlen(line));
    const bool answered = sent && requestDone_.Take(pdMS_TO_TICKS(timeoutMs));

    bool ok;
    {
        LOCK(stateMutex_);
        ok = answered && !pending_.failed;
        len = pending_.len;
        refused = pending_.refused;
        pending_.active = false;
    }

    // Outside the state lock: nothing in the waiting needs it held, and a request
    // that times out should not hold up the frame handling while it closes.
    if (sent && !answered)
    {
        // Close it, so a reply that turns up later is residue rather than the answer
        // to whatever is asked next.
        counters_.requestTimeouts++;
        protocol::SendReset(link, id, "timeout");
    }
    return ok;
}

ModuleLinkManager::SendResult ModuleLinkManager::Send(OpenThermSide side, uint32_t frame)
{
    const SendResult r = SendOnLine(side, frame);
    if (r == SendResult::Ok)
        Record(side, frame, 0, true, FrameRoute::None);
    return r;
}

ModuleLinkManager::SendResult ModuleLinkManager::Forward(OpenThermSide side, uint32_t frame)
{
    return SendOnLine(side, frame);
}

ModuleLinkManager::SendResult ModuleLinkManager::SendOnLine(OpenThermSide side, uint32_t frame)
{
    if (!IsReady())
        return SendResult::NoLink;

    char line[96];
    snprintf(line, sizeof(line), "{\"type\":\"opentherm send\",\"side\":\"%s\",\"frame\":%lu}\n",
             SideName(side), static_cast<unsigned long>(frame));

    char reply[96];
    size_t len = 0;
    bool refused = false;
    if (!Request(line, reply, sizeof(reply), SEND_TIMEOUT_MS, len, refused) || refused)
        return SendResult::Failed;

    if (strstr(reply, "\"ok\":true") != nullptr)
        return SendResult::Ok;
    return strstr(reply, "busy") != nullptr ? SendResult::Busy : SendResult::Failed;
}

bool ModuleLinkManager::Exec(const char* line, char* reply, size_t cap, bool& refused)
{
    char request[128];
    snprintf(request, sizeof(request), "%s\n", line);
    size_t len = 0;
    return Request(request, reply, cap, EXEC_TIMEOUT_MS, len, refused);
}

// ── The ring ─────────────────────────────────────────────────

void ModuleLinkManager::Record(OpenThermSide side, uint32_t frame, uint32_t moduleMs, bool sent, FrameRoute to)
{
    LOCK(ringMutex_);
    ModuleFrame& f = ring_[nextSeq_ % RING];
    f.seq = nextSeq_++;
    f.frame = frame;
    f.gatewayMs = NowMs();
    f.moduleMs = moduleMs;
    f.side = side;
    f.sent = sent;
    f.to = to;
}

size_t ModuleLinkManager::ReadFrames(uint32_t after, ModuleFrame* out, size_t max,
                                     uint32_t& latest, uint32_t& missed)
{
    LOCK(ringMutex_);
    latest = nextSeq_ - 1;
    const uint32_t oldest = nextSeq_ > RING ? nextSeq_ - static_cast<uint32_t>(RING) : 1;

    // A cursor from before a restart is ahead of everything: start from what is held.
    uint32_t from = (after >= nextSeq_) ? oldest : after + 1;
    missed = 0;
    if (from < oldest)
    {
        missed = oldest - from;
        from = oldest;
    }

    size_t n = 0;
    for (uint32_t seq = from; seq < nextSeq_ && n < max; ++seq)
        out[n++] = ring_[seq % RING];
    return n;
}

// ── Command handlers ─────────────────────────────────────────

CommandResult ModuleLinkManager::Cmd_Link(CommandContext& ctx)
{
    auto resp = ctx.reply.object();

    const char* phase = "failed";
    if (paused_.load()) phase = "paused";
    else switch (phase_.load())
    {
    case ConnectionState::Phase::Handshake: phase = "handshake"; break;
    case ConnectionState::Phase::Ready:     phase = "ready"; break;
    case ConnectionState::Phase::Failed:    phase = "failed"; break;
    }
    resp.field("phase", phase);

    bool streamOpen;
    {
        LOCK(stateMutex_);
        streamOpen = stream_ >= 0;
    }
    resp.field("stream", streamOpen);

    uint32_t latest;
    {
        LOCK(ringMutex_);
        latest = nextSeq_ - 1;
    }
    resp.field("framesRecorded", latest);

    auto c = resp.object("counters");
    c.field("in", counters_.framesIn);
    c.field("out", counters_.framesOut);
    c.field("corrupt", counters_.corrupt);
    c.field("tooLong", counters_.tooLong);
    c.field("sendFailed", counters_.sendFailed);
    c.field("requests", counters_.requests);
    c.field("requestTimeouts", counters_.requestTimeouts);
    c.field("badRecords", counters_.badRecords);
    return CommandResult::Ok;
}

CommandResult ModuleLinkManager::Cmd_Send(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    OpenThermSide side;
    if (!ParseSide(ctx.arg(sideArg), side))
    {
        resp.field("ok", false);
        resp.field("error", "unknown side");
        return CommandResult::Ok;
    }

    const SendResult r = Send(side, ctx.arg(frameArg));
    resp.field("ok", r == SendResult::Ok);
    switch (r)
    {
    case SendResult::Ok:     break;
    case SendResult::Busy:   resp.field("error", "busy"); break;
    case SendResult::NoLink: resp.field("error", "no link to the module"); break;
    case SendResult::Failed: resp.field("error", "the module did not answer"); break;
    }
    return CommandResult::Ok;
}

CommandResult ModuleLinkManager::Cmd_Exec(CommandContext& ctx)
{
    // Off the stack: this runs on whichever task serves the command, and `help` from
    // the module is over a kilobyte.
    constexpr size_t REPLY = 2048;
    std::unique_ptr<char[]> reply(new (std::nothrow) char[REPLY]);
    auto resp = ctx.reply.object();
    if (!reply)
    {
        resp.field("ok", false);
        resp.field("error", "out of memory");
        return CommandResult::Ok;
    }

    bool refused = false;
    const bool ok = Exec(ctx.arg(lineArg), reply.get(), REPLY, refused);
    resp.field("ok", ok && !refused);
    if (!ok)
    {
        resp.field("error", IsReady() ? "the module did not answer" : "no link to the module");
        return CommandResult::Ok;
    }
    resp.field(refused ? "error" : "reply", reply.get());
    return CommandResult::Ok;
}

CommandResult ModuleLinkManager::Cmd_Frames(CommandContext& ctx)
{
    constexpr size_t MAX = 64;
    const uint32_t after = ctx.has(afterArg) ? ctx.arg(afterArg) : 0;
    size_t max = ctx.has(maxArg) ? ctx.arg(maxArg) : MAX;
    if (max == 0 || max > MAX) max = MAX;

    std::unique_ptr<ModuleFrame[]> frames(new (std::nothrow) ModuleFrame[MAX]);
    auto resp = ctx.reply.object();
    if (!frames)
    {
        resp.field("error", "out of memory");
        return CommandResult::Ok;
    }

    uint32_t latest = 0, missed = 0;
    const size_t n = ReadFrames(after, frames.get(), max, latest, missed);

    resp.field("latest", latest);
    resp.field("missed", missed);
    resp.field("link", IsReady());
    auto list = resp.array("frames");
    for (size_t i = 0; i < n; ++i)
    {
        const ModuleFrame& f = frames[i];
        auto o = list.object();
        o.field("seq", f.seq);
        o.field("side", SideName(f.side));
        o.field("dir", f.sent ? "tx" : "rx");
        o.field("frame", f.frame);
        o.field("ms", f.gatewayMs);
        if (!f.sent)
            o.field("moduleMs", f.moduleMs);
        if (f.to != FrameRoute::None)
            o.field("to", f.to == FrameRoute::Thermostat ? "thermostat" : "boiler");
    }
    return CommandResult::Ok;
}
