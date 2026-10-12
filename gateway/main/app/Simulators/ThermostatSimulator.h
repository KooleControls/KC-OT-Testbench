#pragma once

#include "TypedSettings.h"
#include "Mutex.h"
#include <cstddef>
#include <cstdint>

class SettingsManager;
class ReplyObject;

// ──────────────────────────────────────────────────────────────
// A thermostat that is not a device: an OpenTherm master running on the testbench.
//
// It asks one question per interval, going round a fixed cycle -- its status (with
// CH and DHW enable), the flow temperature it wants, its room temperature and
// setpoint, and reads of what the boiler has to say -- and remembers what the boiler
// answered. Every request is built from the settings as they are at that moment, so
// a change applies with the next frame.
//
// Driven entirely by the RoutingManager's task: it says when a request is due, takes
// the request and hands back the answer. The lock is only for Report(), which runs
// on whichever task serves the command.
// ──────────────────────────────────────────────────────────────
class ThermostatSimulator
{
    static constexpr const char* TAG = "ThermostatSim";

public:
    void RegisterSettings(SettingsManager& settings);

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
        uint8_t known = 0;
        uint8_t slaveFlags = 0;
        float waterTemp = 0, modulation = 0, otVersion = 0;
        uint8_t slaveConfig = 0, memberId = 0;
    } heard_;

    static constexpr uint8_t KNOWN_STATUS = 1 << 0;
    static constexpr uint8_t KNOWN_WATER = 1 << 1;
    static constexpr uint8_t KNOWN_MODULATION = 1 << 2;
    static constexpr uint8_t KNOWN_CONFIG = 1 << 3;
    static constexpr uint8_t KNOWN_VERSION = 1 << 4;

    // ── Settings (keys at most 15 characters) ──
    inline static FloatSetting roomTemp_{ "tsim.room", "Thermostat Sim Room Temperature", 19.8f };
    inline static FloatSetting setpoint_{ "tsim.setpoint", "Thermostat Sim Room Setpoint", 21.0f };
    inline static FloatSetting flowTemp_{ "tsim.flow", "Thermostat Sim Requested Flow Temperature", 35.0f };
    inline static BoolSetting chEnable_{ "tsim.ch", "Thermostat Sim CH Enable", true };
    inline static BoolSetting dhwEnable_{ "tsim.dhw", "Thermostat Sim DHW Enable", false };
    inline static UInt32Setting interval_{ "tsim.interval", "Thermostat Sim Interval (ms)", 1000 };
    inline static UInt32Setting memberId_{ "tsim.member", "Thermostat Sim Member Id", 0 };
};
