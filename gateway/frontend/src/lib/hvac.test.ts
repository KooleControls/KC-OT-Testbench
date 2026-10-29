import { describe, it, expect } from "vitest"
import { formatPoint } from "./modbus"
import { HVAC_TYPES, hvacType, typeLabel } from "./hvac"

const map = (id: number) => hvacType(id)!.registers

describe("the types", () => {
  it("are the gateway's, by its numbers", () => {
    expect(HVAC_TYPES.map((t) => `${t.id} ${t.code}`)).toEqual([
      "0 NA",
      "1 OTH",
      "2 REL",
      "3 SAM",
      "4 DAI",
      "5 CMN",
      "6 CMA",
      "7 MIT",
      "8 COP",
      "9 HIS",
    ])
  })

  it("read as the gateway's list writes them", () => {
    expect(typeLabel(hvacType(0)!)).toBe("0 - none")
    expect(typeLabel(hvacType(2)!)).toBe("2 - REL - relay on/off")
    expect(typeLabel(hvacType(8)!)).toBe("8 - COP - CoolPlug via Modbus RTU")
    expect(typeLabel(hvacType(5)!)).toBe("5 - CMN - CoolMasterNet via Modbus TCP")
    expect(typeLabel(hvacType(3)!)).toBe("3 - SAM - Samsung via Intesis")
  })

  it("are simulated on Modbus RTU only, for now", () => {
    expect(HVAC_TYPES.filter((t) => t.simulated).map((t) => t.code)).toEqual(["SAM", "DAI", "MIT", "COP", "HIS"])
    expect(HVAC_TYPES.filter((t) => t.simulated).every((t) => t.via === "Modbus RTU")).toBe(true)
  })

  it("each name every code their remote offers", () => {
    for (const t of HVAC_TYPES.filter((x) => x.simulated)) {
      expect(t.modes.length, t.code).toBeGreaterThan(0)
      expect(t.fans.length, t.code).toBeGreaterThan(0)
      expect(t.swings.length, t.code).toBeGreaterThan(0)
    }
  })
})

describe("the CoolPlug's registers", () => {
  const cp = map(8)

  it("read as a CoolPlug means them", () => {
    expect(formatPoint(cp, "holding", 97, 225)).toBe("22.5 °C")
    expect(formatPoint(cp, "holding", 150, 0xfff6)).toBe("-1.0 °C")
    expect(formatPoint(cp, "holding", 144, 1)).toBe("On")
    expect(formatPoint(cp, "holding", 145, 1)).toBe("Heat")
    expect(formatPoint(cp, "holding", 146, 5)).toBe("Very low")
    expect(formatPoint(cp, "holding", 147, 1)).toBe("30°")
    expect(formatPoint(cp, "holding", 153, 2)).toBe("Cooling")
    expect(formatPoint(cp, "holding", 32, 19)).toBe("0.1.9")
  })

  it("spell out the line state: bit 0 is a healthy unit, not a fault", () => {
    expect(formatPoint(cp, "holding", 95, 0x01)).toBe("indoor connected")
    expect(formatPoint(cp, "holding", 95, 0x05)).toBe("indoor connected, failure")
    expect(formatPoint(cp, "holding", 95, 0x00)).toBe("indoor NOT connected")
  })

  it("take the compact status apart", () => {
    const v = 1 | (1 << 1) | (2 << 6) | (1 << 10) | (1 << 14)
    expect(formatPoint(cp, "holding", 96, v)).toBe("On, Heat, fan High, louvres 30°, therm on")
  })

  it("show registers it does not know as numbers", () => {
    expect(formatPoint(cp, "holding", 500, 7)).toBe("7")
  })
})

describe("the Hisense's points", () => {
  const his = map(9)

  it("read the mode with which way auto is going", () => {
    expect(formatPoint(his, "input", 7, 2)).toBe("Cool")
    expect(formatPoint(his, "input", 7, 6)).toBe("Auto (heating)")
    expect(formatPoint(his, "holding", 2, 4)).toBe("Auto")
    expect(formatPoint(his, "input", 7, 4)).toBe("4?")
  })

  it("swap up/down and left/right between writing and reading", () => {
    expect(formatPoint(his, "holding", 4, 1)).toBe("Up/down")
    expect(formatPoint(his, "input", 9, 1)).toBe("Left/right")
  })

  it("keep whole degrees, signed", () => {
    expect(formatPoint(his, "input", 1, 21)).toBe("21 °C")
    expect(formatPoint(his, "input", 1, 0xfffd)).toBe("-3 °C")
  })
})

describe("the Intesis boxes and the Daikin", () => {
  it("read their own codes", () => {
    expect(formatPoint(map(7), "holding", 1, 4)).toBe("Cool")
    expect(formatPoint(map(3), "holding", 3, 1)).toBe("Swing")
    expect(formatPoint(map(4), "holding", 3, 2)).toBe("Auto")
    expect(formatPoint(map(4), "holding", 10, 1)).toBe("Locked")
    expect(formatPoint(map(7), "holding", 11, 0)).toBe("none (OK)")
  })
})
