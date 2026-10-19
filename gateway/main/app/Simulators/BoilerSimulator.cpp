#include "BoilerSimulator.h"
#include "OpenThermFrame.h"
#include "SettingsManager.h"
#include "ReplyWriter.h"

using namespace opentherm;

void BoilerSimulator::RegisterSettings(SettingsManager& settings)
{
    settings.Register({ &waterTemp_, &modulation_, &flame_, &chActive_, &dhwActive_, &maxCapacity_,
                        &config_, &memberId_ });
}

uint8_t BoilerSimulator::SlaveFlags() const
{
    return (chActive_.Get() ? SLAVE_CH_MODE : 0) | (dhwActive_.Get() ? SLAVE_DHW_MODE : 0)
         | (flame_.Get() ? SLAVE_FLAME : 0);
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
    case 0:
        // The master's flags ride on the read; the answer echoes them and adds ours.
        heard_.masterFlags = HighByte(value);
        heard_.known |= KNOWN_STATUS;
        return Bytes(heard_.masterFlags, SlaveFlags());
    case 3:
        // Remembered so `sim status` can say when a changed config last went out.
        heard_.lastConfigReadMs = heard_.lastRequestMs;
        heard_.known |= KNOWN_CONFIG_READ;
        return Bytes(static_cast<uint8_t>(config_.Get()), static_cast<uint8_t>(memberId_.Get()));
    case 5:   return 0;   // no fault flags, no OEM fault code
    case 9:   return 0;   // no remote override of the room setpoint
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
    case 7:     // cooling control
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

    auto thermostat = out.object("thermostat");
    if (heard_.known & KNOWN_STATUS)
    {
        thermostat.field("ch", (heard_.masterFlags & MASTER_CH_ENABLE) != 0);
        thermostat.field("dhw", (heard_.masterFlags & MASTER_DHW_ENABLE) != 0);
    }
    if (heard_.known & KNOWN_CONTROL)
        thermostat.field("controlSetpoint", heard_.controlSetpoint);
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
