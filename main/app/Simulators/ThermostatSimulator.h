#pragma once

#include "TypedSettings.h"
#include "Mutex.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

class SettingsManager;
class ReplyObject;

// ──────────────────────────────────────────────────────────────
// A thermostat that is not a device: an OpenTherm master running on the testbench.
//
// It asks one question per interval, going round a fixed cycle -- its status (with
// CH, DHW and cooling enable), the flow temperature it wants, its room temperature and
// setpoint, and reads of what the boiler has to say -- and remembers what the boiler
// answered. Every request is built from the settings as they are at that moment, so
// a change applies with the next frame. A remote override from the boiler side (id 9)
// moves the setpoint, as on a real thermostat, unless tsim.follow is off. With
// tsim.auto it asks for heat or cooling as the room needs it, from room temperature
// and setpoint, instead of whenever CH or cooling enable is set.
//
// A KC thermostat also has a panel with a mode, which a gateway reads and sets over
// BLE (BleThermostatManager). While that is enabled the mode in force is handed in
// here (SetModeGate) and decides which way this thermostat may ask at all, the way the
// KC thermostat's own control step does; without it, the gate is None and nothing
// differs from a plain OpenTherm thermostat.
//
// Driven entirely by the RoutingManager's task: it says when a request is due, takes
// the request and hands back the answer. The lock is only for Report() and
// ReadPanel(), which run on whichever task asks.
// ──────────────────────────────────────────────────────────────
class ThermostatSimulator
{
    static constexpr const char* TAG = "ThermostatSim";

public:
    /// The setpoint a KC thermostat regulates against in Off, and holds on the wire
    /// when Off is all it may say (see ThermostatPanel).
    static constexpr float FROST_SETPOINT = 5.0f;

    /// Which way a panel in front of this thermostat lets it ask: the KC thermostat's
    /// mode in force. None when there is no panel, and then it asks exactly as its
    /// settings say.
    enum class ModeGate : uint8_t { None, Off, Heat, Cool, Auto };

    /// What a panel shows and the gateway reads back over BLE: the room, the setpoint,
    /// what this thermostat asks for right now, and what the boiler side says about
    /// cooling.
    struct Panel
    {
        float room = 0, setpoint = 0, flow = 0;
        bool ch = false, cool = false;        // asked for now, mode gate included
        bool coolingSupported = false;        // slave config (id 3), held as the KC thermostat holds it
        bool overrideKnown = false;
        float remoteOverride = 0;             // id 9, 0 for none
    };

    void RegisterSettings(SettingsManager& settings);

    /// Any task. Applies from the next status request on.
    void SetModeGate(ModeGate gate) { gate_.store(gate, std::memory_order_relaxed); }
    ModeGate GetModeGate() const { return gate_.load(std::memory_order_relaxed); }

    /// Any task.
    Panel ReadPanel();

    /// Move the room setpoint, as a hand on the panel would; sent with the next id 16.
    void SetSetpoint(float celsius) { setpoint_.Set(celsius); }

    /// Whether the next request is due.
    bool Due(uint32_t nowMs);

    /// The next request in the cycle. A previous one still unanswered is counted.
    uint32_t NextRequest(uint32_t nowMs);

    /// The boiler's answer to the request in flight.
    void OnResponse(uint32_t frame, uint32_t nowMs);

    /// What it asked and what it has heard, for `sim status`.
    void Report(ReplyObject& out, uint32_t nowMs);

private:
    static constexpr uint32_t MIN_INTERVAL_MS = 200;

    // How long the slave config's cooling bit has to stay clear before the KC thermostat
    // believes it: the module in front of a restarting gateway answers with its power-up
    // defaults for some 9 s, and taking that at its word stalled a Cool run on every
    // gateway restart. The KC thermostat's own number (kCoolingHoldUs).
    static constexpr uint32_t COOLING_HOLD_MS = 30000;

    std::atomic<ModeGate> gate_{ ModeGate::None };

    Mutex mutex_;
    size_t cycle_ = 0;
    bool awaiting_ = false;
    bool sentAny_ = false;
    uint32_t lastSentMs_ = 0;

    // What the boiler has told it; `known` says which of these it has heard yet.
    struct Heard
    {
        uint32_t requests = 0, responses = 0, unanswered = 0, unknownIds = 0;
        uint32_t lastResponseMs = 0;
        uint32_t overridesFollowed = 0;
        uint8_t known = 0;
        uint8_t slaveFlags = 0;
        float waterTemp = 0, modulation = 0, otVersion = 0, remoteOverride = 0;
        uint8_t slaveConfig = 0, memberId = 0;
    } heard_;

    // The last id 9 value, raw, so the next reads of one pulse are not taken again.
    uint16_t lastOverride_ = 0;

    // Cooling as the KC thermostat believes it, from id 3 (see COOLING_HOLD_MS).
    bool coolingSupported_ = false;
    bool coolingClearing_ = false;
    uint32_t coolingClearSinceMs_ = 0;

    /// CH and cooling enable as the next status request asks them: the settings, the
    /// mode gate, and with tsim.auto the room.
    void Demand(bool& ch, bool& cool) const;

    static constexpr uint8_t KNOWN_STATUS = 1 << 0;
    static constexpr uint8_t KNOWN_WATER = 1 << 1;
    static constexpr uint8_t KNOWN_MODULATION = 1 << 2;
    static constexpr uint8_t KNOWN_CONFIG = 1 << 3;
    static constexpr uint8_t KNOWN_VERSION = 1 << 4;
    static constexpr uint8_t KNOWN_OVERRIDE = 1 << 5;

    // ── Settings (keys at most 15 characters) ──
    inline static FloatSetting roomTemp_{ "tsim.room", "Thermostat Sim Room Temperature", 19.8f };
    inline static FloatSetting setpoint_{ "tsim.setpoint", "Thermostat Sim Room Setpoint", 21.0f };
    inline static FloatSetting flowTemp_{ "tsim.flow", "Thermostat Sim Requested Flow Temperature", 35.0f };
    inline static BoolSetting chEnable_{ "tsim.ch", "Thermostat Sim CH Enable", true };
    inline static BoolSetting dhwEnable_{ "tsim.dhw", "Thermostat Sim DHW Enable", false };
    inline static BoolSetting coolEnable_{ "tsim.cool", "Thermostat Sim Cooling Enable", false };
    inline static BoolSetting autoDemand_{ "tsim.auto", "Thermostat Sim Asks As The Room Needs", false };
    inline static BoolSetting followOverride_{ "tsim.follow", "Thermostat Sim Follows Remote Override", true };
    inline static UInt32Setting interval_{ "tsim.interval", "Thermostat Sim Interval (ms)", 1000 };
    inline static UInt32Setting memberId_{ "tsim.member", "Thermostat Sim Member Id", 0 };
};
