#pragma once

#include "interfaces/Led.h"
#include "interfaces/Relay.h"
#include "interfaces/DigitalInput.h"
#include "interfaces/SerialPort.h"
#include "interfaces/OpenThermModule.h"

// ──────────────────────────────────────────────────────────────
// The board layer's provider: the ROLES every board owes, in application
// vocabulary. `interfaces/` holds the individual roles; this states which of
// them a board must bind.
//
// The bottom layer's half of the same context/provider pair the two layers
// above use — BoardContext owns the driver instances and answers this.
//
// Deliberately roles ONLY. A concrete driver accessor (the escape hatch, for
// when the application needs a driver's full API) stays off this interface and
// on BoardContext itself, where it is checked at compile time. That is what
// keeps this list from becoming the union of every board's peripherals: the day
// one board grows a display, no other board owes a MockDisplay.
//
// A board without the hardware for a role binds a Mock* driver — MockLed and
// friends exist for exactly that, so satisfying every role is the existing
// discipline and not a new tax. What this adds is where the failure lands: a
// board that forgets a role now fails in the board, saying it left a pure
// virtual unimplemented, instead of failing later at some call site in
// application code.
//
// Multi-instance roles get a semantic enum parameter (Sensor::Ambient, never
// Sensor_2).
// ──────────────────────────────────────────────────────────────

enum class RelayId { Relay1, Relay2 };

enum class InputId { Input1, Input2, ResetButton, SdCardDetect };

enum class SerialPortId
{
    XBee,       // the XBee radio module
    Service,    // the FTDI USB-serial service port
    Rs485,      // the RS485 bus
};

class BoardProvider
{
public:
    virtual ~BoardProvider() = default;

    virtual Led& GetLed() = 0;
    virtual Relay& GetRelay(RelayId id) = 0;
    virtual DigitalInput& GetInput(InputId id) = 0;
    virtual SerialPort& GetSerialPort(SerialPortId id) = 0;
    virtual OpenThermModule& GetOpenThermModule() = 0;
};
