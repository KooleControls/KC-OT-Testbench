import { useCallback, useEffect, useRef, useState } from "react"
import { toast } from "sonner"

import { Badge } from "@/components/ui/badge"
import { Card } from "@/components/ui/card"
import { Label } from "@/components/ui/label"
import { Switch } from "@/components/ui/switch"
import { errorMessage } from "@/hooks/use-led"
import { backend, type IoEvent, type IoSource, type IoState } from "@/lib/backend"

/**
 * The testbench's two relays and two inputs, wired crosswise to a gateway's: its relays
 * switch its +12 V onto these inputs, these relays switch this board's +12 V onto its
 * inputs, each through a normally open contact.
 *
 * A relay follows a source (IoManager): a hand on the switch, or a thermostat's heat or
 * cool request as it asks for it over OpenTherm, so the relay plays the heater or cooler
 * that thermostat would switch. The testbench saves the choice. Below, every change on
 * the four lines in order, with how long after the one before it came and how long the
 * line had been the other way -- what a test reads with `io events` to time a gateway's
 * relays.
 */

const STATUS_POLL_MS = 1000
const EVENTS_POLL_MS = 1000
const PAGE = 64
const KEEP = 200

const SOURCES: { value: IoSource; label: string }[] = [
  { value: "manual", label: "Manual" },
  { value: "thermostat.heat", label: "Thermostat: heat request (CH enable)" },
  { value: "thermostat.cool", label: "Thermostat: cool request (cooling enable)" },
  { value: "thermostatSim.heat", label: "Thermostat simulator: heat request" },
  { value: "thermostatSim.cool", label: "Thermostat simulator: cool request" },
]

const LINE_NAMES: Record<IoEvent["line"], string> = {
  input1: "Input 1",
  input2: "Input 2",
  relay1: "Relay 1",
  relay2: "Relay 2",
}

const selectClass = "border-input dark:bg-input/30 h-9 rounded-lg border bg-transparent px-3 text-sm"

function formatDuration(ms: number): string {
  if (ms < 1000) return `${ms} ms`
  if (ms < 60_000) return `${(ms / 1000).toFixed(2)} s`
  return `${Math.floor(ms / 60_000)} min ${Math.round((ms % 60_000) / 1000)} s`
}

function changeName(e: IoEvent): string {
  const relay = e.line === "relay1" || e.line === "relay2"
  if (relay) return e.on ? "Closed" : "Opened"
  return e.on ? "Active" : "Inactive"
}

// ── Polling ─────────────────────────────────────────────────────

function useIoState() {
  const [state, setState] = useState<IoState | null>(null)

  const refresh = useCallback(() => {
    backend
      .getIo()
      .then((s) => {
        if (s.ok) setState(s)
      })
      .catch(() => {
        /* the sidebar reports the connection */
      })
  }, [])

  useEffect(() => {
    refresh()
    const timer = setInterval(refresh, STATUS_POLL_MS)
    return () => clearInterval(timer)
  }, [refresh])

  return [state, setState] as const
}

function useIoEvents() {
  const [rows, setRows] = useState<IoEvent[]>([])
  const [missed, setMissed] = useState(0)
  const cursor = useRef(0)
  const inFlight = useRef(false)
  const alive = useRef(true)

  useEffect(() => {
    alive.current = true
    return () => {
      alive.current = false
    }
  }, [])

  const poll = useCallback(async () => {
    if (inFlight.current) return
    inFlight.current = true
    try {
      for (;;) {
        const r = await backend.getIoEvents(cursor.current)
        if (!alive.current) return

        // The testbench restarted, and with it its clock: what is on screen no longer
        // lines up with what comes, so start over.
        if (r.latest < cursor.current) {
          cursor.current = 0
          setRows([])
          continue
        }

        if (r.events.length > 0) {
          cursor.current = r.events[r.events.length - 1].seq
          const fresh = [...r.events].reverse()
          setRows((old) => [...fresh, ...old].slice(0, KEEP))
        }
        if (r.missed > 0) setMissed((m) => m + r.missed)
        if (r.events.length < PAGE) break
      }
    } catch {
      /* the sidebar reports the connection */
    } finally {
      inFlight.current = false
    }
  }, [])

  useEffect(() => {
    poll()
    const timer = setInterval(poll, EVENTS_POLL_MS)
    return () => clearInterval(timer)
  }, [poll])

  return { rows, missed }
}

// ── The page ────────────────────────────────────────────────────

export default function IoPage() {
  const [state, setState] = useIoState()
  const { rows, missed } = useIoEvents()
  const [busy, setBusy] = useState(false)

  const change = (request: Promise<IoState>) => {
    setBusy(true)
    request
      .then((next) => {
        if (next.ok) setState(next)
        else toast.error("Not changed", { description: next.error })
      })
      .catch((e) => toast.error("Not changed", { description: errorMessage(e) }))
      .finally(() => setBusy(false))
  }

  return (
    <div className="flex flex-col gap-6">
      <h1 className="text-3xl font-bold">Relays &amp; inputs</h1>

      {state ? (
        <>
          <div className="grid gap-4 md:grid-cols-2">
            {state.relays.map((r) => (
              <RelayCard
                key={r.relay}
                relay={r}
                asking={r.source === "manual" ? undefined : state.sources[r.source]}
                sources={state.sources}
                busy={busy}
                onSource={(source) => change(backend.setIoSource(r.relay, source))}
                onManual={(on) => change(backend.setIoRelay(r.relay, on))}
              />
            ))}
          </div>
          <div className="grid gap-4 md:grid-cols-2">
            {state.inputs.map((i) => (
              <Card key={i.input} className="flex flex-row items-center justify-between gap-4 p-6">
                <div className="flex flex-col gap-1">
                  <h2 className="text-lg font-semibold">Input {i.input}</h2>
                  <p className="text-muted-foreground text-sm">
                    {i.changedAgoMs === undefined
                      ? "Not changed since the testbench started"
                      : `Changed ${formatDuration(i.changedAgoMs)} ago`}
                  </p>
                </div>
                <Badge variant={i.active ? "default" : "outline"}>{i.active ? "Active" : "Inactive"}</Badge>
              </Card>
            ))}
          </div>
        </>
      ) : (
        <Card className="p-6">
          <p className="text-muted-foreground text-sm">Reading the relays and inputs from the testbench…</p>
        </Card>
      )}

      <Card className="flex flex-col gap-4 p-6">
        <div className="flex items-baseline justify-between gap-4">
          <h2 className="text-lg font-semibold">Changes</h2>
          {missed > 0 && <span className="text-destructive text-sm">{missed} left the ring unread</span>}
        </div>
        {rows.length === 0 ? (
          <p className="text-muted-foreground text-sm">No change since the testbench started.</p>
        ) : (
          <EventTable rows={rows} />
        )}
      </Card>

      <p className="text-muted-foreground text-sm">
        Wire the gateway's relays to these inputs and these relays to the gateway's inputs: the
        relay's own board's +12 V (beside RS485 A/B) on its COM, its NO contact to the other
        board's input +, that input's ground to the GND beside the +12 V. A relay
        that follows a thermostat is closed while that thermostat asks for heat or cooling in its
        OpenTherm status; one silent for 10 s asks for nothing. Times are the testbench's clock, an
        input's from before its 20 ms debounce.
      </p>
    </div>
  )
}

function RelayCard({
  relay,
  asking,
  sources,
  busy,
  onSource,
  onManual,
}: {
  relay: IoState["relays"][number]
  asking: boolean | undefined
  sources: IoState["sources"]
  busy: boolean
  onSource: (source: IoSource) => void
  onManual: (on: boolean) => void
}) {
  const id = `relay${relay.relay}`
  return (
    <Card className="flex flex-col gap-4 p-6">
      <div className="flex items-center justify-between gap-4">
        <h2 className="text-lg font-semibold">Relay {relay.relay}</h2>
        <Badge variant={relay.on ? "default" : "outline"}>{relay.on ? "Closed" : "Open"}</Badge>
      </div>

      <div className="flex flex-col gap-2">
        <Label htmlFor={`${id}-source`}>Follows</Label>
        <select
          id={`${id}-source`}
          className={`${selectClass} w-full`}
          disabled={busy}
          value={relay.source}
          onChange={(e) => onSource(e.target.value as IoSource)}
        >
          {SOURCES.map((s) => (
            <option key={s.value} value={s.value}>
              {s.label}
              {s.value !== "manual" && sources[s.value] ? " · asking" : ""}
            </option>
          ))}
        </select>
      </div>

      {asking === undefined ? (
        <div className="flex items-center gap-3">
          <Switch id={`${id}-manual`} checked={relay.on} disabled={busy} onCheckedChange={onManual} />
          <Label htmlFor={`${id}-manual`}>Closed by hand</Label>
        </div>
      ) : (
        <p className="text-muted-foreground text-sm">
          {asking ? "Asking now, so the relay is closed." : "Not asking now, so the relay is open."}
        </p>
      )}
    </Card>
  )
}

function EventTable({ rows }: { rows: IoEvent[] }) {
  return (
    <div className="overflow-x-auto">
      <table className="w-full text-sm">
        <thead className="text-muted-foreground text-left">
          <tr>
            <th className="py-1 pr-4 font-medium">#</th>
            <th className="py-1 pr-4 font-medium">Time</th>
            <th className="py-1 pr-4 font-medium">Line</th>
            <th className="py-1 pr-4 font-medium">Change</th>
            <th className="py-1 pr-4 font-medium">After the one before</th>
            <th className="py-1 font-medium">Was the other way for</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((e, k) => {
            // Rows are newest first: the one before is below, and so is this line's last.
            const before = rows[k + 1]
            const sameLine = rows.slice(k + 1).find((o) => o.line === e.line)
            return (
              <tr key={e.seq} className="border-t">
                <td className="py-1 pr-4 font-mono">{e.seq}</td>
                <td className="py-1 pr-4 font-mono">{(e.ms / 1000).toFixed(3)} s</td>
                <td className="py-1 pr-4">{LINE_NAMES[e.line]}</td>
                <td className="py-1 pr-4">{changeName(e)}</td>
                <td className="py-1 pr-4 font-mono">
                  {before && before.seq === e.seq - 1 ? `+${formatDuration(e.ms - before.ms)}` : "—"}
                </td>
                <td className="py-1 font-mono">{sameLine ? formatDuration(e.ms - sameLine.ms) : "—"}</td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}
