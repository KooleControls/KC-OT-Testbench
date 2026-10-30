import { useCallback, useEffect, useState, type ComponentType, type ReactNode } from "react"
import { AirVentIcon, FlameIcon, InfoIcon, SearchIcon, SearchXIcon, ThermometerIcon, XIcon } from "lucide-react"
import { toast } from "sonner"

import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Switch } from "@/components/ui/switch"
import { HvacSimulators } from "@/components/HvacSimulators"
import { errorMessage } from "@/hooks/use-led"
import { formatMemberId } from "@/hooks/use-opentherm"
import { backend, type SettingEntry, type SimStatus } from "@/lib/backend"
import { hvacType } from "@/lib/hvac"
import type { Endpoint } from "@/lib/routing"
import { matchesSearch, searchTerms, type Searchable } from "@/lib/simSearch"

/**
 * The simulators that run on the testbench. On OpenTherm, two: a thermostat (a master)
 * and a boiler (a slave). Which device each one talks to is chosen on the
 * Connections page; here is what they do. Their behaviour is a set of ordinary
 * settings (tsim.*, bsim.*) that the testbench reads with every frame, so a change
 * applies straight away -- and the live state shows what the other end has said.
 *
 * A simulator that is not connected is drawn in gray, but stays editable: setting it
 * up before connecting it is the usual order. The search narrows both simulators to
 * the settings and rows that match, and each tab says how many.
 *
 * The third tab holds the HVAC units simulated on the RS485 bus, set up as the
 * gateway's HVAC list is (components/HvacSimulators), which the search leaves alone:
 * ten units of the same few settings search poorly.
 */

const STATUS_POLL_MS = 1000

const TABS = ["thermostat", "boiler", "hvac"] as const
type Tab = (typeof TABS)[number]

// The open tab is the hash's second part (#/simulators/hvac), so a link opens it
// and F5 keeps it. Without one, the tab open last time the page was shown.
let lastTab: Tab = "thermostat"

function tabFromHash(): Tab {
  const t = window.location.hash.replace(/^#\/?/, "").split("/")[1]?.toLowerCase()
  return TABS.includes(t as Tab) ? (t as Tab) : lastTab
}

function useTab(): [Tab, (tab: Tab) => void] {
  const [tab, setTabState] = useState(tabFromHash)

  useEffect(() => {
    lastTab = tab
  }, [tab])

  useEffect(() => {
    const onHashChange = () => setTabState(tabFromHash())
    window.addEventListener("hashchange", onHashChange)
    return () => window.removeEventListener("hashchange", onHashChange)
  }, [])

  const setTab = useCallback((t: Tab) => {
    // Replaced, not pushed: Back leaves the page instead of stepping through its tabs.
    history.replaceState(null, "", `#/simulators/${t}`)
    setTabState(t)
  }, [])

  return [tab, setTab]
}

const PARTNER_NAMES: Record<Endpoint, string> = {
  thermostat: "the thermostat",
  thermostatSim: "the thermostat simulator",
  boiler: "the boiler",
  boilerSim: "the boiler simulator",
}

type Field = Searchable & {
  key: string
  /** The heading it is shown under; fields without one are shown without. */
  group?: string
  /** Takes the whole row. */
  wide?: boolean
  /** Why it does nothing with the other settings as they are, or undefined when it does. */
  inactiveWhy?: (values: Values) => string | undefined
} & (
    | { kind: "number"; unit: string; step: number }
    | { kind: "toggle"; description: string }
    // One of a few values. With `mask`, of a part of a setting that packs several
    // answers into one number (the boiler's configuration, id 3).
    | { kind: "choice"; mask?: number; options: { label: string; value: number }[] }
    // A part of such a setting, as a switch: on when the masked part equals `on`.
    | { kind: "part"; mask: number; on: number; description: string }
  )

const THERMOSTAT_FIELDS: Field[] = [
  { key: "tsim.room", label: "Room temperature", kind: "number", unit: "°C", step: 0.1, ids: [24] },
  { key: "tsim.setpoint", label: "Room setpoint", kind: "number", unit: "°C", step: 0.5, ids: [16] },
  { key: "tsim.flow", label: "Requested flow temperature", kind: "number", unit: "°C", step: 1, ids: [1], keywords: "control setpoint" },
  { key: "tsim.interval", label: "Interval between frames", kind: "number", unit: "ms", step: 100, keywords: "polling timing" },
  { key: "tsim.ch", label: "CH enable", kind: "toggle", description: "Ask for central heating", ids: [0], keywords: "status" },
  { key: "tsim.dhw", label: "DHW enable", kind: "toggle", description: "Ask for hot water", ids: [0], keywords: "status" },
  { key: "tsim.member", label: "Member id", kind: "number", unit: "", step: 1, ids: [2], keywords: "master config device" },
]

const boilerDecides = (values: Values) => Number(values["bsim.mode"] ?? 0) !== 0

const BOILER_FIELDS: Field[] = [
  // Who decides between heating and cooling: the thermostat, or the boiler forcing it.
  {
    key: "bsim.mode",
    group: "Heating or cooling",
    label: "Decided by",
    kind: "choice",
    wide: true,
    options: [
      { label: "The thermostat", value: 0 },
      { label: "Boiler: heating", value: 1 },
      { label: "Boiler: cooling", value: 2 },
      { label: "Boiler: off", value: 3 },
    ],
    ids: [0, 3],
    keywords: "mode force forced heat cool heating cooling hcc",
  },
  {
    key: "bsim.hold",
    group: "Heating or cooling",
    label: "Keep the setpoint",
    kind: "toggle",
    description: "Hold the old setpoint across a switch",
    ids: [9, 16],
    keywords: "override remote hysteresis mode change",
    inactiveWhy: (v) => (boilerDecides(v) ? undefined : "Only when the boiler decides"),
  },
  {
    key: "bsim.ch",
    group: "Heating or cooling",
    label: "Heating active",
    kind: "toggle",
    description: "Report heating active",
    ids: [0],
    keywords: "status ch central",
    inactiveWhy: (v) => (boilerDecides(v) ? "Set by the boiler's choice" : undefined),
  },
  {
    key: "bsim.cool",
    group: "Heating or cooling",
    label: "Cooling active",
    kind: "toggle",
    description: "Report cooling active",
    ids: [0],
    keywords: "status",
    inactiveWhy: (v) => (boilerDecides(v) ? "Set by the boiler's choice" : undefined),
  },
  { key: "bsim.water", group: "Boiler", label: "Water temperature", kind: "number", unit: "°C", step: 1, ids: [25], keywords: "boiler flow" },
  { key: "bsim.mod", group: "Boiler", label: "Modulation level", kind: "number", unit: "%", step: 1, ids: [17] },
  { key: "bsim.maxkw", group: "Boiler", label: "Maximum capacity", kind: "number", unit: "kW", step: 1, ids: [15], keywords: "power" },
  { key: "bsim.member", group: "Boiler", label: "Member id", kind: "number", unit: "", step: 1, ids: [3], keywords: "slave config device" },
  { key: "bsim.flame", group: "Boiler", label: "Flame", kind: "toggle", description: "Report the flame on", ids: [0], keywords: "status burner" },
  { key: "bsim.dhw", group: "Boiler", label: "DHW active", kind: "toggle", description: "Report hot water active", ids: [0], keywords: "status" },
  // What kind of boiler it is: the slave configuration (id 3), one control per answer.
  {
    key: "bsim.config",
    group: "Boiler type",
    label: "Hot water",
    kind: "choice",
    mask: 0x09,
    options: [
      { label: "None", value: 0x00 },
      { label: "Instant", value: 0x01 },
      { label: "Storage tank", value: 0x09 },
    ],
    ids: [3],
    keywords: "capability capabilities dhw combi slave config",
  },
  {
    key: "bsim.config",
    group: "Boiler type",
    label: "Burner",
    kind: "choice",
    mask: 0x02,
    options: [
      { label: "Modulating", value: 0x00 },
      { label: "On/off", value: 0x02 },
    ],
    ids: [3],
    keywords: "capability capabilities control type modulation slave config",
  },
  {
    key: "bsim.config",
    group: "Boiler type",
    label: "Cooling",
    kind: "part",
    mask: 0x04,
    on: 0x04,
    description: "Can also cool",
    ids: [3],
    keywords: "capability capabilities heat pump slave config",
  },
  {
    key: "bsim.config",
    group: "Boiler type",
    label: "Second heating circuit",
    kind: "part",
    mask: 0x20,
    on: 0x20,
    description: "Has a second circuit",
    ids: [3],
    keywords: "capability capabilities ch2 slave config",
  },
  {
    key: "bsim.config",
    group: "Boiler type",
    label: "Pump at low load",
    kind: "part",
    mask: 0x10,
    on: 0x00,
    description: "The thermostat may switch it off",
    ids: [3],
    keywords: "capability capabilities low-off pump control slave config",
  },
]

type Values = Record<string, SettingEntry["value"]>

export default function SimulatorsPage() {
  const [status, setStatus] = useState<SimStatus | null>(null)
  const [values, setValues] = useState<Values>({})
  const [query, setQuery] = useState("")
  const [tab, setTab] = useTab()

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
  // Shown at once rather than when the save answers, so a second click on the same
  // flags byte builds on the first instead of on the value from before it.
  const change = (key: string, value: string | boolean) => {
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

  const terms = searchTerms(query)
  const thermostat = filterSections(terms, {
    live: thermostatLiveRows(status?.thermostatSim),
    info: thermostatInfoRows(status?.thermostatSim, values),
    fields: THERMOSTAT_FIELDS,
  })
  const boilerMuted = status !== null && !status.boilerSim.running
  const boiler = filterSections(terms, {
    live: boilerLiveRows(status?.boilerSim),
    info: boilerInfoRows(values, boilerMuted),
    fields: BOILER_FIELDS,
  })
  const nothingMatches = terms.length > 0 && thermostat.count + boiler.count === 0
  const searching = terms.length > 0

  const hvacSimulated = Array.from({ length: 10 }, (_, i) => values[`hvac${i + 1}.type`]).filter(
    (v) => v !== undefined && hvacType(Number(v))?.simulated,
  ).length

  return (
    <div className="flex flex-col gap-6">
      <div>
        <h1 className="text-3xl font-bold">Simulators</h1>
        <p className="text-muted-foreground mt-1">
          The thermostat and boiler the testbench simulates on OpenTherm, and the HVAC units on RS485.
          Connect the OpenTherm ones on the Connections page; a setting here applies with the next frame.
        </p>
      </div>

      <div role="tablist" aria-label="Simulator" className="flex overflow-x-auto border-b">
        <TabButton
          id="thermostat"
          selected={tab === "thermostat"}
          onSelect={setTab}
          icon={ThermometerIcon}
          iconColor="text-blue-500"
          label="Thermostat simulator"
          state={simState(status?.thermostatSim.running)}
          matches={searching ? thermostat.count : undefined}
        />
        <TabButton
          id="boiler"
          selected={tab === "boiler"}
          onSelect={setTab}
          icon={FlameIcon}
          iconColor="text-orange-500"
          label="Boiler simulator"
          state={simState(status?.boilerSim.running)}
          matches={searching ? boiler.count : undefined}
        />
        <TabButton
          id="hvac"
          selected={tab === "hvac"}
          onSelect={setTab}
          icon={AirVentIcon}
          iconColor="text-orange-500"
          label="HVAC simulators"
          state={
            values["hvac1.type"] === undefined
              ? { on: false, text: "…" }
              : { on: hvacSimulated > 0, text: hvacSimulated > 0 ? `${hvacSimulated} simulated` : "None set up" }
          }
        />
      </div>

      <div role="tabpanel" id={`sim-tab-${tab}`} aria-labelledby={`sim-tab-${tab}-button`} className="flex flex-col gap-6">
        {tab === "hvac" ? (
          <HvacSimulators values={values} onSetting={change} />
        ) : (
          <>
            <SearchBox query={query} onChange={setQuery} matches={searching ? thermostat.count + boiler.count : null} />

            {nothingMatches && <NoMatches query={query} onClear={() => setQuery("")} />}

            {tab === "thermostat" ? (
              <SimulatorCard
                title="Thermostat simulator"
                subtitle="An OpenTherm master: asks, every interval"
                icon={ThermometerIcon}
                color="blue"
                running={status?.thermostatSim.running}
                partner={status?.thermostatSim.partner}
                liveTitle="What the boiler reports (live)"
                sections={thermostat}
                searching={searching}
                sayWhenEmpty={!nothingMatches}
                values={values}
                onChange={change}
                note="The thermostat simulator sends one request per interval, built from these settings, to whichever boiler it is connected to."
              />
            ) : (
              <SimulatorCard
                title="Boiler simulator"
                subtitle="An OpenTherm slave: answers every request"
                icon={FlameIcon}
                color="orange"
                running={status?.boilerSim.running}
                partner={status?.boilerSim.partner}
                liveTitle="What the thermostat asks (live)"
                sections={boiler}
                searching={searching}
                sayWhenEmpty={!nothingMatches}
                values={values}
                onChange={change}
                note="The boiler simulator answers every request from the thermostat it is connected to with these settings; what it does not know it answers Unknown-DataId."
              />
            )}
          </>
        )}
      </div>
    </div>
  )
}

// ── Tabs ────────────────────────────────────────────────────────

interface TabState {
  on: boolean
  text: string
}

function simState(running?: boolean): TabState {
  if (running === undefined) return { on: false, text: "…" }
  return running ? { on: true, text: "Running" } : { on: false, text: "Not connected" }
}

function TabButton({
  id,
  selected,
  onSelect,
  icon: Icon,
  iconColor,
  label,
  state,
  matches,
}: {
  id: Tab
  selected: boolean
  onSelect: (tab: Tab) => void
  icon: ComponentType<{ className?: string }>
  iconColor: string
  label: string
  state: TabState
  /** While searching: how many rows and settings in it match. */
  matches?: number
}) {
  return (
    <button
      type="button"
      role="tab"
      id={`sim-tab-${id}-button`}
      aria-selected={selected}
      aria-controls={`sim-tab-${id}`}
      onClick={() => onSelect(id)}
      className={`-mb-px flex shrink-0 items-center gap-3 border-b-2 px-4 py-2.5 text-left whitespace-nowrap transition-colors ${
        selected ? "border-foreground" : "text-muted-foreground hover:text-foreground border-transparent"
      }`}
    >
      <Icon className={`size-5 shrink-0 ${state.on ? iconColor : ""}`} />
      <span className="flex flex-col">
        <span className="flex items-center gap-2 text-sm font-medium">
          {label}
          {matches !== undefined && (
            <span className={`rounded-full px-1.5 text-xs ${matches > 0 ? "bg-foreground text-background" : "bg-muted text-muted-foreground"}`}>
              {matches}
            </span>
          )}
        </span>
        <span className="text-muted-foreground flex items-center gap-1.5 text-xs font-normal">
          <span className={`size-1.5 rounded-full ${state.on ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
          {state.text}
        </span>
      </span>
    </button>
  )
}

// ── Search ──────────────────────────────────────────────────────

interface Sections {
  live: RowDef[]
  info: RowDef[]
  fields: Field[]
  /** How many rows and settings are left in all three. */
  count: number
}

function filterSections(terms: string[], all: Omit<Sections, "count">): Sections {
  const live = all.live.filter((r) => matchesSearch(r, terms))
  const info = all.info.filter((r) => matchesSearch(r, terms))
  const fields = all.fields.filter((f) => matchesSearch(f, terms))
  return { live, info, fields, count: live.length + info.length + fields.length }
}

function SearchBox({
  query,
  onChange,
  matches,
}: {
  query: string
  onChange: (query: string) => void
  matches: number | null
}) {
  return (
    <div className="flex items-center gap-3">
      <div className="relative w-full max-w-md">
        <SearchIcon className="text-muted-foreground pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2" />
        <Input
          type="search"
          aria-label="Search the simulators"
          placeholder="Search settings, capabilities or data ids (e.g. ID 25)"
          value={query}
          onChange={(e) => onChange(e.target.value)}
          onKeyDown={(e) => e.key === "Escape" && onChange("")}
          className="pr-9 pl-9 [&::-webkit-search-cancel-button]:hidden"
        />
        {query && (
          <button
            type="button"
            aria-label="Clear the search"
            onClick={() => onChange("")}
            className="text-muted-foreground hover:text-foreground absolute top-1/2 right-2.5 -translate-y-1/2"
          >
            <XIcon className="size-4" />
          </button>
        )}
      </div>
      {matches !== null && (
        <span className="text-muted-foreground text-sm">
          {matches} {matches === 1 ? "match" : "matches"}
        </span>
      )}
    </div>
  )
}

function NoMatches({ query, onClear }: { query: string; onClear: () => void }) {
  return (
    <div className="flex flex-col items-center gap-2 rounded-xl border border-dashed p-8 text-center">
      <SearchXIcon className="text-muted-foreground size-8" />
      <p className="font-medium">Nothing in either simulator matches “{query.trim()}”</p>
      <p className="text-muted-foreground text-sm">
        Try a setting such as <em>water</em> or <em>setpoint</em>, a capability such as <em>modulation</em>,
        or a data id such as <em>ID 25</em>.
      </p>
      <Button variant="outline" size="sm" className="mt-2" onClick={onClear}>
        Clear search
      </Button>
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
  // A simulator that is not connected: the same card, without its colour.
  muted: {
    header: "bg-muted/60",
    icon: "text-muted-foreground",
    note: "bg-muted/60 text-muted-foreground",
  },
} as const

function SimulatorCard({
  title,
  subtitle,
  icon: Icon,
  color,
  running,
  partner,
  liveTitle,
  sections,
  searching,
  sayWhenEmpty,
  values,
  onChange,
  note,
}: {
  title: string
  subtitle: string
  icon: ComponentType<{ className?: string }>
  color: "blue" | "orange"
  running?: boolean
  partner?: Endpoint
  liveTitle: string
  sections: Sections
  searching: boolean
  /** Say so when nothing here matches; not when the page already says nothing does. */
  sayWhenEmpty: boolean
  values: Values
  onChange: (key: string, value: string | boolean) => void
  note: string
}) {
  // Gray only once it is known to be disconnected, not while the first status loads.
  const muted = running === false
  const c = COLORS[muted ? "muted" : color]
  const { live, info, fields } = sections

  return (
    <Card className={`gap-0 overflow-hidden py-0 transition-colors ${muted ? "bg-muted/20" : ""}`}>
      <div className={`flex items-center gap-4 border-b p-4 transition-colors ${c.header}`}>
        <div className="bg-background flex size-12 shrink-0 items-center justify-center rounded-lg border shadow-sm">
          <Icon className={`size-6 ${c.icon}`} />
        </div>
        <div className="min-w-0 flex-1">
          <div className={`text-lg font-semibold ${muted ? "text-muted-foreground" : ""}`}>{title}</div>
          <div className="text-muted-foreground text-sm">{subtitle}</div>
        </div>
        <ConnectionStatus running={running} partner={partner} />
      </div>

      {(live.length > 0 || info.length > 0) && (
        <div className="grid gap-4 p-4 md:grid-cols-2">
          {live.length > 0 && <Panel title={liveTitle} rows={live} />}
          {info.length > 0 && <Panel title="Device info" rows={info} />}
        </div>
      )}

      {fields.length > 0 && (
        <div className={`p-4 ${live.length > 0 || info.length > 0 ? "border-t" : ""}`}>
          <h3 className="mb-3 font-medium">Simulation settings</h3>
          <div className="flex flex-col gap-5">
            {groupFields(fields).map(([group, inGroup]) => (
              <div key={group ?? ""}>
                {group && (
                  <h4 className="text-muted-foreground mb-2 text-xs font-medium tracking-wide uppercase">{group}</h4>
                )}
                <div className="grid gap-x-6 gap-y-4 sm:grid-cols-2">
                  {inGroup.map((f) => (
                    <SettingField
                      key={`${f.key}:${f.label}`}
                      field={f}
                      value={values[f.key]}
                      inactive={f.inactiveWhy?.(values)}
                      onChange={onChange}
                    />
                  ))}
                </div>
              </div>
            ))}
          </div>
          {!searching && (
            <div className={`mt-4 flex items-start gap-2 rounded-lg p-3 text-sm ${c.note}`}>
              <InfoIcon className="mt-0.5 size-4 shrink-0" />
              <p>{note}</p>
            </div>
          )}
        </div>
      )}

      {searching && sayWhenEmpty && sections.count === 0 && (
        <p className="text-muted-foreground p-4 text-sm">Nothing in this simulator matches.</p>
      )}
    </Card>
  )
}

function ConnectionStatus({ running, partner }: { running?: boolean; partner?: Endpoint }) {
  if (running === undefined) return <span className="text-muted-foreground text-sm">…</span>
  return (
    <div className="text-right text-sm">
      <div className="flex items-center justify-end gap-1.5">
        <span className={`size-2.5 rounded-full ${running ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
        <span className={running ? "text-emerald-700 dark:text-emerald-400" : "text-muted-foreground font-medium"}>
          {running ? "Running" : "Not connected"}
        </span>
      </div>
      <div className="text-muted-foreground text-xs">
        {running && partner ? `with ${PARTNER_NAMES[partner]}` : "Connect it on the Connections page"}
      </div>
    </div>
  )
}

/** The fields under their headings, in the order the headings first appear. */
function groupFields(fields: Field[]): [string | undefined, Field[]][] {
  const groups = new Map<string | undefined, Field[]>()
  for (const f of fields) groups.set(f.group, [...(groups.get(f.group) ?? []), f])
  return [...groups]
}

function SettingField({
  field,
  value,
  inactive,
  onChange,
}: {
  field: Field
  value: SettingEntry["value"] | undefined
  /** Why it has no effect right now: shown instead of its description, and it cannot be changed. */
  inactive?: string
  onChange: (key: string, value: string | boolean) => void
}) {
  if (field.kind === "number") return <NumberField field={field} value={value} onChange={onChange} />

  const n = value === undefined ? undefined : Number(value)
  const disabled = value === undefined || inactive !== undefined
  let control: ReactNode
  if (field.kind === "toggle" || field.kind === "part") {
    const on = field.kind === "toggle" ? value === true : n !== undefined && (n & field.mask) === field.on
    const set = (to: boolean) => {
      if (field.kind === "toggle") onChange(field.key, to)
      else if (n !== undefined)
        onChange(field.key, String((n & ~field.mask) | (to ? field.on : field.on ^ field.mask)))
    }
    control = (
      <label className="flex items-center gap-2 text-sm">
        <Switch checked={on} disabled={disabled} onCheckedChange={set} />
        <span className="text-muted-foreground">{inactive ?? field.description}</span>
      </label>
    )
  } else {
    const mask = field.mask ?? ~0
    control = (
      <>
        <Segmented
          label={field.label}
          options={field.options.map((o) => o.label)}
          selected={n === undefined ? undefined : field.options.findIndex((o) => (n & mask) === o.value)}
          disabled={disabled}
          onSelect={(i) => n !== undefined && onChange(field.key, String((n & ~mask) | field.options[i].value))}
        />
        {inactive && <span className="text-muted-foreground text-sm">{inactive}</span>}
      </>
    )
  }

  return (
    <div className={`flex flex-col gap-1.5 ${field.wide ? "sm:col-span-2" : ""}`}>
      <span className={`text-sm font-medium ${inactive ? "text-muted-foreground" : ""}`}>{field.label}</span>
      {control}
    </div>
  )
}

/** A row of buttons of which one is chosen. */
function Segmented({
  label,
  options,
  selected,
  disabled,
  onSelect,
}: {
  label: string
  options: string[]
  selected?: number
  disabled?: boolean
  onSelect: (index: number) => void
}) {
  return (
    <div role="radiogroup" aria-label={label} className="bg-muted flex w-full flex-wrap rounded-lg p-0.5">
      {options.map((o, i) => (
        <button
          key={o}
          type="button"
          role="radio"
          aria-checked={selected === i}
          disabled={disabled}
          onClick={() => selected !== i && onSelect(i)}
          className={`flex-1 rounded-md px-3 py-1.5 text-sm whitespace-nowrap transition-colors disabled:opacity-50 ${
            selected === i ? "bg-background font-medium shadow-sm" : "text-muted-foreground hover:text-foreground"
          }`}
        >
          {o}
        </button>
      ))}
    </div>
  )
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

/** One row of a panel, searchable by its label, data ids and keywords. */
type RowDef = Searchable & { value: ReactNode }

function Panel({ title, rows }: { title: string; rows: RowDef[] }) {
  return (
    <div className="bg-card rounded-xl border">
      <div className="border-b px-4 py-2.5 text-sm font-medium">{title}</div>
      <dl className="divide-y px-4 text-sm">
        {rows.map((r) => (
          <div key={r.label} className="flex items-center justify-between gap-4 py-2">
            <dt className="text-muted-foreground">{r.label}</dt>
            <dd className="text-right">{r.value}</dd>
          </div>
        ))}
      </dl>
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

function memberId(value: SettingEntry["value"] | number | undefined): ReactNode {
  return value === undefined ? <Unknown /> : formatMemberId(Number(value))
}

function thermostatLiveRows(status?: SimStatus["thermostatSim"]): RowDef[] {
  const b = status?.boiler ?? {}
  return [
    { label: "Mode", ids: [0], keywords: "status heating hot water dhw ch", value: mode(b.ch, b.dhw) },
    { label: "Flame", ids: [0], keywords: "status burner", value: <OnOff on={b.flame} /> },
    { label: "Water temperature", ids: [25], value: <Num value={b.waterTemp} unit="°C" /> },
    { label: "Modulation level", ids: [17], value: <Num value={b.modulation} unit="%" digits={0} /> },
    { label: "Fault", ids: [0, 5], keywords: "status error", value: <OnOff on={b.fault} /> },
    { label: "Last answer", keywords: "response", value: <Ago ms={status?.lastResponseAgoMs} /> },
    {
      label: "Answered",
      keywords: "responses requests",
      value: status ? `${status.responses} of ${status.requests}` : <Unknown />,
    },
  ]
}

function thermostatInfoRows(status: SimStatus["thermostatSim"] | undefined, values: Values): RowDef[] {
  const b = status?.boiler ?? {}
  return [
    { label: "Member id", ids: [2], keywords: "master config device", value: memberId(values["tsim.member"]) },
    { label: "OpenTherm version", ids: [124], keywords: "protocol", value: "2.2" },
    { label: "Boiler member id", ids: [3], keywords: "slave config device", value: memberId(b.memberId) },
    { label: "Boiler OpenTherm version", ids: [125], keywords: "protocol", value: <Num value={b.otVersion} unit="" /> },
    { label: "Unknown data ids", keywords: "unknown-dataid", value: status ? status.unknownIds : <Unknown /> },
    { label: "Unanswered", keywords: "timeout requests", value: status ? status.unanswered : <Unknown /> },
  ]
}

function boilerLiveRows(status?: SimStatus["boilerSim"]): RowDef[] {
  const t = status?.thermostat ?? {}
  return [
    { label: "CH enable", ids: [0], keywords: "status central heating", value: <OnOff on={t.ch} /> },
    { label: "Cooling enable", ids: [0], keywords: "status mode", value: <OnOff on={t.cooling} /> },
    { label: "DHW enable", ids: [0], keywords: "status hot water", value: <OnOff on={t.dhw} /> },
    {
      label: "Flow temperature (requested)",
      ids: [1],
      keywords: "control setpoint",
      value: <Num value={t.controlSetpoint} unit="°C" />,
    },
    { label: "Cooling level (requested)", ids: [7], keywords: "cooling control signal", value: <Num value={t.coolingControl} unit="%" digits={0} /> },
    { label: "Room setpoint", ids: [16], value: <Num value={t.roomSetpoint} unit="°C" /> },
    {
      label: "Holding setpoint",
      ids: [9],
      keywords: "override remote mode change keep",
      value:
        status?.holding === undefined ? (
          <Unknown />
        ) : (
          <span>
            <span className="font-mono">{status.holding.toFixed(1)} °C</span>
            <span className="text-muted-foreground"> · {status.holdReadsLeft} more</span>
          </span>
        ),
    },
    { label: "Room temperature", ids: [24], value: <Num value={t.roomTemp} unit="°C" /> },
    { label: "Max modulation", ids: [14], value: <Num value={t.maxModulation} unit="%" digits={0} /> },
    { label: "Last request", keywords: "requests", value: <Ago ms={status?.lastRequestAgoMs} /> },
    {
      label: "Boiler type last asked",
      ids: [3],
      keywords: "slave config capabilities requests",
      value: <Ago ms={status?.configReadAgoMs} />,
    },
  ]
}

function boilerInfoRows(values: Values, muted: boolean): RowDef[] {
  const config = values["bsim.config"] === undefined ? undefined : Number(values["bsim.config"])
  const has = (bit: number) => config !== undefined && (config & (1 << bit)) !== 0
  const capabilities = (
    config === undefined
      ? []
      : [has(0) && "Hot water", has(1) ? "On/off" : "Modulation", has(2) && "Cooling", has(3) && "Storage tank", has(5) && "CH2"]
  ).filter(Boolean) as string[]
  const chip = muted
    ? "bg-muted text-muted-foreground"
    : "bg-orange-100 text-orange-800 dark:bg-orange-950 dark:text-orange-300"
  return [
    { label: "Member id", ids: [3], keywords: "slave config device", value: memberId(values["bsim.member"]) },
    { label: "OpenTherm version", ids: [125], keywords: "protocol", value: "2.2" },
    {
      label: "Maximum capacity",
      ids: [15],
      keywords: "power kw",
      value: values["bsim.maxkw"] !== undefined ? `${values["bsim.maxkw"]} kW` : <Unknown />,
    },
    {
      label: "Capabilities",
      ids: [3],
      keywords: `capability ${capabilities.join(" ")} dhw modulating`,
      value: (
        <span className="flex flex-wrap justify-end gap-1">
          {capabilities.map((cap) => (
            <span key={cap} className={`rounded-md px-2 py-0.5 text-xs ${chip}`}>
              {cap}
            </span>
          ))}
        </span>
      ),
    },
  ]
}
