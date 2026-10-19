#include "BoilerSimulator.h"
#include "OpenThermFrame.h"
#include "SettingsManager.h"
#include "ReplyWriter.h"

using namespace opentherm;

namespace {

// The plug-in board's counts: the held setpoint for three id 9 reads, then 0 for three
// more, and once more 0 as the read that ends it.
constexpr uint8_t HOLD_SENDS = 3;
constexpr uint8_t HOLD_RESETS = 3;

const char* ModeName(uint32_t mode)
{
    switch (mode)
    {
    case 1:  return "heat";
    case 2:  return "cool";
    case 3:  return "off";
    default: return "thermostat";
    }
}

} // namespace

void BoilerSimulator::RegisterSettings(SettingsManager& settings)
{
    settings.Register({ &waterTemp_, &modulation_, &flame_, &chActive_, &coolActive_, &dhwActive_,
                        &maxCapacity_, &config_, &memberId_, &mode_, &holdSetpoint_ });
}

BoilerSimulator::Mode BoilerSimulator::CurrentMode()
{
    const uint32_t mode = mode_.Get();
    return mode <= static_cast<uint32_t>(Mode::Off) ? static_cast<Mode>(mode) : Mode::Thermostat;
}

uint8_t BoilerSimulator::SlaveFlags() const
{
    uint8_t flags = (dhwActive_.Get() ? SLAVE_DHW_MODE : 0) | (flame_.Get() ? SLAVE_FLAME : 0);
    switch (CurrentMode())
    {
    case Mode::Thermostat:
        flags |= (chActive_.Get() ? SLAVE_CH_MODE : 0) | (coolActive_.Get() ? SLAVE_COOLING : 0);
        break;
    case Mode::Heat: flags |= SLAVE_CH_MODE; break;
    case Mode::Cool: flags |= SLAVE_COOLING; break;
    case Mode::Off:  break;
    }
    return flags;
}

void BoilerSimulator::BeginHold()
{
    if (!holdSetpoint_.Get())
        return;

    // Mid-hold, the value already held is still the one from before the first switch.
    // Otherwise it is the last room setpoint the thermostat wrote: its old mode's.
    const float value = hold_.active ? hold_.value
                      : (heard_.known & KNOWN_ROOM_SETPOINT) ? heard_.roomSetpoint : 0.0f;

    // Nothing to hold before the thermostat has said a setpoint, and 0 on id 9 means
    // "no override" anyway.
    if (value == 0.0f)
        return;

    hold_ = { true, HOLD_SENDS, HOLD_RESETS, value };
}

float BoilerSimulator::NextOverride()
{
    if (!hold_.active)
        return 0;
    if (hold_.sends > 0)
    {
        hold_.sends--;
        return hold_.value;
    }
    if (hold_.resets > 0)
        hold_.resets--;
    else
        hold_.active = false;
    return 0;
}

uint32_t BoilerSimulator::Answer(uint32_t request, uint32_t nowMs)
{
    const uint8_t id = DataIdOf(request);
    const uint16_t value = ValueOf(request);

    LOCK(mutex_);
    heard_.requests++;
    heard_.lastRequestMs = nowMs;

    switch (TypeOf(request))
    {
    case MsgType::ReadData: {
        bool known = false;
        const uint16_t answer = Read(id, value, known);
        if (known)
            return Make(MsgType::ReadAck, id, answer);
        break;
    }
    case MsgType::WriteData:
        if (Write(id, value))
            return Make(MsgType::WriteAck, id, value);
        break;
    default:
        // Not a request a master sends: say so rather than pretend.
        return Make(MsgType::DataInvalid, id, value);
    }

    heard_.unknownIds++;
    return Make(MsgType::UnknownDataId, id, value);
}

uint16_t BoilerSimulator::Read(uint8_t dataId, uint16_t value, bool& known)
{
    known = true;
    switch (dataId)
    {
    case 0: {
        // The master's flags ride on the read; the answer echoes them and adds ours.
        heard_.masterFlags = HighByte(value);
        heard_.known |= KNOWN_STATUS;

        const uint8_t flags = SlaveFlags();
        const bool forced = CurrentMode() != Mode::Thermostat;
        const uint8_t modeAnswer = forced ? (SLAVE_DECIDES_HEAT_COOL | (flags & (SLAVE_CH_MODE | SLAVE_COOLING))) : 0;
        // Noticed here, where the thermostat hears it, as the plug-in board did: a change
        // that came and went between two reads was never a change to the thermostat.
        if (forced && modeAnswer != lastModeAnswer_)
            BeginHold();
        lastModeAnswer_ = modeAnswer;
        return Bytes(heard_.masterFlags, flags);
    }
    case 3: {
        // Remembered so `sim status` can say when a changed config last went out.
        heard_.lastConfigReadMs = heard_.lastRequestMs;
        heard_.known |= KNOWN_CONFIG_READ;
        uint8_t config = static_cast<uint8_t>(config_.Get()) & ~SLAVE_DECIDES_HEAT_COOL;
        if (CurrentMode() != Mode::Thermostat)
            config |= SLAVE_DECIDES_HEAT_COOL;
        return Bytes(config, static_cast<uint8_t>(memberId_.Get()));
    }
    case 5:   return 0;   // no fault flags, no OEM fault code
    case 9:   return FromF88(NextOverride());   // remote override room setpoint
    case 15:  return Bytes(static_cast<uint8_t>(maxCapacity_.Get()), 0);
    case 17:  return FromF88(modulation_.Get());
    case 25:  return FromF88(waterTemp_.Get());
    case 125: return FromF88(PROTOCOL_VERSION);
    case 127: return Bytes(1, 0);   // product type 1, version 0
    default:
        known = false;
        return 0;
    }
}

bool BoilerSimulator::Write(uint8_t dataId, uint16_t value)
{
    switch (dataId)
    {
    case 1:
        heard_.controlSetpoint = ToF88(value);
        heard_.known |= KNOWN_CONTROL;
        return true;
    case 2:
        heard_.memberId = LowByte(value);
        heard_.known |= KNOWN_MEMBER;
        return true;
    case 7:
        heard_.coolingControl = ToF88(value);
        heard_.known |= KNOWN_COOLING_CONTROL;
        return true;
    case 14:
        heard_.maxModulation = ToF88(value);
        heard_.known |= KNOWN_MAX_MODULATION;
        return true;
    case 16:
        heard_.roomSetpoint = ToF88(value);
        heard_.known |= KNOWN_ROOM_SETPOINT;
        return true;
    case 24:
        heard_.roomTemp = ToF88(value);
        heard_.known |= KNOWN_ROOM_TEMP;
        return true;
    case 124:
        heard_.otVersion = ToF88(value);
        heard_.known |= KNOWN_VERSION;
        return true;
    case 8:     // CH2 setpoint
    case 23:    // CH2 room setpoint
    case 126:   // master product version
        return true;   // taken, and nothing here depends on it
    default:
        return false;
    }
}

void BoilerSimulator::Report(ReplyObject& out, uint32_t nowMs)
{
    LOCK(mutex_);
    out.field("requests", heard_.requests);
    out.field("unknownIds", heard_.unknownIds);
    if (heard_.requests > 0)
        out.field("lastRequestAgoMs", nowMs - heard_.lastRequestMs);
    if (heard_.known & KNOWN_CONFIG_READ)
        out.field("configReadAgoMs", nowMs - heard_.lastConfigReadMs);
    out.field("mode", ModeName(mode_.Get()));
    if (hold_.active)
    {
        out.field("holding", hold_.value);
        // Id 9 reads still to come: the sends, the resets and the one that ends it.
        out.field("holdReadsLeft", static_cast<uint32_t>(hold_.sends + hold_.resets + 1));
    }

    auto thermostat = out.object("thermostat");
    if (heard_.known & KNOWN_STATUS)
    {
        thermostat.field("ch", (heard_.masterFlags & MASTER_CH_ENABLE) != 0);
        thermostat.field("dhw", (heard_.masterFlags & MASTER_DHW_ENABLE) != 0);
        thermostat.field("cooling", (heard_.masterFlags & MASTER_COOLING_ENABLE) != 0);
    }
    if (heard_.known & KNOWN_CONTROL)
        thermostat.field("controlSetpoint", heard_.controlSetpoint);
    if (heard_.known & KNOWN_COOLING_CONTROL)
        thermostat.field("coolingControl", heard_.coolingControl);
    if (heard_.known & KNOWN_ROOM_SETPOINT)
        thermostat.field("roomSetpoint", heard_.roomSetpoint);
    if (heard_.known & KNOWN_ROOM_TEMP)
        thermostat.field("roomTemp", heard_.roomTemp);
    if (heard_.known & KNOWN_MAX_MODULATION)
        thermostat.field("maxModulation", heard_.maxModulation);
    if (heard_.known & KNOWN_MEMBER)
        thermostat.field("memberId", static_cast<uint32_t>(heard_.memberId));
    if (heard_.known & KNOWN_VERSION)
        thermostat.field("otVersion", heard_.otVersion);
}
