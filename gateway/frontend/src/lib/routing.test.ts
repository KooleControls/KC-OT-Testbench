import { describe, expect, it } from "vitest"
import { DEFAULT_ROUTING, connect, partnerOf, type Routing } from "./routing"

describe("routing", () => {
  it("starts with the real thermostat on the real boiler", () => {
    expect(partnerOf(DEFAULT_ROUTING, "thermostat")).toBe("boiler")
    expect(partnerOf(DEFAULT_ROUTING, "boiler")).toBe("thermostat")
    expect(partnerOf(DEFAULT_ROUTING, "thermostatSim")).toBeNull()
    expect(partnerOf(DEFAULT_ROUTING, "boilerSim")).toBeNull()
  })

  it("takes a slave away from the master that had it", () => {
    const r = connect(DEFAULT_ROUTING, "thermostatSim", "boiler")
    expect(r).toEqual({ thermostat: null, thermostatSim: "boiler" })
  })

  it("connects from the slave's side too, freeing the master's old slave", () => {
    const start: Routing = { thermostat: "boiler", thermostatSim: null }
    const r = connect(start, "boilerSim", "thermostat")
    expect(r).toEqual({ thermostat: "boilerSim", thermostatSim: null })
    expect(partnerOf(r, "boiler")).toBeNull()
  })

  it("leaves a master open", () => {
    expect(connect(DEFAULT_ROUTING, "thermostat", null)).toEqual({ thermostat: null, thermostatSim: null })
  })

  it("leaves a slave open", () => {
    expect(connect(DEFAULT_ROUTING, "boiler", null)).toEqual({ thermostat: null, thermostatSim: null })
  })

  it("refuses to pair two masters", () => {
    expect(() => connect(DEFAULT_ROUTING, "thermostat", "thermostatSim")).toThrow()
  })

  it("does not change the routing it is given", () => {
    const start: Routing = { thermostat: "boiler", thermostatSim: null }
    connect(start, "thermostatSim", "boiler")
    expect(start).toEqual({ thermostat: "boiler", thermostatSim: null })
  })
})
