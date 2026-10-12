import { useLayoutEffect, useMemo, useRef, useState, type ComponentType, type ReactNode } from "react"
import { CpuIcon, FlameIcon, InfoIcon, PlugIcon, ThermometerIcon } from "lucide-react"

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
import {
  DEFAULT_ROUTING,
  MASTERS,
  SLAVES,
  connect,
  type Master,
  type Routing,
  type Slave,
} from "@/lib/routing"

/**
 * Which thermostat talks to which boiler. The real thermostat and boiler are outside
 * the testbench, each on one line of its PCB1246 module; the simulators are inside it,
 * and so is the routing between all four. Every possible pairing is drawn as a line:
 * clicking one connects it, clicking a connected one opens it again.
 *
 * The firmware routes nothing yet and has no simulators, so this page is the layout
 * only: a choice lives in this tab's state and is not sent to the device. The real
 * devices' status is live, read from the frames like on the Frames page.
 */

const BLUE = "var(--color-blue-500)"
const ORANGE = "var(--color-orange-500)"

const TITLES: Record<Master | Slave, string> = {
  thermostat: "Thermostat line",
  thermostatSim: "Thermostat simulator",
  boiler: "Boiler line",
  boilerSim: "Boiler simulator",
}

export default function ConnectionsPage() {
  const [routing, setRouting] = useState<Routing>(DEFAULT_ROUTING)
  const { rows } = useOpenThermFrames()
  const info = useDeviceInfo()
  const now = useNow(1000)
  const lines = useMemo(
    () => ({ thermostat: summariseLine(rows, "thermostat"), boiler: summariseLine(rows, "boiler") }),
    [rows],
  )

  const toggle = (m: Master, s: Slave) => setRouting((r) => connect(r, m, r[m] === s ? null : s))

  return (
    <div className="flex flex-col gap-6">
      <div className="flex flex-wrap items-center gap-3">
        <h1 className="text-3xl font-bold">Connections</h1>
        <span className="rounded-md bg-amber-100 px-2.5 py-1 text-sm text-amber-800 dark:bg-amber-950 dark:text-amber-300">
          Preview
        </span>
      </div>

      <div className="flex items-start gap-3 rounded-xl border border-amber-200 bg-amber-50/70 p-4 text-sm dark:border-amber-900 dark:bg-amber-950/30">
        <InfoIcon className="mt-0.5 size-4 shrink-0 text-amber-600" />
        <p>
          The testbench does not route frames yet and has no simulators. Choices on this page
          show how the bench will be set up; they stay in this browser tab and change nothing
          on the device. The thermostat and boiler status is live.
        </p>
      </div>

      <Card className="p-6">
        <Diagram
          routing={routing}
          onToggle={toggle}
          benchName={info?.name ?? "Testbench"}
          thermostat={lineStatus(lines.thermostat, now)}
          boiler={lineStatus(lines.boiler, now)}
        />
      </Card>
    </div>
  )
}

// ── Status of the real devices ──────────────────────────────────

interface LineStatus {
  heard: boolean
  details: string
}

function lineStatus(summary: DeviceSummary, now: number): LineStatus {
  const heard = summary.lastSeenAt !== undefined && now - summary.lastSeenAt < CONNECTED_WITHIN_MS
  const details = [
    summary.otVersion && `OT ${summary.otVersion}`,
    summary.memberId !== undefined && `member id ${formatMemberId(summary.memberId)}`,
  ]
    .filter(Boolean)
    .join(" · ")
  return { heard, details: details || "Identity not seen yet" }
}

// ── Diagram ─────────────────────────────────────────────────────

type Point = { x: number; y: number }
type Ref = (el: HTMLElement | null) => void

/// Where each registered element's centre is, relative to `container`. The wires are
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

  // After every render, because a dot moves whenever text around it changes (the
  // device name arriving, a status line) without the container changing size.
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

function Diagram({
  routing,
  onToggle,
  benchName,
  thermostat,
  boiler,
}: {
  routing: Routing
  onToggle: (m: Master, s: Slave) => void
  benchName: string
  thermostat: LineStatus
  boiler: LineStatus
}) {
  const { container, anchor, points } = useAnchors()
  const candidates = MASTERS.flatMap((m) => SLAVES.map((s) => ({ m, s, on: routing[m] === s })))

  return (
    <div ref={container} className="relative">
      <div className="grid gap-4 md:grid-cols-2 md:gap-x-16">
        <RealDevice
          title="Thermostat"
          icon={ThermometerIcon}
          color="blue"
          status={thermostat}
          plug={anchor("thermostat-device")}
        />
        <RealDevice
          title="Boiler"
          icon={FlameIcon}
          color="orange"
          status={boiler}
          plug={anchor("boiler-device")}
        />
      </div>

      {/* The testbench itself: everything inside this border is this device. */}
      <div className="bg-muted/30 relative mt-12 rounded-2xl border-2 border-dashed px-6 pt-10 pb-6">
        <span className="bg-card absolute -top-3.5 left-5 rounded-md border px-2.5 py-0.5 text-sm font-semibold">
          {benchName}
        </span>

        <div className="grid gap-4 md:grid-cols-2 md:gap-x-40 md:gap-y-12">
          <LinePort side="thermostat" plug={anchor("thermostat-line")} port={anchor("thermostat")} />
          <LinePort side="boiler" plug={anchor("boiler-line")} port={anchor("boiler")} />
          <Simulator title={TITLES.thermostatSim} color="blue" port={anchor("thermostatSim")} portSide="right" />
          <Simulator title={TITLES.boilerSim} color="orange" port={anchor("boilerSim")} portSide="left" />
        </div>

        <p className="text-muted-foreground mt-6 hidden text-center text-xs md:block">
          Click a line to connect it, click a connected line to open it again.
        </p>
      </div>

      <svg className="pointer-events-none absolute inset-0 hidden h-full w-full overflow-visible md:block">
        {/* The cables from the real devices into the module: physical, always there. */}
        <Cable from={points["thermostat-device"]} to={points["thermostat-line"]} color={BLUE} />
        <Cable from={points["boiler-device"]} to={points["boiler-line"]} color={ORANGE} />

        {candidates.map(({ m, s, on }) => (
          <Wire
            key={m + s}
            id={`wire-${m}-${s}`}
            from={points[m]}
            to={points[s]}
            on={on}
            label={`${on ? "Disconnect" : "Connect"} ${TITLES[m]} and ${TITLES[s]}`}
            onClick={() => onToggle(m, s)}
          />
        ))}
      </svg>

      <ul className="text-muted-foreground mt-4 text-sm md:hidden">
        {MASTERS.map((m) => (
          <li key={m}>
            {TITLES[m]} → {routing[m] ? TITLES[routing[m]] : "open"}
          </li>
        ))}
      </ul>
    </div>
  )
}

function Cable({ from, to, color }: { from?: Point; to?: Point; color: string }) {
  if (!from || !to) return null
  const midY = (from.y + to.y) / 2
  const d = `M ${from.x} ${from.y} C ${from.x} ${midY}, ${to.x} ${midY}, ${to.x} ${to.y}`
  return <path d={d} fill="none" stroke={color} strokeWidth={3} strokeLinecap="round" />
}

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
      <path d={d} fill="none" stroke="transparent" strokeWidth={18} />
      {on ? (
        <path
          d={d}
          fill="none"
          stroke={`url(#${id})`}
          strokeWidth={4}
          strokeLinecap="round"
          className="group-hover:opacity-60 group-focus-visible:opacity-60"
        />
      ) : (
        <path
          d={d}
          fill="none"
          strokeWidth={2}
          strokeDasharray="5 6"
          className="stroke-muted-foreground/25 group-hover:stroke-muted-foreground/70 group-focus-visible:stroke-muted-foreground/70"
        />
      )}
    </g>
  )
}

// ── Nodes ───────────────────────────────────────────────────────

const COLORS = {
  blue: {
    tint: "border-blue-200 bg-blue-50/60 dark:border-blue-900 dark:bg-blue-950/30",
    icon: "text-blue-500",
  },
  orange: {
    tint: "border-orange-200 bg-orange-50/60 dark:border-orange-900 dark:bg-orange-950/30",
    icon: "text-orange-500",
  },
} as const

function RealDevice({
  title,
  icon: Icon,
  color,
  status,
  plug,
}: {
  title: string
  icon: ComponentType<{ className?: string }>
  color: keyof typeof COLORS
  status: LineStatus
  plug: Ref
}) {
  const c = COLORS[color]
  return (
    <div className={`relative flex items-center gap-4 rounded-xl border p-4 ${c.tint}`}>
      <div className="bg-background flex size-14 shrink-0 items-center justify-center rounded-lg border shadow-sm">
        <Icon className={`size-7 ${c.icon}`} />
      </div>
      <div className="min-w-0">
        <div className="flex flex-wrap items-baseline gap-x-2">
          <span className="font-semibold">{title}</span>
          <span className="text-muted-foreground text-xs">Real device</span>
        </div>
        <div className="mt-1 flex items-center gap-1.5 text-sm">
          <span className={`size-2 rounded-full ${status.heard ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
          <span className={status.heard ? "text-emerald-700 dark:text-emerald-400" : "text-muted-foreground"}>
            {status.heard ? "Connected" : "Not heard from"}
          </span>
        </div>
        <div className="text-muted-foreground truncate text-sm">{status.details}</div>
      </div>
      <Dot anchor={plug} className={`-bottom-1.5 left-1/2 -translate-x-1/2 ${c.icon}`} />
    </div>
  )
}

function LinePort({ side, plug, port }: { side: "thermostat" | "boiler"; plug: Ref; port: Ref }) {
  const c = COLORS[side === "thermostat" ? "blue" : "orange"]
  const portSide = side === "thermostat" ? "-right-1.5" : "-left-1.5"
  return (
    <NodeFrame className="bg-background">
      <PlugIcon className={`size-5 shrink-0 ${c.icon}`} />
      <div className="min-w-0">
        <div className="font-medium">{TITLES[side]}</div>
        <div className="text-muted-foreground text-xs">PCB1246 OpenTherm module</div>
      </div>
      <Dot anchor={plug} className={`-top-1.5 left-1/2 -translate-x-1/2 ${c.icon}`} />
      <Dot anchor={port} className={`${portSide} top-1/2 -translate-y-1/2 ${c.icon}`} />
    </NodeFrame>
  )
}

function Simulator({
  title,
  color,
  port,
  portSide,
}: {
  title: string
  color: keyof typeof COLORS
  port: Ref
  portSide: "left" | "right"
}) {
  const c = COLORS[color]
  const portPosition = portSide === "right" ? "-right-1.5" : "-left-1.5"
  return (
    <NodeFrame className="bg-background border-dashed">
      <CpuIcon className={`size-5 shrink-0 ${c.icon}`} />
      <div className="min-w-0">
        <div className="font-medium">{title}</div>
        <div className="text-muted-foreground text-xs">Simulator · coming later</div>
      </div>
      <Dot anchor={port} className={`${portPosition} top-1/2 -translate-y-1/2 ${c.icon}`} />
    </NodeFrame>
  )
}

function NodeFrame({ className, children }: { className: string; children: ReactNode }) {
  return <div className={`relative flex items-center gap-3 rounded-xl border px-4 py-3 ${className}`}>{children}</div>
}

function Dot({ anchor, className }: { anchor: Ref; className: string }) {
  return (
    <span
      ref={anchor}
      className={`bg-background absolute z-10 hidden size-3 rounded-full border-2 border-current md:block ${className}`}
    />
  )
}
