#include "HvacProfile.h"
#include "ModbusFrame.h"

using namespace modbus;

// ──────────────────────────────────────────────────────────────
// A Hisense B544(E) central control adaptor in its native Modbus RTU mode, with one
// indoor unit behind it, as the gateway's driver has it (HvacHisense.cpp).
//
// Unlike the CoolPlug and the Intesis boxes, status and control are apart: the unit is
// read from discrete inputs and input registers, and set through a coil and holding
// registers at other addresses, with other codes for some of the same values. The
// differences are the point of simulating it:
//   • the mode written is 0 fan, 1 heat, 2 cool, 3 dry, 4 auto; the mode read says, for
//     auto, which way it is going: 5 auto-cool, 6 auto-heat, 7 auto-fan;
//   • the louvre codes swap up/down and left/right between writing and reading;
//   • temperatures are whole °C, the setpoint 18 to 32.
//
// What the driver does not use is guessed: the coil and the holding registers read back
// what was written, and a setpoint outside 18-32 is refused (Illegal Data Value). The
// adaptor has no point to lock the unit's own remote.
// ──────────────────────────────────────────────────────────────

namespace {

enum : uint16_t
{
    // Discrete inputs (FC 02)
    DI_ONOFF = 0,
    // Input registers (FC 04)
    IR_ROOM = 1,       // °C, signed, whole
    IR_SETPOINT = 2,   // °C, whole
    IR_MODE = 7,       // status codes
    IR_FAN = 8,
    IR_SWING = 9,      // status codes
    IR_FAULT = 12,     // 0 none, 1-255 the unit's error code
    // Coils (FC 05, and FC 01 to read back)
    COIL_ONOFF = 0,
    // Holding registers (FC 06, and FC 03 to read back)
    HR_SETPOINT = 0,   // °C, whole, 18-32
    HR_MODE = 2,       // control codes
    HR_FAN = 3,
    HR_SWING = 4,      // control codes
};

// Kept as written: the control codes.
enum Mode : uint8_t { MODE_FAN, MODE_HEAT, MODE_COOL, MODE_DRY, MODE_AUTO };
enum Fan : uint8_t { FAN_AUTO, FAN_HIGH, FAN_LOW, FAN_MID, FAN_COUNT };
enum Swing : uint8_t { SWING_NONE, SWING_UP_DOWN, SWING_LEFT_RIGHT, SWING_BOTH, SWING_COUNT };

// Read as: auto says where it is going.
constexpr uint16_t STATUS_AUTO_COOL = 5;
constexpr uint16_t STATUS_AUTO_HEAT = 6;
constexpr uint16_t STATUS_AUTO_FAN = 7;

constexpr int16_t SETPOINT_MIN = 180;
constexpr int16_t SETPOINT_MAX = 320;

constexpr ModeKind MODES[] = { ModeKind::Fan, ModeKind::Heat, ModeKind::Cool, ModeKind::Dry, ModeKind::Auto };

// What the gateway reads.
constexpr HvacProfile::Point POINTS[] = {
    { ModbusTable::DiscreteInputs, DI_ONOFF },
    { ModbusTable::InputRegisters, IR_MODE },
    { ModbusTable::InputRegisters, IR_SETPOINT },
    { ModbusTable::InputRegisters, IR_ROOM },
    { ModbusTable::InputRegisters, IR_FAN },
    { ModbusTable::InputRegisters, IR_SWING },
    { ModbusTable::InputRegisters, IR_FAULT },
};

uint16_t StatusMode(const HvacState& s)
{
    if (s.mode != MODE_AUTO)
        return s.mode;
    switch (s.demand)
    {
    case HvacDemand::Cooling: return STATUS_AUTO_COOL;
    case HvacDemand::Heating: return STATUS_AUTO_HEAT;
    default:                  return STATUS_AUTO_FAN;
    }
}

/// Up/down and left/right trade places between the two code sets; none and both do not.
uint16_t SwapSwing(uint16_t swing)
{
    if (swing == SWING_UP_DOWN) return SWING_LEFT_RIGHT;
    if (swing == SWING_LEFT_RIGHT) return SWING_UP_DOWN;
    return swing;
}

/// Tenths of a °C to whole ones, to the nearest.
int16_t Whole(int16_t tenths)
{
    return static_cast<int16_t>(tenths >= 0 ? (tenths + 5) / 10 : (tenths - 5) / 10);
}

class Hisense : public HvacProfile
{
public:
    Hisense()
        : HvacProfile({ .modes = MODES, .fans = FAN_COUNT, .swings = SWING_COUNT,
                        .setpointMin = SETPOINT_MIN, .setpointMax = SETPOINT_MAX, .setpointStep = 10,
                        .room = true, .filter = false, .lock = false, .indoorLink = false },
                      POINTS)
    {
    }

    void Reset(HvacState& s) const override
    {
        s.power = false;
        s.mode = MODE_COOL;
        s.setpoint = 230;
        s.fan = FAN_AUTO;
        s.swing = SWING_NONE;
        s.filter = false;
        s.locked = false;
    }

    bool Has(ModbusTable) const override { return true; }

    bool Read(const HvacState& s, ModbusTable table, uint16_t address, uint16_t& value) const override
    {
        switch (table)
        {
        case ModbusTable::DiscreteInputs:
            if (address != DI_ONOFF)
                return false;
            value = s.power ? 1 : 0;
            return true;

        case ModbusTable::Coils:
            if (address != COIL_ONOFF)
                return false;
            value = s.power ? 1 : 0;
            return true;

        case ModbusTable::InputRegisters:
            switch (address)
            {
            case IR_ROOM:     value = static_cast<uint16_t>(Whole(s.room)); return true;
            case IR_SETPOINT: value = static_cast<uint16_t>(s.setpoint / 10); return true;
            case IR_MODE:     value = StatusMode(s); return true;
            case IR_FAN:      value = s.fan; return true;   // the same codes both ways
            case IR_SWING:    value = SwapSwing(s.swing); return true;
            case IR_FAULT:    value = s.failure; return true;
            default:          return false;
            }

        case ModbusTable::HoldingRegisters:
            switch (address)
            {
            case HR_SETPOINT: value = static_cast<uint16_t>(s.setpoint / 10); return true;
            case HR_MODE:     value = s.mode; return true;
            case HR_FAN:      value = s.fan; return true;
            case HR_SWING:    value = s.swing; return true;
            default:          return false;
            }
        }
        return false;
    }

    uint8_t CheckWrite(const HvacState&, ModbusTable table, uint16_t address, uint16_t value) const override
    {
        const auto valid = [](bool ok) { return ok ? 0 : ex::ILLEGAL_DATA_VALUE; };
        if (table == ModbusTable::Coils)
            return address == COIL_ONOFF ? 0 : ex::ILLEGAL_DATA_ADDRESS;

        switch (address)
        {
        case HR_SETPOINT: return valid(value * 10 >= SETPOINT_MIN && value * 10 <= SETPOINT_MAX);
        case HR_MODE:     return valid(KindOf(value) != ModeKind::None);
        case HR_FAN:      return valid(value < FAN_COUNT);
        case HR_SWING:    return valid(value < SWING_COUNT);
        default:          return ex::ILLEGAL_DATA_ADDRESS;
        }
    }

    bool Write(HvacState& s, ModbusTable table, uint16_t address, uint16_t value) const override
    {
        if (table == ModbusTable::Coils)
        {
            s.power = value != 0;
            return true;
        }
        switch (address)
        {
        case HR_SETPOINT: s.setpoint = static_cast<int16_t>(value * 10); break;
        case HR_MODE:     s.mode = static_cast<uint8_t>(value); break;
        case HR_FAN:      s.fan = static_cast<uint8_t>(value); break;
        case HR_SWING:    s.swing = static_cast<uint8_t>(value); break;
        default:          break;
        }
        return true;
    }
};

} // namespace

const HvacProfile& HisenseProfile()
{
    static const Hisense profile;
    return profile;
}
