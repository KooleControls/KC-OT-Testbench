#pragma once

#include "interfaces/SerialPort.h"

// ──────────────────────────────────────────────────────────────
// Role interface: the PCB1246 OpenTherm plug-in board, as the gateway sees it —
// a serial line to it, and the two lines that control its LPC: reset, and ISP
// (boot into the ROM bootloader, for flashing it).
// ──────────────────────────────────────────────────────────────

class OpenThermModule
{
public:
    virtual SerialPort& Serial() = 0;

    /// Hold the module in reset (true) or let it run (false).
    virtual void SetReset(bool asserted) = 0;

    /// Request the ROM bootloader at the next reset (true) or the firmware (false).
    virtual void SetIsp(bool asserted) = 0;

    virtual ~OpenThermModule() = default;
};
