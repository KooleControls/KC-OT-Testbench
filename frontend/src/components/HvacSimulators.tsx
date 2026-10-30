import { useState, type ReactNode } from "react"
import { AirVentIcon, InfoIcon, LockIcon } from "lucide-react"
import { toast } from "sonner"

import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Switch } from "@/components/ui/switch"
import { errorMessage } from "@/hooks/use-led"
import { useHvacSims } from "@/hooks/use-rs485"
import { backend, type HvacSimChange, type HvacSimSlot, type HvacSimUnit, type SettingEntry } from "@/lib/backend"
import { DEMANDS, HVAC_TYPES, POINT_TABLES, hvacType, typeLabel, type Choice, type HvacType } from "@/lib/hvac"
import { TABLE_SHORT, formatPoint, registerName } from "@/lib/modbus"

/**
 * The ten HVAC units the testbench can simulate, set up as the gateway's HVAC list is:
 * a type, by the gateway's numbers, and a Modbus address each. A type on Modbus RTU
 * answers a gateway on the RS485 bus as that interface would -- a CoolPlug, an Intesis
 * box, a Hisense adaptor -- with the register map the gateway's driver for it uses
 * (lib/hvac.ts).
 *
 * Per unit: its type, its address and whether it is on the bus in the card's header,
 * the first things to set. Below, what the gateway reads, as the bus would answer it
 * right now, and the unit's remote -- the indoor unit's state lives on the testbench,
 * and both the gateway and this remote change it (`hvac set`). Then the simulation:
 * room temperature and faults. Those are ordinary settings (hvacN.*), applied within a
 * second; the Simulators page holds them, as it does the other simulators', and sets
 * and saves them in one go.
 */

const STATUS_POLL_MS = 1000

type Values = Record<string, SettingEntry["value"]>

const isSimulated = (u?: HvacSimSlot): u is HvacSimUnit => u?.simulated === true

/** What the card asks for: a change from the remote (`hvac set`), or the filter sign,
 *  which is the unit's own doing (`hvac filter`). */
type Control = HvacSimChange | { filter: boolean }

/** The type a unit's setting names; undefined until the settings are in. */
function typeOf(values: Values, n: number): HvacType | undefined {
  const v = values[`hvac${n}.type`]
  return v === undefined ? undefined : (hvacType(Number(v)) ?? { ...HVAC_TYPES[0], id: Number(v), code: "?", name: `type ${v}`, why: "Not a type the gateway has." })
}

export function HvacSimulators({
  values,
  onSetting,
}: {
  values: Values
  onSetting: (key: string, value: string | boolean) => void
}) {
  const { units, setUnits } = useHvacSims(STATUS_POLL_MS)
  const [selected, setSelected] = useState(1)

  const control = (change: Control) => {
    const request = "filter" in change ? backend.setHvacSimFilter(selected, change.filter) : backend.setHvacSim(selected, change)
    request
      .then((r) => {
        if (!r.ok) return toast.error("Not changed", { description: r.error })
        const unit = r.unit
        if (unit) setUnits((all) => all?.map((u) => (u.unit === unit.unit ? unit : u)) ?? null)
      })
      .catch((e) => toast.error("Not changed", { description: errorMessage(e) }))
  }

  return (
    <section className="flex flex-col gap-4">
      <p className="text-muted-foreground">
        Up to ten HVAC units, set up as in the gateway's HVAC list: a type and an address each. The ones on Modbus
        RTU answer a gateway on the RS485 bus; their traffic is on the{" "}
        <a href="#/rs485" className="hover:text-foreground underline underline-offset-4">
          RS485 page
        </a>
        .
      </p>

      <div role="tablist" aria-label="Simulated HVAC unit" className="grid grid-cols-5 gap-2 lg:grid-cols-10">
        {Array.from({ length: 10 }, (_, i) => i + 1).map((n) => {
          const type = typeOf(values, n)
          const live = type?.simulated === true
          const answering = live && values[`hvac${n}.answer`] === true
          const sub =
            type === undefined
              ? "…"
              : type.id === 0
                ? "not used"
                : `${type.code} · ${live ? `address ${values[`hvac${n}.addr`] ?? "?"}` : "not simulated"}`
          return (
            <button
              key={n}
              type="button"
              role="tab"
              aria-selected={selected === n}
              onClick={() => setSelected(n)}
              className={`flex flex-col items-start rounded-lg border px-3 py-2 text-left text-sm transition-colors ${
                selected === n ? "border-orange-400 ring-2 ring-orange-200 dark:ring-orange-900" : "hover:bg-muted/50"
              } ${live ? "bg-orange-50/60 dark:bg-orange-950/30" : ""}`}
            >
              <span className="flex items-center gap-1.5 font-medium">
                <span
                  className={`size-2 rounded-full ${answering ? "bg-emerald-500" : live ? "bg-amber-500" : "bg-muted-foreground/40"}`}
                />
                Unit {n}
              </span>
              <span className="text-muted-foreground truncate text-xs">{sub}</span>
            </button>
          )
        })}
      </div>

      <UnitCard
        n={selected}
        type={typeOf(values, selected)}
        unit={units?.find((u) => u.unit === selected)}
        values={values}
        onSetting={onSetting}
        onControl={control}
      />
    </section>
  )
}

function UnitCard({
  n,
  type,
  unit,
  values,
  onSetting,
  onControl,
}: {
  n: number
  type?: HvacType
  unit?: HvacSimSlot
  values: Values
  onSetting: (key: string, value: string | boolean) => void
  onControl: (change: Control) => void
}) {
  const key = (name: string) => `hvac${n}.${name}`
  const live = type?.simulated === true
  // The status lags a change of type by up to a second; until it catches up, it is
  // the old unit's and says nothing about this one.
  const sim = type && isSimulated(unit) && unit.type === type.id ? unit : undefined

  const subtitle = !type
    ? "…"
    : !live
      ? type.why
      : [type.via, sim?.serial && `S/N ${sim.serial}`, sim?.firmware && `firmware ${sim.firmware}`].filter(Boolean).join(" · ")

  return (
    <Card className={`gap-0 overflow-hidden py-0 transition-colors ${live ? "" : "bg-muted/20"}`}>
      {/* What it is, where, and whether it answers: the first things to set, so in the
          header rather than with the other settings below the fold. */}
      <div
        className={`flex flex-wrap items-center gap-4 border-b p-4 transition-colors ${
          live ? "bg-orange-50/70 dark:bg-orange-950/30" : "bg-muted/60"
        }`}
      >
        <div className="bg-background flex size-12 shrink-0 items-center justify-center rounded-lg border shadow-sm">
          <AirVentIcon className={`size-6 ${live ? "text-orange-500" : "text-muted-foreground"}`} />
        </div>
        <div className="min-w-0 flex-1">
          <div className={`text-lg font-semibold ${live ? "" : "text-muted-foreground"}`}>
            Unit {n}
            {type && type.id !== 0 && ` · ${type.name}`}
          </div>
          <div className="text-muted-foreground text-sm">{subtitle}</div>
        </div>
        <div className="flex flex-wrap items-start gap-6">
          <TypeSelect id={key("type")} value={values[key("type")]} onChange={(v) => onSetting(key("type"), v)} />
          <NumberSetting
            id={key("addr")}
            label="Modbus address"
            unit=""
            step={1}
            value={values[key("addr")]}
            onChange={(v) => onSetting(key("addr"), v)}
            className="w-28"
          />
          <BusSwitch
            live={live}
            unit={sim}
            value={values[key("answer")]}
            onChange={(v) => onSetting(key("answer"), v)}
          />
        </div>
      </div>

      {type && live ? (
        <>
          <div className="grid gap-4 p-4 md:grid-cols-2">
            <GatewayView type={type} unit={sim} />
            <Remote type={type} unit={sim} onControl={onControl} />
          </div>
          <SimulationSettings type={type} n={n} values={values} onSetting={onSetting} />
        </>
      ) : (
        <p className="text-muted-foreground p-4 text-sm">
          {type?.id === 0
            ? "Not used. Pick a type, as in the gateway's HVAC list, and the unit is on the bus within a second."
            : type
              ? type.why
              : "…"}
        </p>
      )}
    </Card>
  )
}

/** Its type, in the gateway's numbers and words. */
function TypeSelect({
  id,
  value,
  onChange,
}: {
  id: string
  value: SettingEntry["value"] | undefined
  onChange: (value: string) => void
}) {
  return (
    <div className="flex flex-col gap-1.5">
      <label htmlFor={id} className="text-sm font-medium">
        Type
      </label>
      <select
        id={id}
        className={`${selectClass} w-[26rem] max-w-full`}
        disabled={value === undefined}
        value={value === undefined ? "" : String(value)}
        onChange={(e) => onChange(e.target.value)}
      >
        {HVAC_TYPES.map((t) => (
          // OpenTherm and the relay are never on a bus here; a CoolMasterNet will be.
          <option key={t.id} value={t.id} disabled={!t.simulated && t.id !== 0 && t.via !== "Modbus TCP"}>
            {typeLabel(t)}
            {!t.simulated && t.via === "Modbus TCP" ? " (not yet)" : ""}
          </option>
        ))}
      </select>
    </div>
  )
}

/** The hvacN.answer switch, with what the unit is doing on the bus beside it. */
function BusSwitch({
  live,
  unit,
  value,
  onChange,
}: {
  live: boolean
  unit?: HvacSimUnit
  value: SettingEntry["value"] | undefined
  onChange: (on: boolean) => void
}) {
  const on = value === true
  const [text, color] = !live
    ? ["Not simulated", "text-muted-foreground"]
    : !on
      ? ["Off the bus: answers nothing", "text-amber-600 dark:text-amber-400"]
      : unit?.restarting
        ? [unit.pendingAddress ? `Restarting, then at ${unit.pendingAddress}` : "Restarting", "text-amber-600 dark:text-amber-400"]
        : unit?.address === 0
          ? ["No address a master can use", "text-amber-600 dark:text-amber-400"]
          : ["Answering", "text-emerald-700 dark:text-emerald-400"]
  return (
    <div className="flex flex-col gap-1.5">
      <span className="text-sm font-medium">On the bus</span>
      <label className="flex h-9 items-center gap-2 text-sm">
        <Switch checked={on} disabled={value === undefined || !live} onCheckedChange={onChange} />
        <span className={`whitespace-nowrap ${color}`}>{text}</span>
      </label>
    </div>
  )
}

/** The registers the gateway reads, as the bus would answer them now. */
function GatewayView({ type, unit }: { type: HvacType; unit?: HvacSimUnit }) {
  return (
    <Panel title="What the gateway reads (live)">
      {unit ? (
        unit.points.map((p) => {
          const table = POINT_TABLES[p.t]
          return (
            <Row
              key={`${p.t}${p.a}`}
              label={
                <>
                  {registerName(type.registers, table, p.a)}{" "}
                  <span className="text-muted-foreground/70 font-mono text-xs">
                    {TABLE_SHORT[table]} {p.a}
                  </span>
                </>
              }
            >
              {formatPoint(type.registers, table, p.a, p.v)}{" "}
              <span className="text-muted-foreground font-mono text-xs">({p.v})</span>
            </Row>
          )
        })
      ) : (
        <Row label="Registers">
          <Unknown />
        </Row>
      )}
      {!type.room && (
        <Row label="Room temperature (simulated, not reported)">{unit ? <Temp value={unit.room} /> : <Unknown />}</Row>
      )}
      {unit?.ownAmbient !== undefined && unit.ownAmbient !== 0 && (
        <Row label="Room temperature fed by the gateway">
          <Temp value={unit.ownAmbient} />
        </Row>
      )}
      <Row label="Demand">{unit ? (DEMANDS[unit.demand] ?? unit.demand) : <Unknown />}</Row>
      <Row label="Asked">
        {unit ? `${unit.requests} requests, ${unit.writes} writes, ${unit.exceptions} refused` : <Unknown />}
      </Row>
      <Row label="Last request">
        {unit?.lastRequestAgoMs === undefined ? <Unknown /> : `${(unit.lastRequestAgoMs / 1000).toFixed(1)} s ago`}
      </Row>
    </Panel>
  )
}

/** The unit's own remote, in its type's codes. */
function Remote({
  type,
  unit,
  onControl,
}: {
  type: HvacType
  unit?: HvacSimUnit
  onControl: (change: Control) => void
}) {
  const locked = unit?.locked === true
  const off = !unit || locked
  return (
    <Panel title="Remote control (live)">
      <div className="flex flex-col gap-3 py-3">
        {locked && (
          <div className="flex items-start gap-2 rounded-lg bg-amber-50 p-2.5 text-sm text-amber-900 dark:bg-amber-950/40 dark:text-amber-200">
            <LockIcon className="mt-0.5 size-4 shrink-0" />
            <span>The gateway has locked this unit's remote. It changes nothing until the gateway frees it.</span>
          </div>
        )}
        <ControlRow label="Power">
          <div className="flex gap-2">
            <Button size="sm" variant={unit?.power ? "default" : "outline"} disabled={off} onClick={() => onControl({ power: true })}>
              On
            </Button>
            <Button size="sm" variant={unit && !unit.power ? "default" : "outline"} disabled={off} onClick={() => onControl({ power: false })}>
              Off
            </Button>
          </div>
        </ControlRow>
        <ControlRow label="Mode">
          <ChoiceSelect options={type.modes} value={unit?.mode} disabled={off} onChange={(mode) => onControl({ mode })} />
        </ControlRow>
        <ControlRow label="Setpoint">
          <SetpointInput
            value={unit?.setpoint}
            range={type.setpoint}
            disabled={off}
            onChange={(tenths) => onControl({ setpoint: tenths })}
          />
        </ControlRow>
        <ControlRow label="Fan speed">
          <ChoiceSelect options={type.fans} value={unit?.fan} disabled={off} onChange={(fan) => onControl({ fan })} />
        </ControlRow>
        <ControlRow label="Louvres">
          <ChoiceSelect options={type.swings} value={unit?.swing} disabled={off} onChange={(swing) => onControl({ swing })} />
        </ControlRow>
        {type.filter && (
          <ControlRow label="Filter sign">
            <Switch checked={unit?.filter ?? false} disabled={!unit} onCheckedChange={(filter) => onControl({ filter })} />
          </ControlRow>
        )}
        <p className="text-muted-foreground text-xs">
          In the {type.name}'s own codes, as its control registers take them. A change applies at once, as on the
          unit's own remote. The gateway reads it at its next poll and can overwrite it. Not saved: every unit starts
          off, cooling at 23 °C, after a reboot or a change of type.
        </p>
      </div>
    </Panel>
  )
}

function SimulationSettings({
  type,
  n,
  values,
  onSetting,
}: {
  type: HvacType
  n: number
  values: Values
  onSetting: (key: string, value: string | boolean) => void
}) {
  const key = (name: string) => `hvac${n}.${name}`
  return (
    <div className="border-t p-4">
      <h3 className="mb-3 font-medium">Simulation settings</h3>
      <div className="grid gap-x-6 gap-y-4 sm:grid-cols-2">
        <NumberSetting
          id={key("room")}
          label="Room temperature"
          unit="°C"
          step={0.5}
          value={values[key("room")]}
          onChange={(v) => onSetting(key("room"), v)}
          hint={
            type.room
              ? "Reported ±0.1 to 0.5 °C around this, new every 10 s"
              : "Not reported: this type has no register for it. It still decides the demand."
          }
        />
        <NumberSetting
          id={key("fail")}
          label="Failure code"
          unit=""
          step={1}
          value={values[key("fail")]}
          onChange={(v) => onSetting(key("fail"), v)}
          hint="0: no failure. Reported as the type does: its failure code, and its failure flag or bit where it has one"
        />
        {type.indoor && (
          <Toggle
            label="Indoor unit connected"
            description="Off: the line state says the interface lost its indoor unit"
            value={values[key("indoor")]}
            onChange={(v) => onSetting(key("indoor"), v)}
          />
        )}
        <NumberSetting
          id={key("delay")}
          label="Reply delay"
          unit="ms"
          step={10}
          value={values[key("delay")]}
          onChange={(v) => onSetting(key("delay"), v)}
          hint="Answer late, up to 5000 ms, to test the gateway's timeout"
        />
      </div>
      <div className="mt-4 flex items-start gap-2 rounded-lg bg-orange-50/70 p-3 text-sm text-orange-900 dark:bg-orange-950/30 dark:text-orange-200">
        <InfoIcon className="mt-0.5 size-4 shrink-0" />
        <p>
          Settings apply within a second and are saved. {type.source} A point the map does not have is refused
          (Illegal Data Address), so a gateway reading the wrong one shows up on the RS485 page.
        </p>
      </div>
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

function Row({ label, children }: { label: ReactNode; children: ReactNode }) {
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

function Temp({ value }: { value: number }) {
  return <span className="font-mono">{value.toFixed(1)} °C</span>
}

function Unknown() {
  return <span className="text-muted-foreground">—</span>
}

const selectClass =
  "border-input dark:bg-input/30 h-9 rounded-lg border bg-transparent px-3 text-sm disabled:cursor-not-allowed disabled:opacity-50"

function ChoiceSelect({
  options,
  value,
  disabled,
  onChange,
}: {
  options: Choice[]
  value?: number
  disabled?: boolean
  onChange: (value: number) => void
}) {
  return (
    <select
      className={`${selectClass} w-44`}
      disabled={value === undefined || disabled}
      value={value ?? ""}
      onChange={(e) => onChange(Number(e.target.value))}
    >
      {value !== undefined && !options.some((o) => o.value === value) && <option value={value}>{value} - ?</option>}
      {options.map((o) => (
        <option key={o.value} value={o.value}>
          {o.value} - {o.label}
        </option>
      ))}
    </select>
  )
}

/** The setpoint in °C, sent in tenths when it is left or Enter is pressed. */
function SetpointInput({
  value,
  range,
  disabled,
  onChange,
}: {
  value?: number
  range: HvacType["setpoint"]
  disabled?: boolean
  onChange: (tenths: number) => void
}) {
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
        step={range.step < 1 ? 0.5 : range.step}
        min={range.min}
        max={range.max}
        aria-label="Setpoint"
        disabled={value === undefined || disabled}
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
  className = "",
  onChange,
}: {
  id: string
  label: string
  unit: string
  step: number
  value: SettingEntry["value"] | undefined
  hint?: string
  className?: string
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
    <div className={`flex flex-col gap-1.5 ${className}`}>
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
