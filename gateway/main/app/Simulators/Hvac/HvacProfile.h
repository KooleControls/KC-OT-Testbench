#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

class ReplyObject;

// ──────────────────────────────────────────────────────────────
// What one kind of HVAC interface is to a gateway: the registers it answers, the codes
// in them, and what the indoor unit's own remote can set. One profile per type, shared
// by every simulator of that type; the state it reads and writes is the simulator's
// (HvacState), so a profile holds nothing of its own.
//
// The types are the gateway's, by the numbers of its HVAC list (Smarthome,
// HvacManager/Types/HvacType.h), so a unit is set up here as it is there. Each register
// map is the one the gateway's driver for that type uses (HvacManager/HvacUnits/
// Hvac*.cpp). For the CoolPlug that is also its manual (the PRM); for the others no
// manual was at hand, so the driver is the map, and whatever it leaves open is guessed
// and said so where it is done. A real unit on the bus settles those.
//
// A point a profile does not know is refused (Illegal Data Address), on purpose: a
// gateway that reads the wrong register then sees an exception on the RS485 page,
// rather than a plausible 0.
// ──────────────────────────────────────────────────────────────

/// The gateway's HVAC types, with its numbers.
enum class HvacType : uint8_t
{
    NA = 0,    // not used
    OTH = 1,   // OpenTherm: the boiler simulator plays it
    REL = 2,   // a relay contact: no bus to simulate
    SAM = 3,   // Samsung, via an Intesis interface
    DAI = 4,   // Daikin, without Intesis
    CMN = 5,   // CoolAutomation CoolMasterNet
    CMA = 6,   // CoolMasterNet, as Aedes uses it
    MIT = 7,   // Mitsubishi Heavy Industries, via an Intesis INMBSMHI001R000
    COP = 8,   // CoolAutomation CoolPlug
    HIS = 9,   // Hisense B544(E) central control adaptor
};

/// How a gateway reaches a type.
enum class HvacTransport : uint8_t
{
    None,        // not over Modbus at all
    ModbusRtu,   // the RS485 bus (Rs485Manager)
    ModbusTcp,   // the network. Nothing serves it yet.
};

/// The four Modbus tables. Which function codes reach which is the protocol's.
enum class ModbusTable : uint8_t { Coils, DiscreteInputs, HoldingRegisters, InputRegisters };

/// What a mode does, whatever a type calls it. None: not a mode of the type.
enum class ModeKind : uint8_t { None, Cool, Heat, Auto, Dry, Fan };

enum class HvacDemand : uint8_t { None, Heating, Cooling };

/// One simulated unit: the indoor unit, which the gateway and the unit's own remote both
/// change, and the simulation around it, which the settings set. A profile reads and
/// writes the first; the second it only reads.
struct HvacState
{
    // ── The indoor unit, in the type's own codes, as its control registers take them ──
    bool power = false;
    uint8_t mode = 0;
    int16_t setpoint = 230;   // °C x10, whatever the type's register holds
    uint8_t fan = 0;
    uint8_t swing = 0;
    bool filter = false;      // where the type has a filter sign
    bool locked = false;      // the gateway locked the unit's own remote, where it can

    // What only a CoolPlug keeps: its own configuration, in its registers.
    struct
    {
        uint16_t vautThreshold = 0;   // °C x10 in 0.5 steps, 0 off
        bool vautAsAuto = false;
        uint8_t lineRole = 1;         // on the HVAC line: 0 slave, 1 master, 2 auto
        int16_t ownAmbient = 0;       // °C x10, what a controller feeds it
    } coolPlug;

    // ── The simulation: the settings, and what follows from them ──
    uint8_t index = 0;        // which simulator, 0-based
    uint8_t address = 0;      // its Modbus address
    int16_t room = 0;         // °C x10, as it is right now
    uint16_t failure = 0;     // 0: none
    bool indoor = true;       // the interface still reaches its indoor unit
    HvacDemand demand = HvacDemand::None;

    // ── What a write asks of the simulator ──
    bool restart = false;     // go off the bus for a while, as a reset does
    uint8_t newAddress = 0;   // and come back at this address; 0 for the same
};

class HvacProfile
{
public:
    /// What the type has: the codes its remote sets, and the parts not every type has.
    struct Traits
    {
        std::span<const ModeKind> modes;   // by code; ModeKind::None where a code is not a mode
        uint8_t fans;                      // fan codes 0 to fans - 1
        uint8_t swings;                    // louvre codes 0 to swings - 1
        int16_t setpointMin, setpointMax, setpointStep;   // °C x10
        bool room;                         // reports the room temperature
        bool filter;                       // has a filter sign
        bool lock;                         // the gateway can lock the unit's remote
        bool indoorLink;                   // reports whether it reaches its indoor unit
    };

    /// A point worth showing beside the unit: the ones the gateway reads, mostly.
    struct Point
    {
        ModbusTable table;
        uint16_t address;
    };

    const Traits traits;
    const std::span<const Point> points;

    HvacProfile(const Traits& t, std::span<const Point> p) : traits(t), points(p) {}
    virtual ~HvacProfile() = default;

    ModeKind KindOf(uint32_t mode) const
    {
        return mode < traits.modes.size() ? traits.modes[mode] : ModeKind::None;
    }

    /// How the indoor unit starts: at boot, and when a simulator takes this type.
    virtual void Reset(HvacState& s) const = 0;

    /// Whether it has that table. A function on one it has not is refused (Illegal
    /// Function), as it would be by a device without it.
    virtual bool Has(ModbusTable table) const = 0;

    /// A point's value as the unit answers it now; false where it has none.
    virtual bool Read(const HvacState& s, ModbusTable table, uint16_t address, uint16_t& value) const = 0;

    /// 0 when `value` may be written there, or the exception code that refuses it.
    virtual uint8_t CheckWrite(const HvacState& s, ModbusTable table, uint16_t address, uint16_t value) const = 0;

    /// Write it, once checked. False when the unit does not answer this write.
    virtual bool Write(HvacState& s, ModbusTable table, uint16_t address, uint16_t value) const = 0;

    /// A function code besides the eight that read and write the tables: the reply's
    /// PDU length, or 0 to refuse it (Illegal Function).
    virtual size_t Other(const HvacState&, const uint8_t* /*pdu*/, size_t /*len*/, uint8_t* /*reply*/) const { return 0; }

    /// What only this type has to say, for `hvac status`.
    virtual void Report(const HvacState&, ReplyObject&) const {}
};

/// The setpoints taken where a type's own range is not known, °C x10: a guess at what
/// an indoor unit takes. Anything outside is refused with Illegal Data Value.
constexpr int16_t HVAC_SETPOINT_MIN = 100;
constexpr int16_t HVAC_SETPOINT_MAX = 350;

/// One of the gateway's types, and whether and how the testbench plays it.
struct HvacTypeInfo
{
    HvacType type;
    const char* code;             // as the gateway's HVAC list shows it: "COP"
    HvacTransport transport;
    const HvacProfile* profile;   // nullptr: not simulated (yet)
};

/// The type with that number, or nullptr for a number the gateway does not have.
const HvacTypeInfo* FindHvacType(uint32_t number);

// The profiles, one file each.
const HvacProfile& CoolPlugProfile();
const HvacProfile& HisenseProfile();
const HvacProfile& DaikinProfile();
const HvacProfile& MitsubishiProfile();
const HvacProfile& SamsungProfile();
