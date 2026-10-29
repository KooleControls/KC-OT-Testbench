#include "HvacProfile.h"
#include "ModbusFrame.h"

using namespace modbus;

// ──────────────────────────────────────────────────────────────
// A Daikin indoor unit on Modbus RTU without an Intesis box, as the gateway's driver has
// it (HvacDaikin.cpp): holding registers 1-5 for the unit, 10 to lock its own remote,
// 121 and 122 for a failure. It reports no room temperature.
//
// What the driver does not use is guessed: which modes there are besides heat, auto
// and cool (none here), the setpoint range (HVAC_SETPOINT_MIN/MAX), and that register 10
// reads back what was written. The driver calls register 10 "global update" and writes
// 1 to lock the remote, 0 to free it; that is what it does here.
// ──────────────────────────────────────────────────────────────

namespace {

enum Register : uint16_t
{
    REG_SETPOINT = 1,     // °C x10
    REG_FAN = 2,
    REG_MODE = 3,
    REG_SWING = 4,
    REG_ON_OFF = 5,
    REG_LOCK = 10,        // 1: the unit's own remote is locked
    REG_IS_FAULT = 121,
    REG_FAULT_CODE = 122,
};

enum Mode : uint8_t { MODE_HEAT = 1, MODE_AUTO = 2, MODE_COOL = 3 };
enum Fan : uint8_t { FAN_LOW, FAN_MEDIUM, FAN_HIGH, FAN_AUTO, FAN_COUNT };
enum Swing : uint8_t { SWING_OFF, SWING_VERTICAL, SWING_HORIZONTAL, SWING_AUTO, SWING_COUNT };

constexpr ModeKind MODES[] = { ModeKind::None, ModeKind::Heat, ModeKind::Auto, ModeKind::Cool };

constexpr auto H = ModbusTable::HoldingRegisters;

// What the gateway reads.
constexpr HvacProfile::Point POINTS[] = {
    { H, REG_ON_OFF }, { H, REG_MODE }, { H, REG_SETPOINT }, { H, REG_FAN }, { H, REG_SWING },
    { H, REG_LOCK }, { H, REG_IS_FAULT }, { H, REG_FAULT_CODE },
};

class Daikin : public HvacProfile
{
public:
    Daikin()
        : HvacProfile({ .modes = MODES, .fans = FAN_COUNT, .swings = SWING_COUNT,
                        .setpointMin = HVAC_SETPOINT_MIN, .setpointMax = HVAC_SETPOINT_MAX, .setpointStep = 1,
                        .room = false, .filter = false, .lock = true, .indoorLink = false },
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
        case REG_SETPOINT:   value = static_cast<uint16_t>(s.setpoint); return true;
        case REG_FAN:        value = s.fan; return true;
        case REG_MODE:       value = s.mode; return true;
        case REG_SWING:      value = s.swing; return true;
        case REG_ON_OFF:     value = s.power ? 1 : 0; return true;
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
        case REG_SETPOINT: {
            const int16_t t = static_cast<int16_t>(value);
            return valid(t >= HVAC_SETPOINT_MIN && t <= HVAC_SETPOINT_MAX);
        }
        case REG_FAN:    return valid(value < FAN_COUNT);
        case REG_MODE:   return valid(KindOf(value) != ModeKind::None);
        case REG_SWING:  return valid(value < SWING_COUNT);
        case REG_ON_OFF:
        case REG_LOCK:   return valid(value <= 1);
        default:         return ex::ILLEGAL_DATA_ADDRESS;   // the failure is read-only
        }
    }

    bool Write(HvacState& s, ModbusTable, uint16_t reg, uint16_t value) const override
    {
        switch (reg)
        {
        case REG_SETPOINT: s.setpoint = static_cast<int16_t>(value); break;
        case REG_FAN:      s.fan = static_cast<uint8_t>(value); break;
        case REG_MODE:     s.mode = static_cast<uint8_t>(value); break;
        case REG_SWING:    s.swing = static_cast<uint8_t>(value); break;
        case REG_ON_OFF:   s.power = value != 0; break;
        case REG_LOCK:     s.locked = value != 0; break;
        default:           break;
        }
        return true;
    }
};

} // namespace

const HvacProfile& DaikinProfile()
{
    static const Daikin profile;
    return profile;
}
