#include "CoolPlugSimulator.h"
#include "ModbusFrame.h"
#include "SettingsManager.h"
#include "ReplyWriter.h"
#include "esp_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace modbus;

namespace {

// What the CoolPlug config tool reported for a real one: firmware 0.1.9, which register
// 33 encodes as major * 100 + minor * 10 + subminor.
constexpr uint16_t FIRMWARE_VERSION = 19;
constexpr const char* FIRMWARE_TEXT = "0.1.9";

// A real serial is 0x03D0XXXX (PRM 2.1); these are recognisably the testbench's.
constexpr uint32_t SERIAL_BASE = 0x03D05100;

constexpr uint8_t MAX_ADDRESS = 247;
constexpr uint32_t MAX_DELAY_MS = 5000;

// How long a restart keeps it off the bus. A guess at a real one's boot.
constexpr uint32_t RESTART_MS = 3000;

constexpr uint32_t ROOM_MOVE_MS = 10000;
constexpr int8_t ROOM_STEP_MAX = 5;   // tenths: the room is the setting ±0.1..0.5 °C

// Setpoints taken over Modbus, °C x10. The PRM gives no range; this is a guess at what
// an indoor unit takes, and anything outside it is refused with Illegal Data Value.
constexpr int16_t SETPOINT_MIN = 100;
constexpr int16_t SETPOINT_MAX = 350;

// The VAUT changeover threshold, °C x10. Also a guess: 10 °C is a lot of hysteresis.
constexpr uint16_t VAUT_MAX = 100;

// Read Device Identification, basic objects 0-2. Conformity 0x81: basic, plus access to
// one object at a time.
constexpr uint8_t DEVICE_ID_CONFORMITY = 0x81;
constexpr const char* DEVICE_ID_OBJECTS[] = { "CoolAutomation", "CoolPlug", FIRMWARE_TEXT };
constexpr uint8_t DEVICE_ID_COUNT = 3;

bool Due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

} // namespace

// ── Settings ─────────────────────────────────────────────────

struct CoolPlugSimulator::Settings
{
    BoolSetting enabled;
    UInt32Setting address;
    FloatSetting room;
    UInt32Setting failure;
    BoolSetting indoor;
    BoolSetting answers;
    UInt32Setting delay;
};

// Keys at most 15 characters (NVS): "cp10.answer" is 11.
#define COOLPLUG_SETTINGS(n)                                                         \
    {                                                                                \
        { "cp" #n ".en", "CoolPlug Sim " #n " On The Bus", false },                  \
        { "cp" #n ".addr", "CoolPlug Sim " #n " Modbus Address", n },                \
        { "cp" #n ".room", "CoolPlug Sim " #n " Room Temperature", 21.0f },          \
        { "cp" #n ".fail", "CoolPlug Sim " #n " HVAC Failure Code (0 OK)", 0 },      \
        { "cp" #n ".indoor", "CoolPlug Sim " #n " Indoor Unit Connected", true },    \
        { "cp" #n ".answer", "CoolPlug Sim " #n " Answers Requests", true },         \
        { "cp" #n ".delay", "CoolPlug Sim " #n " Reply Delay (ms)", 0 },             \
    }

CoolPlugSimulator::Settings* CoolPlugSimulator::Table()
{
    static Settings table[UNITS] = {
        COOLPLUG_SETTINGS(1), COOLPLUG_SETTINGS(2), COOLPLUG_SETTINGS(3), COOLPLUG_SETTINGS(4),
        COOLPLUG_SETTINGS(5), COOLPLUG_SETTINGS(6), COOLPLUG_SETTINGS(7), COOLPLUG_SETTINGS(8),
        COOLPLUG_SETTINGS(9), COOLPLUG_SETTINGS(10),
    };
    return table;
}

#undef COOLPLUG_SETTINGS

void CoolPlugSimulator::RegisterSettings(SettingsManager& settings)
{
    // Back to front: each registration goes in front of the ones before it, and the
    // settings list reads best from "on the bus" down.
    Settings* table = Table();
    for (int i = UNITS - 1; i >= 0; --i)
    {
        Settings& s = table[i];
        settings.Register({ &s.delay, &s.answers, &s.indoor, &s.failure, &s.room, &s.address, &s.enabled });
    }
}

void CoolPlugSimulator::Init(uint8_t index, SettingsManager& settings)
{
    index_ = index;
    settings_ = &Table()[index];
    manager_ = &settings;
    ReadSettings();
}

void CoolPlugSimulator::Refresh()
{
    if (settings_ == nullptr)
        return;

    // Off the bus, that is all it needs to know. Every read is a flash read, which on
    // the ESP32 stalls both cores for a moment, and ten units read every second add up.
    if (!settings_->enabled.Get())
    {
        LOCK(mutex_);
        config_.enabled = false;
        return;
    }
    ReadSettings();
}

void CoolPlugSimulator::ReadSettings()
{
    // Read outside the lock: NVS is slow next to a command waiting for a report.
    Config c;
    c.enabled = settings_->enabled.Get();
    const uint32_t address = settings_->address.Get();
    // An address no master can use takes it off the bus rather than wrapping round.
    c.address = (address >= 1 && address <= MAX_ADDRESS) ? static_cast<uint8_t>(address) : 0;
    c.room = settings_->room.Get();
    c.failure = static_cast<uint16_t>(settings_->failure.Get());
    c.indoor = settings_->indoor.Get();
    c.answers = settings_->answers.Get();
    const uint32_t delay = settings_->delay.Get();
    c.delayMs = delay < MAX_DELAY_MS ? delay : MAX_DELAY_MS;

    LOCK(mutex_);
    config_ = c;
}

// ── Time passing ─────────────────────────────────────────────

void CoolPlugSimulator::Tick(uint32_t nowMs)
{
    LOCK(mutex_);
    if (restarting_ && Due(nowMs, restartUntilMs_))
    {
        restarting_ = false;
        if (pendingAddress_ != 0)
        {
            // A real one keeps the address it was given, so this does too.
            settings_->address.Set(pendingAddress_);
            manager_->Save();
            config_.address = pendingAddress_;
            pendingAddress_ = 0;
        }
    }

    if (Due(nowMs, nextMoveMs_))
    {
        const uint32_t r = esp_random();
        const int8_t step = static_cast<int8_t>(1 + r % ROOM_STEP_MAX);
        roomOffset_ = (r & 0x100) ? step : static_cast<int8_t>(-step);
        nextMoveMs_ = nowMs + ROOM_MOVE_MS;
    }
}

int16_t CoolPlugSimulator::RoomTenths() const
{
    return static_cast<int16_t>(lroundf(config_.room * 10.0f) + roomOffset_);
}

uint32_t CoolPlugSimulator::Serial() const
{
    return SERIAL_BASE + index_ + 1;
}

CoolPlugSimulator::Demand CoolPlugSimulator::CurrentDemand() const
{
    if (!state_.power || !config_.indoor)
        return DEMAND_NONE;

    const int16_t room = RoomTenths();
    const int16_t set = state_.setpoint;
    switch (state_.mode)
    {
    case MODE_HEAT:
    case MODE_HAUX:
        return room < set ? DEMAND_HEATING : DEMAND_NONE;
    case MODE_COOL:
    case MODE_DRY:
        return room > set ? DEMAND_COOLING : DEMAND_NONE;
    case MODE_AUTO:
        return room < set ? DEMAND_HEATING : room > set ? DEMAND_COOLING : DEMAND_NONE;
    default:
        return DEMAND_NONE;   // fan only
    }
}

// ── The bus ──────────────────────────────────────────────────

bool CoolPlugSimulator::Serves(uint8_t unit, uint32_t)
{
    LOCK(mutex_);
    if (!config_.enabled || config_.address == 0 || restarting_)
        return false;
    return unit == BROADCAST || unit == config_.address;
}

bool CoolPlugSimulator::Answers()
{
    LOCK(mutex_);
    return config_.answers;
}

uint32_t CoolPlugSimulator::ReplyDelayMs()
{
    LOCK(mutex_);
    return config_.delayMs;
}

size_t CoolPlugSimulator::Handle(const uint8_t* pdu, size_t len, bool broadcast, uint8_t* reply, uint32_t nowMs)
{
    LOCK(mutex_);
    counters_.requests++;
    counters_.lastRequestMs = nowMs;

    // The PRM lists four functions; anything else is refused, as the protocol has it.
    size_t n = 0;
    switch (pdu[0])
    {
    case fc::READ_HOLDING_REGISTERS:   n = ReadHolding(pdu, len, reply); break;
    case fc::WRITE_SINGLE_REGISTER:    n = WriteSingle(pdu, len, reply, nowMs); break;
    case fc::WRITE_MULTIPLE_REGISTERS: n = WriteMultiple(pdu, len, reply, nowMs); break;
    case fc::ENCAPSULATED:             n = ReadDeviceId(pdu, len, reply); break;
    default:                           n = Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply); break;
    }

    if (n > 0 && (reply[0] & fc::EXCEPTION))
        counters_.exceptions++;
    // A broadcast is carried out, never answered.
    return broadcast ? 0 : n;
}

size_t CoolPlugSimulator::Exception(uint8_t function, uint8_t code, uint8_t* reply)
{
    reply[0] = function | fc::EXCEPTION;
    reply[1] = code;
    return 2;
}

bool CoolPlugSimulator::Read(uint16_t reg, uint16_t& value) const
{
    switch (reg)
    {
    case REG_RESET:          value = 0; return true;   // write-only in the PRM; read as 0 (a guess)
    case REG_VAUT_THRESHOLD: value = state_.vautThreshold; return true;
    case REG_VAUT_AS_AUTO:   value = state_.vautAsAuto; return true;
    case REG_HVAC_TYPE:      value = 0; return true;   // "internal use"; a real one's value is unknown
    case REG_FIRMWARE:       value = FIRMWARE_VERSION; return true;
    case REG_SERIAL_LOW:     value = Serial() & 0xFFFF; return true;
    case REG_SERIAL_HIGH:    value = Serial() >> 16; return true;
    case REG_SLAVE_ADDRESS:  value = config_.address; return true;
    case REG_LINE_ROLE:      value = state_.lineRole; return true;
    case REG_INDOOR_ADDRESS: value = 0; return true;   // the config tool shows L1.000; encoding unknown

    case REG_LINE_STATE:
    case REG_LINE_STATE_ALT:
        // bit 0: connected to the indoor unit; bit 2: the indoor unit reports a failure.
        value = (config_.indoor ? 0x01 : 0) | (config_.failure != 0 ? 0x04 : 0);
        return true;

    case REG_COMPACT_STATUS:
        value = (state_.power ? 1 : 0)
              | (state_.mode << 1)
              | (state_.fan << 6)
              | (state_.swing << 10)
              | (state_.filter ? 1 << 13 : 0)
              | (CurrentDemand() != DEMAND_NONE ? 1 << 14 : 0);   // therm on
        return true;

    case REG_SETPOINT:
    case REG_SETPOINT_ALT:    value = static_cast<uint16_t>(state_.setpoint); return true;
    case REG_AMBIENT:
    case REG_AMBIENT_ALT:     value = static_cast<uint16_t>(RoomTenths()); return true;
    case REG_FAILURE:
    case REG_FAILURE_ALT:     value = config_.failure; return true;
    case REG_OWN_AMBIENT:
    case REG_OWN_AMBIENT_ALT: value = static_cast<uint16_t>(state_.ownAmbient); return true;
    case REG_ON_OFF:          value = state_.power ? 1 : 0; return true;
    case REG_MODE:            value = state_.mode; return true;
    case REG_FAN:             value = state_.fan; return true;
    case REG_SWING:           value = state_.swing; return true;
    case REG_FILTER:          value = state_.filter ? 1 : 0; return true;
    case REG_OPERATION:       value = CurrentDemand(); return true;
    default:                  return false;
    }
}

uint8_t CoolPlugSimulator::CheckWrite(uint16_t reg, uint16_t value) const
{
    const auto valid = [](bool ok) { return ok ? 0 : ex::ILLEGAL_DATA_VALUE; };
    switch (reg)
    {
    case REG_RESET:           return valid(value == 1 || value == 2);
    case REG_VAUT_THRESHOLD:  return valid(value <= VAUT_MAX);
    case REG_VAUT_AS_AUTO:    return valid(value <= 1);
    case REG_SLAVE_ADDRESS:   return valid(value >= 1 && value <= MAX_ADDRESS);
    case REG_LINE_ROLE:       return valid(value <= 2);
    case REG_SETPOINT:
    case REG_SETPOINT_ALT: {
        const int16_t t = static_cast<int16_t>(value);
        return valid(t >= SETPOINT_MIN && t <= SETPOINT_MAX);
    }
    case REG_OWN_AMBIENT:
    case REG_OWN_AMBIENT_ALT:
    case REG_ON_OFF:          return 0;   // any value: 0 off, anything else on
    case REG_MODE:            return valid(value < MODE_COUNT);
    case REG_FAN:             return valid(value < FAN_COUNT);
    case REG_SWING:           return valid(value < SWING_COUNT);
    case REG_FILTER:          return valid(value <= 1);
    default:                  return ex::ILLEGAL_DATA_ADDRESS;   // read-only, or not there
    }
}

void CoolPlugSimulator::Write(uint16_t reg, uint16_t value, uint32_t nowMs)
{
    counters_.writes++;
    switch (reg)
    {
    case REG_RESET:
        // 1 is "enter boot mode" on a real one. There is no bootloader to enter here, so
        // both restart.
        restarting_ = true;
        restartUntilMs_ = nowMs + RESTART_MS;
        break;
    case REG_VAUT_THRESHOLD:  state_.vautThreshold = (value + 2) / 5 * 5; break;   // to 0.5 °C
    case REG_VAUT_AS_AUTO:    state_.vautAsAuto = value != 0; break;
    case REG_SLAVE_ADDRESS:   pendingAddress_ = static_cast<uint8_t>(value); break;   // after a reset
    case REG_LINE_ROLE:       state_.lineRole = static_cast<uint8_t>(value); break;
    case REG_SETPOINT:
    case REG_SETPOINT_ALT:    state_.setpoint = static_cast<int16_t>(value); break;
    case REG_OWN_AMBIENT:
    case REG_OWN_AMBIENT_ALT: state_.ownAmbient = static_cast<int16_t>(value); break;
    case REG_ON_OFF:          state_.power = value != 0; break;
    case REG_MODE:            state_.mode = static_cast<uint8_t>(value); break;
    case REG_FAN:             state_.fan = static_cast<uint8_t>(value); break;
    case REG_SWING:           state_.swing = static_cast<uint8_t>(value); break;
    case REG_FILTER:          state_.filter = value != 0; break;
    default:                  break;
    }
}

size_t CoolPlugSimulator::ReadHolding(const uint8_t* pdu, size_t len, uint8_t* reply)
{
    if (len != 5)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint32_t start = Get16(pdu + 1);
    const uint16_t count = Get16(pdu + 3);
    if (count < 1 || count > 125)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    // A range with a gap in it is refused whole, as Modbus has it. Whether a real
    // CoolPlug does the same, or reads the gap as 0, is not known.
    for (uint16_t i = 0; i < count; ++i)
    {
        uint16_t value = 0;
        if (start + i > 0xFFFF || !Read(static_cast<uint16_t>(start + i), value))
            return Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        Put16(reply + 2 + 2 * i, value);
    }
    reply[0] = pdu[0];
    reply[1] = static_cast<uint8_t>(count * 2);
    return 2 + 2 * count;
}

size_t CoolPlugSimulator::WriteSingle(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs)
{
    if (len != 5)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint16_t reg = Get16(pdu + 1);
    const uint16_t value = Get16(pdu + 3);
    if (const uint8_t code = CheckWrite(reg, value))
        return Exception(pdu[0], code, reply);

    Write(reg, value, nowMs);
    // "CoolPlug does not respond to the write request into this register."
    if (reg == REG_RESET)
        return 0;
    memcpy(reply, pdu, 5);
    return 5;
}

size_t CoolPlugSimulator::WriteMultiple(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs)
{
    if (len < 6)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint32_t start = Get16(pdu + 1);
    const uint16_t count = Get16(pdu + 3);
    const uint8_t bytes = pdu[5];
    if (count < 1 || count > 123 || bytes != count * 2 || len != 6u + bytes)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    // All or nothing: every register is checked before the first is written.
    for (uint16_t i = 0; i < count; ++i)
    {
        if (start + i > 0xFFFF)
            return Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        if (const uint8_t code = CheckWrite(static_cast<uint16_t>(start + i), Get16(pdu + 6 + 2 * i)))
            return Exception(pdu[0], code, reply);
    }

    bool reset = false;
    for (uint16_t i = 0; i < count; ++i)
    {
        const uint16_t reg = static_cast<uint16_t>(start + i);
        Write(reg, Get16(pdu + 6 + 2 * i), nowMs);
        reset |= reg == REG_RESET;
    }
    if (reset)
        return 0;

    memcpy(reply, pdu, 5);   // function, start, count
    return 5;
}

size_t CoolPlugSimulator::ReadDeviceId(const uint8_t* pdu, size_t len, uint8_t* reply)
{
    if (len != 4)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    if (pdu[1] != MEI_READ_DEVICE_ID)
        return Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);

    const uint8_t code = pdu[2];
    uint8_t first = pdu[3];
    if (code < 1 || code > 4)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    uint8_t last = DEVICE_ID_COUNT - 1;
    if (code == 4)
    {
        // One object, which has to exist.
        if (first >= DEVICE_ID_COUNT)
            return Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        last = first;
    }
    else if (first >= DEVICE_ID_COUNT)
    {
        // A stream from an object that does not exist starts again at the first.
        first = 0;
    }

    // Asked for regular or extended, a basic device answers with what it has.
    reply[0] = pdu[0];
    reply[1] = MEI_READ_DEVICE_ID;
    reply[2] = code;
    reply[3] = DEVICE_ID_CONFORMITY;
    reply[4] = 0;   // nothing more follows
    reply[5] = 0;   // next object
    reply[6] = static_cast<uint8_t>(last - first + 1);
    size_t n = 7;
    for (uint8_t id = first; id <= last; ++id)
    {
        const size_t textLen = strlen(DEVICE_ID_OBJECTS[id]);
        reply[n++] = id;
        reply[n++] = static_cast<uint8_t>(textLen);
        memcpy(reply + n, DEVICE_ID_OBJECTS[id], textLen);
        n += textLen;
    }
    return n;
}

// ── The remote, and the report ───────────────────────────────

bool CoolPlugSimulator::Apply(const Change& change, const char*& error)
{
    if (change.setpoint >= 0 && (change.setpoint < SETPOINT_MIN || change.setpoint > SETPOINT_MAX))
    {
        error = "setpoint out of range (100 to 350, tenths of a °C)";
        return false;
    }
    if (change.mode >= MODE_COUNT) { error = "unknown mode (0 to 5)"; return false; }
    if (change.fan >= FAN_COUNT) { error = "unknown fan speed (0 to 5)"; return false; }
    if (change.swing >= SWING_COUNT) { error = "unknown louvre position (0 to 7)"; return false; }

    LOCK(mutex_);
    if (change.power >= 0)  state_.power = change.power != 0;
    if (change.mode >= 0)   state_.mode = static_cast<uint8_t>(change.mode);
    if (change.fan >= 0)    state_.fan = static_cast<uint8_t>(change.fan);
    if (change.swing >= 0)  state_.swing = static_cast<uint8_t>(change.swing);
    if (change.filter >= 0) state_.filter = change.filter != 0;
    if (change.setpoint >= 0) state_.setpoint = static_cast<int16_t>(change.setpoint);
    return true;
}

void CoolPlugSimulator::Report(ReplyObject& out, uint32_t nowMs)
{
    LOCK(mutex_);
    char serial[9];
    snprintf(serial, sizeof(serial), "%08lX", static_cast<unsigned long>(Serial()));

    out.field("unit", static_cast<uint32_t>(index_ + 1));
    out.field("enabled", config_.enabled);
    out.field("address", static_cast<uint32_t>(config_.address));
    out.field("answers", config_.answers);
    out.field("indoor", config_.indoor);
    out.field("delayMs", config_.delayMs);
    out.field("restarting", restarting_);
    if (pendingAddress_ != 0)
        out.field("pendingAddress", static_cast<uint32_t>(pendingAddress_));
    out.field("serial", serial);
    out.field("firmware", FIRMWARE_TEXT);

    out.field("power", state_.power);
    out.field("mode", static_cast<uint32_t>(state_.mode));
    out.field("setpoint", state_.setpoint / 10.0f);
    out.field("room", RoomTenths() / 10.0f);
    out.field("fan", static_cast<uint32_t>(state_.fan));
    out.field("swing", static_cast<uint32_t>(state_.swing));
    out.field("filter", state_.filter);
    out.field("failure", static_cast<uint32_t>(config_.failure));
    out.field("demand", static_cast<uint32_t>(CurrentDemand()));
    out.field("ownAmbient", state_.ownAmbient / 10.0f);
    out.field("vautThreshold", state_.vautThreshold / 10.0f);
    out.field("lineRole", static_cast<uint32_t>(state_.lineRole));

    out.field("requests", counters_.requests);
    out.field("writes", counters_.writes);
    out.field("exceptions", counters_.exceptions);
    if (counters_.requests > 0)
        out.field("lastRequestAgoMs", nowMs - counters_.lastRequestMs);
}
