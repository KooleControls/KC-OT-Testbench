import { useMemo, useState, type ComponentType } from "react"
import { CpuIcon, FlameIcon, InfoIcon, ThermometerIcon } from "lucide-react"

import { Card } from "@/components/ui/card"
import {
  CONNECTED_WITHIN_MS,
  formatMemberId,
  summariseLine,
  useNow,
  useOpenThermFrames,
  type DeviceSummary,
} from "@/hooks/use-opentherm"
import {
  DEFAULT_ROUTING,
  MASTERS,
  SLAVES,
  connect,
  isMaster,
  partnerOf,
  type Endpoint,
  type Master,
  type Routing,
} from "@/lib/routing"

/**
 * Which thermostat talks to which boiler: the two real devices on the module's lines
 * and, later, a simulator of each in the gateway.
 *
 * The firmware routes nothing yet and has no simulators, so this page is the layout
 * only: a choice lives in this tab's state and is not sent to the device. The real
 * devices' status is live, read from the frames like on the Frames page.
 */

interface EndpointLook {
  title: string
  kind: "Real device" | "Simulator"
  icon: ComponentType<{ className?: string }>
  tint: string
  iconColor: string
}

const LOOKS: Record<Endpoint, EndpointLook> = {
  thermostat: {
    title: "Thermostat",
    kind: "Real device",
    icon: ThermometerIcon,
    tint: "border-blue-200 bg-blue-50/60 dark:border-blue-900 dark:bg-blue-950/30",
    iconColor: "text-blue-500",
  },
  thermostatSim: {
    title: "Thermostat simulator",
    kind: "Simulator",
    icon: CpuIcon,
    tint: "border-dashed",
    iconColor: "text-blue-500",
  },
  boiler: {
    title: "Boiler",
    kind: "Real device",
    icon: FlameIcon,
    tint: "border-orange-200 bg-orange-50/60 dark:border-orange-900 dark:bg-orange-950/30",
    iconColor: "text-orange-500",
  },
  boilerSim: {
    title: "Boiler simulator",
    kind: "Simulator",
    icon: CpuIcon,
    tint: "border-dashed",
    iconColor: "text-orange-500",
  },
}

export default function ConnectionsPage() {
  const [routing, setRouting] = useState<Routing>(DEFAULT_ROUTING)
  const { rows } = useOpenThermFrames()
  const now = useNow(1000)
  const lines = useMemo(
    () => ({ thermostat: summariseLine(rows, "thermostat"), boiler: summariseLine(rows, "boiler") }),
    [rows],
  )

  const choose = (endpoint: Endpoint, partner: Endpoint | null) =>
    setRouting((r) => connect(r, endpoint, partner))

  const statusOf = (endpoint: Endpoint): NodeStatus => {
    if (endpoint === "thermostat" || endpoint === "boiler") return lineStatus(lines[endpoint], now)
    return { state: "later" }
  }

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
          The gateway does not route frames yet and has no simulators. Choices on this page
          show how the bench will be set up; they stay in this browser tab and change nothing
          on the device. The thermostat and boiler status is live.
        </p>
      </div>

      <Card className="p-6">
        <Diagram routing={routing} statusOf={statusOf} />
      </Card>

      <section className="flex flex-col gap-3">
        <div>
          <h2 className="text-xl font-semibold">Connection settings</h2>
          <p className="text-muted-foreground text-sm">
            A thermostat connects to one boiler at most, and a boiler to one thermostat.
            Connecting one end frees whatever it was connected to before.
          </p>
        </div>
        <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
          {[...MASTERS, ...SLAVES].map((e) => (
            <ChoiceCard key={e} endpoint={e} partner={partnerOf(routing, e)} onChoose={choose} />
          ))}
        </div>
      </section>
    </div>
  )
}

// ── Diagram ─────────────────────────────────────────────────────

type NodeStatus =
  | { state: "connected" | "silent"; details: string }
  | { state: "later" }

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

// Masters sit on the left and slaves on the right, real devices on the top row and
// simulators below, so every wire runs left to right: straight when a device stays on
// its row, crossing when a real device talks to a simulator.
const ROW: Record<Endpoint, 0 | 1> = { thermostat: 0, boiler: 0, thermostatSim: 1, boilerSim: 1 }

function Diagram({
  routing,
  statusOf,
}: {
  routing: Routing
  statusOf: (e: Endpoint) => NodeStatus
}) {
  const rowY = (row: 0 | 1) => (row === 0 ? 25 : 75)
  const wires = MASTERS.flatMap((m: Master) => {
    const s = routing[m]
    return s ? [{ key: m + s, from: rowY(ROW[m]), to: rowY(ROW[s]) }] : []
  })
  const open = [...MASTERS, ...SLAVES].filter((e) => partnerOf(routing, e) === null)

  return (
    <div className="grid grid-cols-1 gap-4 md:grid-cols-[minmax(0,1fr)_minmax(140px,0.9fr)_minmax(0,1fr)] md:grid-rows-2 md:gap-0">
      <Node endpoint="thermostat" status={statusOf("thermostat")} className="md:col-start-1 md:row-start-1" />
      <Node endpoint="thermostatSim" status={statusOf("thermostatSim")} className="md:col-start-1 md:row-start-2" />
      <Node endpoint="boiler" status={statusOf("boiler")} className="md:col-start-3 md:row-start-1" />
      <Node endpoint="boilerSim" status={statusOf("boilerSim")} className="md:col-start-3 md:row-start-2" />

      <div className="relative hidden md:col-start-2 md:row-span-2 md:row-start-1 md:block">
        <svg
          className="absolute inset-0 h-full w-full overflow-visible"
          viewBox="0 0 100 100"
          preserveAspectRatio="none"
          aria-hidden="true"
        >
          <defs>
            <linearGradient id="wire" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="100" y2="0">
              <stop offset="0" stopColor="var(--color-blue-500)" />
              <stop offset="1" stopColor="var(--color-orange-500)" />
            </linearGradient>
          </defs>
          {open.map((e) => {
            const y = rowY(ROW[e])
            const [x1, x2] = isMaster(e) ? [0, 18] : [82, 100]
            return (
              <line
                key={e}
                x1={x1}
                y1={y}
                x2={x2}
                y2={y}
                className="stroke-muted-foreground/40"
                strokeWidth={2}
                strokeDasharray="4 4"
                vectorEffect="non-scaling-stroke"
              />
            )
          })}
          {wires.map((w) => (
            <path
              key={w.key}
              d={`M 0 ${w.from} C 50 ${w.from}, 50 ${w.to}, 100 ${w.to}`}
              fill="none"
              stroke="url(#wire)"
              strokeWidth={3}
              strokeLinecap="round"
              vectorEffect="non-scaling-stroke"
            />
          ))}
        </svg>
        <span className="text-muted-foreground bg-card absolute bottom-0 left-1/2 -translate-x-1/2 translate-y-1/2 px-2 text-xs">
          Gateway routing
        </span>
      </div>

      <ul className="text-muted-foreground text-sm md:hidden">
        {MASTERS.map((m) => (
          <li key={m}>
            {LOOKS[m].title} → {routing[m] ? LOOKS[routing[m]].title : "open"}
          </li>
        ))}
      </ul>
    </div>
  )
}

function Node({ endpoint, status, className = "" }: { endpoint: Endpoint; status: NodeStatus; className?: string }) {
  const look = LOOKS[endpoint]
  const portSide = isMaster(endpoint) ? "-right-1.5" : "-left-1.5"
  return (
    <div className={`relative flex items-center gap-4 rounded-xl border p-4 md:my-4 ${look.tint} ${className}`}>
      <div className="bg-background flex size-14 shrink-0 items-center justify-center rounded-lg border shadow-sm">
        <look.icon className={`size-7 ${look.iconColor}`} />
      </div>
      <div className="min-w-0">
        <div className="flex flex-wrap items-baseline gap-x-2">
          <span className="font-semibold">{look.title}</span>
          <span className="text-muted-foreground text-xs">{look.kind}</span>
        </div>
        <StatusLine status={status} />
      </div>
      <span
        className={`bg-background absolute top-1/2 hidden size-3 -translate-y-1/2 rounded-full border-2 border-current md:block ${portSide} ${look.iconColor}`}
      />
    </div>
  )
}

function StatusLine({ status }: { status: NodeStatus }) {
  if (status.state === "later")
    return <div className="text-muted-foreground mt-1 text-sm">Coming later</div>
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

// ── Settings ────────────────────────────────────────────────────

function ChoiceCard({
  endpoint,
  partner,
  onChoose,
}: {
  endpoint: Endpoint
  partner: Endpoint | null
  onChoose: (endpoint: Endpoint, partner: Endpoint | null) => void
}) {
  const look = LOOKS[endpoint]
  const options: (Endpoint | null)[] = [...(isMaster(endpoint) ? SLAVES : MASTERS), null]
  const group = `route-${endpoint}`

  return (
    <fieldset className="bg-card rounded-xl border">
      <legend className="sr-only">{look.title} connects to</legend>
      <div className="bg-muted/40 flex items-center gap-3 rounded-t-xl border-b p-3">
        <look.icon className={`size-5 ${look.iconColor}`} />
        <span className="font-medium">{look.title}</span>
        <span className="text-muted-foreground ml-auto text-xs">{look.kind}</span>
      </div>
      <div className="flex flex-col p-2">
        {options.map((o) => (
          <label
            key={o ?? "open"}
            className="hover:bg-muted/60 flex cursor-pointer items-center gap-3 rounded-lg px-2 py-2 text-sm"
          >
            <input
              type="radio"
              name={group}
              checked={partner === o}
              onChange={() => onChoose(endpoint, o)}
              className="size-4 accent-blue-600"
            />
            {o === null ? (
              <span className="text-muted-foreground">Open (not connected)</span>
            ) : (
              <span>
                {LOOKS[o].title}
                {LOOKS[o].kind === "Simulator" && <span className="text-muted-foreground"> · later</span>}
              </span>
            )}
          </label>
        ))}
      </div>
    </fieldset>
  )
}
