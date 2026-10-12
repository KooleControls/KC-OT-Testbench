import { useCallback, useEffect, useState, type ComponentType, type ReactNode } from "react"
import { FlameIcon, InfoIcon, ThermometerIcon } from "lucide-react"
import { toast } from "sonner"

import { Card } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Switch } from "@/components/ui/switch"
import { errorMessage } from "@/hooks/use-led"
import { formatMemberId } from "@/hooks/use-opentherm"
import { backend, type SettingEntry, type SimStatus } from "@/lib/backend"
import type { Endpoint } from "@/lib/routing"

/**
 * The two simulators that run on the testbench: a thermostat (an OpenTherm master)
 * and a boiler (a slave). Which device each one talks to is chosen on the
 * Connections page; here is what they do. Their behaviour is a set of ordinary
 * settings (tsim.*, bsim.*) that the testbench reads with every frame, so a change
 * applies straight away -- and the live state shows what the other end has said.
 */

const STATUS_POLL_MS = 1000

const PARTNER_NAMES: Record<Endpoint, string> = {
  thermostat: "the thermostat",
  thermostatSim: "the thermostat simulator",
  boiler: "the boiler",
  boilerSim: "the boiler simulator",
}

type Field =
  | { key: string; label: string; kind: "number"; unit: string; step: number }
  | { key: string; label: string; kind: "toggle"; description: string }

const THERMOSTAT_FIELDS: Field[] = [
  { key: "tsim.room", label: "Room temperature", kind: "number", unit: "°C", step: 0.1 },
  { key: "tsim.setpoint", label: "Room setpoint", kind: "number", unit: "°C", step: 0.5 },
  { key: "tsim.flow", label: "Requested flow temperature", kind: "number", unit: "°C", step: 1 },
  { key: "tsim.interval", label: "Interval between frames", kind: "number", unit: "ms", step: 100 },
  { key: "tsim.ch", label: "CH enable", kind: "toggle", description: "Ask for central heating" },
  { key: "tsim.dhw", label: "DHW enable", kind: "toggle", description: "Ask for hot water" },
  { key: "tsim.member", label: "Member id", kind: "number", unit: "", step: 1 },
]

const BOILER_FIELDS: Field[] = [
  { key: "bsim.water", label: "Water temperature", kind: "number", unit: "°C", step: 1 },
  { key: "bsim.mod", label: "Modulation level", kind: "number", unit: "%", step: 1 },
  { key: "bsim.maxkw", label: "Maximum capacity", kind: "number", unit: "kW", step: 1 },
  { key: "bsim.member", label: "Member id", kind: "number", unit: "", step: 1 },
  { key: "bsim.flame", label: "Flame", kind: "toggle", description: "Report the flame on" },
  { key: "bsim.ch", label: "CH active", kind: "toggle", description: "Report central heating active" },
  { key: "bsim.dhw", label: "DHW active", kind: "toggle", description: "Report hot water active" },
  { key: "bsim.dhwpresent", label: "Hot water", kind: "toggle", description: "Has a hot water circuit" },
  { key: "bsim.modulating", label: "Modulating", kind: "toggle", description: "Modulates (else on/off)" },
]

type Values = Record<string, SettingEntry["value"]>

export default function SimulatorsPage() {
  const [status, setStatus] = useState<SimStatus | null>(null)
  const [values, setValues] = useState<Values>({})

  const loadSettings = useCallback(() => {
    backend
      .getSettings()
      .then((r) => setValues(Object.fromEntries(r.settings.map((s) => [s.key, s.value]))))
      .catch((e) => toast.error("Could not read the settings", { description: errorMessage(e) }))
  }, [])

  useEffect(() => {
    loadSettings()
    const poll = () => backend.getSimStatus().then(setStatus).catch(() => {})
    poll()
    const timer = setInterval(poll, STATUS_POLL_MS)
    return () => clearInterval(timer)
  }, [loadSettings])

  // Set and save in one go: a simulator setting is meant to apply now and to stay.
  const change = (key: string, value: string | boolean) => {
    backend
      .setSetting(key, String(value))
      .then((r) => {
        if (!r.ok) throw new Error("refused")
        return backend.saveSettings()
      })
      .then(() => setValues((v) => ({ ...v, [key]: value })))
      .catch((e) => {
        toast.error("Not changed", { description: errorMessage(e) })
        loadSettings()
      })
  }

  return (
    <div className="flex flex-col gap-6">
      <div>
        <h1 className="text-3xl font-bold">Simulators</h1>
        <p className="text-muted-foreground mt-1">
          The thermostat and boiler the testbench simulates. Connect them on the Connections page;
          a setting here applies with the next frame.
        </p>
      </div>

      <div className="grid gap-6 xl:grid-cols-2">
        <SimulatorCard
          title="Thermostat simulator"
          subtitle="An OpenTherm master: asks, every interval"
          icon={ThermometerIcon}
          color="blue"
          running={status?.thermostatSim.running}
          partner={status?.thermostatSim.partner}
          live={<ThermostatLive status={status?.thermostatSim} />}
          info={<ThermostatInfo status={status?.thermostatSim} values={values} />}
          fields={THERMOSTAT_FIELDS}
          values={values}
          onChange={change}
          note="The thermostat simulator sends one request per interval, built from these settings, to whichever boiler it is connected to."
        />
        <SimulatorCard
          title="Boiler simulator"
          subtitle="An OpenTherm slave: answers every request"
          icon={FlameIcon}
          color="orange"
          running={status?.boilerSim.running}
          partner={status?.boilerSim.partner}
          live={<BoilerLive status={status?.boilerSim} />}
          info={<BoilerInfo values={values} />}
          fields={BOILER_FIELDS}
          values={values}
          onChange={change}
          note="The boiler simulator answers every request from the thermostat it is connected to with these settings; what it does not know it answers Unknown-DataId."
        />
      </div>
    </div>
  )
}

// ── The card ────────────────────────────────────────────────────

const COLORS = {
  blue: {
    header: "bg-blue-50/70 dark:bg-blue-950/30",
    icon: "text-blue-500",
    note: "bg-blue-50/70 text-blue-900 dark:bg-blue-950/30 dark:text-blue-200",
  },
  orange: {
    header: "bg-orange-50/70 dark:bg-orange-950/30",
    icon: "text-orange-500",
    note: "bg-orange-50/70 text-orange-900 dark:bg-orange-950/30 dark:text-orange-200",
  },
} as const

function SimulatorCard({
  title,
  subtitle,
  icon: Icon,
  color,
  running,
  partner,
  live,
  info,
  fields,
  values,
  onChange,
  note,
}: {
  title: string
  subtitle: string
  icon: ComponentType<{ className?: string }>
  color: keyof typeof COLORS
  running?: boolean
  partner?: Endpoint
  live: ReactNode
  info: ReactNode
  fields: Field[]
  values: Values
  onChange: (key: string, value: string | boolean) => void
  note: string
}) {
  const c = COLORS[color]
  return (
    <Card className="gap-0 overflow-hidden py-0">
      <div className={`flex items-center gap-4 border-b p-4 ${c.header}`}>
        <div className="bg-background flex size-12 shrink-0 items-center justify-center rounded-lg border shadow-sm">
          <Icon className={`size-6 ${c.icon}`} />
        </div>
        <div className="min-w-0 flex-1">
          <div className="text-lg font-semibold">{title}</div>
          <div className="text-muted-foreground text-sm">{subtitle}</div>
        </div>
        <div className="text-right text-sm">
          <div className="flex items-center justify-end gap-1.5">
            <span className={`size-2.5 rounded-full ${running ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
            <span className={running ? "text-emerald-700 dark:text-emerald-400" : "text-muted-foreground"}>
              {running === undefined ? "…" : running ? "Running" : "Not connected"}
            </span>
          </div>
          {running && partner && <div className="text-muted-foreground text-xs">with {PARTNER_NAMES[partner]}</div>}
        </div>
      </div>

      <div className="grid gap-4 p-4 md:grid-cols-2">
        {live}
        {info}
      </div>

      <div className="border-t p-4">
        <h3 className="mb-3 font-medium">Simulation settings</h3>
        <div className="grid gap-x-6 gap-y-4 sm:grid-cols-2">
          {fields.map((f) => (
            <SettingField key={f.key} field={f} value={values[f.key]} onChange={onChange} />
          ))}
        </div>
        <div className={`mt-4 flex items-start gap-2 rounded-lg p-3 text-sm ${c.note}`}>
          <InfoIcon className="mt-0.5 size-4 shrink-0" />
          <p>{note}</p>
        </div>
      </div>
    </Card>
  )
}

function SettingField({
  field,
  value,
  onChange,
}: {
  field: Field
  value: SettingEntry["value"] | undefined
  onChange: (key: string, value: string | boolean) => void
}) {
  if (field.kind === "toggle")
    return (
      <div className="flex flex-col gap-1.5">
        <span className="text-sm font-medium">{field.label}</span>
        <label className="flex items-center gap-2 text-sm">
          <Switch
            checked={value === true}
            disabled={value === undefined}
            onCheckedChange={(on) => onChange(field.key, on)}
          />
          <span className="text-muted-foreground">{field.description}</span>
        </label>
      </div>
    )
  return <NumberField field={field} value={value} onChange={onChange} />
}

/** Edited as text, sent when it is left or Enter is pressed, and only when changed. */
function NumberField({
  field,
  value,
  onChange,
}: {
  field: Extract<Field, { kind: "number" }>
  value: SettingEntry["value"] | undefined
  onChange: (key: string, value: string | boolean) => void
}) {
  const shown = value === undefined ? "" : String(roundForDisplay(Number(value)))
  const [draft, setDraft] = useState<string | null>(null)

  const commit = () => {
    if (draft === null) return
    const n = Number(draft)
    if (draft.trim() !== "" && Number.isFinite(n) && draft !== shown) onChange(field.key, draft.trim())
    setDraft(null)
  }

  return (
    <div className="flex flex-col gap-1.5">
      <label htmlFor={field.key} className="text-sm font-medium">
        {field.label}
      </label>
      <div className="relative">
        <Input
          id={field.key}
          type="number"
          step={field.step}
          disabled={value === undefined}
          value={draft ?? shown}
          onChange={(e) => setDraft(e.target.value)}
          onBlur={commit}
          onKeyDown={(e) => e.key === "Enter" && commit()}
          className={field.unit ? "pr-12" : ""}
        />
        {field.unit && (
          <span className="text-muted-foreground pointer-events-none absolute top-1/2 right-3 -translate-y-1/2 text-sm">
            {field.unit}
          </span>
        )}
      </div>
    </div>
  )
}

/** A float setting comes back as e.g. 19.799999; show it as it was typed. */
function roundForDisplay(n: number): number {
  return Math.round(n * 100) / 100
}

// ── Live state and device info ──────────────────────────────────

function Panel({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="rounded-xl border">
      <div className="border-b px-4 py-2.5 text-sm font-medium">{title}</div>
      <dl className="divide-y px-4 text-sm">{children}</dl>
    </div>
  )
}

function Row({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-4 py-2">
      <dt className="text-muted-foreground">{label}</dt>
      <dd className="text-right">{children}</dd>
    </div>
  )
}

function OnOff({ on }: { on?: boolean }) {
  if (on === undefined) return <Unknown />
  return (
    <span
      className={`rounded-md px-2 py-0.5 text-xs ${
        on ? "bg-emerald-100 text-emerald-700 dark:bg-emerald-950 dark:text-emerald-300" : "bg-muted text-muted-foreground"
      }`}
    >
      {on ? "On" : "Off"}
    </span>
  )
}

function Unknown() {
  return <span className="text-muted-foreground">—</span>
}

function Num({ value, unit, digits = 1 }: { value?: number; unit: string; digits?: number }) {
  if (value === undefined) return <Unknown />
  return (
    <span className="font-mono">
      {value.toFixed(digits)} {unit}
    </span>
  )
}

function Ago({ ms }: { ms?: number }) {
  if (ms === undefined) return <Unknown />
  return <span>{(ms / 1000).toFixed(1)} s ago</span>
}

function mode(ch?: boolean, dhw?: boolean): string {
  if (ch === undefined && dhw === undefined) return "—"
  if (dhw) return "Hot water"
  if (ch) return "Heating"
  return "Idle"
}

function ThermostatLive({ status }: { status?: SimStatus["thermostatSim"] }) {
  const b = status?.boiler ?? {}
  return (
    <Panel title="What the boiler reports (live)">
      <Row label="Mode">{mode(b.ch, b.dhw)}</Row>
      <Row label="Flame"><OnOff on={b.flame} /></Row>
      <Row label="Water temperature"><Num value={b.waterTemp} unit="°C" /></Row>
      <Row label="Modulation level"><Num value={b.modulation} unit="%" digits={0} /></Row>
      <Row label="Fault"><OnOff on={b.fault} /></Row>
      <Row label="Last answer"><Ago ms={status?.lastResponseAgoMs} /></Row>
      <Row label="Answered">
        {status ? `${status.responses} of ${status.requests}` : <Unknown />}
      </Row>
    </Panel>
  )
}

function ThermostatInfo({ status, values }: { status?: SimStatus["thermostatSim"]; values: Values }) {
  const b = status?.boiler ?? {}
  return (
    <Panel title="Device info">
      <Row label="Member id">{values["tsim.member"] !== undefined ? formatMemberId(Number(values["tsim.member"])) : <Unknown />}</Row>
      <Row label="OpenTherm version">2.2</Row>
      <Row label="Boiler member id">{b.memberId !== undefined ? formatMemberId(b.memberId) : <Unknown />}</Row>
      <Row label="Boiler OpenTherm version"><Num value={b.otVersion} unit="" /></Row>
      <Row label="Unknown data ids">{status ? status.unknownIds : <Unknown />}</Row>
      <Row label="Unanswered">{status ? status.unanswered : <Unknown />}</Row>
    </Panel>
  )
}

function BoilerLive({ status }: { status?: SimStatus["boilerSim"] }) {
  const t = status?.thermostat ?? {}
  return (
    <Panel title="What the thermostat asks (live)">
      <Row label="CH enable"><OnOff on={t.ch} /></Row>
      <Row label="DHW enable"><OnOff on={t.dhw} /></Row>
      <Row label="Flow temperature (requested)"><Num value={t.controlSetpoint} unit="°C" /></Row>
      <Row label="Room setpoint"><Num value={t.roomSetpoint} unit="°C" /></Row>
      <Row label="Room temperature"><Num value={t.roomTemp} unit="°C" /></Row>
      <Row label="Max modulation"><Num value={t.maxModulation} unit="%" digits={0} /></Row>
      <Row label="Last request"><Ago ms={status?.lastRequestAgoMs} /></Row>
    </Panel>
  )
}

function BoilerInfo({ values }: { values: Values }) {
  const capabilities = [
    values["bsim.dhwpresent"] === true && "Hot water",
    values["bsim.modulating"] === true ? "Modulation" : values["bsim.modulating"] === false && "On/off",
  ].filter(Boolean) as string[]
  return (
    <Panel title="Device info">
      <Row label="Member id">{values["bsim.member"] !== undefined ? formatMemberId(Number(values["bsim.member"])) : <Unknown />}</Row>
      <Row label="OpenTherm version">2.2</Row>
      <Row label="Maximum capacity">{values["bsim.maxkw"] !== undefined ? `${values["bsim.maxkw"]} kW` : <Unknown />}</Row>
      <Row label="Capabilities">
        <span className="flex flex-wrap justify-end gap-1">
          {capabilities.map((cap) => (
            <span key={cap} className="rounded-md bg-orange-100 px-2 py-0.5 text-xs text-orange-800 dark:bg-orange-950 dark:text-orange-300">
              {cap}
            </span>
          ))}
        </span>
      </Row>
    </Panel>
  )
}
