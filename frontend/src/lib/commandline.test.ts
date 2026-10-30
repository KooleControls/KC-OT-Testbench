import { describe, expect, it } from "vitest"

import { matchCommand, signature, suggest, tokenize } from "./commandline"
import type { CommandDesc } from "./backend"

// A registry shaped like the device's, and small enough to reason about. Nothing
// under test knows these names — they arrive as data, exactly as `help` delivers
// them — so this fixture stands in for a build rather than copying one.
const commands: CommandDesc[] = [
  { name: "help", description: "Describe every command.", arguments: [] },
  { name: "system ping", description: "Check the device answers.", arguments: [] },
  { name: "system info", arguments: [] },
  {
    name: "led set",
    description: "Turn the indicator on or off.",
    arguments: [
      { name: "enabled", type: "bool", required: false, description: "Light it." },
    ],
  },
  {
    name: "partition write",
    arguments: [
      { name: "partition", type: "string", required: true, maxLength: 16 },
      { name: "offset", type: "uint32", required: false, description: "Where." },
    ],
  },
  { name: "partition list", arguments: [] },
]

const values = (input: string) => suggest(input, commands).map((s) => s.value)

describe("tokenize", () => {
  it("splits on spaces", () => {
    expect(tokenize("led set enabled=true")).toEqual(["led", "set", "enabled=true"])
  })

  it("keeps a quoted value whole", () => {
    expect(tokenize('settings set value="two words"')).toEqual([
      "settings",
      "set",
      "value=two words",
    ])
  })

  it("keeps an empty quoted value as a token", () => {
    expect(tokenize('value=""')).toEqual(["value="])
  })
})

describe("matchCommand", () => {
  it("prefers the longest name", () => {
    expect(matchCommand("partition write partition=x", commands)?.name).toBe(
      "partition write",
    )
  })

  it("matches a one-word command", () => {
    expect(matchCommand("help", commands)?.name).toBe("help")
  })

  it("does not match a prefix of a word", () => {
    expect(matchCommand("part", commands)).toBeUndefined()
  })
})

describe("suggest", () => {
  it("offers nothing for an empty line", () => {
    expect(suggest("", commands)).toEqual([])
  })

  it("offers the commands a prefix could become", () => {
    expect(values("part")).toEqual(["partition write", "partition list"])
  })

  it("carries the whole line each suggestion would produce", () => {
    const [first] = suggest("led s", commands)
    expect(first.value).toBe("led set")
    expect(first.line).toBe("led set ")
    expect(first.description).toBe("Turn the indicator on or off.")
  })

  it("still offers an exactly-typed name, so Tab adds the space", () => {
    expect(suggest("led set", commands)[0].line).toBe("led set ")
  })

  it("switches to arguments once the command is named", () => {
    const offered = suggest("partition write ", commands)
    expect(offered.map((s) => s.value)).toEqual(["partition", "offset"])
    expect(offered[0].kind).toBe("argument")
    expect(offered[0].line).toBe("partition write partition=")
    expect(offered[0].arg?.required).toBe(true)
    expect(offered[1].description).toBe("Where.")
  })

  it("narrows arguments by what has been typed", () => {
    expect(values("partition write off")).toEqual(["offset"])
  })

  it("does not offer an argument already on the line", () => {
    expect(values("partition write offset=1 ")).toEqual(["partition"])
  })

  it("offers a bool's values, the one type with a closed set", () => {
    const offered = suggest("led set enabled=", commands)
    expect(offered.map((s) => s.value)).toEqual(["true", "false"])
    expect(offered[0].kind).toBe("value")
    expect(offered[0].line).toBe("led set enabled=true")
    expect(offered[0].arg?.name).toBe("enabled")
  })

  it("narrows a bool's values by what has been typed", () => {
    expect(values("led set enabled=t")).toEqual(["true"])
  })

  it("offers nothing for a value whose type has no closed set", () => {
    // Inventing candidates here would be inventing metadata the device never
    // gave — a partition label is exactly the case that would tempt it.
    expect(suggest("partition write partition=o", commands)).toEqual([])
  })

  it("offers nothing for a line it knows nothing about", () => {
    expect(suggest("zzz", commands)).toEqual([])
  })
})

describe("signature", () => {
  it("brackets what is optional", () => {
    expect(signature(commands[4])).toBe(
      "partition write partition=<string> [offset=<uint32>]",
    )
  })

  it("is just the name when there are no arguments", () => {
    expect(signature(commands[0])).toBe("help")
  })
})
