#include "IoManager.h"
#include "StruxProvider.h"
#include "SettingsManager.h"
#include "CommandManager.h"
#include "BoardContext.h"
#include "RoutingManager.h"
#include "OpenThermFrame.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

constexpr RelayId RELAY_IDS[IoManager::RELAYS] = { RelayId::Relay1, RelayId::Relay2 };
constexpr InputId INPUT_IDS[IoManager::INPUTS] = { InputId::Input1, InputId::Input2 };
constexpr IoManager::Line RELAY_LINES[IoManager::RELAYS] = { IoManager::Line::Relay1, IoManager::Line::Relay2 };
constexpr IoManager::Line INPUT_LINES[IoManager::INPUTS] = { IoManager::Line::Input1, IoManager::Line::Input2 };

/// Every source but manual, in the order `io status` lists them.
constexpr IoManager::Source SIGNALS[] = {
    IoManager::Source::ThermostatHeat,
    IoManager::Source::ThermostatCool,
    IoManager::Source::ThermostatSimHeat,
    IoManager::Source::ThermostatSimCool,
};

const char* SourceName(IoManager::Source source)
{
    switch (source)
    {
    case IoManager::Source::Manual:            return "manual";
    case IoManager::Source::ThermostatHeat:    return "thermostat.heat";
    case IoManager::Source::ThermostatCool:    return "thermostat.cool";
    case IoManager::Source::ThermostatSimHeat: return "thermostatSim.heat";
    case IoManager::Source::ThermostatSimCool: return "thermostatSim.cool";
    }
    return "unknown";
}

/// "" is manual as well as "manual": an empty setting is how manual is stored.
bool ParseSource(const char* name, IoManager::Source& out)
{
    if (name[0] == '\0' || strcmp(name, "manual") == 0) { out = IoManager::Source::Manual; return true; }
    for (IoManager::Source s : SIGNALS)
        if (strcmp(name, SourceName(s)) == 0) { out = s; return true; }
    return false;
}

const char* LineName(IoManager::Line line)
{
    switch (line)
    {
    case IoManager::Line::Input1: return "input1";
    case IoManager::Line::Input2: return "input2";
    case IoManager::Line::Relay1: return "relay1";
    case IoManager::Line::Relay2: return "relay2";
    }
    return "unknown";
}

constexpr CommandArg<uint32_t> relayArg{
    "relay", "Which relay: 1 or 2, as printed on the board." };

constexpr CommandArg<bool> onArg{
    "on", "Close the relay (true) or open it (false)." };

constexpr CommandArg<const char*> sourceArg{
    "source", "What the relay follows: 'manual' (io relay), or a thermostat's request as it "
              "asks for it over OpenTherm: 'thermostat.heat' and 'thermostat.cool' (the real "
              "one, on the module's thermostat line: CH enable, cooling enable), "
              "'thermostatSim.heat' and 'thermostatSim.cool' (the simulator).",
    24 };

constexpr CommandArg<uint32_t> afterArg{
    "after", "Only events after this seq; 0 or left out for all the ring holds.", Presence::Optional };

constexpr CommandArg<uint32_t> maxArg{
    "max", "At most this many events, 64 at most (the default).", Presence::Optional };

} // namespace

// ── Commands ─────────────────────────────────────────────────

CommandEntry IoManager::statusCommand_{
    "io status", &InvokeCommand<&IoManager::Cmd_Status>,
    "Report the two relays (closed or not, and what each follows), the two inputs "
    "(active or not, and how long ago they changed), whether each source asks for its "
    "relay right now, and the newest event's seq."
};

CommandEntry IoManager::relayCommand_{
    "io relay", &InvokeCommand<&IoManager::Cmd_Relay>,
    "Close or open a relay by hand. Only a relay that follows 'manual'; one that follows a "
    "thermostat is refused. Not saved: both relays are open after a reboot.",
    { &relayArg, &onArg }
};

CommandEntry IoManager::sourceCommand_{
    "io source", &InvokeCommand<&IoManager::Cmd_Source>,
    "Choose what a relay follows: 'manual', or a thermostat's heat or cool request, so the "
    "relay plays the heater or cooler that thermostat would switch. Applied at once and "
    "saved (io.relay1.src, io.relay2.src). Handed to manual, a relay stays where it is.",
    { &relayArg, &sourceArg }
};

CommandEntry IoManager::eventsCommand_{
    "io events", &InvokeCommand<&IoManager::Cmd_Events>,
    "The changes on the four lines (input1, input2, relay1, relay2), oldest first, from a "
    "ring of the last 128: each with its seq, the testbench's clock in ms when the line "
    "changed (an input's before the 20 ms debounce), and whether it went on. Poll with the "
    "last seq seen. 'missed' counts events that left the ring unread; a 'latest' below "
    "'after' means the testbench restarted.",
    { &afterArg, &maxArg }
};

IoManager::IoManager(AppProvider& app)
    : app_(app)
{
}

void IoManager::Init()
{
    auto initAttempt = initState_.TryBeginInit();
    if (!initAttempt)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    StruxProvider& strux = app_.getStrux();
    strux.getSettingsManager().Register({ &relay1Source_, &relay2Source_ });
    strux.getCommandManager().Register(this, { &statusCommand_, &relayCommand_, &sourceCommand_, &eventsCommand_ });

    {
        LOCK(mutex_);
        LoadSources();
        // The inputs as they are at boot are where they start, not a change.
        for (size_t i = 0; i < INPUTS; ++i)
            inputs_[i].active = app_.getBoard().GetInput(INPUT_IDS[i]).IsActive();
    }

    task_.Init("io", 5, TASK_STACK);
    task_.SetHandler([this] { TaskLoop(); });
    if (!task_.Run())
        ESP_LOGE(TAG, "Failed to start the IO task");

    initAttempt.SetReady();
    ESP_LOGI(TAG, "Initialized: relay 1 follows %s, relay 2 follows %s",
             SourceName(sources_[0]), SourceName(sources_[1]));
}

void IoManager::TaskLoop()
{
    nextRefreshMs_ = NowMs() + REFRESH_MS;
    for (;;)
    {
        const uint32_t now = NowMs();
        {
            LOCK(mutex_);
            // Again every second, so `settings set io.relay1.src` applies like any other
            // setting does, without a command.
            if (Due(now, nextRefreshMs_))
            {
                nextRefreshMs_ = now + REFRESH_MS;
                LoadSources();
            }
            SampleInputs(now);
            Drive(now);
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ── Sources ──────────────────────────────────────────────────

void IoManager::LoadSources()
{
    StringSetting* stored[RELAYS] = { &relay1Source_, &relay2Source_ };
    for (size_t i = 0; i < RELAYS; ++i)
    {
        char value[32];
        stored[i]->Get(value, sizeof(value));
        Source source;
        const bool known = ParseSource(value, source);
        if (!known)
        {
            if (!badSource_[i])
                ESP_LOGW(TAG, "Relay %u follows '%s', which is no source; manual instead",
                         static_cast<unsigned>(i + 1), value);
            source = Source::Manual;
        }
        badSource_[i] = !known;
        ApplySource(i, source);
    }
}

void IoManager::ApplySource(size_t relay, Source source)
{
    if (sources_[relay] == source)
        return;
    // Handed to manual it stays where it is: nothing clicks because of who drives it.
    if (source == Source::Manual)
        manual_[relay] = app_.getBoard().GetRelay(RELAY_IDS[relay]).IsOn();
    sources_[relay] = source;
    ESP_LOGI(TAG, "Relay %u follows %s", static_cast<unsigned>(relay + 1), SourceName(source));
}

bool IoManager::SourceValue(Source source, uint32_t nowMs) const
{
    using Master = RoutingManager::Master;
    const RoutingManager& routing = app_.getRoutingManager();
    switch (source)
    {
    case Source::Manual:            return false;
    case Source::ThermostatHeat:    return routing.Requests(Master::ThermostatLine, opentherm::MASTER_CH_ENABLE, nowMs);
    case Source::ThermostatCool:    return routing.Requests(Master::ThermostatLine, opentherm::MASTER_COOLING_ENABLE, nowMs);
    case Source::ThermostatSimHeat: return routing.Requests(Master::ThermostatSim, opentherm::MASTER_CH_ENABLE, nowMs);
    case Source::ThermostatSimCool: return routing.Requests(Master::ThermostatSim, opentherm::MASTER_COOLING_ENABLE, nowMs);
    }
    return false;
}

bool IoManager::SetManual(size_t relay, bool on)
{
    LOCK(mutex_);
    if (sources_[relay] != Source::Manual)
        return false;
    manual_[relay] = on;
    Drive(NowMs());
    return true;
}

void IoManager::SetSource(size_t relay, Source source)
{
    StringSetting* stored[RELAYS] = { &relay1Source_, &relay2Source_ };
    {
        LOCK(mutex_);
        stored[relay]->Set(source == Source::Manual ? "" : SourceName(source));
        badSource_[relay] = false;
        ApplySource(relay, source);
        Drive(NowMs());
    }
    app_.getStrux().getSettingsManager().Save();
}

// ── The lines ────────────────────────────────────────────────

void IoManager::SampleInputs(uint32_t nowMs)
{
    for (size_t i = 0; i < INPUTS; ++i)
    {
        InputState& s = inputs_[i];
        const bool level = app_.getBoard().GetInput(INPUT_IDS[i]).IsActive();
        if (level == s.active)
        {
            s.pending = 0;
            continue;
        }
        if (s.pending == 0)
            s.pendingMs = nowMs;
        if (++s.pending < DEBOUNCE_SAMPLES)
            continue;

        // Timed from the first sample at the new level: the debounce is ours, not the
        // gateway's, and must not show in what a test measures.
        s.active = level;
        s.changed = true;
        s.sinceMs = s.pendingMs;
        s.pending = 0;
        Record(INPUT_LINES[i], level, s.pendingMs);
    }
}

void IoManager::Drive(uint32_t nowMs)
{
    for (size_t i = 0; i < RELAYS; ++i)
    {
        const bool want = sources_[i] == Source::Manual ? manual_[i] : SourceValue(sources_[i], nowMs);
        Relay& relay = app_.getBoard().GetRelay(RELAY_IDS[i]);
        if (relay.IsOn() == want)
            continue;
        relay.Set(want);
        Record(RELAY_LINES[i], want, nowMs);
    }
}

// ── The record ───────────────────────────────────────────────

void IoManager::Record(Line line, bool on, uint32_t ms)
{
    Event& e = ring_[nextSeq_ % RING];
    e.seq = nextSeq_++;
    e.ms = ms;
    e.line = line;
    e.on = on;
}

size_t IoManager::ReadEvents(uint32_t after, Event* out, size_t max, uint32_t& latest, uint32_t& missed)
{
    LOCK(mutex_);
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

void IoManager::WriteState(CommandContext& ctx)
{
    const uint32_t now = NowMs();
    LOCK(mutex_);

    auto resp = ctx.reply.object();
    resp.field("ok", true);

    auto relays = resp.array("relays");
    for (size_t i = 0; i < RELAYS; ++i)
    {
        auto o = relays.object();
        o.field("relay", static_cast<uint32_t>(i + 1));
        o.field("on", app_.getBoard().GetRelay(RELAY_IDS[i]).IsOn());
        o.field("source", SourceName(sources_[i]));
    }

    auto inputs = resp.array("inputs");
    for (size_t i = 0; i < INPUTS; ++i)
    {
        const InputState& s = inputs_[i];
        auto o = inputs.object();
        o.field("input", static_cast<uint32_t>(i + 1));
        o.field("active", s.active);
        if (s.changed)
            o.field("changedAgoMs", now - s.sinceMs);
    }

    auto sources = resp.object("sources");
    for (Source s : SIGNALS)
        sources.field(SourceName(s), SourceValue(s, now));

    resp.field("latest", nextSeq_ - 1);
}

CommandResult IoManager::Cmd_Status(CommandContext& ctx)
{
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult IoManager::Cmd_Relay(CommandContext& ctx)
{
    const uint32_t n = ctx.arg(relayArg);
    if (n < 1 || n > RELAYS)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "no such relay: 1 or 2");
        return CommandResult::Ok;
    }

    if (!SetManual(n - 1, ctx.arg(onArg)))
    {
        Source source;
        {
            LOCK(mutex_);
            source = sources_[n - 1];
        }
        char error[80];
        snprintf(error, sizeof(error), "relay %u follows %s; set its source to manual first",
                 static_cast<unsigned>(n), SourceName(source));
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", error);
        return CommandResult::Ok;
    }

    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult IoManager::Cmd_Source(CommandContext& ctx)
{
    // Which names exist is meaning, not form: to the framework it is a string. So an
    // unknown one is a reply, not a refusal.
    const uint32_t n = ctx.arg(relayArg);
    Source source;
    if (n < 1 || n > RELAYS || !ParseSource(ctx.arg(sourceArg), source))
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", n < 1 || n > RELAYS ? "no such relay: 1 or 2" : "unknown source");
        return CommandResult::Ok;
    }

    SetSource(n - 1, source);
    WriteState(ctx);
    return CommandResult::Ok;
}

CommandResult IoManager::Cmd_Events(CommandContext& ctx)
{
    constexpr size_t MAX = 64;
    const uint32_t after = ctx.has(afterArg) ? ctx.arg(afterArg) : 0;
    size_t max = ctx.has(maxArg) ? ctx.arg(maxArg) : MAX;
    if (max == 0 || max > MAX) max = MAX;

    std::unique_ptr<Event[]> events(new (std::nothrow) Event[MAX]);
    auto resp = ctx.reply.object();
    if (!events)
    {
        resp.field("error", "out of memory");
        return CommandResult::Ok;
    }

    uint32_t latest = 0, missed = 0;
    const size_t n = ReadEvents(after, events.get(), max, latest, missed);

    resp.field("latest", latest);
    resp.field("missed", missed);
    resp.field("ms", NowMs());
    auto list = resp.array("events");
    for (size_t i = 0; i < n; ++i)
    {
        const Event& e = events[i];
        auto o = list.object();
        o.field("seq", e.seq);
        o.field("ms", e.ms);
        o.field("line", LineName(e.line));
        o.field("on", e.on);
    }
    return CommandResult::Ok;
}
