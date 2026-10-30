import { describe, expect, it } from "vitest"
import { matchesSearch, searchTerms, type Searchable } from "./simSearch"

const water: Searchable = { label: "Water temperature", key: "bsim.water", unit: "°C", ids: [25] }
const dhwPresent: Searchable = {
  label: "Hot water",
  description: "Has a hot water circuit",
  key: "bsim.dhwpresent",
  ids: [3],
  keywords: "capability dhw",
}
const otVersion: Searchable = { label: "OpenTherm version", ids: [125] }

const matches = (item: Searchable, query: string) => matchesSearch(item, searchTerms(query))

describe("simulator search", () => {
  it("shows everything for an empty query", () => {
    expect(matches(water, "")).toBe(true)
    expect(matches(water, "   ")).toBe(true)
  })

  it("matches the label, description, key and keywords, ignoring case", () => {
    expect(matches(water, "WATER")).toBe(true)
    expect(matches(dhwPresent, "circuit")).toBe(true)
    expect(matches(water, "bsim.water")).toBe(true)
    expect(matches(dhwPresent, "capability")).toBe(true)
  })

  it("needs every term to match", () => {
    expect(matches(water, "water temp")).toBe(true)
    expect(matches(water, "water pressure")).toBe(false)
  })

  it("finds a data id however it is written", () => {
    for (const q of ["25", "id 25", "ID25", "#25", "data id 25"]) expect(matches(water, q)).toBe(true)
  })

  it("matches a number only as a whole word", () => {
    expect(matches(otVersion, "25")).toBe(false)
    expect(matches(otVersion, "125")).toBe(true)
    expect(matches(water, "2")).toBe(false)
  })

  it("does not match what is not there", () => {
    expect(matches(water, "flame")).toBe(false)
    expect(matches(water, "id 17")).toBe(false)
  })
})
