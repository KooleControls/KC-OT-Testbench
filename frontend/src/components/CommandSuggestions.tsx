import { CircleIcon, SquareTerminalIcon, TagIcon, ToggleLeftIcon } from "lucide-react"

import type { Suggestion } from "@/lib/commandline"

/**
 * What the prompt can offer for the line so far, and what the device says about
 * it: a short list above the input, and the metadata for whichever row is
 * selected.
 *
 * Every word in here came off `help` at runtime — the names, the types, which
 * arguments are required, and every description. There is no documentation in
 * this file and no permanent panel: it appears while it is relevant and goes
 * away, because a helper that is always on screen stops being read.
 */
export function CommandSuggestions({
  suggestions,
  selected,
  onSelect,
  onAccept,
}: {
  suggestions: Suggestion[]
  selected: number
  onSelect: (index: number) => void
  onAccept: (suggestion: Suggestion) => void
}) {
  if (suggestions.length === 0) return null
  const current = suggestions[Math.min(selected, suggestions.length - 1)]

  return (
    // Anchored to the input and drawn upward, so the transcript above it keeps
    // its place instead of the page reflowing on every keystroke.
    <div className="absolute bottom-full left-0 z-20 mb-2 flex max-w-full items-end gap-2">
      <div className="max-h-56 w-80 shrink-0 overflow-y-auto rounded-xl border bg-popover p-1 shadow-md">
        {suggestions.map((suggestion, index) => (
          <button
            key={`${suggestion.kind}-${suggestion.value}`}
            type="button"
            // The input keeps the keyboard, so a click must not take it away —
            // this is a helper for typing, not a menu to leave the prompt for.
            onMouseDown={(e) => e.preventDefault()}
            onMouseEnter={() => onSelect(index)}
            onClick={() => onAccept(suggestion)}
            className={`flex w-full items-center gap-2 rounded-md px-2 py-1 text-left ${
              index === selected ? "bg-accent" : ""
            }`}
          >
            <KindIcon kind={suggestion.kind} />
            <span className="shrink-0 font-mono text-xs">{suggestion.value}</span>
            {suggestion.description && (
              <span className="truncate text-xs text-muted-foreground">
                {suggestion.description}
              </span>
            )}
          </button>
        ))}
      </div>

      {/* Hidden where it would take the transcript's room rather than help. */}
      <div className="hidden max-h-72 w-80 shrink-0 overflow-y-auto rounded-xl border bg-popover p-3 shadow-md lg:block">
        <Details suggestion={current} />
      </div>
    </div>
  )
}

/** What the device declared about whatever is selected.
 *
 *  A command shows its arguments, because that is the next thing you have to
 *  type; an argument or one of its values shows itself. */
function Details({ suggestion }: { suggestion: Suggestion }) {
  if (suggestion.kind === "command") {
    const args = suggestion.command.arguments
    return (
      <div className="space-y-2">
        <div className="font-mono text-xs">{suggestion.command.name}</div>
        {suggestion.command.description ? (
          <p className="text-xs leading-snug text-muted-foreground">
            {suggestion.command.description}
          </p>
        ) : (
          <p className="text-xs leading-snug text-muted-foreground italic">
            No description declared.
          </p>
        )}
        {args.length === 0 ? (
          <p className="text-xs text-muted-foreground">Takes no arguments.</p>
        ) : (
          args.map((arg) => <ArgDetails key={arg.name} arg={arg} />)
        )}
      </div>
    )
  }

  return suggestion.arg ? <ArgDetails arg={suggestion.arg} /> : null
}

function ArgDetails({
  arg,
}: {
  arg: NonNullable<Suggestion["arg"]>
}) {
  return (
    <div className="space-y-0.5">
      <div className="flex items-baseline gap-2">
        <CircleIcon className="size-2.5 shrink-0 text-muted-foreground" />
        <span className="font-mono text-xs">{arg.name}</span>
        <span className="text-xs text-muted-foreground">{arg.type}</span>
        <span
          className={`ml-auto text-xs ${
            arg.required ? "text-destructive" : "text-muted-foreground"
          }`}
        >
          {arg.required ? "(required)" : "(optional)"}
        </span>
      </div>
      {arg.description && (
        <p className="pl-[1.125rem] text-xs leading-snug text-muted-foreground">
          {arg.description}
        </p>
      )}
      {arg.maxLength !== undefined && (
        <p className="pl-[1.125rem] text-xs text-muted-foreground">
          At most {arg.maxLength} characters.
        </p>
      )}
    </div>
  )
}

function KindIcon({ kind }: { kind: Suggestion["kind"] }) {
  const className = "size-3.5 shrink-0 text-muted-foreground"
  if (kind === "command") return <SquareTerminalIcon className={className} />
  if (kind === "argument") return <TagIcon className={className} />
  return <ToggleLeftIcon className={className} />
}
