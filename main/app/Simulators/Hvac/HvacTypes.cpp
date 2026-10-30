#include "HvacProfile.h"

const HvacTypeInfo* FindHvacType(uint32_t number)
{
    // Every type the gateway has, so a simulator takes the gateway's numbers, even the
    // ones not played here.
    //
    // OpenTherm and the relay are not Modbus: the boiler simulator plays the first, and
    // the second is a contact with nothing on a bus to answer.
    //
    // A CoolMasterNet (CMN, CMA) is reached over the network, and it is one Modbus slave
    // for all its indoor units: the gateway asks every one at unit id 1, and its address
    // is where that unit's registers start (HvacCoolAutomation.cpp, ten from there). So
    // its simulators will be one Modbus TCP server at unit 1 that finds the simulator by
    // register, not by unit id -- a profile per indoor unit as here, and a Find() of its
    // own in HvacSimManager. Until that server is there, they are listed and not served.
    static const HvacTypeInfo TYPES[] = {
        { HvacType::NA, "NA", HvacTransport::None, nullptr },
        { HvacType::OTH, "OTH", HvacTransport::None, nullptr },
        { HvacType::REL, "REL", HvacTransport::None, nullptr },
        { HvacType::SAM, "SAM", HvacTransport::ModbusRtu, &SamsungProfile() },
        { HvacType::DAI, "DAI", HvacTransport::ModbusRtu, &DaikinProfile() },
        { HvacType::CMN, "CMN", HvacTransport::ModbusTcp, nullptr },
        { HvacType::CMA, "CMA", HvacTransport::ModbusTcp, nullptr },
        { HvacType::MIT, "MIT", HvacTransport::ModbusRtu, &MitsubishiProfile() },
        { HvacType::COP, "COP", HvacTransport::ModbusRtu, &CoolPlugProfile() },
        { HvacType::HIS, "HIS", HvacTransport::ModbusRtu, &HisenseProfile() },
    };

    for (const HvacTypeInfo& t : TYPES)
        if (static_cast<uint32_t>(t.type) == number)
            return &t;
    return nullptr;
}
