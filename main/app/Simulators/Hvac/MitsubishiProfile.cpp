#include "HvacProfile.h"
#include "ModbusFrame.h"

using namespace modbus;

// ──────────────────────────────────────────────────────────────
// A Mitsubishi Heavy Industries indoor unit behind an Intesis INMBSMHI001R000, as the
// gateway's driver has it (HvacMitsubishi.cpp): holding registers 0-5 for the unit, 8
// to lock its own remote, 10 and 11 for a failure.
//
// What the driver does not use is guessed: the setpoint range (HVAC_SETPOINT_MIN/MAX),
// which values on/off and the lock take (0 and 1), and everything else an Intesis box
// has besides these. The driver defines a failure reset at 45 but never writes it, so it
// is not here.
// ──────────────────────────────────────────────────────────────

namespace {

enum Register : uint16_t
{
    REG_ON_OFF = 0,
    REG_MODE = 1,
    REG_FAN = 2,
    REG_SWING = 3,
    REG_SETPOINT = 4,     // °C x10
    REG_ROOM = 5,         // °C x10, read-only
    REG_LOCK = 8,         // 1: the unit's own remote is locked
    REG_IS_FAULT = 10,
    REG_FAULT_CODE = 11,
};

enum Mode : uint8_t { MODE_AUTO, MODE_HEAT, MODE_DRY, MODE_FAN, MODE_COOL };
enum Fan : uint8_t { FAN_LOW, FAN_MEDIUM, FAN_HIGH, FAN_AUTO, FAN_COUNT };
enum Swing : uint8_t { SWING_OFF, SWING_VERTICAL, SWING_HORIZONTAL, SWING_AUTO, SWING_COUNT };

constexpr ModeKind MODES[] = { ModeKind::Auto, ModeKind::Heat, ModeKind::Dry, ModeKind::Fan, ModeKind::Cool };

constexpr auto H = ModbusTable::HoldingRegisters;

// What the gateway reads.
constexpr HvacProfile::Point POINTS[] = {
    { H, REG_ON_OFF }, { H, REG_MODE }, { H, REG_SETPOINT }, { H, REG_ROOM }, { H, REG_FAN },
    { H, REG_SWING }, { H, REG_LOCK }, { H, REG_IS_FAULT }, { H, REG_FAULT_CODE },
};

class Mitsubishi : public HvacProfile
{
public:
    Mitsubishi()
        : HvacProfile({ .modes = MODES, .fans = FAN_COUNT, .swings = SWING_COUNT,
                        .setpointMin = HVAC_SETPOINT_MIN, .setpointMax = HVAC_SETPOINT_MAX, .setpointStep = 1,
                        .room = true, .filter = false, .lock = true, .indoorLink = false },
                      POINTS)
    {
    }

    void Reset(HvacState& s) const override
    {
        s.power = false;
        s.mode = MODE_COOL;
        s.setpoint = 230;
        s.fan = FAN_AUTO;
        s.swing = SWING_OFF;
        s.filter = false;
        s.locked = false;
    }

    bool Has(ModbusTable table) const override { return table == H; }

    bool Read(const HvacState& s, ModbusTable table, uint16_t reg, uint16_t& value) const override
    {
        if (table != H)
            return false;
        switch (reg)
        {
        case REG_ON_OFF:     value = s.power ? 1 : 0; return true;
        case REG_MODE:       value = s.mode; return true;
        case REG_FAN:        value = s.fan; return true;
        case REG_SWING:      value = s.swing; return true;
        case REG_SETPOINT:   value = static_cast<uint16_t>(s.setpoint); return true;
        case REG_ROOM:       value = static_cast<uint16_t>(s.room); return true;
        case REG_LOCK:       value = s.locked ? 1 : 0; return true;
        case REG_IS_FAULT:   value = s.failure != 0 ? 1 : 0; return true;
        case REG_FAULT_CODE: value = s.failure; return true;
        default:             return false;
        }
    }

    uint8_t CheckWrite(const HvacState&, ModbusTable, uint16_t reg, uint16_t value) const override
    {
        const auto valid = [](bool ok) { return ok ? 0 : ex::ILLEGAL_DATA_VALUE; };
        switch (reg)
        {
        case REG_ON_OFF:
        case REG_LOCK:     return valid(value <= 1);
        case REG_MODE:     return valid(KindOf(value) != ModeKind::None);
        case REG_FAN:      return valid(value < FAN_COUNT);
        case REG_SWING:    return valid(value < SWING_COUNT);
        case REG_SETPOINT: {
            const int16_t t = static_cast<int16_t>(value);
            return valid(t >= HVAC_SETPOINT_MIN && t <= HVAC_SETPOINT_MAX);
        }
        default:           return ex::ILLEGAL_DATA_ADDRESS;   // the room and the failure are read-only
        }
    }

    bool Write(HvacState& s, ModbusTable, uint16_t reg, uint16_t value) const override
    {
        switch (reg)
        {
        case REG_ON_OFF:   s.power = value != 0; break;
        case REG_MODE:     s.mode = static_cast<uint8_t>(value); break;
        case REG_FAN:      s.fan = static_cast<uint8_t>(value); break;
        case REG_SWING:    s.swing = static_cast<uint8_t>(value); break;
        case REG_SETPOINT: s.setpoint = static_cast<int16_t>(value); break;
        case REG_LOCK:     s.locked = value != 0; break;
        default:           break;
        }
        return true;
    }
};

} // namespace

const HvacProfile& MitsubishiProfile()
{
    static const Mitsubishi profile;
    return profile;
}
