#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"
#include "TypedSettings.h"
#include "Task.h"
#include "Mutex.h"
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The board's two relays and two inputs, for wiring the testbench to a gateway's own:
// the gateway's relays switch its +12 V output onto these inputs, and these relays switch
// this board's +12 V onto the gateway's inputs, each through a normally open contact.
//
// An input is only read: sampled every tick, debounced, and every change recorded with
// the time it began. A relay follows its source. `manual` is the default: the relay is
// where `io relay` put it, and off after a boot. Any other source is a signal on the
// bench, and the relay then plays the device it stands for -- a thermostat's heat
// request (OpenTherm CH enable) closing a heat relay, its cool request (cooling enable)
// a cool relay -- the way the wires would be laid on a real installation. Which source
// a relay follows is a setting per relay, saved, and read again every second.
//
// The relays' changes go into the same ring as the inputs', with one numbering, so
// `io events after=<seq>` gives a test the order and the timing of everything on these
// four lines: the gateway's relay closing, and how long after its thermostat asked.
//
// One task samples, decides and drives; the commands change what it decides from
// under the same lock and drive at once, so a reply already shows the relay moved.
// ──────────────────────────────────────────────────────────────
class IoManager
{
    static constexpr const char* TAG = "IoManager";

public:
    static constexpr size_t RELAYS = 2;
    static constexpr size_t INPUTS = 2;

    /// What a relay follows.
    enum class Source : uint8_t
    {
        Manual,              // `io relay`
        ThermostatHeat,      // the real thermostat's CH enable
        ThermostatCool,      // the real thermostat's cooling enable
        ThermostatSimHeat,   // the thermostat simulator's CH enable
        ThermostatSimCool,   // the thermostat simulator's cooling enable
    };

    /// The four lines, as the events name them.
    enum class Line : uint8_t { Input1, Input2, Relay1, Relay2 };

    /// One change on one line.
    struct Event
    {
        uint32_t seq;   // this testbench's numbering, from 1, gap-free
        uint32_t ms;    // this testbench's clock when the line changed: before the debounce
        Line line;
        bool on;        // an input active, a relay closed
    };

    explicit IoManager(AppProvider& app);

    IoManager(const IoManager&) = delete;
    IoManager& operator=(const IoManager&) = delete;
    IoManager(IoManager&&) = delete;
    IoManager& operator=(IoManager&&) = delete;

    void Init();

    /// Close (true) or open relay `relay` (0-based) by hand. False when it follows
    /// another source: a test that switches a relay the thermostat drives has a wire
    /// too many. Not saved.
    bool SetManual(size_t relay, bool on);

    /// Let relay `relay` (0-based) follow `source`. Saved. Handed to manual, it stays
    /// where it is.
    void SetSource(size_t relay, Source source);

    /// Events after `after`, oldest first, at most `max`. `latest` is the newest seq
    /// there is; `missed` counts events after `after` that have already left the ring.
    /// A `latest` below `after` means this testbench restarted.
    size_t ReadEvents(uint32_t after, Event* out, size_t max, uint32_t& latest, uint32_t& missed);

private:
    static constexpr int TASK_STACK = 4096;
    static constexpr uint32_t TICK_MS = 10;
    static constexpr uint32_t REFRESH_MS = 1000;
    static constexpr size_t RING = 128;

    // A relay contact bounces for a few milliseconds; two samples in a row is 20 ms.
    static constexpr uint8_t DEBOUNCE_SAMPLES = 2;

    struct InputState
    {
        bool active = false;
        bool changed = false;     // since boot, so `sinceMs` means something
        uint32_t sinceMs = 0;
        uint8_t pending = 0;      // samples in a row at the other level
        uint32_t pendingMs = 0;   // when the first of them was taken
    };

    AppProvider& app_;
    InitState initState_;
    Task task_;

    // ── The task's, and the commands' under mutex_ ──
    Mutex mutex_;
    Source sources_[RELAYS] = { Source::Manual, Source::Manual };
    bool badSource_[RELAYS] = {};   // the stored source is no name; logged once
    bool manual_[RELAYS] = {};
    InputState inputs_[INPUTS];
    uint32_t nextRefreshMs_ = 0;

    Event ring_[RING] = {};
    uint32_t nextSeq_ = 1;

    void TaskLoop();
    void LoadSources();
    void ApplySource(size_t relay, Source source);
    void SampleInputs(uint32_t nowMs);
    void Drive(uint32_t nowMs);
    bool SourceValue(Source source, uint32_t nowMs) const;
    void Record(Line line, bool on, uint32_t ms);

    // ── Settings. Keys are at most 15 characters (NVS). The value is the source's
    // name as the commands spell it, empty for manual. ──
    inline static StringSetting relay1Source_{ "io.relay1.src", "Relay 1 Follows", "" };
    inline static StringSetting relay2Source_{ "io.relay2.src", "Relay 2 Follows", "" };

    // ── Commands ──
    CommandResult Cmd_Status(CommandContext& ctx);
    CommandResult Cmd_Relay(CommandContext& ctx);
    CommandResult Cmd_Source(CommandContext& ctx);
    CommandResult Cmd_Events(CommandContext& ctx);
    void WriteState(CommandContext& ctx);
    static CommandEntry statusCommand_;
    static CommandEntry relayCommand_;
    static CommandEntry sourceCommand_;
    static CommandEntry eventsCommand_;
};
