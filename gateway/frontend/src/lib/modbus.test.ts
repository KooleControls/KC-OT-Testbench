import { describe, it, expect } from "vitest"
import { bytesToHex, crc16, decodeFrame, formatPoint, hexToBytes, registerName, tableOf, withCrc } from "./modbus"
import { hvacType } from "./hvac"

const COOLPLUG = hvacType(8)!.registers
const HISENSE = hvacType(9)!.registers

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

describe("tables", () => {
  it("knows which table a function reaches", () => {
    expect(tableOf(0x01)).toBe("coils")
    expect(tableOf(0x05)).toBe("coils")
    expect(tableOf(0x02)).toBe("discrete")
    expect(tableOf(0x06)).toBe("holding")
    expect(tableOf(0x04)).toBe("input")
    expect(tableOf(0x2b)).toBeUndefined()
  })

  it("names a point from a map, and without one by its table", () => {
    expect(registerName(COOLPLUG, "holding", 145)).toBe("Mode")
    expect(registerName(undefined, "input", 7)).toBe("IR 7")
    expect(formatPoint(undefined, "holding", 145, 2)).toBe("2")
  })
})

describe("decodeFrame", () => {
  it("reads a read request and, with it, its reply", () => {
    const request = decodeFrame(withCrc([0x03, 0x03, 0x00, 0x91, 0x00, 0x01]), "request", 8, undefined, COOLPLUG)
    expect(request.unit).toBe(3)
    expect(request.functionName).toBe("Read holding registers")
    expect(request.registerText).toBe("145 Mode")
    expect(request.crcOk).toBe(true)
    expect(request.read).toEqual({ start: 145, count: 1 })

    const reply = decodeFrame(withCrc([0x03, 0x03, 0x02, 0x00, 0x01]), "reply", 7, request.read, COOLPLUG)
    expect(reply.registerText).toBe("145 Mode")
    expect(reply.valueText).toBe("Heat")
  })

  it("reads a frame without a map as plain numbers", () => {
    const request = decodeFrame(withCrc([0x03, 0x03, 0x00, 0x91, 0x00, 0x01]), "request", 8)
    expect(request.registerText).toBe("145")
    const reply = decodeFrame(withCrc([0x03, 0x03, 0x02, 0x00, 0x01]), "reply", 7, request.read)
    expect(reply.valueText).toBe("1")
  })

  it("reads a reply on its own as plain numbers", () => {
    expect(decodeFrame(withCrc([0x03, 0x03, 0x02, 0x00, 0xe6]), "reply", 7, undefined, COOLPLUG).valueText).toBe("230")
  })

  it("reads a range", () => {
    const reply = decodeFrame(
      withCrc([0x01, 0x03, 0x04, 0x00, 0x01, 0x00, 0x00]),
      "reply",
      9,
      { start: 144, count: 2 },
      COOLPLUG,
    )
    expect(reply.registerText).toBe("144-145")
    expect(reply.valueText).toBe("On/off: On | Mode: Cool")
  })

  it("reads writes", () => {
    const w6 = decodeFrame(withCrc([0x01, 0x06, 0x00, 0x61, 0x00, 0xd7]), "request", 8, undefined, COOLPLUG)
    expect(w6.registerText).toBe("97 Setpoint")
    expect(w6.valueText).toBe("21.5 °C")

    const w16 = decodeFrame(
      withCrc([0x01, 0x10, 0x00, 0x90, 0x00, 0x02, 0x04, 0x00, 0x01, 0x00, 0x01]),
      "request",
      13,
      undefined,
      COOLPLUG,
    )
    expect(w16.registerText).toBe("144-145")
    expect(w16.valueText).toBe("On/off: On | Mode: Heat")
  })

  it("reads a Hisense: discrete inputs, input registers and a coil", () => {
    const di = decodeFrame(withCrc([0x02, 0x02, 0x00, 0x00, 0x00, 0x01]), "request", 8, undefined, HISENSE)
    expect(di.functionName).toBe("Read discrete inputs")
    expect(di.registerText).toBe("0 On/off")
    expect(decodeFrame(withCrc([0x02, 0x02, 0x01, 0x01]), "reply", 6, di.read, HISENSE).valueText).toBe("On")

    const ir = decodeFrame(withCrc([0x02, 0x04, 0x00, 0x07, 0x00, 0x01]), "request", 8, undefined, HISENSE)
    expect(ir.registerText).toBe("7 Mode")
    expect(decodeFrame(withCrc([0x02, 0x04, 0x02, 0x00, 0x05]), "reply", 7, ir.read, HISENSE).valueText).toBe(
      "Auto (cooling)",
    )

    const coil = decodeFrame(withCrc([0x02, 0x05, 0x00, 0x00, 0xff, 0x00]), "request", 8, undefined, HISENSE)
    expect(coil.registerText).toBe("0 On/off")
    expect(coil.valueText).toBe("On")
    expect(decodeFrame(withCrc([0x02, 0x05, 0x00, 0x00, 0x00, 0x00]), "request", 8, undefined, HISENSE).valueText).toBe("Off")

    // The same address in another table is another point: HR 2 is the mode written.
    const hr = decodeFrame(withCrc([0x02, 0x06, 0x00, 0x02, 0x00, 0x04]), "request", 8, undefined, HISENSE)
    expect(hr.registerText).toBe("2 Mode")
    expect(hr.valueText).toBe("Auto")
  })

  it("reads bits for as many points as were asked", () => {
    const reply = decodeFrame(withCrc([0x02, 0x01, 0x01, 0x05]), "reply", 6, { start: 0, count: 3 })
    expect(reply.valueText).toBe("Coil 0: 1 | Coil 1: 0 | Coil 2: 1")
  })

  it("reads a write of several coils", () => {
    const w15 = decodeFrame(withCrc([0x02, 0x0f, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01]), "request", 10, undefined, HISENSE)
    expect(w15.registerText).toBe("0 On/off")
    expect(w15.valueText).toBe("On")
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
