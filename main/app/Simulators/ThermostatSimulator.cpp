#include "ThermostatSimulator.h"
#include "OpenThermFrame.h"
#include "SettingsManager.h"
#include "ReplyWriter.h"
#include <algorithm>

using namespace opentherm;

namespace {

// One question per interval, round and round. Status every other frame, because it
// carries CH, DHW and cooling enable and a real thermostat keeps it that close. The
// remote override (9) three times a round: the KC module answers an override for three
// reads of it and 0 after, so it must not wait a whole round between two reads.
constexpr uint8_t CYCLE[] = { 0, 1, 0, 9, 0, 25, 0, 17, 0, 24, 0, 9, 0, 16, 0, 3, 0, 125, 0, 9, 0, 124, 0, 2 };
constexpr size_t CYCLE_LEN = sizeof(CYCLE) / sizeof(CYCLE[0]);

const char* GateName(ThermostatSimulator::ModeGate gate)
{
    switch (gate)
    {
    case ThermostatSimulator::ModeGate::None: return "none";
    case ThermostatSimulator::ModeGate::Off:  return "off";
    case ThermostatSimulator::ModeGate::Heat: return "heat";
    case ThermostatSimulator::ModeGate::Cool: return "cool";
    case ThermostatSimulator::ModeGate::Auto: return "auto";
    }
    return "none";
}

} // namespace

void ThermostatSimulator::RegisterSettings(SettingsManager& settings)
{
    settings.Register({ &roomTemp_, &setpoint_, &flowTemp_, &chEnable_, &dhwEnable_, &coolEnable_, &autoDemand_,
                        &followOverride_, &interval_, &memberId_ });
}

bool ThermostatSimulator::Due(uint32_t nowMs)
{
    LOCK(mutex_);
    const uint32_t interval = std::max(interval_.Get(), MIN_INTERVAL_MS);
    return !sentAny_ || nowMs - lastSentMs_ >= interval;
}

void ThermostatSimulator::Demand(bool& ch, bool& cool) const
{
    ch = chEnable_.Get();
    cool = coolEnable_.Get();
    float target = setpoint_.Get();

    // The KC thermostat's mode says which way it may ask at all, the way its control
    // step does: Heat only heats, Cool only cools, Auto leaves both to the room and the
    // changeover to the gateway, and Off heats only against the frost setpoint -- which
    // this simulator can only tell with tsim.auto, as it has no room otherwise.
    switch (gate_.load(std::memory_order_relaxed))
    {
    case ModeGate::None:
    case ModeGate::Auto:
        break;
    case ModeGate::Heat:
        cool = false;
        break;
    case ModeGate::Cool:
        ch = false;
        break;
    case ModeGate::Off:
        cool = false;
        ch = ch && autoDemand_.Get();
        target = FROST_SETPOINT;
        break;
    }

    // With tsim.auto, what is still allowed is asked for as the room needs it: heat
    // below the setpoint, cooling above.
    if (autoDemand_.Get())
    {
        const float room = roomTemp_.Get();
        ch = ch && room < target;
        cool = cool && room > target;
    }
}

uint32_t ThermostatSimulator::NextRequest(uint32_t nowMs)
{
    const uint8_t id = CYCLE[cycle_];
    cycle_ = (cycle_ + 1) % CYCLE_LEN;

    uint32_t frame;
    switch (id)
    {
    case 0: {
        bool ch, cool;
        Demand(ch, cool);
        const uint8_t flags = (ch ? MASTER_CH_ENABLE : 0) | (dhwEnable_.Get() ? MASTER_DHW_ENABLE : 0)
                            | (cool ? MASTER_COOLING_ENABLE : 0);
        frame = Make(MsgType::ReadData, id, Bytes(flags, 0));
        break;
    }
    case 1:   frame = Make(MsgType::WriteData, id, FromF88(flowTemp_.Get())); break;
    case 2:   frame = Make(MsgType::WriteData, id, Bytes(0, static_cast<uint8_t>(memberId_.Get()))); break;
    case 16:  frame = Make(MsgType::WriteData, id, FromF88(setpoint_.Get())); break;
    case 24:  frame = Make(MsgType::WriteData, id, FromF88(roomTemp_.Get())); break;
    case 124: frame = Make(MsgType::WriteData, id, FromF88(PROTOCOL_VERSION)); break;
    default:  frame = Make(MsgType::ReadData, id, 0); break;   // 3, 9, 17, 25, 125
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
    case 3: {
        heard_.slaveConfig = HighByte(value);
        heard_.memberId = LowByte(value);

        // Cooling is held through a short absence, as the KC thermostat holds it: a
        // cleared bit is believed once it has stayed clear for COOLING_HOLD_MS. The
        // first answer is taken as it is, and so is cooling coming back.
        const bool cooling = (heard_.slaveConfig & SLAVE_COOLING_SUPPORTED) != 0;
        if (cooling || !(heard_.known & KNOWN_CONFIG) || !coolingSupported_)
        {
            coolingSupported_ = cooling;
            coolingClearing_ = false;
        }
        else if (!coolingClearing_)
        {
            coolingClearing_ = true;
            coolingClearSinceMs_ = nowMs;
        }
        else if (nowMs - coolingClearSinceMs_ >= COOLING_HOLD_MS)
        {
            coolingSupported_ = false;
            coolingClearing_ = false;
        }
        heard_.known |= KNOWN_CONFIG;
        break;
    }
    case 9:
        // A remote override: the boiler side wants the room at this setpoint, 0 for
        // none. The KC module sends one as a pulse, the value for three reads and then
        // 0, so it is taken once per pulse, as the KC thermostat does: a hand turning
        // the setpoint afterwards keeps what it set until the next override.
        heard_.remoteOverride = ToF88(value);
        heard_.known |= KNOWN_OVERRIDE;
        if (value != lastOverride_ && value != 0 && followOverride_.Get())
        {
            setpoint_.Set(heard_.remoteOverride);
            heard_.overridesFollowed++;
        }
        lastOverride_ = value;
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

ThermostatSimulator::Panel ThermostatSimulator::ReadPanel()
{
    Panel p;
    p.room = roomTemp_.Get();
    p.setpoint = setpoint_.Get();
    p.flow = flowTemp_.Get();
    Demand(p.ch, p.cool);

    LOCK(mutex_);
    p.coolingSupported = coolingSupported_;
    p.overrideKnown = (heard_.known & KNOWN_OVERRIDE) != 0;
    p.remoteOverride = heard_.remoteOverride;
    return p;
}

void ThermostatSimulator::Report(ReplyObject& out, uint32_t nowMs)
{
    // Only with a panel in front of it, so `sim status` reads as it always did without.
    const ModeGate gate = gate_.load(std::memory_order_relaxed);
    if (gate != ModeGate::None)
        out.field("mode", GateName(gate));

    LOCK(mutex_);
    out.field("requests", heard_.requests);
    out.field("responses", heard_.responses);
    out.field("unanswered", heard_.unanswered);
    out.field("unknownIds", heard_.unknownIds);
    out.field("overridesFollowed", heard_.overridesFollowed);
    if (heard_.responses > 0)
        out.field("lastResponseAgoMs", nowMs - heard_.lastResponseMs);

    auto boiler = out.object("boiler");
    if (heard_.known & KNOWN_STATUS)
    {
        boiler.field("fault", (heard_.slaveFlags & SLAVE_FAULT) != 0);
        boiler.field("ch", (heard_.slaveFlags & SLAVE_CH_MODE) != 0);
        boiler.field("dhw", (heard_.slaveFlags & SLAVE_DHW_MODE) != 0);
        boiler.field("flame", (heard_.slaveFlags & SLAVE_FLAME) != 0);
        boiler.field("cooling", (heard_.slaveFlags & SLAVE_COOLING) != 0);
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
        boiler.field("coolingSupported", coolingSupported_);
    }
    if (heard_.known & KNOWN_VERSION)
        boiler.field("otVersion", heard_.otVersion);
    if (heard_.known & KNOWN_OVERRIDE)
        boiler.field("override", heard_.remoteOverride);
}
