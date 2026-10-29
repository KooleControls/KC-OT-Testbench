import { describe, it, expect } from "vitest"
import { bytesToHex, crc16, decodeFrame, formatRegister, hexToBytes, withCrc } from "./modbus"

describe("crc16", () => {
  it("matches the Modbus spec's example", () => {
    // 11 03 00 6B 00 03 goes on the wire with 76 87.
    expect(withCrc([0x11, 0x03, 0x00, 0x6b, 0x00, 0x03]).slice(-2)).toEqual([0x76, 0x87])
    expect(crc16([0x01, 0x03, 0x00, 0x00, 0x00, 0x0a])).toBe(0xcdc5)
  })
})

describe("hexToBytes", () => {
  it("takes spaces, and refuses half a byte", () => {
    expect(hexToBytes("01 03 00 61")).toEqual([0x01, 0x03, 0x00, 0x61])
    expect(hexToBytes("0103")).toEqual([0x01, 0x03])
    expect(hexToBytes("013")).toBeNull()
    expect(hexToBytes("zz")).toBeNull()
    expect(hexToBytes("")).toBeNull()
  })

  it("round-trips", () => {
    expect(bytesToHex(hexToBytes("0a ff 10")!)).toBe("0A FF 10")
  })
})

describe("formatRegister", () => {
  it("reads the gateway's registers as a CoolPlug means them", () => {
    expect(formatRegister(97, 225)).toBe("22.5 °C")
    expect(formatRegister(150, 0xfff6)).toBe("-1.0 °C")
    expect(formatRegister(144, 1)).toBe("On")
    expect(formatRegister(145, 1)).toBe("Heat")
    expect(formatRegister(146, 5)).toBe("Very low")
    expect(formatRegister(147, 1)).toBe("30°")
    expect(formatRegister(153, 2)).toBe("Cooling")
    expect(formatRegister(32, 19)).toBe("0.1.9")
  })

  it("spells out the line state: bit 0 is a healthy unit, not a fault", () => {
    expect(formatRegister(95, 0x01)).toBe("indoor connected")
    expect(formatRegister(95, 0x05)).toBe("indoor connected, failure")
    expect(formatRegister(95, 0x00)).toBe("indoor NOT connected")
  })

  it("takes the compact status apart", () => {
    const v = 1 | (1 << 1) | (2 << 6) | (1 << 10) | (1 << 14)
    expect(formatRegister(96, v)).toBe("On, Heat, fan High, louvres 30°, therm on")
  })

  it("shows registers it does not know as numbers", () => {
    expect(formatRegister(500, 7)).toBe("7")
  })
})

describe("decodeFrame", () => {
  it("reads a read request and, with it, its reply", () => {
    const request = decodeFrame(withCrc([0x03, 0x03, 0x00, 0x91, 0x00, 0x01]), "request", 8)
    expect(request.unit).toBe(3)
    expect(request.functionName).toBe("Read holding registers")
    expect(request.registerText).toBe("145 Mode")
    expect(request.crcOk).toBe(true)
    expect(request.read).toEqual({ start: 145, count: 1 })

    const reply = decodeFrame(withCrc([0x03, 0x03, 0x02, 0x00, 0x01]), "reply", 7, request.read)
    expect(reply.registerText).toBe("145 Mode")
    expect(reply.valueText).toBe("Heat")
  })

  it("reads a reply on its own as plain numbers", () => {
    expect(decodeFrame(withCrc([0x03, 0x03, 0x02, 0x00, 0xe6]), "reply", 7).valueText).toBe("230")
  })

  it("reads a range", () => {
    const reply = decodeFrame(
      withCrc([0x01, 0x03, 0x04, 0x00, 0x01, 0x00, 0x00]),
      "reply",
      9,
      { start: 144, count: 2 },
    )
    expect(reply.registerText).toBe("144-145")
    expect(reply.valueText).toBe("On/off: On | Mode: Cool")
  })

  it("reads writes", () => {
    const w6 = decodeFrame(withCrc([0x01, 0x06, 0x00, 0x61, 0x00, 0xd7]), "request", 8)
    expect(w6.registerText).toBe("97 Setpoint")
    expect(w6.valueText).toBe("21.5 °C")

    const w16 = decodeFrame(
      withCrc([0x01, 0x10, 0x00, 0x90, 0x00, 0x02, 0x04, 0x00, 0x01, 0x00, 0x01]),
      "request",
      13,
    )
    expect(w16.registerText).toBe("144-145")
    expect(w16.valueText).toBe("On/off: On | Mode: Heat")
  })

  it("reads an exception", () => {
    const d = decodeFrame(withCrc([0x01, 0x86, 0x03]), "reply", 5)
    expect(d.fc).toBe(6)
    expect(d.exception).toBe("Illegal data value")
  })

  it("reads device identification", () => {
    const d = decodeFrame(
      withCrc([0x01, 0x2b, 0x0e, 0x01, 0x81, 0x00, 0x00, 0x02, 0x00, 0x02, 0x43, 0x41, 0x01, 0x01, 0x50]),
      "reply",
      17,
    )
    expect(d.valueText).toBe("CA | P")
  })

  it("flags a bad CRC, and a frame kept only in part", () => {
    const bytes = withCrc([0x01, 0x03, 0x00, 0x61, 0x00, 0x01])
    bytes[7] ^= 1
    expect(decodeFrame(bytes, "request", 8).crcOk).toBe(false)

    const part = decodeFrame([0x01, 0x03, 0x00, 0x61], "request", 8)
    expect(part.truncated).toBe(true)
    expect(part.crcOk).toBe(false)
  })
})
