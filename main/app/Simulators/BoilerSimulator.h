#pragma once

#include "TypedSettings.h"
#include "Mutex.h"
#include "OpenThermFrame.h"
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
// It can also decide heating or cooling for the thermostat, the way the KC plug-in
// board (PCB1246, 2020 firmware) does: id 3 says the boiler decides, and the CH mode
// and cooling bits of the id 0 answer say which. When that changes, it holds the room
// setpoint the thermostat had before the switch by answering id 9 (remote override
// room setpoint) with it three times and then 0 four times, so the thermostat does not
// come back on its other mode's setpoint.
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

    // Who decides between heating and cooling: the bsim.mode setting.
    enum class Mode : uint8_t { Thermostat = 0, Heat = 1, Cool = 2, Off = 3 };

    // What the thermostat has told it; `known` says which of these it has heard yet.
    struct Heard
    {
        uint32_t requests = 0, unknownIds = 0;
        uint32_t lastRequestMs = 0, lastConfigReadMs = 0;
        uint16_t known = 0;
        uint8_t masterFlags = 0, memberId = 0;
        float controlSetpoint = 0, roomSetpoint = 0, roomTemp = 0, maxModulation = 0, otVersion = 0;
        float coolingControl = 0;
    } heard_;

    static constexpr uint16_t KNOWN_STATUS = 1 << 0;
    static constexpr uint16_t KNOWN_CONTROL = 1 << 1;
    static constexpr uint16_t KNOWN_ROOM_SETPOINT = 1 << 2;
    static constexpr uint16_t KNOWN_ROOM_TEMP = 1 << 3;
    static constexpr uint16_t KNOWN_MAX_MODULATION = 1 << 4;
    static constexpr uint16_t KNOWN_MEMBER = 1 << 5;
    static constexpr uint16_t KNOWN_VERSION = 1 << 6;
    static constexpr uint16_t KNOWN_CONFIG_READ = 1 << 7;   // it has asked for id 3
    static constexpr uint16_t KNOWN_COOLING_CONTROL = 1 << 8;

    // The setpoint being held across a mode change, answered on id 9. The counts are of
    // id 9 reads, not time: the thermostat paces it.
    struct Hold
    {
        bool active = false;
        uint8_t sends = 0, resets = 0;
        float value = 0;
    } hold_;

    // The mode bits of the last id 0 answer, with SLAVE_DECIDES_HEAT_COOL when it was
    // forced; 0xFF before the first. A change while forcing is a mode change.
    uint8_t lastModeAnswer_ = 0xFF;

    static Mode CurrentMode();
    uint8_t SlaveFlags() const;
    void BeginHold();
    float NextOverride();
    uint16_t Read(uint8_t dataId, uint16_t value, bool& known);
    bool Write(uint8_t dataId, uint16_t value);

    // ── Settings (keys at most 15 characters) ──
    inline static FloatSetting waterTemp_{ "bsim.water", "Boiler Sim Water Temperature", 54.0f };
    inline static FloatSetting modulation_{ "bsim.mod", "Boiler Sim Modulation Level (%)", 45.0f };
    inline static BoolSetting flame_{ "bsim.flame", "Boiler Sim Flame", true };
    inline static BoolSetting chActive_{ "bsim.ch", "Boiler Sim CH Active", true };
    inline static BoolSetting coolActive_{ "bsim.cool", "Boiler Sim Cooling Active", false };
    inline static BoolSetting dhwActive_{ "bsim.dhw", "Boiler Sim DHW Active", false };
    inline static UInt32Setting maxCapacity_{ "bsim.maxkw", "Boiler Sim Max Capacity (kW)", 24 };
    // Id 3's high byte as it goes on the line, so a test can answer with any combination
    // of slave configuration flags. Only the low byte is used, and its top bit is the
    // mode's (bsim.mode). The default is a modulating boiler with hot water.
    inline static UInt32Setting config_{ "bsim.config", "Boiler Sim Slave Config Flags (ID 3)", opentherm::SLAVE_DHW_PRESENT };
    inline static UInt32Setting memberId_{ "bsim.member", "Boiler Sim Member Id", 0 };
    // 0: the thermostat chooses, and CH/cooling active are plain status (bsim.ch, bsim.cool).
    // 1 heat, 2 cool, 3 off: the boiler decides, and says so in id 3 and id 0.
    inline static UInt32Setting mode_{ "bsim.mode", "Boiler Sim Mode (0 thermostat, 1 heat, 2 cool, 3 off)", 0 };
    inline static BoolSetting holdSetpoint_{ "bsim.hold", "Boiler Sim Hold Setpoint On Mode Change", true };
};
