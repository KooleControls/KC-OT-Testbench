#include "HvacProfile.h"
#include "ModbusFrame.h"
#include "ReplyWriter.h"
#include <cstdio>
#include <cstring>

using namespace modbus;

// ──────────────────────────────────────────────────────────────
// A CoolAutomation CoolPlug, with the indoor unit behind it, as its manual has it
// (CoolPlug PRM rev 0.3, the register map in section 2.2): meant to be
// indistinguishable on the bus from a real one. Holding registers only; besides
// reading and writing those it answers Read Device Identification, as the PRM lists.
//
// What the PRM does not say is guessed, and said so where it is done: what an unmapped
// register or an out-of-range value gets, and what the HVAC type and the indoor
// centralised address read.
// ──────────────────────────────────────────────────────────────

namespace {

// What the CoolPlug config tool reported for a real one: firmware 0.1.9, which register
// 33 encodes as major * 100 + minor * 10 + subminor.
constexpr uint16_t FIRMWARE_VERSION = 19;
constexpr const char* FIRMWARE_TEXT = "0.1.9";

// A real serial is 0x03D0XXXX (PRM 2.1); these are recognisably the testbench's.
constexpr uint32_t SERIAL_BASE = 0x03D05100;

constexpr uint8_t MAX_ADDRESS = 247;

// The VAUT changeover threshold, °C x10. A guess: 10 °C is a lot of hysteresis.
constexpr uint16_t VAUT_MAX = 100;

// Read Device Identification, basic objects 0-2. Conformity 0x81: basic, plus access to
// one object at a time.
constexpr uint8_t DEVICE_ID_CONFORMITY = 0x81;
constexpr const char* DEVICE_ID_OBJECTS[] = { "CoolAutomation", "CoolPlug", FIRMWARE_TEXT };
constexpr uint8_t DEVICE_ID_COUNT = 3;

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

// Encodings, from the PRM.
enum Mode : uint8_t { MODE_COOL, MODE_HEAT, MODE_AUTO, MODE_DRY, MODE_HAUX, MODE_FAN };
enum Fan : uint8_t { FAN_LOW, FAN_MEDIUM, FAN_HIGH, FAN_AUTO, FAN_TOP, FAN_VERY_LOW, FAN_COUNT };
enum Swing : uint8_t { SWING_VERTICAL, SWING_30, SWING_45, SWING_60, SWING_HORIZONTAL, SWING_AUTO, SWING_OFF, SWING_NO, SWING_COUNT };

constexpr ModeKind MODES[] = {
    ModeKind::Cool, ModeKind::Heat, ModeKind::Auto, ModeKind::Dry,
    ModeKind::Heat,   // heat aux
    ModeKind::Fan,
};

constexpr auto H = ModbusTable::HoldingRegisters;

// The gateway's registers (HvacCoolPlug.cpp: 95, 97, 144-147, 150, 151), and the filter
// sign and operation status beside them.
constexpr HvacProfile::Point POINTS[] = {
    { H, REG_ON_OFF }, { H, REG_MODE }, { H, REG_SETPOINT }, { H, REG_AMBIENT_ALT },
    { H, REG_FAN }, { H, REG_SWING }, { H, REG_LINE_STATE }, { H, REG_FAILURE_ALT },
    { H, REG_FILTER }, { H, REG_OPERATION },
};

uint32_t Serial(const HvacState& s)
{
    return SERIAL_BASE + s.index + 1;
}

size_t Exception(uint8_t function, uint8_t code, uint8_t* reply)
{
    reply[0] = function | fc::EXCEPTION;
    reply[1] = code;
    return 2;
}

class CoolPlug : public HvacProfile
{
public:
    CoolPlug()
        : HvacProfile({ .modes = MODES, .fans = FAN_COUNT, .swings = SWING_COUNT,
                        .setpointMin = HVAC_SETPOINT_MIN, .setpointMax = HVAC_SETPOINT_MAX, .setpointStep = 1,
                        .room = true, .filter = true, .lock = false, .indoorLink = true },
                      POINTS)
    {
    }

    // As the CoolPlug config tool showed a real one at rest.
    void Reset(HvacState& s) const override
    {
        s.power = false;
        s.mode = MODE_COOL;
        s.setpoint = 230;
        s.fan = FAN_HIGH;
        s.swing = SWING_30;
        s.filter = false;
        s.locked = false;
        s.coolPlug = {};
    }

    bool Has(ModbusTable table) const override { return table == H; }

    bool Read(const HvacState& s, ModbusTable table, uint16_t reg, uint16_t& value) const override
    {
        if (table != H)
            return false;

        switch (reg)
        {
        case REG_RESET:          value = 0; return true;   // write-only in the PRM; read as 0 (a guess)
        case REG_VAUT_THRESHOLD: value = s.coolPlug.vautThreshold; return true;
        case REG_VAUT_AS_AUTO:   value = s.coolPlug.vautAsAuto; return true;
        case REG_HVAC_TYPE:      value = 0; return true;   // "internal use"; a real one's value is unknown
        case REG_FIRMWARE:       value = FIRMWARE_VERSION; return true;
        case REG_SERIAL_LOW:     value = Serial(s) & 0xFFFF; return true;
        case REG_SERIAL_HIGH:    value = Serial(s) >> 16; return true;
        case REG_SLAVE_ADDRESS:  value = s.address; return true;
        case REG_LINE_ROLE:      value = s.coolPlug.lineRole; return true;
        case REG_INDOOR_ADDRESS: value = 0; return true;   // the config tool shows L1.000; encoding unknown

        case REG_LINE_STATE:
        case REG_LINE_STATE_ALT:
            // bit 0: connected to the indoor unit; bit 2: the indoor unit reports a failure.
            value = (s.indoor ? 0x01 : 0) | (s.failure != 0 ? 0x04 : 0);
            return true;

        case REG_COMPACT_STATUS:
            value = (s.power ? 1 : 0)
                  | (s.mode << 1)
                  | (s.fan << 6)
                  | (s.swing << 10)
                  | (s.filter ? 1 << 13 : 0)
                  | (s.demand != HvacDemand::None ? 1 << 14 : 0);   // therm on
            return true;

        case REG_SETPOINT:
        case REG_SETPOINT_ALT:    value = static_cast<uint16_t>(s.setpoint); return true;
        case REG_AMBIENT:
        case REG_AMBIENT_ALT:     value = static_cast<uint16_t>(s.room); return true;
        case REG_FAILURE:
        case REG_FAILURE_ALT:     value = s.failure; return true;
        case REG_OWN_AMBIENT:
        case REG_OWN_AMBIENT_ALT: value = static_cast<uint16_t>(s.coolPlug.ownAmbient); return true;
        case REG_ON_OFF:          value = s.power ? 1 : 0; return true;
        case REG_MODE:            value = s.mode; return true;
        case REG_FAN:             value = s.fan; return true;
        case REG_SWING:           value = s.swing; return true;
        case REG_FILTER:          value = s.filter ? 1 : 0; return true;
        // 0 none, 1 heating, 2 cooling: HvacDemand's own numbers.
        case REG_OPERATION:       value = static_cast<uint16_t>(s.demand); return true;
        default:                  return false;
        }
    }

    uint8_t CheckWrite(const HvacState&, ModbusTable table, uint16_t reg, uint16_t value) const override
    {
        if (table != H)
            return ex::ILLEGAL_DATA_ADDRESS;

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
            // The PRM gives no range; HVAC_SETPOINT_MIN/MAX are a guess.
            const int16_t t = static_cast<int16_t>(value);
            return valid(t >= HVAC_SETPOINT_MIN && t <= HVAC_SETPOINT_MAX);
        }
        case REG_OWN_AMBIENT:
        case REG_OWN_AMBIENT_ALT:
        case REG_ON_OFF:          return 0;   // any value: 0 off, anything else on
        case REG_MODE:            return valid(KindOf(value) != ModeKind::None);
        case REG_FAN:             return valid(value < FAN_COUNT);
        case REG_SWING:           return valid(value < SWING_COUNT);
        case REG_FILTER:          return valid(value <= 1);
        default:                  return ex::ILLEGAL_DATA_ADDRESS;   // read-only, or not there
        }
    }

    bool Write(HvacState& s, ModbusTable, uint16_t reg, uint16_t value) const override
    {
        switch (reg)
        {
        case REG_RESET:
            // 1 is "enter boot mode" on a real one. There is no bootloader to enter here,
            // so both restart. "CoolPlug does not respond to the write request into this
            // register."
            s.restart = true;
            return false;
        case REG_VAUT_THRESHOLD:  s.coolPlug.vautThreshold = (value + 2) / 5 * 5; break;   // to 0.5 °C
        case REG_VAUT_AS_AUTO:    s.coolPlug.vautAsAuto = value != 0; break;
        case REG_SLAVE_ADDRESS:   s.newAddress = static_cast<uint8_t>(value); break;   // after a reset
        case REG_LINE_ROLE:       s.coolPlug.lineRole = static_cast<uint8_t>(value); break;
        case REG_SETPOINT:
        case REG_SETPOINT_ALT:    s.setpoint = static_cast<int16_t>(value); break;
        case REG_OWN_AMBIENT:
        case REG_OWN_AMBIENT_ALT: s.coolPlug.ownAmbient = static_cast<int16_t>(value); break;
        case REG_ON_OFF:          s.power = value != 0; break;
        case REG_MODE:            s.mode = static_cast<uint8_t>(value); break;
        case REG_FAN:             s.fan = static_cast<uint8_t>(value); break;
        case REG_SWING:           s.swing = static_cast<uint8_t>(value); break;
        case REG_FILTER:          s.filter = value != 0; break;
        default:                  break;
        }
        return true;
    }

    size_t Other(const HvacState&, const uint8_t* pdu, size_t len, uint8_t* reply) const override
    {
        if (pdu[0] != fc::ENCAPSULATED)
            return 0;
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

    void Report(const HvacState& s, ReplyObject& out) const override
    {
        char serial[9];
        snprintf(serial, sizeof(serial), "%08lX", static_cast<unsigned long>(Serial(s)));
        out.field("serial", serial);
        out.field("firmware", FIRMWARE_TEXT);
        out.field("ownAmbient", s.coolPlug.ownAmbient / 10.0f);
        out.field("vautThreshold", s.coolPlug.vautThreshold / 10.0f);
        out.field("lineRole", static_cast<uint32_t>(s.coolPlug.lineRole));
    }
};

} // namespace

const HvacProfile& CoolPlugProfile()
{
    static const CoolPlug profile;
    return profile;
}
