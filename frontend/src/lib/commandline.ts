/** Offering the next word of a typed console line, from what the device declared.
 *
 *  What this file is NOT: a parser. The line the terminal types is the line the
 *  device receives, verbatim — the console codec lives in the firmware
 *  (main/lib/protocol/ConsoleEnvelope.h) and nothing here translates a line into
 *  anything else. Every function below only ever offers, and the offer carries
 *  the line that accepting it would produce.
 *
 *  Even the offering knows no command: names, argument names, argument types and
 *  every description come from `help` at runtime.
 */

import type { CommandArgDesc, CommandDesc } from "@/lib/backend"

/** Split on spaces, except inside quotes, the way the device's decoder does. */
export function tokenize(input: string): string[] {
  const tokens: string[] = []
  let current = ""
  let quote: string | null = null
  let open = false

  for (const ch of input) {
    if (quote !== null) {
      if (ch === quote) quote = null
      else current += ch
      continue
    }
    if (ch === '"' || ch === "'") {
      quote = ch
      open = true
      continue
    }
    if (ch === " ") {
      if (open) tokens.push(current)
      current = ""
      open = false
      continue
    }
    current += ch
    open = true
  }
  if (open) tokens.push(current)
  return tokens
}

/** The longest registry name the line starts with. Longest, because a command
 *  name is usually two words and its first word may also be a command. */
export function matchCommand(
  input: string,
  commands: CommandDesc[],
): CommandDesc | undefined {
  const line = input.trimStart()
  let best: CommandDesc | undefined
  for (const command of commands) {
    if (line !== command.name && !line.startsWith(`${command.name} `)) continue
    if (!best || command.name.length > best.name.length) best = command
  }
  return best
}

/** What is being offered. Three kinds, because three things are completable and
 *  each is worth saying something different about. */
export type SuggestionKind = "command" | "argument" | "value"

export interface Suggestion {
  kind: SuggestionKind
  /** The word offered: `partition write`, `offset`, `true`. */
  value: string
  /** The WHOLE input line once this is accepted. Carried rather than computed by
   *  the caller, because only this file knows where the token being completed
   *  started. */
  line: string
  /** One line about it, when the device declared one. */
  description?: string
  /** The command this belongs to — itself, for a command suggestion. */
  command: CommandDesc
  /** The argument, for an argument or a value. */
  arg?: CommandArgDesc
}

/**
 * What the prompt offers for the line so far.
 *
 * Three completable things, all off the registry: a command name, an argument
 * name once a command is named, and — for a bool, the one type with a closed set
 * of values — the value itself. Empty when there is nothing to say, which is how
 * the popup knows to stay out of the way.
 */
export function suggest(input: string, commands: CommandDesc[]): Suggestion[] {
  const command = matchCommand(input, commands)

  // Still naming the command: the prefix is the WHOLE line, because a name has a
  // space in it. An exactly-typed name still offers itself, so Tab adds the
  // space that moves on to the arguments.
  if (!command || input.trimStart().length <= command.name.length) {
    const prefix = input.trimStart()
    if (prefix === "") return []
    return commands
      .filter((c) => c.name.startsWith(prefix))
      .map((c) => ({
        kind: "command" as const,
        value: c.name,
        line: `${c.name} `,
        description: c.description,
        command: c,
      }))
  }

  const token = input.endsWith(" ") ? "" : (tokenize(input).at(-1) ?? "")
  const head = input.slice(0, input.length - token.length)

  const eq = token.indexOf("=")
  if (eq > 0) {
    const name = token.slice(0, eq)
    const arg = command.arguments.find((a) => a.name === name)
    // Only a bool has a closed set of values. Anything else is the caller's to
    // know, and guessing at it would be inventing metadata the device never gave.
    if (arg?.type !== "bool") return []
    return ["true", "false"]
      .filter((v) => v.startsWith(token.slice(eq + 1)))
      .map((v) => ({
        kind: "value" as const,
        value: v,
        line: `${head}${name}=${v}`,
        command,
        arg,
      }))
  }

  // Arguments already on the line are not offered again.
  const used = new Set(
    tokenize(input.slice(command.name.length))
      .map((t) => t.slice(0, t.indexOf("=")))
      .filter(Boolean),
  )
  return command.arguments
    .filter((a) => !used.has(a.name) && a.name.startsWith(token))
    .map((a) => ({
      kind: "argument" as const,
      value: a.name,
      line: `${head}${a.name}=`,
      description: a.description,
      command,
      arg: a,
    }))
}

/** How a command reads above the prompt:
 *  `partition write partition=<string> [offset=<uint32>]`. */
export function signature(command: CommandDesc): string {
  const args = command.arguments.map((a) =>
    a.required ? `${a.name}=<${a.type}>` : `[${a.name}=<${a.type}>]`,
  )
  return [command.name, ...args].join(" ")
}
