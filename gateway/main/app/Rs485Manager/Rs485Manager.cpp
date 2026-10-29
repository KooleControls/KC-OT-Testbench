#include "Rs485Manager.h"
#include "BoardContext.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace modbus;

namespace {

constexpr uint32_t DEFAULT_BAUD = 9600;
constexpr uint32_t MIN_BAUD = 1200;
constexpr uint32_t MAX_BAUD = 115200;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

const char* SenderName(BusSender from)
{
    switch (from)
    {
    case BusSender::Bus:       return "bus";
    case BusSender::Sim:       return "sim";
    case BusSender::Testbench: return "testbench";
    }
    return "unknown";
}

const char* KindName(BusFrameKind kind)
{
    switch (kind)
    {
    case BusFrameKind::Request: return "request";
    case BusFrameKind::Reply:   return "reply";
    case BusFrameKind::Noise:   return "noise";
    }
    return "unknown";
}

void ToHex(const uint8_t* bytes, size_t n, char* out)
{
    static constexpr char DIGITS[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i)
    {
        *out++ = DIGITS[bytes[i] >> 4];
        *out++ = DIGITS[bytes[i] & 0x0F];
    }
    *out = '\0';
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Hex bytes, two digits each, with spaces, colons or dashes between them if anyone
/// likes. Returns the byte count, or 0 when it is not that.
size_t ParseHex(const char* text, uint8_t* out, size_t cap)
{
    size_t n = 0;
    for (const char* p = text; *p != '\0';)
    {
        if (*p == ' ' || *p == ':' || *p == '-')
        {
            ++p;
            continue;
        }
        const int hi = HexDigit(p[0]);
        const int lo = hi < 0 ? -1 : HexDigit(p[1]);
        if (lo < 0 || n == cap)
            return 0;
        out[n++] = static_cast<uint8_t>(hi << 4 | lo);
        p += 2;
    }
    return n;
}

constexpr CommandArg<uint32_t> afterArg{
    "after", "Only frames with a seq above this. 0 or absent: everything still held.",
    Presence::Optional };

constexpr CommandArg<uint32_t> maxArg{
    "max", "At most this many frames, oldest first (default and limit 32). Ask again "
           "from the last seq for the rest.",
    Presence::Optional };

constexpr CommandArg<const char*> frameArg{
    "frame", "The frame as hex bytes, unit address first and without the CRC, which is "
             "added: '01 03 00 61 00 01' reads register 97 of unit 1. Spaces are allowed.",
    200 };

constexpr CommandArg<uint32_t> unitArg{
    "unit", "Which simulated CoolPlug: 1 to 10 (not its Modbus address)." };

constexpr CommandArg<bool> powerArg{
    "power", "Switch the indoor unit on (true) or off (false).", Presence::Optional };

constexpr CommandArg<uint32_t> modeArg{
    "mode", "0 cool, 1 heat, 2 auto, 3 dry, 4 heat aux, 5 fan.", Presence::Optional };

constexpr CommandArg<uint32_t> setpointArg{
    "setpoint", "The set temperature in tenths of a °C, as the register holds it: 225 is "
                "22.5 °C. 100 to 350.",
    Presence::Optional };

constexpr CommandArg<uint32_t> fanArg{
    "fan", "0 low, 1 medium, 2 high, 3 auto, 4 top, 5 very low.", Presence::Optional };

constexpr CommandArg<uint32_t> swingArg{
    "swing", "The louvres: 0 vertical, 1 30°, 2 45°, 3 60°, 4 horizontal, 5 auto, 6 off, "
             "7 no louvres.",
    Presence::Optional };

constexpr CommandArg<bool> filterArg{
    "on", "Raise the filter sign (true) or clear it (false)." };

} // namespace

// ── Commands ─────────────────────────────────────────────────

CommandEntry Rs485Manager::statusCommand_{
    "rs485 status", &InvokeCommand<&Rs485Manager::Cmd_Status>,
    "Report the RS485 bus: its baud rate, and counters since boot of the frames heard, "
    "answered by the simulated CoolPlugs, and sent by hand."
};

CommandEntry Rs485Manager::framesCommand_{
    "rs485 frames", &InvokeCommand<&Rs485Manager::Cmd_Frames>,
    "The Modbus RTU frames on the RS485 bus, from a ring of the last 256: who sent each "
    "(bus, sim or testbench), whether it is a request, a reply or noise, and its bytes "
    "as hex. Poll with after=<last seq seen> to follow them.",
    { &afterArg, &maxArg }
};

CommandEntry Rs485Manager::sendCommand_{
    "rs485 send", &InvokeCommand<&Rs485Manager::Cmd_Send>,
    "Put one Modbus request on the RS485 bus, CRC added, as soon as the bus is quiet. "
    "Its reply is in 'rs485 frames'. A unit simulated here answers it as it would the "
    "gateway.",
    { &frameArg }
};

CommandEntry Rs485Manager::coolPlugStatusCommand_{
    "coolplug status", &InvokeCommand<&Rs485Manager::Cmd_CoolPlugStatus>,
    "Report the ten simulated CoolPlugs: whether each is on the bus and at which "
    "address, its indoor unit's state, and what the gateway asked of it. Their "
    "simulation settings are cp1.* to cp10.*, which apply within a second."
};

CommandEntry Rs485Manager::coolPlugSetCommand_{
    "coolplug set", &InvokeCommand<&Rs485Manager::Cmd_CoolPlugSet>,
    "Change a simulated indoor unit as its own remote would: power, mode, setpoint, fan "
    "and louvres. The gateway reads the change at its next poll, and can overwrite it. "
    "Not saved: every unit starts the same after a reboot.",
    { &unitArg, &powerArg, &modeArg, &setpointArg, &fanArg, &swingArg }
};

CommandEntry Rs485Manager::coolPlugFilterCommand_{
    "coolplug filter", &InvokeCommand<&Rs485Manager::Cmd_CoolPlugFilter>,
    "Raise or clear a simulated indoor unit's filter sign, as the unit does when its "
    "filter needs cleaning. The gateway can clear it too.",
    { &unitArg, &filterArg }
};

Rs485Manager::Rs485Manager(AppProvider& app)
    : app_(app)
{
}

void Rs485Manager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    StruxProvider& strux = app_.getStrux();
    SettingsManager& settings = strux.getSettingsManager();
    settings.Register({ &baudSetting_ });
    CoolPlugSimulator::RegisterSettings(settings);
    for (uint8_t i = 0; i < CoolPlugSimulator::UNITS; ++i)
        units_[i].Init(i, settings);

    strux.getCommandManager().Register(this, { &statusCommand_, &framesCommand_, &sendCommand_,
                                               &coolPlugStatusCommand_, &coolPlugSetCommand_,
                                               &coolPlugFilterCommand_ });

    ring_ = static_cast<BusFrame*>(heap_caps_calloc(RING, sizeof(BusFrame), MALLOC_CAP_SPIRAM));
    if (ring_ == nullptr)
        ring_ = static_cast<BusFrame*>(calloc(RING, sizeof(BusFrame)));
    if (ring_ == nullptr)
        ESP_LOGE(TAG, "No memory for the frame ring; the bus is not recorded");

    outbox_ = xQueueCreateStatic(OUTBOX_LEN, sizeof(Outgoing), outboxStorage_, &outboxBuffer_);
    port_ = &app_.getBoard().GetSerialPort(SerialPortId::Rs485);

    // Below the routing task: an OpenTherm frame is late within 800 ms, and a Modbus
    // master waits a second or more for its answer.
    task_.Init("rs485", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
        ESP_LOGE(TAG, "Failed to start the RS485 task");

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized");
}

// ── The task ─────────────────────────────────────────────────

void Rs485Manager::TaskLoop()
{
    Refresh(NowMs());

    const auto onNoise = [this](const uint8_t* held, size_t heldLen, size_t total) { OnNoise(held, heldLen, total); };

    uint8_t chunk[64];
    for (;;)
    {
        const size_t n = port_->read(chunk, sizeof(chunk),
                                     pdMS_TO_TICKS(splitter_.Partial() ? GAP_MS : IDLE_WAIT_MS));
        const uint32_t now = NowMs();
        if (n > 0)
        {
            lastRxMs_ = now;
            splitter_.Feed(chunk, n,
                           [this, now](const uint8_t* adu, size_t len, ModbusSplitter::Kind kind) {
                               OnFrame(adu, len, kind, now);
                           },
                           onNoise);
        }
        else if (now - lastRxMs_ >= GAP_MS)
        {
            splitter_.Quiet(onNoise);
        }

        if (awaiting_.active && now - awaiting_.sinceMs >= AWAIT_REPLY_MS)
            Await(false);
        if (Due(now, nextRefreshMs_))
            Refresh(now);
        for (CoolPlugSimulator& unit : units_)
            unit.Tick(now);
        SendQueued(now);
    }
}

void Rs485Manager::Refresh(uint32_t nowMs)
{
    nextRefreshMs_ = nowMs + REFRESH_MS;
    for (CoolPlugSimulator& unit : units_)
        unit.Refresh();

    uint32_t baud = baudSetting_.Get();
    if (baud < MIN_BAUD || baud > MAX_BAUD)
        baud = DEFAULT_BAUD;
    if (baud != baud_.load())
    {
        port_->SetBaudRate(baud);
        baud_ = baud;
        ESP_LOGI(TAG, "%lu baud", static_cast<unsigned long>(baud));
    }
}

// ── Frames ───────────────────────────────────────────────────

void Rs485Manager::OnFrame(const uint8_t* adu, size_t len, ModbusSplitter::Kind kind, uint32_t nowMs)
{
    if (kind == ModbusSplitter::Kind::Reply)
    {
        Record(BusSender::Bus, BusFrameKind::Reply, adu, len, len);
        counters_.replies++;
        Await(false);
        return;
    }
    Record(BusSender::Bus, BusFrameKind::Request, adu, len, len);
    counters_.requests++;
    Dispatch(adu, len, nowMs);
}

void Rs485Manager::OnNoise(const uint8_t* held, size_t heldLen, size_t total)
{
    counters_.noiseBytes += total;
    Record(BusSender::Bus, BusFrameKind::Noise, held, heldLen, total);
}

void Rs485Manager::Await(bool active, uint8_t unit, uint32_t nowMs)
{
    awaiting_ = { active, unit, nowMs };
    splitter_.ExpectReply(active);
}

void Rs485Manager::Dispatch(const uint8_t* adu, size_t len, uint32_t nowMs)
{
    const uint8_t address = adu[0];
    if (address == BROADCAST)
    {
        // Every unit that is up carries it out; none answers.
        uint8_t reply[MAX_PDU];
        for (CoolPlugSimulator& unit : units_)
            if (unit.Serves(BROADCAST, nowMs) && unit.Answers())
                unit.Handle(adu + 1, len - 3, true, reply, nowMs);
        Await(false);
        return;
    }

    if (CoolPlugSimulator* unit = UnitAt(address, nowMs))
    {
        Await(false);
        Answer(*unit, adu, len, nowMs);
        return;
    }

    Await(true, address, nowMs);
}

CoolPlugSimulator* Rs485Manager::UnitAt(uint8_t address, uint32_t nowMs)
{
    // Two units at one address is a test setup mistake; the first one has it.
    for (CoolPlugSimulator& unit : units_)
        if (unit.Serves(address, nowMs))
            return &unit;
    return nullptr;
}

void Rs485Manager::Answer(CoolPlugSimulator& unit, const uint8_t* adu, size_t len, uint32_t nowMs)
{
    // A unit that does not answer does nothing at all, as a dead one would.
    if (!unit.Answers())
    {
        counters_.silent++;
        return;
    }

    uint8_t reply[MAX_ADU];
    reply[0] = adu[0];
    const size_t pdu = unit.Handle(adu + 1, len - 3, false, reply + 1, nowMs);
    if (pdu == 0)
        return;

    // Late on purpose: rounded up to whole ticks, so 1 ms is still late.
    if (const uint32_t delay = unit.ReplyDelayMs())
        vTaskDelay(pdMS_TO_TICKS(delay + portTICK_PERIOD_MS - 1));

    if (Transmit(reply, AppendCrc(reply, 1 + pdu), BusSender::Sim, BusFrameKind::Reply))
        counters_.answered++;
}

bool Rs485Manager::Transmit(const uint8_t* adu, size_t len, BusSender from, BusFrameKind kind)
{
    if (port_->write(adu, len, pdMS_TO_TICKS(500)) != len)
    {
        counters_.sendFailed++;
        return false;
    }

    Record(from, kind, adu, len, len);
    return true;
}

void Rs485Manager::SendQueued(uint32_t nowMs)
{
    // Only into a quiet bus: not over a frame coming in, nor straight after one.
    if (splitter_.Partial() || nowMs - lastRxMs_ < GAP_MS)
        return;

    Outgoing out;
    if (xQueueReceive(outbox_, &out, 0) != pdTRUE)
        return;
    if (!Transmit(out.bytes, out.len, BusSender::Testbench, BusFrameKind::Request))
        return;
    counters_.sent++;

    // Not heard back (the chip does not listen while it sends), so it is dispatched
    // here: a unit simulated here answers it on the bus.
    Dispatch(out.bytes, out.len, nowMs);
}

// ── The ring ─────────────────────────────────────────────────

void Rs485Manager::Record(BusSender from, BusFrameKind kind, const uint8_t* bytes, size_t len, size_t total)
{
    if (ring_ == nullptr)
        return;
    LOCK(ringMutex_);
    BusFrame& f = ring_[nextSeq_ % RING];
    f.seq = nextSeq_++;
    f.ms = NowMs();
    f.len = static_cast<uint16_t>(std::min<size_t>(total, UINT16_MAX));
    f.kind = kind;
    f.from = from;
    memcpy(f.bytes, bytes, std::min(len, BusFrame::KEEP));
}

size_t Rs485Manager::ReadFrames(uint32_t after, BusFrame* out, size_t max, uint32_t& latest, uint32_t& missed)
{
    LOCK(ringMutex_);
    latest = nextSeq_ - 1;
    missed = 0;
    if (ring_ == nullptr)
        return 0;
    const uint32_t oldest = nextSeq_ > RING ? nextSeq_ - static_cast<uint32_t>(RING) : 1;

    // A cursor from before a restart is ahead of everything: start from what is held.
    uint32_t from = (after >= nextSeq_) ? oldest : after + 1;
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

CommandResult Rs485Manager::Cmd_Status(CommandContext& ctx)
{
    auto resp = ctx.reply.object();
    resp.field("baud", baud_.load());
    {
        LOCK(ringMutex_);
        resp.field("framesRecorded", nextSeq_ - 1);
    }
    auto c = resp.object("counters");
    c.field("requests", counters_.requests.load());
    c.field("replies", counters_.replies.load());
    c.field("noiseBytes", counters_.noiseBytes.load());
    c.field("answered", counters_.answered.load());
    c.field("silent", counters_.silent.load());
    c.field("sent", counters_.sent.load());
    c.field("sendFailed", counters_.sendFailed.load());
    return CommandResult::Ok;
}

CommandResult Rs485Manager::Cmd_Frames(CommandContext& ctx)
{
    // Fewer per page than the OpenTherm frames: each carries up to 64 bytes as hex.
    constexpr size_t MAX = 32;
    const uint32_t after = ctx.has(afterArg) ? ctx.arg(afterArg) : 0;
    size_t max = ctx.has(maxArg) ? ctx.arg(maxArg) : MAX;
    if (max == 0 || max > MAX) max = MAX;

    std::unique_ptr<BusFrame[]> frames(new (std::nothrow) BusFrame[MAX]);
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
    resp.field("baud", baud_.load());
    auto list = resp.array("frames");
    char hex[BusFrame::KEEP * 2 + 1];
    for (size_t i = 0; i < n; ++i)
    {
        const BusFrame& f = frames[i];
        ToHex(f.bytes, std::min<size_t>(f.len, BusFrame::KEEP), hex);
        auto o = list.object();
        o.field("seq", f.seq);
        o.field("ms", f.ms);
        o.field("from", SenderName(f.from));
        o.field("kind", KindName(f.kind));
        o.field("len", static_cast<uint32_t>(f.len));
        o.field("hex", hex);
    }
    return CommandResult::Ok;
}

CommandResult Rs485Manager::Cmd_Send(CommandContext& ctx)
{
    // Off the stack: this runs on whichever task serves the command.
    std::unique_ptr<Outgoing> out(new (std::nothrow) Outgoing);
    auto resp = ctx.reply.object();
    if (!out)
    {
        resp.field("ok", false);
        resp.field("error", "out of memory");
        return CommandResult::Ok;
    }

    const size_t n = ParseHex(ctx.arg(frameArg), out->bytes, MAX_ADU - 2);
    if (n < 2)
    {
        resp.field("ok", false);
        resp.field("error", "not a frame: hex bytes, unit and function code at least");
        return CommandResult::Ok;
    }
    out->len = static_cast<uint16_t>(AppendCrc(out->bytes, n));

    if (outbox_ == nullptr || xQueueSend(outbox_, out.get(), 0) != pdTRUE)
    {
        resp.field("ok", false);
        resp.field("error", "busy: frames are still waiting for a quiet bus");
        return CommandResult::Ok;
    }

    // The caller has the rest of the frame already.
    char crc[5];
    ToHex(out->bytes + n, 2, crc);
    resp.field("ok", true);
    resp.field("crc", crc);
    return CommandResult::Ok;
}

CommandResult Rs485Manager::Cmd_CoolPlugStatus(CommandContext& ctx)
{
    const uint32_t now = NowMs();
    auto resp = ctx.reply.object();
    resp.field("baud", baud_.load());
    auto list = resp.array("units");
    for (CoolPlugSimulator& unit : units_)
    {
        auto o = list.object();
        unit.Report(o, now);
    }
    return CommandResult::Ok;
}

CommandResult Rs485Manager::Cmd_CoolPlugSet(CommandContext& ctx)
{
    CoolPlugSimulator::Change change;
    if (ctx.has(powerArg))    change.power = ctx.arg(powerArg) ? 1 : 0;
    if (ctx.has(modeArg))     change.mode = static_cast<int32_t>(std::min<uint32_t>(ctx.arg(modeArg), INT32_MAX));
    if (ctx.has(setpointArg)) change.setpoint = static_cast<int32_t>(std::min<uint32_t>(ctx.arg(setpointArg), INT32_MAX));
    if (ctx.has(fanArg))      change.fan = static_cast<int32_t>(std::min<uint32_t>(ctx.arg(fanArg), INT32_MAX));
    if (ctx.has(swingArg))    change.swing = static_cast<int32_t>(std::min<uint32_t>(ctx.arg(swingArg), INT32_MAX));
    return ApplyToUnit(ctx, ctx.arg(unitArg), change);
}

CommandResult Rs485Manager::Cmd_CoolPlugFilter(CommandContext& ctx)
{
    CoolPlugSimulator::Change change;
    change.filter = ctx.arg(filterArg) ? 1 : 0;
    return ApplyToUnit(ctx, ctx.arg(unitArg), change);
}

CommandResult Rs485Manager::ApplyToUnit(CommandContext& ctx, uint32_t unit, const CoolPlugSimulator::Change& change)
{
    auto resp = ctx.reply.object();
    if (unit < 1 || unit > CoolPlugSimulator::UNITS)
    {
        resp.field("ok", false);
        resp.field("error", "no such unit: 1 to 10");
        return CommandResult::Ok;
    }

    const char* error = nullptr;
    CoolPlugSimulator& sim = units_[unit - 1];
    if (!sim.Apply(change, error))
    {
        resp.field("ok", false);
        resp.field("error", error);
        return CommandResult::Ok;
    }

    resp.field("ok", true);
    auto o = resp.object("unit");
    sim.Report(o, NowMs());
    return CommandResult::Ok;
}
