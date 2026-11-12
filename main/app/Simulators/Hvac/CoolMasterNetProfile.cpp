#include "HvacProfile.h"
#include "ModbusFrame.h"
#include "ReplyWriter.h"

using namespace modbus;

// ──────────────────────────────────────────────────────────────
// An indoor unit behind a CoolAutomation CoolMasterNet, as the gateway's driver has it
// (HvacCoolAutomation.cpp) and as a CoolMasterNet v0.9.8C with simulated indoor units
// (`simul 4`, `va auto`) answered on the bench on 2026-11-12. A CoolMasterNet is one
// Modbus TCP slave for every indoor unit on it; it answers any unit id. Per indoor unit it
// has a slot of 16 registers from the unit's virtual address on (VA 1 at 17, VA 2 at 33...
// counted from 1; the gateway counts from 0, so 16, 32, 48, 64): ten the gateway uses,
// two more that read and write (meaning unknown, 0 on a fresh unit), and four reserved
// ones that are refused (Illegal Data Address). The PRM says a block read that starts on
// a real register answers the reserved ones as 0xFFFF; here such a read is refused whole,
// which the gateway, reading single registers, never sees. The register map itself is in
// CoolAutomation's "Modbus Integration Guidelines", which was not at hand.
//
// Measured on the real one: the setpoint takes any value (99.9 °C included); a mode, fan
// or louvre code outside the table is refused (Illegal Data Value); a power value other
// than 0 or 1 and a 1 to the filter sign are acknowledged and ignored (0 clears the sign);
// the room temperature and the failure code refuse a write; the limits register takes
// high °C x2 in the high byte and low °C x2 in the low byte, 0 for none; the lock word
// keeps its low four bits (on/off, mode, set temperature; on/off also sets bit 3) and
// drops everything else -- so the 0x0080 the gateway writes to lock a unit's remote reads
// back 0 and locks nothing. The two types (5 CMN, 6 CMA) are one driver in the gateway,
// so one profile here.
// ──────────────────────────────────────────────────────────────

namespace {

/// The slot, from the unit's first register on.
enum Offset : uint16_t
{
    OFF_MODE = 0,
    OFF_FAN = 1,
    OFF_SETPOINT = 2,   // °C x10
    OFF_POWER = 3,
    OFF_FILTER = 4,     // 1: the filter sign is up; only 0 can be written (clears it)
    OFF_SWING = 5,
    OFF_ROOM = 6,       // °C x10, read-only
    OFF_FAILURE = 7,    // 0: none, read-only
    OFF_LOCK = 8,       // the lock word: 0 free; bits 0-3 lock the unit's own remote
    OFF_LIMITS = 9,     // set temperature limits: high °C x2 in the high byte, low °C x2 in the low; 0 none
    OFF_SPARE_10 = 10,  // read and write on the real one, meaning unknown
    OFF_SPARE_11 = 11,
    REGISTERS = 12,     // 12 to 15 are reserved and refused
    SPAN = 16,
};

enum Mode : uint8_t { MODE_COOL, MODE_HEAT, MODE_AUTO, MODE_DRY, MODE_HEAT_AUX, MODE_FAN, MODE_HEAT_HH, MODE_COUNT };
enum Fan : uint8_t { FAN_LOW, FAN_MEDIUM, FAN_HIGH, FAN_AUTO, FAN_TOP, FAN_VERY_LOW, FAN_COUNT };
enum Swing : uint8_t { SWING_VERTICAL, SWING_30, SWING_45, SWING_60, SWING_HORIZONTAL, SWING_AUTO, SWING_OFF, SWING_COUNT };

constexpr ModeKind MODES[] = {
    ModeKind::Cool, ModeKind::Heat, ModeKind::Auto, ModeKind::Dry, ModeKind::Heat, ModeKind::Fan, ModeKind::Heat,
};

// The lock word's bits as the real one keeps them: on/off (which also raises bit 3, as
// `lock +o` does), mode, set temperature. Anything above is dropped, 0x0080 included.
constexpr uint16_t LOCK_ONOFF = 0x0001;
constexpr uint16_t LOCK_ONOFF_SHADOW = 0x0008;
constexpr uint16_t LOCK_KEPT = 0x000F;

constexpr auto H = ModbusTable::HoldingRegisters;

// What the gateway reads, by offset: HvacSimulator adds the unit's first register.
constexpr HvacProfile::Point POINTS[] = {
    { H, OFF_POWER }, { H, OFF_MODE }, { H, OFF_SETPOINT }, { H, OFF_ROOM }, { H, OFF_FAN },
    { H, OFF_SWING }, { H, OFF_FILTER }, { H, OFF_LOCK }, { H, OFF_FAILURE }, { H, OFF_LIMITS },
};

/// The lock word as the real one reads it back after `value` was written.
uint16_t LockKept(uint16_t value)
{
    uint16_t bits = value & LOCK_KEPT;
    if (bits & LOCK_ONOFF)
        bits |= LOCK_ONOFF_SHADOW;
    return bits;
}

class CoolMasterNet : public HvacProfile
{
public:
    CoolMasterNet()
        : HvacProfile({ .modes = MODES, .fans = FAN_COUNT, .swings = SWING_COUNT,
                        .setpointMin = HVAC_SETPOINT_MIN, .setpointMax = HVAC_SETPOINT_MAX, .setpointStep = 1,
                        .room = true, .filter = true, .lock = true, .indoorLink = false },
                      POINTS)
    {
    }

    uint16_t Span() const override { return SPAN; }

    void Reset(HvacState& s) const override
    {
        s.power = false;
        s.mode = MODE_COOL;
        s.setpoint = 230;
        s.fan = FAN_AUTO;
        s.swing = SWING_OFF;
        s.filter = false;
        s.locked = false;
        s.coolMaster = {};
    }

    // Every table "exists": a coil or input register read is refused by address, as the
    // real one does, not by function.
    bool Has(ModbusTable) const override { return true; }

    bool Read(const HvacState& s, ModbusTable table, uint16_t reg, uint16_t& value) const override
    {
        if (table != H || !Owns(s, reg))
            return false;
        switch (reg - s.address)
        {
        case OFF_MODE:     value = s.mode; return true;
        case OFF_FAN:      value = s.fan; return true;
        case OFF_SETPOINT: value = static_cast<uint16_t>(s.setpoint); return true;
        case OFF_POWER:    value = s.power ? 1 : 0; return true;
        case OFF_FILTER:   value = s.filter ? 1 : 0; return true;
        case OFF_SWING:    value = s.swing; return true;
        case OFF_ROOM:     value = static_cast<uint16_t>(s.room); return true;
        case OFF_FAILURE:  value = s.failure; return true;
        case OFF_LOCK:     value = s.coolMaster.lockBits; return true;
        case OFF_LIMITS:   value = s.coolMaster.limits; return true;
        case OFF_SPARE_10: value = s.coolMaster.spare[0]; return true;
        case OFF_SPARE_11: value = s.coolMaster.spare[1]; return true;
        default:           return false;   // reserved
        }
    }

    uint8_t CheckWrite(const HvacState& s, ModbusTable table, uint16_t reg, uint16_t value) const override
    {
        if (table != H || !Owns(s, reg))
            return ex::ILLEGAL_DATA_ADDRESS;
        const auto valid = [](bool ok) { return ok ? 0 : ex::ILLEGAL_DATA_VALUE; };
        switch (reg - s.address)
        {
        case OFF_MODE:     return valid(KindOf(value) != ModeKind::None);
        case OFF_FAN:      return valid(value < FAN_COUNT);
        case OFF_SWING:    return valid(value < SWING_COUNT);
        case OFF_SETPOINT:   // any value, as measured
        case OFF_POWER:      // other than 0 and 1 acknowledged and ignored
        case OFF_FILTER:     // 1 acknowledged and ignored, 0 clears the sign
        case OFF_LOCK:
        case OFF_LIMITS:
        case OFF_SPARE_10:
        case OFF_SPARE_11: return 0;
        default:           return ex::ILLEGAL_DATA_ADDRESS;   // the room, the failure code, the reserved ones
        }
    }

    bool Write(HvacState& s, ModbusTable, uint16_t reg, uint16_t value) const override
    {
        switch (reg - s.address)
        {
        case OFF_MODE:     s.mode = static_cast<uint8_t>(value); break;
        case OFF_FAN:      s.fan = static_cast<uint8_t>(value); break;
        case OFF_SETPOINT: s.setpoint = static_cast<int16_t>(value); break;
        case OFF_POWER:
            if (value <= 1)
                s.power = value != 0;
            break;
        case OFF_FILTER:
            if (value == 0)
                s.filter = false;
            break;
        case OFF_SWING:    s.swing = static_cast<uint8_t>(value); break;
        case OFF_LOCK:
            s.coolMaster.lockBits = LockKept(value);
            s.locked = s.coolMaster.lockBits != 0;
            break;
        case OFF_LIMITS:   s.coolMaster.limits = value; break;
        case OFF_SPARE_10: s.coolMaster.spare[0] = value; break;
        case OFF_SPARE_11: s.coolMaster.spare[1] = value; break;
        default:           break;
        }
        return true;
    }

    void Report(const HvacState& s, ReplyObject& out) const override
    {
        out.field("lockBits", static_cast<uint32_t>(s.coolMaster.lockBits));
        out.field("limitLow", (s.coolMaster.limits & 0xFF) / 2.0f);
        out.field("limitHigh", (s.coolMaster.limits >> 8) / 2.0f);
    }

private:
    static bool Owns(const HvacState& s, uint16_t reg)
    {
        return reg >= s.address && reg < s.address + SPAN;
    }
};

} // namespace

const HvacProfile& CoolMasterNetProfile()
{
    static const CoolMasterNet profile;
    return profile;
}
