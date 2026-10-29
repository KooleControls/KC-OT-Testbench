#pragma once

#include "TypedSettings.h"
#include "Mutex.h"
#include <cstddef>
#include <cstdint>

class SettingsManager;
class ReplyObject;

// ──────────────────────────────────────────────────────────────
// A CoolPlug that is not a device: a CoolAutomation CoolPlug, with the indoor unit
// behind it, as a Modbus RTU slave on the testbench's RS485 port. Meant to be
// indistinguishable on the bus from a real one, so a gateway configured for CoolPlugs
// can be tested against it (CoolPlug PRM rev 0.3, the register map in section 2.2).
//
// Two kinds of state, kept apart on purpose:
//   • the indoor unit's: power, mode, setpoint, fan, louvre, filter sign. Both the
//     gateway (over Modbus) and the unit's own remote change these on a real system,
//     so they live in RAM, start the same at every boot, and the web UI plays the
//     remote (`coolplug set`) -- the "Control (live)" of the CoolPlug config tool;
//   • the simulation's: whether it is on the bus at all, its address, the room
//     temperature, a failure code, and the faults a test wants (indoor unit
//     disconnected, not answering, answering late). Those are settings, cpN.*, read
//     again every second.
//
// The room temperature is a set value that moves: every 10 s it is the setting plus or
// minus 0.1 to 0.5 °C, so a gateway reading it keeps seeing a living value. It comes
// from one place (RoomTenths), for a real source to take over later.
//
// What the PRM does not say is guessed, and said so where it is done: what an unmapped
// register or an out-of-range value gets, and what the HVAC type and the indoor
// centralised address read. A real CoolPlug on the bus settles those.
//
// Driven by the Rs485Manager's task; the lock is for the command handlers, which run on
// whichever task serves the command.
// ──────────────────────────────────────────────────────────────
class CoolPlugSimulator
{
    static constexpr const char* TAG = "CoolPlugSim";

public:
    /// As many as a Smarthome gateway can drive.
    static constexpr uint8_t UNITS = 10;

    // Encodings, from the PRM.
    enum Mode : uint8_t { MODE_COOL, MODE_HEAT, MODE_AUTO, MODE_DRY, MODE_HAUX, MODE_FAN, MODE_COUNT };
    enum Fan : uint8_t { FAN_LOW, FAN_MEDIUM, FAN_HIGH, FAN_AUTO, FAN_TOP, FAN_VERY_LOW, FAN_COUNT };
    enum Swing : uint8_t { SWING_VERTICAL, SWING_30, SWING_45, SWING_60, SWING_HORIZONTAL, SWING_AUTO, SWING_OFF, SWING_NO, SWING_COUNT };
    enum Demand : uint8_t { DEMAND_NONE, DEMAND_HEATING, DEMAND_COOLING };

    /// Every unit's settings, cp1.* to cp10.*. Once, before any unit's Init().
    static void RegisterSettings(SettingsManager& settings);

    /// Bind it to the index'th unit's settings (0-based) and read them.
    void Init(uint8_t index, SettingsManager& settings);

    /// Read the settings again. The bus task does this every second.
    void Refresh();

    /// Time passing: the room temperature moving, a restart coming to its end.
    void Tick(uint32_t nowMs);

    /// Whether a request to `unit` is this one's: switched on, at that address, and not
    /// restarting. Broadcasts (0) are for every unit that is up.
    bool Serves(uint8_t unit, uint32_t nowMs);

    /// The faults a test can ask for: not answering at all, or answering late.
    bool Answers();
    uint32_t ReplyDelayMs();

    /// One request addressed to it: `pdu` from the function code on, without the unit or
    /// the CRC. Writes the reply's PDU to `reply` (room for modbus::MAX_PDU) and returns
    /// its length; 0 for none -- a broadcast, or a write to the reset register, which a
    /// CoolPlug does not answer.
    size_t Handle(const uint8_t* pdu, size_t len, bool broadcast, uint8_t* reply, uint32_t nowMs);

    /// What the remote would change, for `coolplug set`. A field left negative is left
    /// alone; the setpoint is in tenths of a °C, as the register holds it. False, with
    /// the reason, when a value is out of range.
    struct Change
    {
        int32_t power = -1, mode = -1, fan = -1, swing = -1, filter = -1, setpoint = -1;
    };
    bool Apply(const Change& change, const char*& error);

    /// Everything about it, for `coolplug status`.
    void Report(ReplyObject& out, uint32_t nowMs);

private:
    // ── Wire addresses: the PRM's table numbers minus one (its footnote: on the wire,
    // register addresses start from 0). The gateway uses these numbers. ──
    enum Register : uint16_t
    {
        REG_RESET = 1,
        REG_VAUT_THRESHOLD = 29,
        REG_VAUT_AS_AUTO = 30,
        REG_HVAC_TYPE = 31,
        REG_FIRMWARE = 32,
        REG_SERIAL_LOW = 33,
        REG_SERIAL_HIGH = 34,
        REG_SLAVE_ADDRESS = 35,
        REG_LINE_ROLE = 36,
        REG_INDOOR_ADDRESS = 37,
        REG_LINE_STATE = 95,
        REG_COMPACT_STATUS = 96,
        REG_SETPOINT = 97,
        REG_AMBIENT = 98,
        REG_FAILURE = 99,
        REG_OWN_AMBIENT = 100,
        // The same again, one value per register (the PRM's "alternative" block).
        REG_LINE_STATE_ALT = 143,
        REG_ON_OFF = 144,
        REG_MODE = 145,
        REG_FAN = 146,
        REG_SWING = 147,
        REG_FILTER = 148,
        REG_SETPOINT_ALT = 149,
        REG_AMBIENT_ALT = 150,
        REG_FAILURE_ALT = 151,
        REG_OWN_AMBIENT_ALT = 152,
        REG_OPERATION = 153,
    };

    // One unit's settings. Their keys and labels must be string literals, so the ten sets
    // are written out in the .cpp rather than built from the index.
    struct Settings;
    static Settings* Table();

    Mutex mutex_;
    Settings* settings_ = nullptr;
    SettingsManager* manager_ = nullptr;
    uint8_t index_ = 0;

    // The settings as last read.
    struct Config
    {
        bool enabled = false, indoor = true, answers = true;
        uint8_t address = 0;
        uint16_t failure = 0;
        uint32_t delayMs = 0;
        float room = 0;
    } config_;

    // The indoor unit, as the CoolPlug config tool showed a real one at rest.
    struct State
    {
        bool power = false;
        uint8_t mode = MODE_COOL;
        int16_t setpoint = 230;   // °C x10
        uint8_t fan = FAN_HIGH;
        uint8_t swing = SWING_30;
        bool filter = false;
        int16_t ownAmbient = 0;   // °C x10, what a controller feeds it
        // The CoolPlug's own configuration.
        uint16_t vautThreshold = 0;   // °C x10 in 0.5 steps, 0 off
        bool vautAsAuto = false;
        uint8_t lineRole = 1;         // on the HVAC line: 0 slave, 1 master, 2 auto
    } state_;

    // The room temperature's movement: tenths of a degree off the setting, and when it
    // moves next.
    int8_t roomOffset_ = 0;
    uint32_t nextMoveMs_ = 0;

    // A restart (the reset register) keeps it off the bus until restartUntilMs_; an
    // address written to register 36 takes effect then.
    bool restarting_ = false;
    uint32_t restartUntilMs_ = 0;
    uint8_t pendingAddress_ = 0;

    struct Counters
    {
        uint32_t requests = 0, writes = 0, exceptions = 0, lastRequestMs = 0;
    } counters_;

    void ReadSettings();
    uint32_t Serial() const;
    int16_t RoomTenths() const;
    Demand CurrentDemand() const;
    bool Read(uint16_t reg, uint16_t& value) const;
    uint8_t CheckWrite(uint16_t reg, uint16_t value) const;
    void Write(uint16_t reg, uint16_t value, uint32_t nowMs);
    size_t ReadHolding(const uint8_t* pdu, size_t len, uint8_t* reply);
    size_t WriteSingle(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs);
    size_t WriteMultiple(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs);
    size_t ReadDeviceId(const uint8_t* pdu, size_t len, uint8_t* reply);
    size_t Exception(uint8_t function, uint8_t code, uint8_t* reply);
};
