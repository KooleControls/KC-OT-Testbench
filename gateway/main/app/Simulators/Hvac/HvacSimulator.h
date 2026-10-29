#pragma once

#include "HvacProfile.h"
#include "TypedSettings.h"
#include "Mutex.h"
#include <cstddef>
#include <cstdint>

class SettingsManager;
class ReplyObject;

// ──────────────────────────────────────────────────────────────
// One simulated HVAC unit: an indoor unit with the interface a gateway talks to in front
// of it -- a CoolPlug, an Intesis box, a Hisense adaptor -- answering as that one would.
// There are ten, units 1 to 10, set up as the gateway's HVAC list is: a type, by the
// gateway's numbers, and an address. The type's profile (HvacProfile.h) is what makes
// it that type; this is what every type shares.
//
// Two kinds of state, kept apart on purpose:
//   • the indoor unit's: power, mode, setpoint, fan, louvres, filter sign, remote lock.
//     Both the gateway (over Modbus) and the unit's own remote change these on a real
//     system, so they live in RAM, start the same at every boot and at a change of type,
//     and the web UI plays the remote (`hvac set`);
//   • the simulation's: the type, the address, whether it answers at all, the room
//     temperature, a failure code, and the faults a test wants (indoor unit
//     disconnected, answering late). Those are settings, hvacN.*, read again every
//     second.
//
// The room temperature is a set value that moves: every 10 s it is the setting plus or
// minus 0.1 to 0.5 °C, so a gateway reading it keeps seeing a living value. It comes
// from one place (RoomTenths), for a real source to take over later.
//
// It does not know which bus it is on. Whoever serves a transport asks every unit
// whether a request is its (Serves), and the type says which transport reaches it.
//
// Driven by HvacSimManager's task (Refresh, Tick); the lock is for whichever task serves
// a request or a command.
// ──────────────────────────────────────────────────────────────
class HvacSimulator
{
    static constexpr const char* TAG = "HvacSim";

public:
    /// As many as a Smarthome gateway can drive.
    static constexpr uint8_t UNITS = 10;

    /// Every unit's settings, hvac1.* to hvac10.*. Once, before any unit's Init().
    static void RegisterSettings(SettingsManager& settings);

    /// Bind it to the index'th unit's settings (0-based) and read them.
    void Init(uint8_t index, SettingsManager& settings);

    /// Read the settings again.
    void Refresh();

    /// Time passing: the room temperature moving, a restart coming to its end.
    void Tick(uint32_t nowMs);

    /// Whether a request to `unit` over `transport` is this one's: a type that is
    /// simulated and reached that way, at that address, and not restarting. Broadcasts
    /// (0) are for every unit that is up.
    bool Serves(HvacTransport transport, uint8_t unit);

    /// The faults a test can ask for: not answering at all, or answering late.
    bool Answers();
    uint32_t ReplyDelayMs();

    /// One request addressed to it: `pdu` from the function code on, without the unit or
    /// the CRC. Writes the reply's PDU to `reply` (room for modbus::MAX_PDU) and returns
    /// its length; 0 for none -- a broadcast, or a write the type does not answer.
    size_t Handle(const uint8_t* pdu, size_t len, bool broadcast, uint8_t* reply, uint32_t nowMs);

    /// What the remote would change, for `hvac set` and `hvac filter`, in the type's own codes. A field
    /// left negative is left alone; the setpoint is in tenths of a °C. False, with the
    /// reason, when the type has no such value or the gateway locked the remote.
    struct Change
    {
        int32_t power = -1, mode = -1, fan = -1, swing = -1, filter = -1, setpoint = -1;
    };
    bool Apply(const Change& change, const char*& error);

    /// Everything about it, for `hvac status`.
    void Report(ReplyObject& out, uint32_t nowMs);

private:
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
        uint32_t type = 0;   // the gateway's number
        bool indoor = true, answers = true;
        uint8_t address = 0;
        uint16_t failure = 0;
        uint32_t delayMs = 0;
        float room = 0;
    } config_;

    // The type those settings name; nullptr for a number the gateway does not have.
    const HvacTypeInfo* type_ = nullptr;
    HvacState state_;

    // The room temperature's movement: tenths of a degree off the setting, and when it
    // moves next.
    int8_t roomOffset_ = 0;
    uint32_t nextMoveMs_ = 0;

    // A restart keeps it off the bus until restartUntilMs_; an address written for after
    // it (state_.newAddress) is taken then.
    bool restarting_ = false;
    uint32_t restartUntilMs_ = 0;

    struct Counters
    {
        uint32_t requests = 0, writes = 0, exceptions = 0, lastRequestMs = 0;
    } counters_;

    const HvacProfile* Profile() const { return type_ != nullptr ? type_->profile : nullptr; }
    void ReadSettings();
    void TakeType();
    void Derive();
    int16_t RoomTenths() const;
    HvacDemand CurrentDemand() const;

    size_t ReadBits(ModbusTable table, const uint8_t* pdu, size_t len, uint8_t* reply);
    size_t ReadWords(ModbusTable table, const uint8_t* pdu, size_t len, uint8_t* reply);
    size_t WriteCoil(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs);
    size_t WriteRegister(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs);
    size_t WriteCoils(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs);
    size_t WriteRegisters(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs);
    bool Store(ModbusTable table, uint16_t address, uint16_t value, uint32_t nowMs);
    size_t Exception(uint8_t function, uint8_t code, uint8_t* reply);
};
