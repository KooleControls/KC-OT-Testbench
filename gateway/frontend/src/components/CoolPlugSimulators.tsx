import { useCallback, useEffect, useState, type ReactNode } from "react"
import { AirVentIcon, InfoIcon } from "lucide-react"
import { toast } from "sonner"

import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Switch } from "@/components/ui/switch"
import { errorMessage } from "@/hooks/use-led"
import { useCoolPlugs } from "@/hooks/use-rs485"
import { backend, type CoolPlugChange, type CoolPlugUnit, type SettingEntry } from "@/lib/backend"
import { DEMANDS, FAN_SPEEDS, MODES, SWINGS } from "@/lib/modbus"

/**
 * The ten CoolPlugs the testbench can simulate on its RS485 port: Modbus RTU slaves a
 * gateway configured for CoolPlugs cannot tell from real ones.
 *
 * Laid out like the CoolPlug config tool: what the indoor unit reports on the left,
 * its remote on the right -- the indoor unit's state lives on the testbench, and both
 * the gateway and this remote change it (`coolplug set`). Below, the simulation
 * itself: whether the unit is on the bus, its address, its room temperature and the
 * faults to simulate. Those are ordinary settings (cpN.*), applied within a second.
 */

const STATUS_POLL_MS = 1000

type Values = Record<string, SettingEntry["value"]>

export function CoolPlugSimulators() {
  const { units, setUnits } = useCoolPlugs(STATUS_POLL_MS)
  const [values, setValues] = useState<Values>({})
  const [selected, setSelected] = useState(1)

  const loadSettings = useCallback(() => {
    backend
      .getSettings()
      .then((r) => setValues(Object.fromEntries(r.settings.map((s) => [s.key, s.value]))))
      .catch((e) => toast.error("Could not read the settings", { description: errorMessage(e) }))
  }, [])

  useEffect(loadSettings, [loadSettings])

  // Set and save in one go, as the other simulators do: a simulation setting is meant
  // to apply now and to stay.
  const changeSetting = (key: string, value: string | boolean) => {
    setValues((v) => ({ ...v, [key]: value }))
    backend
      .setSetting(key, String(value))
      .then((r) => {
        if (!r.ok) throw new Error("refused")
        return backend.saveSettings()
      })
      .catch((e) => {
        toast.error("Not changed", { description: errorMessage(e) })
        loadSettings()
      })
  }

  const applied = (unit?: CoolPlugUnit) => {
    if (unit) setUnits((all) => all?.map((u) => (u.unit === unit.unit ? unit : u)) ?? null)
  }

  const control = (change: CoolPlugChange) => {
    backend
      .setCoolPlug(selected, change)
      .then((r) => (r.ok ? applied(r.unit) : toast.error("Not changed", { description: r.error })))
      .catch((e) => toast.error("Not changed", { description: errorMessage(e) }))
  }

  const setFilter = (on: boolean) => {
    backend
      .setCoolPlugFilter(selected, on)
      .then((r) => (r.ok ? applied(r.unit) : toast.error("Not changed", { description: r.error })))
      .catch((e) => toast.error("Not changed", { description: errorMessage(e) }))
  }

  const unit = units?.find((u) => u.unit === selected)

  return (
    <section className="flex flex-col gap-4">
      <div>
        <h2 className="text-2xl font-semibold">CoolPlug simulators</h2>
        <p className="text-muted-foreground mt-1">
          Up to ten CoolPlugs, each with its indoor unit, answering a gateway on the RS485 bus. Their
          traffic is on the RS485 page.
        </p>
      </div>

      <div role="tablist" aria-label="Simulated CoolPlug" className="grid grid-cols-5 gap-2 lg:grid-cols-10">
        {Array.from({ length: 10 }, (_, i) => i + 1).map((n) => {
          const u = units?.find((x) => x.unit === n)
          const on = values[`cp${n}.en`] === true
          return (
            <button
              key={n}
              type="button"
              role="tab"
              aria-selected={selected === n}
              onClick={() => setSelected(n)}
              className={`flex flex-col items-start rounded-lg border px-3 py-2 text-left text-sm transition-colors ${
                selected === n ? "border-orange-400 ring-2 ring-orange-200 dark:ring-orange-900" : "hover:bg-muted/50"
              } ${on ? "bg-orange-50/60 dark:bg-orange-950/30" : ""}`}
            >
              <span className="flex items-center gap-1.5 font-medium">
                <span className={`size-2 rounded-full ${on ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
                Sim {n}
              </span>
              <span className="text-muted-foreground text-xs">
                {on ? `address ${u?.address ?? values[`cp${n}.addr`] ?? "?"}` : "off the bus"}
              </span>
            </button>
          )
        })}
      </div>

      <UnitCard
        n={selected}
        unit={unit}
        values={values}
        onSetting={changeSetting}
        onControl={control}
        onFilter={setFilter}
      />
    </section>
  )
}

function UnitCard({
  n,
  unit,
  values,
  onSetting,
  onControl,
  onFilter,
}: {
  n: number
  unit?: CoolPlugUnit
  values: Values
  onSetting: (key: string, value: string | boolean) => void
  onControl: (change: CoolPlugChange) => void
  onFilter: (on: boolean) => void
}) {
  const on = values[`cp${n}.en`] === true
  const key = (name: string) => `cp${n}.${name}`

  return (
    <Card className={`gap-0 overflow-hidden py-0 transition-colors ${on ? "" : "bg-muted/20"}`}>
      <div
        className={`flex items-center gap-4 border-b p-4 transition-colors ${
          on ? "bg-orange-50/70 dark:bg-orange-950/30" : "bg-muted/60"
        }`}
      >
        <div className="bg-background flex size-12 shrink-0 items-center justify-center rounded-lg border shadow-sm">
          <AirVentIcon className={`size-6 ${on ? "text-orange-500" : "text-muted-foreground"}`} />
        </div>
        <div className="min-w-0 flex-1">
          <div className={`text-lg font-semibold ${on ? "" : "text-muted-foreground"}`}>CoolPlug simulator {n}</div>
          <div className="text-muted-foreground text-sm">
            {unit ? `S/N ${unit.serial} · firmware ${unit.firmware}` : "…"}
          </div>
        </div>
        <BusState unit={unit} on={on} />
      </div>

      <div className="grid gap-4 p-4 md:grid-cols-2">
        <Panel title="Indoor unit (what the gateway reads)">
          <Row label="On / off">{unit ? <OnOff on={unit.power} /> : <Unknown />}</Row>
          <Row label="Mode">{unit ? MODES[unit.mode] : <Unknown />}</Row>
          <Row label="Setpoint">{unit ? <Temp value={unit.setpoint} /> : <Unknown />}</Row>
          <Row label="Room temperature">{unit ? <Temp value={unit.room} /> : <Unknown />}</Row>
          <Row label="Fan speed">{unit ? FAN_SPEEDS[unit.fan] : <Unknown />}</Row>
          <Row label="Louvres">{unit ? SWINGS[unit.swing] : <Unknown />}</Row>
          <Row label="Failure">
            {unit ? (unit.failure === 0 ? "none (OK)" : <span className="text-destructive">code {unit.failure}</span>) : <Unknown />}
          </Row>
          <Row label="Filter">{unit ? (unit.filter ? "clean the filter" : "ok") : <Unknown />}</Row>
          <Row label="Demand">{unit ? DEMANDS[unit.demand] : <Unknown />}</Row>
          {unit && unit.ownAmbient !== 0 && (
            <Row label="Room temperature fed by the gateway">
              <Temp value={unit.ownAmbient} />
            </Row>
          )}
          <Row label="Asked">
            {unit ? `${unit.requests} requests, ${unit.writes} writes, ${unit.exceptions} refused` : <Unknown />}
          </Row>
          <Row label="Last request">
            {unit?.lastRequestAgoMs === undefined ? <Unknown /> : `${(unit.lastRequestAgoMs / 1000).toFixed(1)} s ago`}
          </Row>
        </Panel>

        <Panel title="Remote control (live)">
          <div className="flex flex-col gap-3 py-3">
            <ControlRow label="Power">
              <div className="flex gap-2">
                <Button size="sm" variant={unit?.power ? "default" : "outline"} disabled={!unit} onClick={() => onControl({ power: true })}>
                  On
                </Button>
                <Button size="sm" variant={unit && !unit.power ? "default" : "outline"} disabled={!unit} onClick={() => onControl({ power: false })}>
                  Off
                </Button>
              </div>
            </ControlRow>
            <ControlRow label="Mode">
              <Choice options={MODES} value={unit?.mode} onChange={(mode) => onControl({ mode })} />
            </ControlRow>
            <ControlRow label="Setpoint">
              <SetpointInput value={unit?.setpoint} onChange={(tenths) => onControl({ setpoint: tenths })} />
            </ControlRow>
            <ControlRow label="Fan speed">
              <Choice options={FAN_SPEEDS} value={unit?.fan} onChange={(fan) => onControl({ fan })} />
            </ControlRow>
            <ControlRow label="Louvres">
              <Choice options={SWINGS} value={unit?.swing} onChange={(swing) => onControl({ swing })} />
            </ControlRow>
            <ControlRow label="Filter sign">
              <Switch checked={unit?.filter ?? false} disabled={!unit} onCheckedChange={onFilter} />
            </ControlRow>
            <p className="text-muted-foreground text-xs">
              A change applies at once, as on the unit's own remote. The gateway reads it at its next poll and can
              overwrite it. Not saved: every unit starts off, cooling at 23.0 °C after a reboot.
            </p>
          </div>
        </Panel>
      </div>

      <div className="border-t p-4">
        <h3 className="mb-3 font-medium">Simulation settings</h3>
        <div className="grid gap-x-6 gap-y-4 sm:grid-cols-2">
          <Toggle label="On the bus" description="Answer at its Modbus address" value={values[key("en")]} onChange={(v) => onSetting(key("en"), v)} />
          <NumberSetting id={key("addr")} label="Modbus address" unit="" step={1} value={values[key("addr")]} onChange={(v) => onSetting(key("addr"), v)} />
          <NumberSetting id={key("room")} label="Room temperature" unit="°C" step={0.5} value={values[key("room")]} onChange={(v) => onSetting(key("room"), v)} hint="Reported ±0.1 to 0.5 °C around this, new every 10 s" />
          <NumberSetting id={key("fail")} label="HVAC failure code" unit="" step={1} value={values[key("fail")]} onChange={(v) => onSetting(key("fail"), v)} hint="0: no failure. Anything else also sets the line state's failure bit" />
          <Toggle label="Indoor unit connected" description="Off: the line state says the CoolPlug lost its indoor unit" value={values[key("indoor")]} onChange={(v) => onSetting(key("indoor"), v)} />
          <Toggle label="Answers requests" description="Off: silent on the bus, as a dead unit" value={values[key("answer")]} onChange={(v) => onSetting(key("answer"), v)} />
          <NumberSetting id={key("delay")} label="Reply delay" unit="ms" step={10} value={values[key("delay")]} onChange={(v) => onSetting(key("delay"), v)} hint="Answer late, up to 5000 ms, to test the gateway's timeout" />
        </div>
        <div className="mt-4 flex items-start gap-2 rounded-lg bg-orange-50/70 p-3 text-sm text-orange-900 dark:bg-orange-950/30 dark:text-orange-200">
          <InfoIcon className="mt-0.5 size-4 shrink-0" />
          <p>
            Settings apply within a second and are saved. The register map is the CoolPlug's (PRM rev 0.3); the
            gateway's registers are 95, 97, 144-147, 150 and 151. A gateway can move the unit to another address by
            writing register 35 and restarting it (register 1); the new address is saved too.
          </p>
        </div>
      </div>
    </Card>
  )
}

function BusState({ unit, on }: { unit?: CoolPlugUnit; on: boolean }) {
  const [dot, text, sub] = !on
    ? ["bg-muted-foreground/40", "Off the bus", "Switch it on below"]
    : unit?.restarting
      ? ["bg-amber-500", "Restarting", unit.pendingAddress ? `then at address ${unit.pendingAddress}` : ""]
      : unit && !unit.answers
        ? ["bg-amber-500", "Silent", `at address ${unit.address}, answering nothing`]
        : ["bg-emerald-500", "On the bus", unit ? `Modbus address ${unit.address}` : ""]
  return (
    <div className="text-right text-sm">
      <div className="flex items-center justify-end gap-1.5">
        <span className={`size-2.5 rounded-full ${dot}`} />
        <span className={on ? "text-emerald-700 dark:text-emerald-400" : "text-muted-foreground font-medium"}>{text}</span>
      </div>
      <div className="text-muted-foreground text-xs">{sub}</div>
    </div>
  )
}

function Panel({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="bg-card rounded-xl border">
      <div className="border-b px-4 py-2.5 text-sm font-medium">{title}</div>
      <div className="divide-y px-4 text-sm">{children}</div>
    </div>
  )
}

function Row({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-4 py-2">
      <span className="text-muted-foreground">{label}</span>
      <span className="text-right">{children}</span>
    </div>
  )
}

function ControlRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-4">
      <span className="text-muted-foreground">{label}</span>
      {children}
    </div>
  )
}

function OnOff({ on }: { on: boolean }) {
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

function Temp({ value }: { value: number }) {
  return <span className="font-mono">{value.toFixed(1)} °C</span>
}

function Unknown() {
  return <span className="text-muted-foreground">—</span>
}

const selectClass = "border-input dark:bg-input/30 h-9 w-44 rounded-lg border bg-transparent px-3 text-sm"

function Choice({
  options,
  value,
  onChange,
}: {
  options: readonly string[]
  value?: number
  onChange: (value: number) => void
}) {
  return (
    <select
      className={selectClass}
      disabled={value === undefined}
      value={value ?? ""}
      onChange={(e) => onChange(Number(e.target.value))}
    >
      {options.map((o, i) => (
        <option key={o} value={i}>
          {i} - {o}
        </option>
      ))}
    </select>
  )
}

/** The setpoint in °C, sent in tenths when it is left or Enter is pressed. */
function SetpointInput({ value, onChange }: { value?: number; onChange: (tenths: number) => void }) {
  const [draft, setDraft] = useState<string | null>(null)
  const shown = value === undefined ? "" : value.toFixed(1)

  const commit = () => {
    if (draft === null) return
    const t = Math.round(Number(draft) * 10)
    if (draft.trim() !== "" && Number.isFinite(t) && draft !== shown) onChange(t)
    setDraft(null)
  }

  return (
    <div className="relative w-44">
      <Input
        type="number"
        step={0.5}
        min={10}
        max={35}
        aria-label="Setpoint"
        disabled={value === undefined}
        value={draft ?? shown}
        onChange={(e) => setDraft(e.target.value)}
        onBlur={commit}
        onKeyDown={(e) => e.key === "Enter" && commit()}
        className="pr-10"
      />
      <span className="text-muted-foreground pointer-events-none absolute top-1/2 right-3 -translate-y-1/2 text-sm">°C</span>
    </div>
  )
}

function Toggle({
  label,
  description,
  value,
  onChange,
}: {
  label: string
  description: string
  value: SettingEntry["value"] | undefined
  onChange: (value: boolean) => void
}) {
  return (
    <div className="flex flex-col gap-1.5">
      <span className="text-sm font-medium">{label}</span>
      <label className="flex items-center gap-2 text-sm">
        <Switch checked={value === true} disabled={value === undefined} onCheckedChange={onChange} />
        <span className="text-muted-foreground">{description}</span>
      </label>
    </div>
  )
}

/** Edited as text, sent when it is left or Enter is pressed, and only when changed. */
function NumberSetting({
  id,
  label,
  unit,
  step,
  value,
  hint,
  onChange,
}: {
  id: string
  label: string
  unit: string
  step: number
  value: SettingEntry["value"] | undefined
  hint?: string
  onChange: (value: string) => void
}) {
  // A float setting comes back as e.g. 21.299999; show it as it was typed.
  const shown = value === undefined ? "" : String(Math.round(Number(value) * 100) / 100)
  const [draft, setDraft] = useState<string | null>(null)

  const commit = () => {
    if (draft === null) return
    if (draft.trim() !== "" && Number.isFinite(Number(draft)) && draft !== shown) onChange(draft.trim())
    setDraft(null)
  }

  return (
    <div className="flex flex-col gap-1.5">
      <label htmlFor={id} className="text-sm font-medium">
        {label}
      </label>
      <div className="relative">
        <Input
          id={id}
          type="number"
          step={step}
          disabled={value === undefined}
          value={draft ?? shown}
          onChange={(e) => setDraft(e.target.value)}
          onBlur={commit}
          onKeyDown={(e) => e.key === "Enter" && commit()}
          className={unit ? "pr-12" : ""}
        />
        {unit && (
          <span className="text-muted-foreground pointer-events-none absolute top-1/2 right-3 -translate-y-1/2 text-sm">
            {unit}
          </span>
        )}
      </div>
      {hint && <span className="text-muted-foreground text-xs">{hint}</span>}
    </div>
  )
}
