import { useEffect, useRef, useState } from "react"
import { backend, NUMERIC_SETTING_TYPES, type SettingEntry, type WifiNetwork } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { SaveIcon, Undo2Icon, PowerIcon, SearchIcon, LockIcon, BracesIcon, ChevronDownIcon } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Input } from "@/components/ui/input"
import { Card, CardContent, CardHeader } from "@/components/ui/card"
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogFooter } from "@/components/ui/dialog"
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from "@/components/ui/alert-dialog"
import { Switch } from "@/components/ui/switch"
import { toast } from "sonner"
import Editor from "react-simple-code-editor"
import Prism from "prismjs"
import "prismjs/components/prism-json"
import "prismjs/themes/prism-tomorrow.css"

function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : "Unknown error"
}

type SettingGroup = {
  id: string
  label: string
  items: SettingEntry[]
}

// How settings are PRESENTED, which is not how they are stored.
//
// The device namespaces by the manager that owns a setting — `telem.*` belongs to
// TelemetryManager, `ntp.*` to TimeManager — and that is right on the device,
// where a namespace says who is responsible for a value. It is the wrong axis for
// this page: someone looking for "how often does it report, and against which
// clock" does not care that two managers answer, and eight cards of one or two
// rows each made them hunt. So a card is a PURPOSE, and this table is the whole
// mapping: one row per card in display order, listing the key prefixes it gathers.
//
// Nothing about storage or addressing moves. A setting is still registered, read
// and written by its own key — this is a lens over the same list.
const UI_GROUPS: { id: string; label: string; prefixes: string[] }[] = [
  { id: "general", label: "General", prefixes: ["device", "led"] },
  { id: "relay", label: "Relay", prefixes: ["relay"] },
  { id: "web", label: "Web", prefixes: ["web"] },
  { id: "telemetry", label: "Telemetry & Time", prefixes: ["telem", "ntp"] },
  { id: "network", label: "Network", prefixes: ["wifi", "net"] },
]

// A prefix no row above claims still has to appear. A fork registering `foo.bar`,
// or a framework manager gaining a setting before this table catches up, must end
// up visible and editable rather than quietly absent — a settings page that hides
// a setting is worse than one with a plainly named card on the end.
const OTHER_GROUP = { id: "other", label: "Other" }

function prefixOf(key: string): string {
  const dot = key.indexOf(".")
  return dot > 0 ? key.slice(0, dot) : key
}

function groupSettings(settings: SettingEntry[]): SettingGroup[] {
  const owner = new Map<string, string>()
  for (const g of UI_GROUPS) for (const p of g.prefixes) owner.set(p, g.id)

  const byGroup = new Map<string, SettingEntry[]>()
  for (const s of settings) {
    const id = owner.get(prefixOf(s.key)) ?? OTHER_GROUP.id
    if (!byGroup.has(id)) byGroup.set(id, [])
    byGroup.get(id)!.push(s)
  }

  const groups: SettingGroup[] = []
  for (const g of UI_GROUPS) {
    const items = byGroup.get(g.id)
    if (!items || items.length === 0) continue

    // Rows follow the order their prefixes are listed above, and the device's own
    // order within a prefix — so General reads Device Name then LED, and because
    // the sort is stable each prefix's own block is left alone.
    const rank = new Map(g.prefixes.map((p, i) => [p, i]))
    items.sort((a, b) => (rank.get(prefixOf(a.key)) ?? 0) - (rank.get(prefixOf(b.key)) ?? 0))

    groups.push({ id: g.id, label: g.label, items })
  }

  const rest = byGroup.get(OTHER_GROUP.id)
  if (rest && rest.length > 0) groups.push({ ...OTHER_GROUP, items: rest })

  return groups
}

// ── Column packing ───────────────────────────────────────────
//
// A `grid-cols-2` put both columns on shared rows, so a short card sat beside a
// tall one and the row grew to the taller of the two — the gap under Net was the
// height of Relay. The columns have to flow independently.
//
// `columns-2` is the CSS answer and it is one class, but a multi-column box
// clips absolutely positioned descendants to the column, and this page has
// one: the WiFi scan dropdown. So the split is done here and each column is an
// ordinary flex stack, which also leaves `position: absolute` behaving normally.
//
// Cards are handed out as a PREFIX, not round-robin: the left column takes
// groups until it is the taller half, then everything else goes right. That
// keeps document order, so stacking the two columns on a narrow screen gives
// back exactly the device's own ordering with no reshuffle.
function packColumns(groups: SettingGroup[]): [SettingGroup[], SettingGroup[]] {
  // Rows dominate the height of a card; the 1 is its header.
  const weight = (g: SettingGroup) => 1 + g.items.length
  const total = groups.reduce((n, g) => n + weight(g), 0)

  const left: SettingGroup[] = []
  const right: SettingGroup[] = []
  let filled = 0
  let filling = true

  for (const g of groups) {
    const w = weight(g)
    // Straddle the midpoint rather than stopping short of it, so the card that
    // crosses half lands wherever it leaves the two columns closest in height.
    if (filling && filled + w / 2 <= total / 2) {
      left.push(g)
      filled += w
    } else {
      // Once the split has happened it must not reopen, or a later small group
      // would jump back to the left and break document order.
      filling = false
      right.push(g)
    }
  }

  // One very tall first card can clear the midpoint on its own and leave the
  // left column empty; it still belongs on the left.
  if (left.length === 0 && right.length > 0) left.push(right.shift()!)

  return [left, right]
}

// ── Category filter row ──────────────────────────────────────
//
// This replaces the old right-hand "On this page" list. That column cost ~12rem
// of width to do nothing but scroll the page it sat beside; a chip row costs one
// line and can *filter*, which is the cheaper way to stop scrolling altogether.

function CategoryFilter({
  groups,
  active,
  onSelect,
}: {
  groups: SettingGroup[]
  active: string | null
  onSelect: (prefix: string | null) => void
}) {
  const total = groups.reduce((n, g) => n + g.items.length, 0)

  return (
    <div className="flex min-w-0 flex-wrap items-center gap-1">
      <Button
        size="xs"
        variant={active === null ? "default" : "ghost"}
        onClick={() => onSelect(null)}
      >
        All
        <span className="ml-1 tabular-nums opacity-60">{total}</span>
      </Button>
      {groups.map((g) => (
        <Button
          key={g.id}
          size="xs"
          variant={active === g.id ? "default" : "ghost"}
          onClick={() => onSelect(active === g.id ? null : g.id)}
        >
          {g.label}
          <span className="ml-1 tabular-nums opacity-60">{g.items.length}</span>
        </Button>
      ))}
    </div>
  )
}

export default function SettingsPage() {
  const connection = useConnectionStatus()
  const [settings, setSettings] = useState<SettingEntry[]>([])
  const [dirty, setDirty] = useState(false)
  const [saving, setSaving] = useState(false)
  const [category, setCategory] = useState<string | null>(null)
  const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(new Set())
  const [search, setSearch] = useState("")
  const [jsonOpen, setJsonOpen] = useState(false)
  const [jsonText, setJsonText] = useState("")
  const [jsonError, setJsonError] = useState("")

  useEffect(() => {
    if (connection !== "connected") return
    backend.getSettings().then((r) => {
      setSettings(r.settings)
      setDirty(false)
    }).catch((e) => toast.error("Failed to load settings", { description: errorMessage(e) }))
  }, [connection])

  async function handleChange(key: string, value: string) {
    try {
      await backend.setSetting(key, value)
      // Only mirror the value locally once the device accepted it — a
      // failed write must not leave the UI claiming a change it never made.
      setSettings((prev) =>
        prev.map((s) =>
          s.key === key
            ? { ...s, value: NUMERIC_SETTING_TYPES.includes(s.type) ? Number(value) : s.type === "bool" ? value === "true" : value }
            : s,
        ),
      )
      setDirty(true)
    } catch (e) {
      toast.error(`Failed to update ${key}`, { description: errorMessage(e) })
    }
  }

  async function handleSave() {
    setSaving(true)
    try {
      await backend.saveSettings()
      setDirty(false)
      toast.success("Settings saved")
    } catch (e) {
      toast.error("Failed to save settings", { description: errorMessage(e) })
    }
    setSaving(false)
  }

  function handleReboot() {
    backend.reboot()
      .then(() => toast.info("Rebooting device…", { description: "The connection will drop for a few seconds." }))
      .catch((e) => toast.error("Reboot command failed", { description: errorMessage(e) }))
  }

  async function handleReload() {
    try {
      const r = await backend.getSettings()
      setSettings(r.settings)
      setDirty(false)
    } catch (e) {
      toast.error("Failed to reload settings", { description: errorMessage(e) })
    }
  }

  function openJsonEditor() {
    const obj: Record<string, unknown> = {}
    for (const s of settings) obj[s.key] = s.value
    setJsonText(JSON.stringify(obj, null, 2))
    setJsonError("")
    setJsonOpen(true)
  }

  async function applyJson() {
    try {
      const obj = JSON.parse(jsonText) as Record<string, unknown>
      for (const [key, value] of Object.entries(obj)) {
        await handleChange(key, String(value))
      }
      setJsonOpen(false)
    } catch {
      setJsonError("Invalid JSON — fix the syntax and try again.")
    }
  }

  const groups = groupSettings(settings)

  // Search and category compose: the chips narrow to one group, the search box
  // narrows within whatever is showing.
  const needle = search.trim().toLowerCase()
  const visibleGroups = groups
    .filter((g) => category === null || g.id === category)
    .map((g) =>
      needle
        ? {
            ...g,
            items: g.items.filter(
              (s) => s.label.toLowerCase().includes(needle) || s.key.toLowerCase().includes(needle),
            ),
          }
        : g,
    )
    .filter((g) => g.items.length > 0)

  const [leftColumn, rightColumn] = packColumns(visibleGroups)

  function toggleCollapsed(id: string) {
    setCollapsed((prev) => {
      const next = new Set(prev)
      if (!next.delete(id)) next.add(id)
      return next
    })
  }

  function renderColumn(column: SettingGroup[]) {
    return (
      <div className="flex min-w-0 flex-1 flex-col gap-3">
        {column.map((group) => (
          <GroupCard
            key={group.id}
            group={group}
            collapsed={collapsed.has(group.id)}
            onToggle={() => toggleCollapsed(group.id)}
            onChange={handleChange}
          />
        ))}
      </div>
    )
  }

  return (
    <div className="flex h-full w-full flex-col">
      {/* Toolbar — title, state, actions; then search + category chips */}
      <div className="shrink-0 space-y-2 pb-3">
        <div className="flex items-center gap-2">
          <div className="min-w-0">
            <h1 className="text-xl font-bold leading-tight">Settings</h1>
            <p className="truncate text-sm text-muted-foreground">Configure your device</p>
          </div>
          {dirty && (
            <span className="rounded-md bg-amber-500/10 px-2 py-0.5 text-xs font-medium text-amber-500">
              Unsaved
            </span>
          )}
          <div className="ml-auto flex items-center gap-1.5">
            <AlertDialog>
              <AlertDialogTrigger asChild>
                <Button variant="outline" size="sm" className="text-destructive hover:text-destructive">
                  <PowerIcon />
                  <span className="hidden sm:inline">Reboot</span>
                </Button>
              </AlertDialogTrigger>
              <AlertDialogContent>
                <AlertDialogHeader>
                  <AlertDialogTitle>Reboot the device?</AlertDialogTitle>
                  <AlertDialogDescription>
                    The device restarts and drops this connection for a few seconds.
                    {dirty && " Unsaved settings changes will be lost."}
                  </AlertDialogDescription>
                </AlertDialogHeader>
                <AlertDialogFooter>
                  <AlertDialogCancel>Cancel</AlertDialogCancel>
                  <AlertDialogAction onClick={handleReboot}>Reboot</AlertDialogAction>
                </AlertDialogFooter>
              </AlertDialogContent>
            </AlertDialog>
            <Button variant="outline" size="sm" onClick={openJsonEditor}>
              <BracesIcon />
              <span className="hidden sm:inline">JSON</span>
            </Button>
            <Button variant="outline" size="sm" onClick={handleReload}>
              <Undo2Icon />
              <span className="hidden sm:inline">Undo</span>
            </Button>
            <Button size="sm" onClick={handleSave} disabled={!dirty || saving}>
              <SaveIcon />
              {saving ? "Saving..." : "Save"}
            </Button>
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
          <div className="relative w-full sm:w-52">
            <SearchIcon className="pointer-events-none absolute left-2.5 top-1/2 size-3.5 -translate-y-1/2 text-muted-foreground" />
            <Input
              className="h-7 pl-8 text-[13px]"
              placeholder="Search settings…"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
            />
          </div>
          {groups.length > 0 && (
            <CategoryFilter groups={groups} active={category} onSelect={setCategory} />
          )}
        </div>
      </div>

      {/* Groups — two columns of compact cards on desktop, one on narrow */}
      <div className="min-h-0 flex-1 overflow-y-auto">
        {settings.length === 0 ? (
          <Card size="sm">
            <CardContent className="text-sm text-muted-foreground">Loading…</CardContent>
          </Card>
        ) : visibleGroups.length === 0 ? (
          <p className="text-sm text-muted-foreground">
            No settings match {needle ? `"${search}"` : "this filter"}.
          </p>
        ) : (
          // Two independent stacks side by side, one above the other on narrow
          // screens — where the prefix split means they read in device order.
          <div className="flex flex-col gap-3 pb-6 lg:flex-row lg:items-start">
            {renderColumn(leftColumn)}
            {renderColumn(rightColumn)}
          </div>
        )}
      </div>

      {/* JSON editor modal */}
      <Dialog open={jsonOpen} onOpenChange={setJsonOpen}>
        <DialogContent className="flex h-[80vh] flex-col sm:max-w-5xl">
          <DialogHeader>
            <DialogTitle>Edit Settings as JSON</DialogTitle>
          </DialogHeader>

          <div className="min-h-0 flex-1 overflow-auto rounded-lg border bg-neutral-950 font-mono text-sm">
            <Editor
              value={jsonText}
              onValueChange={(v) => { setJsonText(v); setJsonError("") }}
              highlight={(code) => Prism.highlight(code, Prism.languages.json, "json")}
              padding={16}
              style={{ minHeight: "100%" }}
            />
          </div>

          {jsonError && <p className="text-sm text-destructive">{jsonError}</p>}

          <DialogFooter>
            <Button variant="outline" onClick={() => setJsonOpen(false)}>Cancel</Button>
            <Button onClick={applyJson}>Apply</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  )
}


// ── Group card ───────────────────────────────────────────────

function GroupCard({
  group,
  collapsed,
  onToggle,
  onChange,
}: {
  group: SettingGroup
  collapsed: boolean
  onToggle: () => void
  onChange: (key: string, value: string) => void
}) {
  return (
    // The boundary is a real BORDER, and it has to be. Card's own edge is
    // `ring-1`, which is a box-shadow: its spread does not participate in layout
    // and so is not snapped to the device pixel grid. Under Windows display
    // scaling a 1px ring anti-aliases to a fraction of a pixel and disappears —
    // visibly so over the white rows, where the header's tint was left doing the
    // whole job of suggesting an edge. A border is laid out, so it snaps and
    // stays crisp. `ring-0` retires the inherited one rather than stacking.
    //
    // The cost is one pixel of radius: a border insets the padding box, so the
    // card's inner curve is a pixel tighter than its outer one, and a header
    // asking for the outer radius would bleed its tint past the curve. It asks
    // for `--radius-xl` minus that pixel, derived rather than hardcoded so the
    // two stay in step. That keeps `overflow-hidden` unnecessary — and it has to stay
    // unnecessary, because it is the property that clips the WiFi scan dropdown
    // out of existence.
    <Card
      size="sm"
      className="gap-0 overflow-visible rounded-xl border border-border py-0 shadow-sm ring-0"
    >
      <CardHeader className="gap-0 rounded-t-[calc(var(--radius-xl)_-_1px)] border-b bg-muted/40 p-0">
        <button
          type="button"
          onClick={onToggle}
          aria-expanded={!collapsed}
          title={collapsed ? `Expand ${group.label}` : `Collapse ${group.label}`}
          className="flex w-full items-center gap-1.5 rounded-t-[calc(var(--radius-xl)_-_1px)] px-3 py-2 text-left"
        >
          {/* Desktop has no chevron; the affordance appears where collapsing is
              what makes one tall column usable. */}
          <ChevronDownIcon
            className={`size-3.5 shrink-0 text-muted-foreground transition-transform lg:hidden ${
              collapsed ? "-rotate-90" : ""
            }`}
          />
          <span className="truncate text-sm font-semibold">{group.label}</span>
        </button>
      </CardHeader>
      {!collapsed && (
        <CardContent className="px-0">
          <ul className="divide-y">
            {group.items.map((setting) => (
              <SettingRow
                key={setting.key}
                setting={setting}
                onChange={(value) => onChange(setting.key, value)}
              />
            ))}
          </ul>
        </CardContent>
      )}
    </Card>
  )
}

// ── Setting row ──────────────────────────────────────────────

const sensitiveKeys = ["password", "pass"]

function isSensitive(key: string): boolean {
  const field = key.split(".").pop() ?? ""
  return sensitiveKeys.includes(field)
}

// Every control sits in this fixed-width well, so the right edge of a card is a
// single line whatever the mix of switches, inputs and the SSID picker above it.
// It is the widest thing a row can afford: the label beside it truncates, and a
// URL or an SSID is the value most worth reading in full.
const CONTROL_WELL = "flex w-44 shrink-0 items-center justify-end sm:w-64"

function SettingRow({
  setting,
  onChange,
}: {
  setting: SettingEntry
  onChange: (value: string) => void
}) {
  const isWifiSsid = setting.key === "wifi.ssid"
  const isPassword = setting.type === "string" && isSensitive(setting.key)

  return (
    <li className="flex items-center justify-between gap-3 px-3 py-1.5">
      <div className="min-w-0">
        <div className="truncate text-[13px] font-medium leading-tight">{setting.label}</div>
        <div className="truncate font-mono text-[10px] leading-tight text-muted-foreground">
          {setting.key}
        </div>
      </div>

      <div className={CONTROL_WELL}>
        {setting.type === "bool" ? (
          <Switch
            checked={Boolean(setting.value)}
            onCheckedChange={(checked) => onChange(checked ? "true" : "false")}
          />
        ) : isWifiSsid ? (
          <WifiSsidInput value={String(setting.value)} onChange={onChange} />
        ) : (
          <Input
            className="h-7 w-full text-[13px]"
            type={isPassword ? "password" : NUMERIC_SETTING_TYPES.includes(setting.type) ? "number" : "text"}
            defaultValue={String(setting.value)}
            onBlur={(e) => {
              if (e.target.value !== String(setting.value)) {
                onChange(e.target.value)
              }
            }}
            onKeyDown={(e) => {
              if (e.key === "Enter") {
                ;(e.target as HTMLInputElement).blur()
              }
            }}
          />
        )}
      </div>
    </li>
  )
}

// ── WiFi SSID input with scan ────────────────────────────────

function rssiToStrength(rssi: number): number {
  if (rssi >= -50) return 4
  if (rssi >= -60) return 3
  if (rssi >= -70) return 2
  return 1
}

function WifiSsidInput({
  value,
  onChange,
}: {
  value: string
  onChange: (value: string) => void
}) {
  const inputRef = useRef<HTMLInputElement>(null)
  const [showScan, setShowScan] = useState(false)
  const [scanning, setScanning] = useState(false)
  const [networks, setNetworks] = useState<WifiNetwork[]>([])

  async function handleScan() {
    setShowScan(true)
    setScanning(true)
    setNetworks([])
    try {
      const result = await backend.wifiScan()
      if (result.ok) {
        // Deduplicate by SSID, keeping strongest signal
        const best = new Map<string, WifiNetwork>()
        for (const n of result.networks) {
          if (!n.ssid) continue
          const existing = best.get(n.ssid)
          if (!existing || n.rssi > existing.rssi) {
            best.set(n.ssid, n)
          }
        }
        setNetworks([...best.values()].sort((a, b) => b.rssi - a.rssi))
      } else {
        // A failed scan must not masquerade as "No networks found".
        setShowScan(false)
        toast.error("WiFi scan failed", { description: "The device reported a scan error." })
      }
    } catch (e) {
      setShowScan(false)
      toast.error("WiFi scan failed", { description: errorMessage(e) })
    }
    setScanning(false)
  }

  function selectNetwork(ssid: string) {
    setShowScan(false)
    onChange(ssid)
    if (inputRef.current) {
      inputRef.current.value = ssid
    }
  }

  return (
    <div className="relative w-full">
      <div className="flex gap-1">
        <Button
          variant="outline"
          size="icon-sm"
          onClick={handleScan}
          disabled={scanning}
          title="Scan WiFi networks"
          className="shrink-0"
        >
          <SearchIcon className="size-3.5" />
        </Button>
        <Input
          ref={inputRef}
          className="h-7 w-full text-[13px]"
          defaultValue={value}
          onBlur={(e) => {
            if (e.target.value !== value) {
              onChange(e.target.value)
            }
          }}
          onKeyDown={(e) => {
            if (e.key === "Enter") {
              ;(e.target as HTMLInputElement).blur()
            }
          }}
        />
      </div>

      {showScan && (
        <>
          <div className="fixed inset-0 z-40" onClick={() => setShowScan(false)} />
          <div className="absolute right-0 top-full z-50 mt-1 w-72 rounded-lg border bg-card p-2 shadow-lg">
            <div className="mb-2 flex items-center justify-between px-2">
              <span className="text-xs font-medium text-muted-foreground">WiFi Networks</span>
              <Button variant="ghost" size="xs" onClick={handleScan} disabled={scanning}>
                {scanning ? "Scanning..." : "Rescan"}
              </Button>
            </div>

            {scanning && networks.length === 0 ? (
              <p className="px-2 py-4 text-center text-xs text-muted-foreground">Scanning...</p>
            ) : networks.length === 0 ? (
              <p className="px-2 py-4 text-center text-xs text-muted-foreground">No networks found</p>
            ) : (
              <div className="max-h-60 overflow-y-auto">
                {networks.map((n) => (
                  <button
                    key={`${n.ssid}-${n.channel}`}
                    className="flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left text-sm hover:bg-muted"
                    onClick={() => selectNetwork(n.ssid)}
                  >
                    <SignalBars strength={rssiToStrength(n.rssi)} />
                    <span className="min-w-0 flex-1 truncate">{n.ssid}</span>
                    <span className="text-xs text-muted-foreground">ch{n.channel}</span>
                    {n.secure && <LockIcon className="size-3 text-muted-foreground" />}
                    <span className="w-10 text-right text-xs text-muted-foreground">{n.rssi}dB</span>
                  </button>
                ))}
              </div>
            )}
          </div>
        </>
      )}
    </div>
  )
}

function SignalBars({ strength }: { strength: number }) {
  return (
    <div className="flex items-end gap-px" title={`Signal: ${strength}/4`}>
      {[1, 2, 3, 4].map((i) => (
        <div
          key={i}
          className={`w-1 rounded-sm ${i <= strength ? "bg-foreground" : "bg-muted"}`}
          style={{ height: `${4 + i * 3}px` }}
        />
      ))}
    </div>
  )
}
