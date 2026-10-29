import { useEffect, useMemo, useRef, useState, type ReactNode } from "react"
import { AirVentIcon, PauseIcon, PlayIcon, SendIcon, TrashIcon } from "lucide-react"
import { toast } from "sonner"

import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import { Switch } from "@/components/ui/switch"
import { errorMessage } from "@/hooks/use-led"
import { useNow } from "@/hooks/use-opentherm"
import {
  ANSWERED_WITHIN_MS,
  summariseUnits,
  useBusFrames,
  useCoolPlugs,
  type BusRow,
  type UnitSummary,
} from "@/hooks/use-rs485"
import { backend, type BusStatus, type CoolPlugUnit } from "@/lib/backend"
import { COOLPLUG_REGISTERS, MODES, bytesToHex, hexToBytes, withCrc } from "@/lib/modbus"

/**
 * The Modbus RTU traffic on the testbench's RS485 port, as it happens.
 *
 * On top, one card per Modbus address on the bus: every address a master asked, and
 * every simulated CoolPlug that is switched on, whether it was asked yet or not. Below
 * that, every frame on the bus, newest first, decoded for reading with the CoolPlug's
 * register names (lib/modbus.ts). The testbench only listens and answers for its
 * simulated CoolPlugs; the master is whoever is on the other end, usually a gateway.
 *
 * At the bottom, a request composer: put one request on the bus with `rs485 send`, to
 * a real CoolPlug or to one of the testbench's own.
 */

type KindFilter = "all" | "requests" | "replies" | "exceptions" | "noise"
type UnitFilter = "all" | number

const KIND_FILTERS: { value: KindFilter; label: string }[] = [
  { value: "all", label: "All frames" },
  { value: "requests", label: "Requests" },
  { value: "replies", label: "Replies" },
  { value: "exceptions", label: "Exceptions" },
  { value: "noise", label: "Noise" },
]

function matchesKind(r: BusRow, filter: KindFilter): boolean {
  switch (filter) {
    case "all":
      return true
    case "requests":
      return r.kind === "request"
    case "replies":
      return r.kind === "reply"
    case "exceptions":
      return r.decoded?.exception !== undefined
    case "noise":
      return r.kind === "noise"
  }
}

/** Who sent a frame, as the table shows it. On a CoolPlug bus the master is the gateway. */
function sender(r: BusRow): { title: string; dot: string } {
  if (r.kind === "noise") return { title: "Noise", dot: "bg-red-400" }
  if (r.from === "testbench") return { title: "Testbench", dot: "bg-muted-foreground" }
  if (r.from === "sim") return { title: "CoolPlug simulator", dot: "bg-orange-300" }
  return r.kind === "request"
    ? { title: "Gateway", dot: "bg-blue-500" }
    : { title: "CoolPlug", dot: "bg-orange-500" }
}

export default function Rs485Page() {
  const { rows, missed, status, error, clear } = useBusFrames()
  const { units: plugs } = useCoolPlugs(3000)
  const [paused, setPaused] = useState<BusRow[] | null>(null)
  const [unitFilter, setUnitFilter] = useState<UnitFilter>("all")
  const [kindFilter, setKindFilter] = useState<KindFilter>("all")
  const [autoScroll, setAutoScroll] = useState(true)
  const now = useNow(500)

  // Paused means the VIEW stops; the hook keeps collecting underneath.
  const shown = paused ?? rows
  const visible = useMemo(
    () =>
      shown.filter(
        (r) =>
          matchesKind(r, kindFilter) &&
          (unitFilter === "all" || (r.decoded !== undefined && r.decoded.unit === unitFilter)),
      ),
    [shown, kindFilter, unitFilter],
  )

  const summaries = useMemo(() => summariseUnits(rows), [rows])
  const addresses = useMemo(() => {
    const all = new Set(summaries.keys())
    for (const p of plugs ?? []) if (p.enabled && p.address > 0) all.add(p.address)
    return [...all].sort((a, b) => a - b)
  }, [summaries, plugs])

  // Newest is on top, so following the traffic means staying at the top.
  const scroller = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (autoScroll) scroller.current?.scrollTo({ top: 0 })
  }, [visible, autoScroll])

  return (
    <div className="flex h-full flex-col gap-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <div className="flex items-center gap-3">
          <h1 className="text-3xl font-bold">RS485</h1>
          <BusBadge status={status} />
          <span className="text-muted-foreground">
            {shown.length} frames{missed > 0 && `, ${missed} missed`}
          </span>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          <Button variant="outline" onClick={() => setPaused((p) => (p ? null : rows))}>
            {paused ? <PlayIcon /> : <PauseIcon />}
            {paused ? "Resume" : "Pause"}
          </Button>
          <Button variant="outline" onClick={() => { clear(); setPaused(null) }}>
            <TrashIcon />
            Clear
          </Button>
          <LiveIndicator paused={paused !== null} failed={error !== null} />
        </div>
      </div>

      {error && <p className="text-destructive text-sm">Polling failed: {error}</p>}

      {addresses.length > 0 ? (
        <div className="grid shrink-0 gap-3 sm:grid-cols-2 xl:grid-cols-4">
          {addresses.map((a) => (
            <UnitCard
              key={a}
              address={a}
              summary={summaries.get(a)}
              plug={plugs?.find((p) => p.enabled && p.address === a)}
              now={now}
              selected={unitFilter === a}
              onSelect={() => setUnitFilter((f) => (f === a ? "all" : a))}
            />
          ))}
        </div>
      ) : (
        <p className="text-muted-foreground text-sm">
          Nobody on the bus yet. Switch a CoolPlug simulator on under{" "}
          <a href="#/simulators/coolplug" className="hover:text-foreground underline underline-offset-4">
            Simulators
          </a>
          , or connect a gateway.
        </p>
      )}

      <Card className="min-h-0 flex-1 gap-0 overflow-hidden py-0">
        <div className="flex flex-wrap items-end justify-between gap-4 border-b p-4">
          <div className="flex flex-wrap items-end gap-6">
            <div className="flex flex-col gap-1.5">
              <span className="text-sm">Show</span>
              <div className="flex overflow-hidden rounded-lg border">
                {KIND_FILTERS.map((f) => (
                  <button
                    key={f.value}
                    type="button"
                    onClick={() => setKindFilter(f.value)}
                    className={`border-r px-4 py-1.5 text-sm last:border-r-0 ${
                      kindFilter === f.value ? "bg-blue-600 text-white" : "hover:bg-muted"
                    }`}
                  >
                    {f.label}
                  </button>
                ))}
              </div>
            </div>
            <div className="flex flex-col gap-1.5">
              <Label htmlFor="rs485-unit-filter" className="font-normal">
                Unit
              </Label>
              <select
                id="rs485-unit-filter"
                className={`${selectClass} w-40`}
                value={unitFilter}
                onChange={(e) => setUnitFilter(e.target.value === "all" ? "all" : Number(e.target.value))}
              >
                <option value="all">All units</option>
                {addresses.map((a) => (
                  <option key={a} value={a}>
                    Unit {a}
                  </option>
                ))}
              </select>
            </div>
          </div>
          <div className="flex items-center gap-4">
            <Label className="font-normal">
              <Switch checked={autoScroll} onCheckedChange={setAutoScroll} />
              Auto scroll
            </Label>
            <span className="text-muted-foreground text-sm">{visible.length} frames</span>
          </div>
        </div>

        <div ref={scroller} className="min-h-0 flex-1 overflow-auto">
          <table className="w-full text-left text-sm">
            <thead className="bg-card text-muted-foreground sticky top-0 z-10">
              <tr className="border-b">
                <Th>Time (s)</Th>
                <Th>From</Th>
                <Th>Unit</Th>
                <Th>Function</Th>
                <Th>Register</Th>
                <Th>Value</Th>
                <Th className="text-right">Reply ms</Th>
                <Th>Frame (hex)</Th>
              </tr>
            </thead>
            <tbody>
              {visible.map((r) => (
                <FrameRowView key={r.seq + ":" + r.ms} row={r} />
              ))}
              {visible.length === 0 && (
                <tr>
                  <td colSpan={8} className="text-muted-foreground p-6 text-center">
                    No frames yet. A gateway polls each of its units every few seconds.
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </Card>

      <Composer plugs={plugs} />
    </div>
  )
}

function UnitCard({
  address,
  summary,
  plug,
  now,
  selected,
  onSelect,
}: {
  address: number
  summary?: UnitSummary
  plug?: CoolPlugUnit
  now: number
  selected: boolean
  onSelect: () => void
}) {
  const answeredAgo = summary?.lastAnsweredAt === undefined ? undefined : now - summary.lastAnsweredAt
  const askedAgo = summary?.lastAskedAt === undefined ? undefined : now - summary.lastAskedAt
  const answering = answeredAgo !== undefined && answeredAgo < ANSWERED_WITHIN_MS
  // Asked lately and not answered lately: the gateway would call it not responding.
  const silent = !answering && askedAgo !== undefined && askedAgo < ANSWERED_WITHIN_MS

  const [dot, state] = answering
    ? ["bg-emerald-500", "Answering"]
    : silent
      ? ["bg-amber-500", "Not answering"]
      : ["bg-muted-foreground/40", "Not asked"]

  const who = plug ? `Simulator ${plug.unit}` : summary?.replies ? "CoolPlug on the bus" : "Unknown"
  const details = plug
    ? [plug.power ? "On" : "Off", MODES[plug.mode] ?? "?", `${plug.setpoint.toFixed(1)} °C`]
    : []

  return (
    <button
      type="button"
      onClick={onSelect}
      className={`flex items-center gap-3 rounded-xl border p-3 text-left transition-colors ${
        selected ? "border-blue-400 ring-2 ring-blue-200 dark:ring-blue-900" : "hover:bg-muted/50"
      } ${plug ? "border-orange-200 bg-orange-50/60 dark:border-orange-900 dark:bg-orange-950/30" : ""}`}
    >
      <div className="bg-background flex size-10 shrink-0 items-center justify-center rounded-lg border shadow-sm">
        <AirVentIcon className={`size-5 ${plug ? "text-orange-500" : "text-muted-foreground"}`} />
      </div>
      <div className="min-w-0 flex-1">
        <div className="flex items-center gap-2">
          <span className="font-semibold">Unit {address}</span>
          <span className="text-muted-foreground truncate text-xs">{who}</span>
        </div>
        <div className="flex items-center gap-1.5 text-sm">
          <span className={`size-2 shrink-0 rounded-full ${dot}`} />
          <span className="truncate">
            {state}
            {answering && summary?.lastReplyMs !== undefined && (
              <span className="text-muted-foreground"> · {summary.lastReplyMs} ms</span>
            )}
          </span>
        </div>
        <div className="text-muted-foreground truncate text-xs">
          {summary ? `${summary.requests} asked, ${summary.replies} answered` : "no traffic"}
          {summary && summary.exceptions > 0 && `, ${summary.exceptions} exceptions`}
          {details.length > 0 && ` · ${details.join(" · ")}`}
        </div>
      </div>
    </button>
  )
}

function FrameRowView({ row }: { row: BusRow }) {
  const s = sender(row)
  const d = row.decoded
  return (
    <tr className={`border-b last:border-0 ${row.from === "testbench" ? "bg-muted/40" : ""}`}>
      <Td className="text-muted-foreground font-mono">{(row.ms / 1000).toFixed(3)}</Td>
      <Td className="whitespace-nowrap">
        <span className="flex items-center gap-2">
          <span className={`size-2.5 rounded-full ${s.dot}`} />
          {s.title}
        </span>
      </Td>
      <Td>{d ? d.unit : ""}</Td>
      <Td>
        {row.kind === "noise" ? (
          <span className="rounded-md bg-red-100 px-2 py-0.5 whitespace-nowrap text-red-700 dark:bg-red-950 dark:text-red-300">
            {row.len} bytes
          </span>
        ) : d ? (
          <span className={`rounded-md px-2 py-0.5 whitespace-nowrap ${functionPill(row)}`}>
            {d.exception ? `Exception: ${d.exception}` : d.functionName}
          </span>
        ) : null}
      </Td>
      <Td className="whitespace-nowrap">{d?.registerText}</Td>
      <Td>{d?.valueText}</Td>
      <Td className="text-muted-foreground text-right">{row.replyMs ?? ""}</Td>
      <Td className="font-mono text-xs">
        {bytesToHex(row.bytes)}
        {row.len > row.bytes.length && <span className="text-muted-foreground"> … ({row.len} bytes)</span>}
        {d && !d.crcOk && !d.truncated && (
          <Badge variant="destructive" className="ml-2">
            CRC
          </Badge>
        )}
      </Td>
    </tr>
  )
}

function functionPill(row: BusRow): string {
  if (row.decoded?.exception) return "bg-amber-100 text-amber-800 dark:bg-amber-950 dark:text-amber-300"
  if (row.kind === "request") return "bg-blue-100 text-blue-700 dark:bg-blue-950 dark:text-blue-300"
  return "bg-emerald-100 text-emerald-700 dark:bg-emerald-950 dark:text-emerald-300"
}

// ── Composer ─────────────────────────────────────────────────────

type ComposeFunction = "read" | "write" | "deviceId" | "raw"

const REGISTER_OPTIONS = Object.entries(COOLPLUG_REGISTERS).map(([address, info]) => ({
  address: Number(address),
  name: info.name,
}))

function Composer({ plugs }: { plugs: CoolPlugUnit[] | null }) {
  const [fn, setFn] = useState<ComposeFunction>("read")
  const [unit, setUnit] = useState("1")
  const [register, setRegister] = useState("145")
  const [amount, setAmount] = useState("1")
  const [raw, setRaw] = useState("")
  const [busy, setBusy] = useState(false)

  const u = parseNumber(unit)
  const reg = parseNumber(register)
  const n = parseNumber(amount)
  const unitValid = u !== null && u <= 247
  const regValid = reg !== null && reg <= 0xffff
  const amountValid = n !== null && (fn === "read" ? n >= 1 && n <= 125 : n <= 0xffff)

  // The frame without its CRC, or null while the fields do not make one.
  const compose = (): number[] | null => {
    if (fn === "raw") {
      const b = hexToBytes(raw)
      return b !== null && b.length >= 2 ? b : null
    }
    if (u === null || !unitValid) return null
    if (fn === "deviceId") return [u, 0x2b, 0x0e, 0x01, 0x00]
    if (reg === null || n === null || !regValid || !amountValid) return null
    return [u, fn === "read" ? 0x03 : 0x06, reg >> 8, reg & 0xff, n >> 8, n & 0xff]
  }
  const bytes = compose()
  const simulated = bytes ? plugs?.find((p) => p.enabled && p.address === bytes[0]) : undefined

  const send = () => {
    if (bytes === null) return
    setBusy(true)
    backend
      .sendBusFrame(bytesToHex(bytes))
      .then((r) => {
        if (r.ok) toast.success(`Sent ${bytesToHex(withCrc(bytes))}`)
        else toast.error("Not sent", { description: r.error })
      })
      .catch((e) => toast.error("Not sent", { description: errorMessage(e) }))
      .finally(() => setBusy(false))
  }

  return (
    <Card className="shrink-0 gap-4 p-4">
      <h2 className="text-xl font-semibold">Send a request (manual injection)</h2>
      <div className="flex flex-wrap items-start gap-6">
        <Field label="Function" htmlFor="rs485-fn">
          <select
            id="rs485-fn"
            className={`${selectClass} w-60`}
            value={fn}
            onChange={(e) => setFn(e.target.value as ComposeFunction)}
          >
            <option value="read">03 Read holding registers</option>
            <option value="write">06 Write single register</option>
            <option value="deviceId">2B Read device identification</option>
            <option value="raw">Raw frame (hex)</option>
          </select>
        </Field>

        {fn === "raw" ? (
          <Field label="Frame, without the CRC" htmlFor="rs485-raw" hint="Unit first, e.g. 01 03 00 91 00 01">
            <Input
              id="rs485-raw"
              className="w-96 font-mono"
              value={raw}
              onChange={(e) => setRaw(e.target.value)}
              aria-invalid={raw !== "" && bytes === null}
            />
          </Field>
        ) : (
          <>
            <Field
              label="Unit (Modbus address)"
              htmlFor="rs485-unit"
              hint={simulated ? `Simulator ${simulated.unit} answers` : "1 - 247"}
            >
              <Input
                id="rs485-unit"
                className="w-40"
                value={unit}
                onChange={(e) => setUnit(e.target.value)}
                aria-invalid={!unitValid}
              />
            </Field>
            {fn !== "deviceId" && (
              <>
                <Field
                  label="Register (wire address)"
                  htmlFor="rs485-reg"
                  hint={regValid ? (COOLPLUG_REGISTERS[reg]?.name ?? "Not a CoolPlug register") : "0 - 65535"}
                >
                  <Input
                    id="rs485-reg"
                    className="w-48"
                    list="rs485-registers"
                    value={register}
                    onChange={(e) => setRegister(e.target.value)}
                    aria-invalid={!regValid}
                  />
                  <datalist id="rs485-registers">
                    {REGISTER_OPTIONS.map((o) => (
                      <option key={o.address} value={o.address}>
                        {o.name}
                      </option>
                    ))}
                  </datalist>
                </Field>
                <Field
                  label={fn === "read" ? "Count" : "Value"}
                  htmlFor="rs485-amount"
                  hint={fn === "read" ? "1 - 125 registers" : "Temperatures in tenths: 215 is 21.5 °C"}
                >
                  <Input
                    id="rs485-amount"
                    className="w-40"
                    value={amount}
                    onChange={(e) => setAmount(e.target.value)}
                    aria-invalid={!amountValid}
                  />
                </Field>
              </>
            )}
          </>
        )}

        <div className="mt-6 ml-auto flex flex-col items-end gap-1.5">
          <Button size="lg" onClick={send} disabled={bytes === null || busy}>
            <SendIcon />
            Send request
          </Button>
          <span className="text-muted-foreground font-mono text-xs">
            {bytes ? bytesToHex(withCrc(bytes)) : " "}
          </span>
        </div>
      </div>
    </Card>
  )
}

function BusBadge({ status }: { status: BusStatus | null }) {
  if (!status) return <Badge variant="outline">bus …</Badge>
  return (
    <span className="rounded-md bg-blue-100 px-2.5 py-1 text-sm text-blue-700 dark:bg-blue-950 dark:text-blue-300">
      Modbus RTU · {status.baud} 8N1
    </span>
  )
}

function LiveIndicator({ paused, failed }: { paused: boolean; failed: boolean }) {
  const [dot, label] = failed
    ? ["bg-red-500", "Offline"]
    : paused
      ? ["bg-amber-500", "Paused"]
      : ["bg-emerald-500", "Live"]
  return (
    <span className="flex items-center gap-2 px-2">
      <span className={`size-3 rounded-full ${dot}`} />
      {label}
    </span>
  )
}

const selectClass =
  "border-input dark:bg-input/30 h-9 rounded-lg border bg-transparent px-3 text-sm"

function Field({
  label,
  htmlFor,
  hint,
  children,
}: {
  label: string
  htmlFor: string
  hint?: string
  children: ReactNode
}) {
  return (
    <div className="flex flex-col gap-1.5">
      <Label htmlFor={htmlFor} className="font-normal">
        {label}
      </Label>
      {children}
      {hint && <span className="text-muted-foreground text-xs">{hint}</span>}
    </div>
  )
}

function Th({ children, className = "" }: { children: ReactNode; className?: string }) {
  return <th className={`px-4 py-2 font-medium ${className}`}>{children}</th>
}

function Td({ children, className = "" }: { children: ReactNode; className?: string }) {
  return <td className={`px-4 py-2 align-middle ${className}`}>{children}</td>
}

/** Decimal or 0x-hex, whole numbers only; null when it is neither. */
function parseNumber(text: string): number | null {
  const t = text.trim()
  if (/^0x[0-9a-f]+$/i.test(t)) return parseInt(t.slice(2), 16)
  if (/^\d+$/.test(t)) return parseInt(t, 10)
  return null
}
