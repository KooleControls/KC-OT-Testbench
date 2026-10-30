import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type ComponentType } from "react"
import { CpuIcon, FlameIcon, ThermometerIcon } from "lucide-react"
import { toast } from "sonner"

import { Card } from "@/components/ui/card"
import {
  CONNECTED_WITHIN_MS,
  formatMemberId,
  summariseLine,
  useNow,
  useOpenThermFrames,
  type DeviceSummary,
} from "@/hooks/use-opentherm"
import { useDeviceInfo } from "@/hooks/use-device-info"
import { errorMessage } from "@/hooks/use-led"
import { backend, type RoutingState } from "@/lib/backend"
import { MASTERS, SLAVES, isMaster, type Endpoint, type Master, type Routing, type Slave } from "@/lib/routing"

/**
 * Which thermostat talks to which boiler. The four ends sit outside; the testbench is
 * the box in the middle, and every pairing it can make is drawn inside it. Clicking a
 * line connects that pair, clicking a connected one opens it again. The testbench
 * forwards the frames (RoutingManager) and keeps the choice across reboots.
 */

const BLUE = "var(--color-blue-500)"
const ORANGE = "var(--color-orange-500)"
const ROUTING_POLL_MS = 3000

interface EndpointLook {
  title: string
  kind: "Real device" | "Simulator"
  icon: ComponentType<{ className?: string }>
  tint: string
  color: string
  iconColor: string
}

const LOOKS: Record<Endpoint, EndpointLook> = {
  thermostat: {
    title: "Thermostat",
    kind: "Real device",
    icon: ThermometerIcon,
    tint: "border-blue-200 bg-blue-50/60 dark:border-blue-900 dark:bg-blue-950/30",
    color: BLUE,
    iconColor: "text-blue-500",
  },
  thermostatSim: {
    title: "Thermostat simulator",
    kind: "Simulator",
    icon: CpuIcon,
    tint: "border-dashed",
    color: BLUE,
    iconColor: "text-blue-500",
  },
  boiler: {
    title: "Boiler",
    kind: "Real device",
    icon: FlameIcon,
    tint: "border-orange-200 bg-orange-50/60 dark:border-orange-900 dark:bg-orange-950/30",
    color: ORANGE,
    iconColor: "text-orange-500",
  },
  boilerSim: {
    title: "Boiler simulator",
    kind: "Simulator",
    icon: CpuIcon,
    tint: "border-dashed",
    color: ORANGE,
    iconColor: "text-orange-500",
  },
}

function toRouting(state: RoutingState): Routing {
  const slave = (s: Slave | "open") => (s === "open" ? null : s)
  return { thermostat: slave(state.thermostat), thermostatSim: slave(state.thermostatSim) }
}

export default function ConnectionsPage() {
  const [state, setState] = useState<RoutingState | null>(null)
  const [busy, setBusy] = useState(false)
  const { rows } = useOpenThermFrames()
  const info = useDeviceInfo()
  const now = useNow(1000)
  const lines = useMemo(
    () => ({ thermostat: summariseLine(rows, "thermostat"), boiler: summariseLine(rows, "boiler") }),
    [rows],
  )

  const refresh = useCallback(() => {
    backend.getRouting().then(setState).catch(() => {
      /* the frame poll and the sidebar report the connection */
    })
  }, [])

  useEffect(() => {
    refresh()
    const timer = setInterval(refresh, ROUTING_POLL_MS)
    return () => clearInterval(timer)
  }, [refresh])

  const toggle = (m: Master, s: Slave) => {
    if (!state || busy) return
    const connected = toRouting(state)[m] === s
    setBusy(true)
    backend
      .setRoute(m, connected ? "open" : s)
      .then((next) => {
        if (next.ok) setState(next)
        else toast.error("Not changed", { description: next.error })
      })
      .catch((e) => toast.error("Not changed", { description: errorMessage(e) }))
      .finally(() => setBusy(false))
  }

  const status: Record<Endpoint, NodeStatus> = {
    thermostat: lineStatus(lines.thermostat, now),
    boiler: lineStatus(lines.boiler, now),
    // A simulator runs exactly when it is connected to something.
    thermostatSim: { state: "sim", running: state !== null && state.thermostatSim !== "open" },
    boilerSim: {
      state: "sim",
      running: state !== null && (state.thermostat === "boilerSim" || state.thermostatSim === "boilerSim"),
    },
  }

  return (
    <div className="flex flex-col gap-6">
      <h1 className="text-3xl font-bold">Connections</h1>

      <Card className="p-6 pb-10">
        {state ? (
          <Diagram
            routing={toRouting(state)}
            onToggle={toggle}
            status={status}
            benchName={info?.name ?? "Testbench"}
            forwarded={state.counters.forwarded}
          />
        ) : (
          <p className="text-muted-foreground text-sm">Reading the routing from the testbench…</p>
        )}
      </Card>

      <p className="text-muted-foreground text-sm">
        Click a line in the testbench to connect that thermostat and boiler, click a connected
        line to open it again. The testbench saves the choice. A simulator runs while it is
        connected; what it does is set on the Simulators page.
      </p>
    </div>
  )
}

// ── Status of the ends ──────────────────────────────────────────

type NodeStatus = { state: "connected" | "silent"; details: string } | { state: "sim"; running: boolean }

function lineStatus(summary: DeviceSummary, now: number): NodeStatus {
  const heard = summary.lastSeenAt !== undefined && now - summary.lastSeenAt < CONNECTED_WITHIN_MS
  const details = [
    summary.otVersion && `OT ${summary.otVersion}`,
    summary.memberId !== undefined && `member id ${formatMemberId(summary.memberId)}`,
  ]
    .filter(Boolean)
    .join(" · ")
  return { state: heard ? "connected" : "silent", details: details || "Identity not seen yet" }
}

// ── Diagram ─────────────────────────────────────────────────────

type Point = { x: number; y: number }
type Ref = (el: HTMLElement | null) => void

/// Where each registered element's centre is, relative to `container`. The lines are
/// drawn between these, so they follow the layout instead of assuming one.
function useAnchors() {
  const container = useRef<HTMLDivElement>(null)
  const elements = useRef(new Map<string, HTMLElement>())
  const [points, setPoints] = useState<Record<string, Point>>({})

  const measure = () => {
    const root = container.current
    if (!root) return
    const origin = root.getBoundingClientRect()
    const next: Record<string, Point> = {}
    elements.current.forEach((el, key) => {
      const r = el.getBoundingClientRect()
      next[key] = { x: r.left + r.width / 2 - origin.left, y: r.top + r.height / 2 - origin.top }
    })
    // Only a real move updates the state; otherwise measuring after every render
    // would render again forever.
    setPoints((old) => (JSON.stringify(old) === JSON.stringify(next) ? old : next))
  }

  // After every render, because a dot moves whenever text around it changes (a status
  // line) without the container changing size.
  useLayoutEffect(measure)

  useLayoutEffect(() => {
    const root = container.current
    if (!root) return
    const observer = new ResizeObserver(measure)
    observer.observe(root)
    return () => observer.disconnect()
  }, [])

  const anchor =
    (key: string): Ref =>
    (el) => {
      if (el) elements.current.set(key, el)
      else elements.current.delete(key)
    }
  return { container, anchor, points }
}

// Masters on the left, slaves on the right, real devices on the top row.
const PLACE: Record<Endpoint, string> = {
  thermostat: "md:col-start-1 md:row-start-1",
  thermostatSim: "md:col-start-1 md:row-start-2",
  boiler: "md:col-start-3 md:row-start-1",
  boilerSim: "md:col-start-3 md:row-start-2",
}

function Diagram({
  routing,
  onToggle,
  status,
  benchName,
  forwarded,
}: {
  routing: Routing
  onToggle: (m: Master, s: Slave) => void
  status: Record<Endpoint, NodeStatus>
  benchName: string
  forwarded: number
}) {
  const { container, anchor, points } = useAnchors()
  const pairs = MASTERS.flatMap((m) => SLAVES.map((s) => ({ m, s, on: routing[m] === s })))
  const isConnected = (e: Endpoint) => (isMaster(e) ? routing[e] !== null : MASTERS.some((m) => routing[m] === e))
  const ends: Endpoint[] = [...MASTERS, ...SLAVES]

  return (
    <div ref={container} className="relative">
      <div className="grid gap-4 md:grid-cols-[minmax(0,1fr)_10rem_minmax(0,1fr)] md:grid-rows-2 md:gap-x-20 md:gap-y-0">
        {ends.map((e) => (
          <Node key={e} endpoint={e} status={status[e]} dot={anchor(`node-${e}`)} className={`md:my-4 ${PLACE[e]}`} />
        ))}

        {/* The testbench: everything between its ports happens inside this device. */}
        <div className="bg-muted/50 relative hidden rounded-xl border md:col-start-2 md:row-span-2 md:row-start-1 md:block">
          {ends.map((e) => (
            <span
              key={e}
              ref={anchor(`port-${e}`)}
              className={`absolute size-2 -translate-y-1/2 ${isMaster(e) ? "left-0" : "right-0"} ${
                e === "thermostat" || e === "boiler" ? "top-1/4" : "top-3/4"
              }`}
            />
          ))}
          <div className="absolute inset-x-0 -bottom-9 text-center text-xs leading-tight">
            <div className="font-semibold">{benchName}</div>
            <div className="text-muted-foreground">{forwarded} forwarded</div>
          </div>
        </div>
      </div>

      <svg className="pointer-events-none absolute inset-0 hidden h-full w-full overflow-visible md:block">
        {ends.map((e) => (
          <Lead key={e} from={points[`node-${e}`]} to={points[`port-${e}`]} color={LOOKS[e].color} on={isConnected(e)} />
        ))}
        {pairs.map(({ m, s, on }) => (
          <Wire
            key={m + s}
            id={`wire-${m}-${s}`}
            from={points[`port-${m}`]}
            to={points[`port-${s}`]}
            on={on}
            label={`${on ? "Disconnect" : "Connect"} ${LOOKS[m].title.toLowerCase()} and ${LOOKS[s].title.toLowerCase()}`}
            onClick={() => onToggle(m, s)}
          />
        ))}
      </svg>

      <ul className="text-muted-foreground mt-4 text-sm md:hidden">
        {MASTERS.map((m) => {
          const s = routing[m]
          return (
            <li key={m}>
              {LOOKS[m].title} → {s ? LOOKS[s].title : "open"}
            </li>
          )
        })}
      </ul>
    </div>
  )
}

/** From a device to its port on the testbench: coloured while it is connected. */
function Lead({ from, to, color, on }: { from?: Point; to?: Point; color: string; on: boolean }) {
  if (!from || !to) return null
  return on ? (
    <line x1={from.x} y1={from.y} x2={to.x} y2={to.y} stroke={color} strokeWidth={3} />
  ) : (
    <line
      x1={from.x}
      y1={from.y}
      x2={to.x}
      y2={to.y}
      strokeWidth={2}
      strokeDasharray="4 5"
      className="stroke-muted-foreground/40"
    />
  )
}

/** One pairing inside the testbench; clickable. */
function Wire({
  id,
  from,
  to,
  on,
  label,
  onClick,
}: {
  id: string
  from?: Point
  to?: Point
  on: boolean
  label: string
  onClick: () => void
}) {
  if (!from || !to) return null
  const midX = (from.x + to.x) / 2
  const d = `M ${from.x} ${from.y} C ${midX} ${from.y}, ${midX} ${to.y}, ${to.x} ${to.y}`

  return (
    <g
      role="button"
      tabIndex={0}
      aria-label={label}
      aria-pressed={on}
      onClick={onClick}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault()
          onClick()
        }
      }}
      className="group pointer-events-auto cursor-pointer outline-none"
    >
      <title>{label}</title>
      {/* userSpaceOnUse: a straight horizontal wire has a zero-height bounding box,
          and a bounding-box gradient on it would not paint at all. */}
      <defs>
        <linearGradient id={id} gradientUnits="userSpaceOnUse" x1={from.x} y1={0} x2={to.x} y2={0}>
          <stop offset="0" stopColor={BLUE} />
          <stop offset="1" stopColor={ORANGE} />
        </linearGradient>
      </defs>
      {/* A wide invisible stroke, so the thin line is easy to hit. */}
      <path d={d} fill="none" stroke="transparent" strokeWidth={14} />
      {on ? (
        <path
          d={d}
          fill="none"
          stroke={`url(#${id})`}
          strokeWidth={3}
          className="group-hover:opacity-60 group-focus-visible:opacity-60"
        />
      ) : (
        <path
          d={d}
          fill="none"
          strokeWidth={2}
          strokeDasharray="4 5"
          className="stroke-muted-foreground/30 group-hover:stroke-muted-foreground/80 group-focus-visible:stroke-muted-foreground/80"
        />
      )}
    </g>
  )
}

// ── Nodes ───────────────────────────────────────────────────────

function Node({
  endpoint,
  status,
  dot,
  className,
}: {
  endpoint: Endpoint
  status: NodeStatus
  dot: Ref
  className: string
}) {
  const look = LOOKS[endpoint]
  const dotSide = isMaster(endpoint) ? "-right-1.5" : "-left-1.5"
  return (
    <div className={`relative flex items-center gap-4 rounded-xl border p-4 ${look.tint} ${className}`}>
      <div className="bg-background flex size-12 shrink-0 items-center justify-center rounded-lg border shadow-sm">
        <look.icon className={`size-6 ${look.iconColor}`} />
      </div>
      <div className="min-w-0">
        <div className="flex flex-wrap items-baseline gap-x-2">
          <span className="font-semibold">{look.title}</span>
          <span className="text-muted-foreground text-xs">{look.kind}</span>
        </div>
        <StatusLine status={status} />
      </div>
      <span
        ref={dot}
        className={`bg-background absolute top-1/2 z-10 hidden size-3 -translate-y-1/2 rounded-full border-2 border-current md:block ${dotSide} ${look.iconColor}`}
      />
    </div>
  )
}

function StatusLine({ status }: { status: NodeStatus }) {
  if (status.state === "sim")
    return (
      <div className="mt-1 flex items-center gap-1.5 text-sm">
        <span className={`size-2 rounded-full ${status.running ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
        <span className={status.running ? "text-emerald-700 dark:text-emerald-400" : "text-muted-foreground"}>
          {status.running ? "Running" : "Not connected"}
        </span>
      </div>
    )
  const connected = status.state === "connected"
  return (
    <div className="mt-1 text-sm">
      <span className="flex items-center gap-1.5">
        <span className={`size-2 rounded-full ${connected ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
        <span className={connected ? "text-emerald-700 dark:text-emerald-400" : "text-muted-foreground"}>
          {connected ? "Connected" : "Not heard from"}
        </span>
      </span>
      <div className="text-muted-foreground truncate">{status.details}</div>
    </div>
  )
}
