#pragma once

#include "TypedSettings.h"
#include "Mutex.h"
#include <cstdint>

class SettingsManager;
class ReplyObject;

// ──────────────────────────────────────────────────────────────
// A boiler that is not a device: an OpenTherm slave running on the testbench.
//
// It answers every request from whichever thermostat it is connected to, from its
// settings as they are at that moment: its status (CH, DHW, flame), water
// temperature, modulation, configuration and capacity. It accepts the writes a
// thermostat makes (flow setpoint, room temperature and setpoint, ...) and remembers
// them; anything else is answered Unknown-DataId, as the protocol has it.
//
// Driven entirely by the RoutingManager's task. The lock is only for Report(), which
// runs on whichever task serves the command.
// ──────────────────────────────────────────────────────────────
class BoilerSimulator
{
    static constexpr const char* TAG = "BoilerSim";

public:
    void RegisterSettings(SettingsManager& settings);

    /// The answer to one request from the thermostat.
    uint32_t Answer(uint32_t request, uint32_t nowMs);

    /// What it was asked and what it answers, for `sim status`.
    void Report(ReplyObject& out, uint32_t nowMs);

private:
    Mutex mutex_;

    // What the thermostat has told it; `known` says which of these it has heard yet.
    struct Heard
    {
        uint32_t requests = 0, unknownIds = 0;
        uint32_t lastRequestMs = 0;
        uint8_t known = 0;
        uint8_t masterFlags = 0, memberId = 0;
        float controlSetpoint = 0, roomSetpoint = 0, roomTemp = 0, maxModulation = 0, otVersion = 0;
    } heard_;

    static constexpr uint8_t KNOWN_STATUS = 1 << 0;
    static constexpr uint8_t KNOWN_CONTROL = 1 << 1;
    static constexpr uint8_t KNOWN_ROOM_SETPOINT = 1 << 2;
    static constexpr uint8_t KNOWN_ROOM_TEMP = 1 << 3;
    static constexpr uint8_t KNOWN_MAX_MODULATION = 1 << 4;
    static constexpr uint8_t KNOWN_MEMBER = 1 << 5;
    static constexpr uint8_t KNOWN_VERSION = 1 << 6;

    uint8_t SlaveFlags() const;
    uint16_t Read(uint8_t dataId, uint16_t value, bool& known);
    bool Write(uint8_t dataId, uint16_t value);

    // ── Settings (keys at most 15 characters) ──
    inline static FloatSetting waterTemp_{ "bsim.water", "Boiler Sim Water Temperature", 54.0f };
    inline static FloatSetting modulation_{ "bsim.mod", "Boiler Sim Modulation Level (%)", 45.0f };
    inline static BoolSetting flame_{ "bsim.flame", "Boiler Sim Flame", true };
    inline static BoolSetting chActive_{ "bsim.ch", "Boiler Sim CH Active", true };
    inline static BoolSetting dhwActive_{ "bsim.dhw", "Boiler Sim DHW Active", false };
    inline static UInt32Setting maxCapacity_{ "bsim.maxkw", "Boiler Sim Max Capacity (kW)", 24 };
    inline static BoolSetting dhwPresent_{ "bsim.dhwpresent", "Boiler Sim Has Hot Water", true };
    inline static BoolSetting modulating_{ "bsim.modulating", "Boiler Sim Modulating", true };
    inline static UInt32Setting memberId_{ "bsim.member", "Boiler Sim Member Id", 0 };
};
