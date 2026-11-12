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
    // its units are served by the Modbus TCP server (ModbusTcpManager), which finds the
    // simulator by register, not by unit id (HvacSimManager::FindByRegisters). The two
    // types are one driver in the gateway, so one profile here.
    static const HvacTypeInfo TYPES[] = {
        { HvacType::NA, "NA", HvacTransport::None, nullptr },
        { HvacType::OTH, "OTH", HvacTransport::None, nullptr },
        { HvacType::REL, "REL", HvacTransport::None, nullptr },
        { HvacType::SAM, "SAM", HvacTransport::ModbusRtu, &SamsungProfile() },
        { HvacType::DAI, "DAI", HvacTransport::ModbusRtu, &DaikinProfile() },
        { HvacType::CMN, "CMN", HvacTransport::ModbusTcp, &CoolMasterNetProfile() },
        { HvacType::CMA, "CMA", HvacTransport::ModbusTcp, &CoolMasterNetProfile() },
        { HvacType::MIT, "MIT", HvacTransport::ModbusRtu, &MitsubishiProfile() },
        { HvacType::COP, "COP", HvacTransport::ModbusRtu, &CoolPlugProfile() },
        { HvacType::HIS, "HIS", HvacTransport::ModbusRtu, &HisenseProfile() },
    };

    for (const HvacTypeInfo& t : TYPES)
        if (static_cast<uint32_t>(t.type) == number)
            return &t;
    return nullptr;
}
