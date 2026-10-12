#pragma once

#include <cmath>
#include <cstdint>

// An OpenTherm frame (Protocol 2.2) taken apart and put together, for the simulators.
// The modem and the link pass frames through as numbers; only the simulators need to
// know what is in one.
//
//   bit 31      parity: makes the number of 1 bits in the frame even
//   bits 30-28  message type
//   bits 23-16  data id
//   bits 15-0   data value: HB (15-8) and LB (7-0)
namespace opentherm {

enum class MsgType : uint8_t
{
    ReadData = 0,
    WriteData = 1,
    InvalidData = 2,
    ReadAck = 4,
    WriteAck = 5,
    DataInvalid = 6,
    UnknownDataId = 7,
};

// Master status (id 0, high byte) and slave status (id 0, low byte) flags.
constexpr uint8_t MASTER_CH_ENABLE = 1 << 0;
constexpr uint8_t MASTER_DHW_ENABLE = 1 << 1;
constexpr uint8_t SLAVE_FAULT = 1 << 0;
constexpr uint8_t SLAVE_CH_MODE = 1 << 1;
constexpr uint8_t SLAVE_DHW_MODE = 1 << 2;
constexpr uint8_t SLAVE_FLAME = 1 << 3;

// Slave configuration (id 3, high byte).
constexpr uint8_t SLAVE_DHW_PRESENT = 1 << 0;
constexpr uint8_t SLAVE_ON_OFF_CONTROL = 1 << 1;   // clear: modulating

/// OpenTherm version these simulators claim to speak.
constexpr float PROTOCOL_VERSION = 2.2f;

inline MsgType TypeOf(uint32_t frame) { return static_cast<MsgType>((frame >> 28) & 0x7); }
inline uint8_t DataIdOf(uint32_t frame) { return static_cast<uint8_t>(frame >> 16); }
inline uint16_t ValueOf(uint32_t frame) { return static_cast<uint16_t>(frame); }
inline uint8_t HighByte(uint16_t value) { return static_cast<uint8_t>(value >> 8); }
inline uint8_t LowByte(uint16_t value) { return static_cast<uint8_t>(value); }

/// A frame from its parts, with the parity bit set so it is valid on the line.
inline uint32_t Make(MsgType type, uint8_t dataId, uint16_t value)
{
    const uint32_t body = (static_cast<uint32_t>(type) << 28) | (static_cast<uint32_t>(dataId) << 16) | value;
    return (__builtin_popcount(body) % 2 == 0) ? body : (body | 0x80000000u);
}

inline uint16_t Bytes(uint8_t high, uint8_t low) { return static_cast<uint16_t>((high << 8) | low); }

/// f8.8: signed fixed point, 1/256 per step. Temperatures, percentages, versions.
inline uint16_t FromF88(float v) { return static_cast<uint16_t>(static_cast<int16_t>(lroundf(v * 256.0f))); }
inline float ToF88(uint16_t v) { return static_cast<int16_t>(v) / 256.0f; }

} // namespace opentherm
