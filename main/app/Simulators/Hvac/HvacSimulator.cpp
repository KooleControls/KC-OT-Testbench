#include "HvacSimulator.h"
#include "ModbusFrame.h"
#include "SettingsManager.h"
#include "ReplyWriter.h"
#include "esp_log.h"
#include "esp_random.h"
#include <cmath>
#include <cstring>

using namespace modbus;

namespace {

constexpr uint8_t MAX_ADDRESS = 247;
constexpr uint32_t MAX_DELAY_MS = 5000;

// How long a restart keeps it off the bus. A guess at a real CoolPlug's boot, the one
// type that restarts when asked to.
constexpr uint32_t RESTART_MS = 3000;

constexpr uint32_t ROOM_MOVE_MS = 10000;
constexpr int8_t ROOM_STEP_MAX = 5;   // tenths: the room is the setting ±0.1..0.5 °C

// The protocol's limits on how many points one request reads or writes.
constexpr uint16_t MAX_READ_BITS = 2000;
constexpr uint16_t MAX_READ_WORDS = 125;
constexpr uint16_t MAX_WRITE_BITS = 1968;
constexpr uint16_t MAX_WRITE_WORDS = 123;

// Write Single Coil's only two values.
constexpr uint16_t COIL_ON = 0xFF00;
constexpr uint16_t COIL_OFF = 0x0000;

bool Due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }

/// The table a point is in, as `hvac status` names it.
const char* TableCode(ModbusTable table)
{
    switch (table)
    {
    case ModbusTable::Coils:            return "c";
    case ModbusTable::DiscreteInputs:   return "d";
    case ModbusTable::HoldingRegisters: return "h";
    case ModbusTable::InputRegisters:   return "i";
    }
    return "?";
}

} // namespace

// ── Settings ─────────────────────────────────────────────────

struct HvacSimulator::Settings
{
    UInt32Setting type;
    UInt32Setting address;
    BoolSetting answers;
    FloatSetting room;
    UInt32Setting failure;
    BoolSetting indoor;
    UInt32Setting delay;
};

// Keys at most 15 characters (NVS): "hvac10.answer" is 13.
#define HVAC_SETTINGS(n)                                                                  \
    {                                                                                     \
        { "hvac" #n ".type", "HVAC Sim " #n " Type (gateway's number, 0 none)", 0 },      \
        { "hvac" #n ".addr", "HVAC Sim " #n " Modbus Address", n },                       \
        { "hvac" #n ".answer", "HVAC Sim " #n " On The Bus (answers)", true },            \
        { "hvac" #n ".room", "HVAC Sim " #n " Room Temperature", 21.0f },                 \
        { "hvac" #n ".fail", "HVAC Sim " #n " Failure Code (0 OK)", 0 },                  \
        { "hvac" #n ".indoor", "HVAC Sim " #n " Indoor Unit Connected", true },           \
        { "hvac" #n ".delay", "HVAC Sim " #n " Reply Delay (ms)", 0 },                    \
    }

HvacSimulator::Settings* HvacSimulator::Table()
{
    static Settings table[UNITS] = {
        HVAC_SETTINGS(1), HVAC_SETTINGS(2), HVAC_SETTINGS(3), HVAC_SETTINGS(4),
        HVAC_SETTINGS(5), HVAC_SETTINGS(6), HVAC_SETTINGS(7), HVAC_SETTINGS(8),
        HVAC_SETTINGS(9), HVAC_SETTINGS(10),
    };
    return table;
}

#undef HVAC_SETTINGS

void HvacSimulator::RegisterSettings(SettingsManager& settings)
{
    // Back to front: each registration goes in front of the ones before it, and the
    // settings list reads best from the type down.
    Settings* table = Table();
    for (int i = UNITS - 1; i >= 0; --i)
    {
        Settings& s = table[i];
        settings.Register({ &s.delay, &s.indoor, &s.failure, &s.room, &s.answers, &s.address, &s.type });
    }
}

void HvacSimulator::Init(uint8_t index, SettingsManager& settings)
{
    index_ = index;
    settings_ = &Table()[index];
    manager_ = &settings;
    // No type yet, so the first read takes whichever the setting holds.
    config_.type = UINT32_MAX;
    ReadSettings();
}

void HvacSimulator::Refresh()
{
    if (settings_ == nullptr)
        return;

    // Not simulated, its type is all it needs to know. Every read is a flash read, which
    // on the ESP32 stalls both cores for a moment, and ten units read every second add up.
    const uint32_t type = settings_->type.Get();
    const HvacTypeInfo* info = FindHvacType(type);
    if (info == nullptr || info->profile == nullptr)
    {
        LOCK(mutex_);
        if (type != config_.type)
        {
            config_.type = type;
            TakeType();
        }
        return;
    }
    ReadSettings();
}

void HvacSimulator::ReadSettings()
{
    // Read outside the lock: NVS is slow next to a request waiting for its answer.
    Config c;
    c.type = settings_->type.Get();
    const uint32_t address = settings_->address.Get();
    // An address no master can use takes it off the bus rather than wrapping round.
    c.address = (address >= 1 && address <= MAX_ADDRESS) ? static_cast<uint8_t>(address) : 0;
    c.answers = settings_->answers.Get();
    c.room = settings_->room.Get();
    c.failure = static_cast<uint16_t>(settings_->failure.Get());
    c.indoor = settings_->indoor.Get();
    const uint32_t delay = settings_->delay.Get();
    c.delayMs = delay < MAX_DELAY_MS ? delay : MAX_DELAY_MS;

    LOCK(mutex_);
    const bool retyped = c.type != config_.type;
    config_ = c;
    if (retyped)
        TakeType();
}

void HvacSimulator::TakeType()
{
    // Another type is another unit: its own codes, starting as that type does, and
    // nothing asked of it yet.
    type_ = FindHvacType(config_.type);
    state_ = HvacState{};
    if (const HvacProfile* profile = Profile())
    {
        profile->Reset(state_);
        ESP_LOGI(TAG, "Unit %u: %s", index_ + 1, type_->code);
    }
    restarting_ = false;
    counters_ = {};
}

// ── Time passing ─────────────────────────────────────────────

void HvacSimulator::Tick(uint32_t nowMs)
{
    LOCK(mutex_);
    if (restarting_ && Due(nowMs, restartUntilMs_))
    {
        restarting_ = false;
        if (state_.newAddress != 0)
        {
            // A real one keeps the address it was given, so this does too.
            settings_->address.Set(state_.newAddress);
            manager_->Save();
            config_.address = state_.newAddress;
            state_.newAddress = 0;
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

int16_t HvacSimulator::RoomTenths() const
{
    return static_cast<int16_t>(lroundf(config_.room * 10.0f) + roomOffset_);
}

void HvacSimulator::Derive()
{
    // The simulation as the profile reads it, as it is right now.
    const HvacProfile* profile = Profile();
    state_.index = index_;
    state_.address = config_.address;
    state_.room = RoomTenths();
    state_.failure = config_.failure;
    // Only a type that reports whether it reaches its indoor unit can lose it.
    state_.indoor = profile == nullptr || !profile->traits.indoorLink || config_.indoor;
    state_.demand = CurrentDemand();
}

HvacDemand HvacSimulator::CurrentDemand() const
{
    const HvacProfile* profile = Profile();
    if (profile == nullptr || !state_.power || !state_.indoor)
        return HvacDemand::None;

    const int16_t room = state_.room;
    const int16_t set = state_.setpoint;
    switch (profile->KindOf(state_.mode))
    {
    case ModeKind::Heat:
        return room < set ? HvacDemand::Heating : HvacDemand::None;
    case ModeKind::Cool:
    case ModeKind::Dry:
        return room > set ? HvacDemand::Cooling : HvacDemand::None;
    case ModeKind::Auto:
        return room < set ? HvacDemand::Heating : room > set ? HvacDemand::Cooling : HvacDemand::None;
    default:
        return HvacDemand::None;   // fan only
    }
}

// ── The bus ──────────────────────────────────────────────────

bool HvacSimulator::Serves(HvacTransport transport, uint8_t unit)
{
    LOCK(mutex_);
    if (Profile() == nullptr || type_->transport != transport || config_.address == 0 || restarting_)
        return false;
    return unit == BROADCAST || unit == config_.address;
}

bool HvacSimulator::Answers()
{
    LOCK(mutex_);
    return config_.answers;
}

uint32_t HvacSimulator::ReplyDelayMs()
{
    LOCK(mutex_);
    return config_.delayMs;
}

size_t HvacSimulator::Handle(const uint8_t* pdu, size_t len, bool broadcast, uint8_t* reply, uint32_t nowMs)
{
    LOCK(mutex_);
    const HvacProfile* profile = Profile();
    if (profile == nullptr)
        return 0;   // its type changed since Serves() said yes
    counters_.requests++;
    counters_.lastRequestMs = nowMs;
    Derive();

    // The eight functions that read and write the four tables are every type's; a type
    // without the table refuses them. Anything else is the type's own business.
    size_t n = 0;
    switch (pdu[0])
    {
    case fc::READ_COILS:               n = ReadBits(ModbusTable::Coils, pdu, len, reply); break;
    case fc::READ_DISCRETE_INPUTS:     n = ReadBits(ModbusTable::DiscreteInputs, pdu, len, reply); break;
    case fc::READ_HOLDING_REGISTERS:   n = ReadWords(ModbusTable::HoldingRegisters, pdu, len, reply); break;
    case fc::READ_INPUT_REGISTERS:     n = ReadWords(ModbusTable::InputRegisters, pdu, len, reply); break;
    case fc::WRITE_SINGLE_COIL:        n = WriteCoil(pdu, len, reply, nowMs); break;
    case fc::WRITE_SINGLE_REGISTER:    n = WriteRegister(pdu, len, reply, nowMs); break;
    case fc::WRITE_MULTIPLE_COILS:     n = WriteCoils(pdu, len, reply, nowMs); break;
    case fc::WRITE_MULTIPLE_REGISTERS: n = WriteRegisters(pdu, len, reply, nowMs); break;
    default:
        n = profile->Other(state_, pdu, len, reply);
        if (n == 0)
            n = Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
        break;
    }

    if (n > 0 && (reply[0] & fc::EXCEPTION))
        counters_.exceptions++;
    // A broadcast is carried out, never answered.
    return broadcast ? 0 : n;
}

size_t HvacSimulator::Exception(uint8_t function, uint8_t code, uint8_t* reply)
{
    reply[0] = function | fc::EXCEPTION;
    reply[1] = code;
    return 2;
}

size_t HvacSimulator::ReadBits(ModbusTable table, const uint8_t* pdu, size_t len, uint8_t* reply)
{
    const HvacProfile* profile = Profile();
    if (!profile->Has(table))
        return Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
    if (len != 5)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint32_t start = Get16(pdu + 1);
    const uint16_t count = Get16(pdu + 3);
    if (count < 1 || count > MAX_READ_BITS)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    // A range with a gap in it is refused whole, as Modbus has it. Whether a real unit
    // does the same, or reads the gap as 0, is not known.
    const size_t bytes = (count + 7) / 8;
    memset(reply + 2, 0, bytes);
    for (uint16_t i = 0; i < count; ++i)
    {
        uint16_t value = 0;
        if (start + i > 0xFFFF || !profile->Read(state_, table, static_cast<uint16_t>(start + i), value))
            return Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        if (value != 0)
            reply[2 + i / 8] |= static_cast<uint8_t>(1 << (i % 8));   // first point in the low bit
    }
    reply[0] = pdu[0];
    reply[1] = static_cast<uint8_t>(bytes);
    return 2 + bytes;
}

size_t HvacSimulator::ReadWords(ModbusTable table, const uint8_t* pdu, size_t len, uint8_t* reply)
{
    const HvacProfile* profile = Profile();
    if (!profile->Has(table))
        return Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
    if (len != 5)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint32_t start = Get16(pdu + 1);
    const uint16_t count = Get16(pdu + 3);
    if (count < 1 || count > MAX_READ_WORDS)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    // A gap refuses the range whole, as for bits.
    for (uint16_t i = 0; i < count; ++i)
    {
        uint16_t value = 0;
        if (start + i > 0xFFFF || !profile->Read(state_, table, static_cast<uint16_t>(start + i), value))
            return Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        Put16(reply + 2 + 2 * i, value);
    }
    reply[0] = pdu[0];
    reply[1] = static_cast<uint8_t>(count * 2);
    return 2 + 2 * count;
}

size_t HvacSimulator::WriteCoil(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs)
{
    const HvacProfile* profile = Profile();
    if (!profile->Has(ModbusTable::Coils))
        return Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
    if (len != 5)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint16_t address = Get16(pdu + 1);
    const uint16_t raw = Get16(pdu + 3);
    if (raw != COIL_ON && raw != COIL_OFF)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    const uint16_t value = raw == COIL_ON ? 1 : 0;
    if (const uint8_t code = profile->CheckWrite(state_, ModbusTable::Coils, address, value))
        return Exception(pdu[0], code, reply);
    if (!Store(ModbusTable::Coils, address, value, nowMs))
        return 0;
    memcpy(reply, pdu, 5);   // an echo
    return 5;
}

size_t HvacSimulator::WriteRegister(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs)
{
    const HvacProfile* profile = Profile();
    if (!profile->Has(ModbusTable::HoldingRegisters))
        return Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
    if (len != 5)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint16_t address = Get16(pdu + 1);
    const uint16_t value = Get16(pdu + 3);
    if (const uint8_t code = profile->CheckWrite(state_, ModbusTable::HoldingRegisters, address, value))
        return Exception(pdu[0], code, reply);
    if (!Store(ModbusTable::HoldingRegisters, address, value, nowMs))
        return 0;
    memcpy(reply, pdu, 5);   // an echo
    return 5;
}

size_t HvacSimulator::WriteCoils(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs)
{
    const HvacProfile* profile = Profile();
    if (!profile->Has(ModbusTable::Coils))
        return Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
    if (len < 6)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint32_t start = Get16(pdu + 1);
    const uint16_t count = Get16(pdu + 3);
    const uint8_t bytes = pdu[5];
    if (count < 1 || count > MAX_WRITE_BITS || bytes != (count + 7) / 8 || len != 6u + bytes)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    const auto bit = [pdu](uint16_t i) -> uint16_t { return (pdu[6 + i / 8] >> (i % 8)) & 1; };

    // All or nothing: every point is checked before the first is written.
    for (uint16_t i = 0; i < count; ++i)
    {
        if (start + i > 0xFFFF)
            return Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        if (const uint8_t code = profile->CheckWrite(state_, ModbusTable::Coils, static_cast<uint16_t>(start + i), bit(i)))
            return Exception(pdu[0], code, reply);
    }

    bool answered = true;
    for (uint16_t i = 0; i < count; ++i)
        answered &= Store(ModbusTable::Coils, static_cast<uint16_t>(start + i), bit(i), nowMs);
    if (!answered)
        return 0;

    memcpy(reply, pdu, 5);   // function, start, count
    return 5;
}

size_t HvacSimulator::WriteRegisters(const uint8_t* pdu, size_t len, uint8_t* reply, uint32_t nowMs)
{
    const HvacProfile* profile = Profile();
    if (!profile->Has(ModbusTable::HoldingRegisters))
        return Exception(pdu[0], ex::ILLEGAL_FUNCTION, reply);
    if (len < 6)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);
    const uint32_t start = Get16(pdu + 1);
    const uint16_t count = Get16(pdu + 3);
    const uint8_t bytes = pdu[5];
    if (count < 1 || count > MAX_WRITE_WORDS || bytes != count * 2 || len != 6u + bytes)
        return Exception(pdu[0], ex::ILLEGAL_DATA_VALUE, reply);

    // All or nothing: every register is checked before the first is written.
    for (uint16_t i = 0; i < count; ++i)
    {
        if (start + i > 0xFFFF)
            return Exception(pdu[0], ex::ILLEGAL_DATA_ADDRESS, reply);
        if (const uint8_t code = profile->CheckWrite(state_, ModbusTable::HoldingRegisters,
                                                     static_cast<uint16_t>(start + i), Get16(pdu + 6 + 2 * i)))
            return Exception(pdu[0], code, reply);
    }

    bool answered = true;
    for (uint16_t i = 0; i < count; ++i)
        answered &= Store(ModbusTable::HoldingRegisters, static_cast<uint16_t>(start + i), Get16(pdu + 6 + 2 * i), nowMs);
    if (!answered)
        return 0;

    memcpy(reply, pdu, 5);   // function, start, count
    return 5;
}

bool HvacSimulator::Store(ModbusTable table, uint16_t address, uint16_t value, uint32_t nowMs)
{
    counters_.writes++;
    const bool answered = Profile()->Write(state_, table, address, value);
    if (state_.restart)
    {
        state_.restart = false;
        restarting_ = true;
        restartUntilMs_ = nowMs + RESTART_MS;
    }
    return answered;
}

// ── The remote, and the report ───────────────────────────────

bool HvacSimulator::Apply(const Change& change, const char*& error)
{
    LOCK(mutex_);
    const HvacProfile* profile = Profile();
    if (profile == nullptr)
    {
        error = "no unit simulated here: give it a type first";
        return false;
    }
    const HvacProfile::Traits& t = profile->traits;

    // The filter sign is the unit's own doing, not the remote's: a lock does not stop it.
    const bool remote = change.power >= 0 || change.mode >= 0 || change.fan >= 0 || change.swing >= 0 || change.setpoint >= 0;
    if (remote && state_.locked)
    {
        error = "the gateway has locked this unit's own remote";
        return false;
    }
    if (change.mode >= 0 && profile->KindOf(static_cast<uint32_t>(change.mode)) == ModeKind::None)
    {
        error = "not a mode of this type";
        return false;
    }
    if (change.fan >= t.fans) { error = "not a fan speed of this type"; return false; }
    if (change.swing >= t.swings) { error = "not a louvre position of this type"; return false; }
    if (change.setpoint >= 0
        && (change.setpoint < t.setpointMin || change.setpoint > t.setpointMax
            || (change.setpoint - t.setpointMin) % t.setpointStep != 0))
    {
        error = "setpoint outside this type's range, or between its steps";
        return false;
    }
    if (change.filter >= 0 && !t.filter)
    {
        error = "this type has no filter sign";
        return false;
    }

    if (change.power >= 0)    state_.power = change.power != 0;
    if (change.mode >= 0)     state_.mode = static_cast<uint8_t>(change.mode);
    if (change.fan >= 0)      state_.fan = static_cast<uint8_t>(change.fan);
    if (change.swing >= 0)    state_.swing = static_cast<uint8_t>(change.swing);
    if (change.filter >= 0)   state_.filter = change.filter != 0;
    if (change.setpoint >= 0) state_.setpoint = static_cast<int16_t>(change.setpoint);
    return true;
}

void HvacSimulator::Report(ReplyObject& out, uint32_t nowMs)
{
    LOCK(mutex_);
    out.field("unit", static_cast<uint32_t>(index_ + 1));
    out.field("type", config_.type);
    const HvacProfile* profile = Profile();
    out.field("simulated", profile != nullptr);
    if (profile == nullptr)
        return;

    Derive();
    const HvacProfile::Traits& t = profile->traits;
    out.field("address", static_cast<uint32_t>(config_.address));
    out.field("answers", config_.answers);
    out.field("delayMs", config_.delayMs);
    out.field("restarting", restarting_);
    if (state_.newAddress != 0)
        out.field("pendingAddress", static_cast<uint32_t>(state_.newAddress));
    if (t.indoorLink)
        out.field("indoor", config_.indoor);

    out.field("power", state_.power);
    out.field("mode", static_cast<uint32_t>(state_.mode));
    out.field("setpoint", state_.setpoint / 10.0f);
    out.field("room", state_.room / 10.0f);
    out.field("fan", static_cast<uint32_t>(state_.fan));
    out.field("swing", static_cast<uint32_t>(state_.swing));
    if (t.filter)
        out.field("filter", state_.filter);
    if (t.lock)
        out.field("locked", state_.locked);
    out.field("failure", static_cast<uint32_t>(config_.failure));
    out.field("demand", static_cast<uint32_t>(state_.demand));
    profile->Report(state_, out);

    out.field("requests", counters_.requests);
    out.field("writes", counters_.writes);
    out.field("exceptions", counters_.exceptions);
    if (counters_.requests > 0)
        out.field("lastRequestAgoMs", nowMs - counters_.lastRequestMs);

    // The registers worth showing, as the bus would answer them right now.
    auto points = out.array("points");
    for (const HvacProfile::Point& p : profile->points)
    {
        uint16_t value = 0;
        if (!profile->Read(state_, p.table, p.address, value))
            continue;
        auto o = points.object();
        o.field("t", TableCode(p.table));
        o.field("a", static_cast<uint32_t>(p.address));
        o.field("v", static_cast<uint32_t>(value));
    }
}
