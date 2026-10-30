#pragma once

#include <cstdint>

/// Who sends or receives an OpenTherm frame on the bench.
enum class Endpoint : uint8_t
{
    None,            // nobody: a frame going nowhere
    Thermostat,      // the real thermostat, on the module's thermostat line
    Boiler,          // the real boiler, on the module's boiler line
    ThermostatSim,   // the thermostat simulator, on this testbench
    BoilerSim,       // the boiler simulator, on this testbench
    Testbench,       // a frame put on a line by hand (`module send`)
};

/// The name the commands and the web UI use.
inline const char* EndpointName(Endpoint e)
{
    switch (e)
    {
    case Endpoint::None:          return "none";
    case Endpoint::Thermostat:    return "thermostat";
    case Endpoint::Boiler:        return "boiler";
    case Endpoint::ThermostatSim: return "thermostatSim";
    case Endpoint::BoilerSim:     return "boilerSim";
    case Endpoint::Testbench:     return "testbench";
    }
    return "unknown";
}
