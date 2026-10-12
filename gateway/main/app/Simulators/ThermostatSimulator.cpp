#include "ThermostatSimulator.h"
#include "OpenThermFrame.h"
#include "SettingsManager.h"
#include "ReplyWriter.h"
#include <algorithm>

using namespace opentherm;

namespace {

// One question per interval, round and round. Status every other frame, because it
// carries CH and DHW enable and a real thermostat keeps it that close.
constexpr uint8_t CYCLE[] = { 0, 1, 0, 25, 0, 17, 0, 24, 0, 16, 0, 3, 0, 125, 0, 124, 0, 2 };
constexpr size_t CYCLE_LEN = sizeof(CYCLE) / sizeof(CYCLE[0]);

} // namespace

void ThermostatSimulator::RegisterSettings(SettingsManager& settings)
{
    settings.Register({ &roomTemp_, &setpoint_, &flowTemp_, &chEnable_, &dhwEnable_, &interval_, &memberId_ });
}

bool ThermostatSimulator::Due(uint32_t nowMs)
{
    LOCK(mutex_);
    const uint32_t interval = std::max(interval_.Get(), MIN_INTERVAL_MS);
    return !sentAny_ || nowMs - lastSentMs_ >= interval;
}

uint32_t ThermostatSimulator::NextRequest(uint32_t nowMs)
{
    const uint8_t id = CYCLE[cycle_];
    cycle_ = (cycle_ + 1) % CYCLE_LEN;

    uint32_t frame;
    switch (id)
    {
    case 0: {
        const uint8_t flags = (chEnable_.Get() ? MASTER_CH_ENABLE : 0) | (dhwEnable_.Get() ? MASTER_DHW_ENABLE : 0);
        frame = Make(MsgType::ReadData, id, Bytes(flags, 0));
        break;
    }
    case 1:   frame = Make(MsgType::WriteData, id, FromF88(flowTemp_.Get())); break;
    case 2:   frame = Make(MsgType::WriteData, id, Bytes(0, static_cast<uint8_t>(memberId_.Get()))); break;
    case 16:  frame = Make(MsgType::WriteData, id, FromF88(setpoint_.Get())); break;
    case 24:  frame = Make(MsgType::WriteData, id, FromF88(roomTemp_.Get())); break;
    case 124: frame = Make(MsgType::WriteData, id, FromF88(PROTOCOL_VERSION)); break;
    default:  frame = Make(MsgType::ReadData, id, 0); break;   // 3, 17, 25, 125
    }

    LOCK(mutex_);
    if (awaiting_)
        heard_.unanswered++;
    awaiting_ = true;
    sentAny_ = true;
    lastSentMs_ = nowMs;
    heard_.requests++;
    return frame;
}

void ThermostatSimulator::OnResponse(uint32_t frame, uint32_t nowMs)
{
    LOCK(mutex_);
    awaiting_ = false;
    heard_.responses++;
    heard_.lastResponseMs = nowMs;

    const MsgType type = TypeOf(frame);
    if (type == MsgType::UnknownDataId)
    {
        heard_.unknownIds++;
        return;
    }
    if (type != MsgType::ReadAck && type != MsgType::WriteAck)
        return;

    const uint16_t value = ValueOf(frame);
    switch (DataIdOf(frame))
    {
    case 0:
        heard_.slaveFlags = LowByte(value);
        heard_.known |= KNOWN_STATUS;
        break;
    case 3:
        heard_.slaveConfig = HighByte(value);
        heard_.memberId = LowByte(value);
        heard_.known |= KNOWN_CONFIG;
        break;
    case 17:
        heard_.modulation = ToF88(value);
        heard_.known |= KNOWN_MODULATION;
        break;
    case 25:
        heard_.waterTemp = ToF88(value);
        heard_.known |= KNOWN_WATER;
        break;
    case 125:
        heard_.otVersion = ToF88(value);
        heard_.known |= KNOWN_VERSION;
        break;
    default:
        break;
    }
}

void ThermostatSimulator::Report(ReplyObject& out, uint32_t nowMs)
{
    LOCK(mutex_);
    out.field("requests", heard_.requests);
    out.field("responses", heard_.responses);
    out.field("unanswered", heard_.unanswered);
    out.field("unknownIds", heard_.unknownIds);
    if (heard_.responses > 0)
        out.field("lastResponseAgoMs", nowMs - heard_.lastResponseMs);

    auto boiler = out.object("boiler");
    if (heard_.known & KNOWN_STATUS)
    {
        boiler.field("fault", (heard_.slaveFlags & SLAVE_FAULT) != 0);
        boiler.field("ch", (heard_.slaveFlags & SLAVE_CH_MODE) != 0);
        boiler.field("dhw", (heard_.slaveFlags & SLAVE_DHW_MODE) != 0);
        boiler.field("flame", (heard_.slaveFlags & SLAVE_FLAME) != 0);
    }
    if (heard_.known & KNOWN_WATER)
        boiler.field("waterTemp", heard_.waterTemp);
    if (heard_.known & KNOWN_MODULATION)
        boiler.field("modulation", heard_.modulation);
    if (heard_.known & KNOWN_CONFIG)
    {
        boiler.field("memberId", static_cast<uint32_t>(heard_.memberId));
        boiler.field("dhwPresent", (heard_.slaveConfig & SLAVE_DHW_PRESENT) != 0);
        boiler.field("modulating", (heard_.slaveConfig & SLAVE_ON_OFF_CONTROL) == 0);
    }
    if (heard_.known & KNOWN_VERSION)
        boiler.field("otVersion", heard_.otVersion);
}
