import { describe, it, expect } from "vitest"
import { decodeFrame, encodeFrame } from "./opentherm"

describe("decodeFrame", () => {
  it("reads a master status request", () => {
    // Read-Data, id 0, CH and DHW enabled: 0x00000300 has two 1 bits, so no parity.
    const d = decodeFrame(0x00000300)
    expect(d.msgTypeName).toBe("Read-Data")
    expect(d.fromMaster).toBe(true)
    expect(d.dataId).toBe(0)
    expect(d.parityOk).toBe(true)
    expect(d.valueText).toBe("master: CH, DHW")
  })

  it("reads a slave status reply with its flags", () => {
    // Read-Ack, id 0, HB 0x03, LB 0x0A (CH mode, flame).
    const d = decodeFrame(encodeFrame(4, 0, 0x030a))
    expect(d.msgTypeName).toBe("Read-Ack")
    expect(d.fromMaster).toBe(false)
    expect(d.valueText).toBe("master: CH, DHW | slave: CH, Flame")
  })

  it("reads an f8.8 temperature, negative included", () => {
    expect(decodeFrame(encodeFrame(4, 25, 0x3780)).valueText).toBe("55.50 °C")
    expect(decodeFrame(encodeFrame(4, 27, 0xfe80)).valueText).toBe("-1.50 °C")
    expect(decodeFrame(encodeFrame(1, 1, 0x2800)).valueText).toBe("40 °C")
  })

  it("flags a bad parity bit", () => {
    const good = encodeFrame(0, 25, 0)
    expect(decodeFrame(good).parityOk).toBe(true)
    expect(decodeFrame((good ^ 0x80000000) >>> 0).parityOk).toBe(false)
  })

  it("names unknown ids by number and shows the bytes", () => {
    const d = decodeFrame(encodeFrame(4, 200, 0x0102))
    expect(d.dataIdName).toBe("Data id 200")
    expect(d.valueText).toBe("1 / 2")
  })

  it("formats the raw frame as eight hex digits", () => {
    expect(decodeFrame(0x80190000).hex).toBe("0x80190000")
    expect(decodeFrame(0x300).hex).toBe("0x00000300")
  })
})

describe("encodeFrame", () => {
  it("sets parity so the frame has an even number of 1 bits", () => {
    for (const [t, id, v] of [[0, 0, 0x300], [0, 25, 0], [1, 1, 0x2a80], [4, 3, 0xffff]]) {
      const f = encodeFrame(t, id, v)
      expect(f.toString(2).split("").filter((b) => b === "1").length % 2).toBe(0)
      const d = decodeFrame(f)
      expect([d.msgType, d.dataId, d.value]).toEqual([t, id, v])
    }
  })

  it("matches a known frame: Read-Data of the boiler temperature", () => {
    // 0x00190000 has three 1 bits, so the parity bit goes on.
    expect(encodeFrame(0, 25, 0)).toBe(0x80190000)
  })
})
