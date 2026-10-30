#pragma once

#include <cstddef>
#include <cstdint>

// A Modbus RTU frame (an ADU) taken apart and put together, for the RS485 layer and the
// simulators on it:
//
//   unit (1) | function code (1) | data (0-252) | CRC-16 (2, low byte first)
//
// RTU marks a frame's end with a silence of 3.5 characters, which this side cannot see:
// the bytes reach it through the MAX14830's FIFO and a stream buffer. So a frame's
// length is worked out from its own bytes instead -- from the function code, and for
// the variable ones from a count inside the frame -- and the CRC says whether that
// reading was right. A request and its reply have different lengths for the same
// function code, so each is asked for separately.
namespace modbus {

constexpr size_t MAX_ADU = 256;
constexpr size_t MAX_PDU = MAX_ADU - 3;   // without the unit and the CRC
constexpr uint8_t BROADCAST = 0;
constexpr uint8_t MAX_UNIT = 247;          // 248-255 are reserved

// The protocol's own limits on a count inside a frame: 125 registers or 2000 coils read
// (250 bytes), 123 registers or 1968 coils written (246 bytes). A count beyond them is
// not a frame, which saves waiting for one.
constexpr uint8_t MAX_READ_BYTES = 250;
constexpr uint8_t MAX_WRITE_BYTES = 246;

namespace fc {
constexpr uint8_t READ_COILS = 0x01;
constexpr uint8_t READ_DISCRETE_INPUTS = 0x02;
constexpr uint8_t READ_HOLDING_REGISTERS = 0x03;
constexpr uint8_t READ_INPUT_REGISTERS = 0x04;
constexpr uint8_t WRITE_SINGLE_COIL = 0x05;
constexpr uint8_t WRITE_SINGLE_REGISTER = 0x06;
constexpr uint8_t WRITE_MULTIPLE_COILS = 0x0F;
constexpr uint8_t WRITE_MULTIPLE_REGISTERS = 0x10;
constexpr uint8_t READ_WRITE_MULTIPLE_REGISTERS = 0x17;
constexpr uint8_t ENCAPSULATED = 0x2B;   // with MEI type 0x0E: Read Device Identification
constexpr uint8_t EXCEPTION = 0x80;       // or-ed into the function code of an exception
} // namespace fc

constexpr uint8_t MEI_READ_DEVICE_ID = 0x0E;

namespace ex {
constexpr uint8_t ILLEGAL_FUNCTION = 0x01;
constexpr uint8_t ILLEGAL_DATA_ADDRESS = 0x02;
constexpr uint8_t ILLEGAL_DATA_VALUE = 0x03;
constexpr uint8_t SLAVE_DEVICE_FAILURE = 0x04;
} // namespace ex

/// Modbus' CRC-16: polynomial 0xA001 (reflected 0x8005), from 0xFFFF.
inline uint16_t Crc16(const uint8_t* p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--)
    {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

/// Whether the last two bytes of `adu` are the CRC of the ones before them.
inline bool CrcOk(const uint8_t* adu, size_t len)
{
    if (len < 4)
        return false;
    const uint16_t crc = Crc16(adu, len - 2);
    return adu[len - 2] == (crc & 0xFF) && adu[len - 1] == (crc >> 8);
}

/// Append the CRC to the `len` bytes at `adu`, which has room for two more. Returns the
/// new length.
inline size_t AppendCrc(uint8_t* adu, size_t len)
{
    const uint16_t crc = Crc16(adu, len);
    adu[len] = crc & 0xFF;
    adu[len + 1] = crc >> 8;
    return len + 2;
}

/// Registers go on the wire high byte first.
inline uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline void Put16(uint8_t* p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

/// What a length function says about the bytes it was given.
constexpr size_t NEED_MORE = 0;            // the length is in bytes that have not come yet
constexpr size_t UNKNOWN = SIZE_MAX;       // no frame of this kind starts like this

/// The length of the request starting at `b`, of which `n` bytes are in.
inline size_t RequestLength(const uint8_t* b, size_t n)
{
    if (n >= 1 && b[0] > MAX_UNIT)
        return UNKNOWN;
    if (n < 2)
        return NEED_MORE;
    switch (b[1])
    {
    case fc::READ_COILS:
    case fc::READ_DISCRETE_INPUTS:
    case fc::READ_HOLDING_REGISTERS:
    case fc::READ_INPUT_REGISTERS:
    case fc::WRITE_SINGLE_COIL:
    case fc::WRITE_SINGLE_REGISTER:
        return 8;   // unit, fc, address, quantity or value, CRC
    case fc::WRITE_MULTIPLE_COILS:
        if (n < 7)
            return NEED_MORE;
        return b[6] > MAX_WRITE_BYTES ? UNKNOWN : 9 + b[6];   // ..., quantity, byte count, data, CRC
    case fc::WRITE_MULTIPLE_REGISTERS:
        if (n < 7)
            return NEED_MORE;
        return (b[6] > MAX_WRITE_BYTES || (b[6] & 1)) ? UNKNOWN : 9 + b[6];
    case fc::READ_WRITE_MULTIPLE_REGISTERS:
        if (n < 11)
            return NEED_MORE;
        return (b[10] > MAX_WRITE_BYTES || (b[10] & 1)) ? UNKNOWN : 13 + b[10];
    case fc::ENCAPSULATED:
        return 7;   // unit, fc, MEI type, read device id code, object id, CRC
    default:
        return UNKNOWN;
    }
}

/// The length of the reply starting at `b`, of which `n` bytes are in.
inline size_t ReplyLength(const uint8_t* b, size_t n)
{
    // A broadcast is never answered, so a reply is from 1-247.
    if (n >= 1 && (b[0] == BROADCAST || b[0] > MAX_UNIT))
        return UNKNOWN;
    if (n < 2)
        return NEED_MORE;
    if (b[1] & fc::EXCEPTION)
        return 5;   // unit, fc | 0x80, exception code, CRC
    switch (b[1])
    {
    case fc::READ_COILS:
    case fc::READ_DISCRETE_INPUTS:
        if (n < 3)
            return NEED_MORE;
        return b[2] > MAX_READ_BYTES ? UNKNOWN : 5 + b[2];   // unit, fc, byte count, data, CRC
    case fc::READ_HOLDING_REGISTERS:
    case fc::READ_INPUT_REGISTERS:
    case fc::READ_WRITE_MULTIPLE_REGISTERS:
        if (n < 3)
            return NEED_MORE;
        return (b[2] > MAX_READ_BYTES || (b[2] & 1)) ? UNKNOWN : 5 + b[2];
    case fc::WRITE_SINGLE_COIL:
    case fc::WRITE_SINGLE_REGISTER:
    case fc::WRITE_MULTIPLE_COILS:
    case fc::WRITE_MULTIPLE_REGISTERS:
        return 8;
    case fc::ENCAPSULATED: {
        // unit, fc, MEI, code, conformity, more follows, next object, object count,
        // then per object its id, its length and its bytes; then the CRC.
        if (n < 8)
            return NEED_MORE;
        size_t len = 8;
        for (uint8_t i = 0; i < b[7]; ++i)
        {
            if (n < len + 2)
                return NEED_MORE;
            len += 2 + b[len + 1];
            if (len > MAX_ADU)
                return UNKNOWN;
        }
        return len + 2;
    }
    default:
        return UNKNOWN;
    }
}

} // namespace modbus
