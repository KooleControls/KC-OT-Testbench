#pragma once

#include "host/ble_uuid.h"
#include <cstddef>
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// The BLE contract between a KC thermostat and a KC1245 gateway, as the thermostat
// sees it: the simulator here stands in for the thermostat, so this is the
// thermostat's half, copied from its BleGattProtocol.h and SessionProtocol.h. The
// gateway's KcBleProtocol.h is the other half. Change one side and the link stops
// pairing without a word, so a difference from either is a bug here.
//
// The gateway advertises and hosts the GATT table; the thermostat scans, connects as
// the central, pairs with the gateway's install code and subscribes.
// ──────────────────────────────────────────────────────────────
namespace kcble
{
    // 4b4300xx-9c1d-4f2e-be3a-6d5c1f0a7e10. BLE_UUID128_INIT takes the bytes
    // little-endian, i.e. the string reversed.

    // The command-transport service.
    inline constexpr ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(
        0x10, 0x7e, 0x0a, 0x1f, 0x5c, 0x6d, 0x3a, 0xbe,
        0x2e, 0x4f, 0x1d, 0x9c, 0x01, 0x00, 0x43, 0x4b);

    // Gateway -> thermostat: requests, as notifications we subscribe to.
    inline constexpr ble_uuid128_t CHR_INBOUND_UUID = BLE_UUID128_INIT(
        0x10, 0x7e, 0x0a, 0x1f, 0x5c, 0x6d, 0x3a, 0xbe,
        0x2e, 0x4f, 0x1d, 0x9c, 0x02, 0x00, 0x43, 0x4b);

    // Thermostat -> gateway: replies, as Write Requests. Encrypted and authenticated
    // only, so nothing reaches it before the install code has been proven.
    inline constexpr ble_uuid128_t CHR_OUTBOUND_UUID = BLE_UUID128_INIT(
        0x10, 0x7e, 0x0a, 0x1f, 0x5c, 0x6d, 0x3a, 0xbe,
        0x2e, 0x4f, 0x1d, 0x9c, 0x03, 0x00, 0x43, 0x4b);

    // ── Advertising payload ────────────────────────────────────
    // [ company:u16 LE ][ type:u8 ][ gateway id:u32 LE ] as manufacturer data in the
    // advertisement; the name rides in the scan response. The company is the
    // Bluetooth SIG's test id: KC holds none.
    inline constexpr uint16_t COMPANY_ID = 0xFFFF;
    inline constexpr uint8_t  MFG_TYPE_GATEWAY_V1 = 0x01;
    inline constexpr size_t   MFG_DATA_LEN = 2 + 1 + 4;
    inline constexpr size_t   MFG_OFF_TYPE = 2;
    inline constexpr size_t   MFG_OFF_ID = 3;

    // ── Session chunks ─────────────────────────────────────────
    // [ session:u16 LE ][ flags:u8 ][ payload ], one chunk per notification or write.
    // Only the gateway opens sessions; a reply goes back on the request's id.
    inline constexpr uint8_t FLAG_FINAL  = 0x01;   // last chunk this direction
    inline constexpr uint8_t FLAG_REJECT = 0x02;   // refused; the payload is the reason
    inline constexpr size_t  HEADER_LEN  = 3;

    // One chunk is one GATT operation: the 247-byte MTU both sides ask for, less the
    // 3-byte ATT header.
    inline constexpr uint16_t PREFERRED_MTU = 247;
    inline constexpr size_t   CHUNK_MAX = PREFERRED_MTU - 3;

    inline uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

    inline void WriteHeader(uint8_t* out, uint16_t session, uint8_t flags)
    {
        out[0] = static_cast<uint8_t>(session & 0xFF);
        out[1] = static_cast<uint8_t>(session >> 8);
        out[2] = flags;
    }
}
