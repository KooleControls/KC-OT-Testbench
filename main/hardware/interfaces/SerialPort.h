#pragma once

#include "Stream.h"
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// Role interface: a serial line. A byte Stream — read() blocks up to its timeout
// for the first byte, write() queues — plus the line's speed.
// ──────────────────────────────────────────────────────────────

class SerialPort : public Stream
{
public:
    virtual void SetBaudRate(uint32_t baud) = 0;
};
