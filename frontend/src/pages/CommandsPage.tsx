import { useLayoutEffect, useMemo, useRef, useState } from "react"
import { SendIcon, SquareTerminalIcon, TrashIcon } from "lucide-react"

import { CommandSuggestions } from "@/components/CommandSuggestions"
import { Button } from "@/components/ui/button"
import { useCommands } from "@/hooks/use-commands"
import { backend } from "@/lib/backend"
import { matchCommand, signature, suggest, type Suggestion } from "@/lib/commandline"

/**
 * Running commands, the old-school way: type a line, read the answer.
 *
 * What is typed here goes to the device as those bytes — `led set enabled=true`
 * and not an envelope — and what comes back is what the device wrote, which is
 * YAML because that is the console codec's reply half. Nothing in this file
 * translates between the two: the codec is in the firmware
 * (main/lib/protocol/ConsoleEnvelope.h and YamlReplyWriter.h), and the browser
 * is simply a second client of it.
 *
 * The registry is used, and only for the prompt: the popup above the input
 * offers commands, then that command's arguments, then a bool's values, with
 * whatever the device declared about each beside it. It is assistance and never
 * a rewrite — a line the registry has never heard of is sent exactly as typed,
 * and the device is what refuses it. So adding a command to the firmware adds it
 * here, and this page holds no list of its own.
 *
 * NOT the Console. That page is the device's stdout — lines it printed without
 * being asked — and the two are kept apart on purpose: a reply belongs next to
 * the request that asked for it, and a log line belongs to nothing.
 */

/** Scrollback, in exchanges. Each keeps its whole text, so this is a memory
 *  bound as much as a scrollback one. */
const MAX_EXCHANGES = 200

/** Lines of one record drawn before it is folded. Enough for an ordinary reply
 *  whole, short enough that `help` does not bury the prompt. */
const FOLD_AFTER = 14

/** How much of one record is kept at all. `partition read` answers in megabytes,
 *  and a megabyte of text in the DOM hangs the tab. */
const RECORD_LIMIT = 32 * 1024

interface Exchange {
  id: number
  at: Date
  /** The line as sent. */
  command: string
  /** Reply records, in order, as they arrive. */
  records: string[]
  /** A refusal that never became a record: RESET, a timeout, a dropped socket. */
  error?: string
  elapsedMs?: number
}

let nextId = 1

export default function CommandsPage() {
  const { commands } = useCommands()
  const [exchanges, setExchanges] = useState<Exchange[]>([])
  const [input, setInput] = useState("")
  const [busy, setBusy] = useState(false)
  const [selected, setSelected] = useState(0)
  const [dismissed, setDismissed] = useState(false)
  const history = useRef<string[]>([])
  const historyAt = useRef(-1)
  const inputRef = useRef<HTMLInputElement>(null)

  const suggestions = useMemo(
    () => (dismissed ? [] : suggest(input, commands)),
    [dismissed, input, commands],
  )
  const current = matchCommand(input, commands)

  function edit(line: string) {
    setInput(line)
    setSelected(0)
    setDismissed(false)
  }

  function update(id: number, change: (e: Exchange) => Exchange) {
    setExchanges((prev) => prev.map((e) => (e.id === id ? change(e) : e)))
  }

  async function run() {
    const line = input.trim()
    if (!line || busy) return

    history.current = [...history.current.filter((h) => h !== line), line]
    historyAt.current = -1
    setInput("")
    setDismissed(false)
    setBusy(true)

    const id = nextId++
    setExchanges((prev) =>
      [...prev, { id, at: new Date(), command: line, records: [] }].slice(-MAX_EXCHANGES),
    )

    const started = performance.now()
    try {
      const reply = await backend.runConsole(line, {
        // Records completed before the last one, as they arrive: a command that
        // reports progress is watched rather than summarised at the end.
        onRecord: (text) =>
          update(id, (e) => ({ ...e, records: [...e.records, clip(text)] })),
      })
      const elapsedMs = Math.round(performance.now() - started)

      // The records already seen came through onRecord; only the last is new.
      // A reply that declared a body has none left over — its header WAS that
      // last record, and everything after the divider is bytes.
      const tail =
        reply.body === null
          ? reply.records.slice(-1).map(clip)
          : [
              `<${reply.body.length} bytes of ${reply.contentType}` +
                `${reply.contentEncoding ? `, ${reply.contentEncoding}` : ""}>`,
            ]

      update(id, (e) => ({ ...e, records: [...e.records, ...tail], elapsedMs }))
    } catch (e) {
      // A RESET carries the device's reason — an unknown command, a missing
      // argument, `busy`. A timeout and a dropped socket arrive the same way,
      // and all three are the device's answer as much as a record would be.
      update(id, (x) => ({
        ...x,
        error: e instanceof Error ? e.message : "Failed",
        elapsedMs: Math.round(performance.now() - started),
      }))
    } finally {
      setBusy(false)
      inputRef.current?.focus()
    }
  }

  function accept(suggestion: Suggestion) {
    edit(suggestion.line)
    inputRef.current?.focus()
  }

  function onKeyDown(e: React.KeyboardEvent<HTMLInputElement>) {
    const open = suggestions.length > 0

    if (e.key === "Tab") {
      e.preventDefault()
      if (open) accept(suggestions[Math.min(selected, suggestions.length - 1)])
      return
    }

    if (e.key === "Escape") {
      e.preventDefault()
      setDismissed(true)
      return
    }

    // The popup borrows the arrows while it is open, the way a shell's
    // completion menu does; history has them the rest of the time.
    if (e.key === "ArrowUp" || e.key === "ArrowDown") {
      e.preventDefault()
      if (open) {
        const step = e.key === "ArrowDown" ? 1 : -1
        setSelected((at) => (at + step + suggestions.length) % suggestions.length)
        return
      }
      if (history.current.length === 0) return
      const at =
        e.key === "ArrowUp"
          ? Math.min(historyAt.current + 1, history.current.length - 1)
          : historyAt.current - 1
      historyAt.current = Math.max(at, -1)
      setInput(
        historyAt.current < 0
          ? ""
          : history.current[history.current.length - 1 - historyAt.current],
      )
      setDismissed(true)   // recalling a line is not a request to complete it
      return
    }

    if (e.key === "l" && e.ctrlKey) {
      e.preventDefault()
      setExchanges([])
      return
    }

    if (e.key === "c" && e.ctrlKey) {
      // A shell's Ctrl+C on a prompt: abandon the line. The device is
      // single-in-flight and will still answer whatever is running — this stops
      // waiting for it, it does not reach across and stop the device.
      e.preventDefault()
      setInput("")
      setDismissed(false)
    }
  }

  return (
    <div className="flex h-full flex-col gap-2">
      <div className="flex shrink-0 flex-wrap items-center justify-between gap-2">
        <div className="flex items-center gap-2">
          <SquareTerminalIcon className="size-5 text-muted-foreground" />
          <h1 className="text-2xl font-bold">Commands</h1>
          <span className="hidden text-sm text-muted-foreground sm:inline">
            {commands.length > 0 ? `${commands.length} discovered` : "connecting…"}
          </span>
        </div>
        <Button
          variant="outline"
          size="sm"
          onClick={() => setExchanges([])}
          disabled={exchanges.length === 0}
        >
          <TrashIcon className="mr-1.5 size-3.5" />
          Clear
        </Button>
      </div>

      <Transcript exchanges={exchanges} />

      <form
        className="relative flex shrink-0 items-center gap-2"
        onSubmit={(e) => {
          e.preventDefault()
          void run()
        }}
      >
        <CommandSuggestions
          suggestions={suggestions}
          selected={Math.min(selected, Math.max(suggestions.length - 1, 0))}
          onSelect={setSelected}
          onAccept={accept}
        />

        <div className="flex min-w-0 flex-1 items-center gap-2 rounded-md border px-3 py-2 focus-within:border-ring focus-within:ring-[3px] focus-within:ring-ring/50">
          <span className="shrink-0 font-mono text-sm text-muted-foreground select-none">
            &gt;
          </span>
          <input
            ref={inputRef}
            value={input}
            onChange={(e) => edit(e.target.value)}
            onKeyDown={onKeyDown}
            spellCheck={false}
            autoComplete="off"
            autoCapitalize="off"
            autoFocus
            placeholder="Type a command… (e.g. 'system ping', 'led set enabled=true')"
            className="min-w-0 flex-1 bg-transparent font-mono text-sm outline-none placeholder:text-muted-foreground"
          />
        </div>
        <Button type="submit" disabled={busy} className="shrink-0">
          <SendIcon className="mr-1.5 size-3.5" />
          {busy ? "Running…" : "Send"}
        </Button>
      </form>

      <p className="flex shrink-0 flex-wrap items-baseline gap-x-3 px-1 text-xs text-muted-foreground">
        <span>
          <Key>↑ ↓</Key> {suggestions.length > 0 ? "suggestions" : "history"}
        </span>
        <span>
          <Key>Tab</Key> accept
        </span>
        <span>
          <Key>Esc</Key> dismiss
        </span>
        <span>
          <Key>Ctrl+L</Key> clear
        </span>
        {/* The signature of whatever the line names, when the popup is not
            already saying it. */}
        {suggestions.length === 0 && current && (
          <span className="ml-auto truncate font-mono">{signature(current)}</span>
        )}
      </p>
    </div>
  )
}

function Key({ children }: { children: React.ReactNode }) {
  return <span className="font-mono text-foreground">{children}</span>
}

/**
 * Everything typed and everything answered, oldest at the top.
 *
 * Stuck to the bottom while you are already there, and left alone when you are
 * not: scrolling up to read what a command did twenty lines ago should not be
 * undone by the next reply landing.
 */
function Transcript({ exchanges }: { exchanges: Exchange[] }) {
  const ref = useRef<HTMLDivElement>(null)
  const pinned = useRef(true)

  useLayoutEffect(() => {
    const el = ref.current
    if (el && pinned.current) el.scrollTop = el.scrollHeight
  }, [exchanges])

  return (
    <div
      ref={ref}
      onScroll={(e) => {
        const el = e.currentTarget
        pinned.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24
      }}
      className="min-h-0 flex-1 overflow-y-auto rounded-xl border bg-muted/30 py-2 font-mono text-xs"
    >
      {exchanges.length === 0 ? (
        <p className="px-3 py-1 text-muted-foreground">
          Nothing run yet. Type <span className="text-foreground">help</span> to see
          what this device can do.
        </p>
      ) : (
        exchanges.map((exchange) => <Entry key={exchange.id} exchange={exchange} />)
      )}
    </div>
  )
}

/** One command and its answer, as one block with a blank line under it — so the
 *  eye lands on a command together with what it said, rather than on whichever
 *  two lines happen to be adjacent. */
function Entry({ exchange }: { exchange: Exchange }) {
  return (
    <div className="mb-3 last:mb-0">
      <Row at={exchange.at} meta={exchange.elapsedMs}>
        <span className="text-muted-foreground select-none">&gt; </span>
        <span className="text-blue-600 dark:text-blue-400">{exchange.command}</span>
      </Row>

      {exchange.records.map((record, i) => (
        <Record key={i} at={exchange.at} text={record} />
      ))}

      {exchange.error && (
        <Row at={exchange.at}>
          <span className="text-destructive">{exchange.error}</span>
        </Row>
      )}
    </div>
  )
}

/** One reply record. Folded when it is long, because `help` is the whole
 *  registry and burying the prompt under it helps nobody. */
function Record({ at, text }: { at: Date; text: string }) {
  const [expanded, setExpanded] = useState(false)
  const lines = text.split("\n")
  const hidden = lines.length - FOLD_AFTER

  if (expanded || hidden <= 0) return <Row at={at}>{text}</Row>

  return (
    <>
      <Row at={at}>{lines.slice(0, FOLD_AFTER).join("\n")}</Row>
      <Row at={null}>
        <button
          type="button"
          onClick={() => setExpanded(true)}
          className="text-muted-foreground hover:text-foreground"
        >
          … ({hidden} more)
        </button>
      </Row>
    </>
  )
}

/** One line of the transcript, in the columns every other line uses. A stamp of
 *  null leaves the column empty: a record is one event however many lines it
 *  takes to write down. */
function Row({
  at,
  meta,
  children,
}: {
  at: Date | null
  meta?: number
  children: React.ReactNode
}) {
  return (
    <div className="flex items-baseline gap-3 px-3 leading-5">
      <span className="w-16 shrink-0 text-right text-muted-foreground tabular-nums">
        {at ? stamp(at) : ""}
      </span>
      <pre className="min-w-0 flex-1 whitespace-pre-wrap">{children}</pre>
      {meta !== undefined && (
        <span className="shrink-0 text-muted-foreground tabular-nums">{meta} ms</span>
      )}
    </div>
  )
}

function clip(text: string): string {
  if (text.length <= RECORD_LIMIT) return text
  return `${text.slice(0, RECORD_LIMIT)}\n… ${text.length - RECORD_LIMIT} more characters`
}

function stamp(at: Date): string {
  const pad = (v: number) => String(v).padStart(2, "0")
  return `${pad(at.getHours())}:${pad(at.getMinutes())}:${pad(at.getSeconds())}`
}
