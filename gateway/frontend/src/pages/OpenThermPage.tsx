import { useEffect, useMemo, useRef, useState, type ReactNode } from "react"
import { FlameIcon, PauseIcon, PlayIcon, SendIcon, ThermometerIcon, TrashIcon } from "lucide-react"
import { toast } from "sonner"

import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { Input } from "@/components/ui/input"
import { Label } from "@/components/ui/label"
import { Switch } from "@/components/ui/switch"
import {
  CONNECTED_WITHIN_MS,
  formatMemberId,
  summariseLine,
  useNow,
  useOpenThermFrames,
  type FrameRow,
} from "@/hooks/use-opentherm"
import { errorMessage } from "@/hooks/use-led"
import { backend, type ModuleLinkState, type OpenThermSide } from "@/lib/backend"
import { MSG_TYPES, decodeFrame, encodeFrame } from "@/lib/opentherm"

/**
 * The OpenTherm traffic through the PCB1246 module, as it happens.
 *
 * On top, one card per line saying who is on it, as far as the frames tell. Below
 * that, every frame either line receives and every frame the testbench puts on one,
 * newest first, decoded for reading. The decoding is this page's only: the modem
 * and the testbench pass frames through as numbers (lib/opentherm.ts).
 *
 * At the bottom, a frame composer for the bench: build a frame from its parts and
 * put it on a line with `module send`.
 */

type TrafficFilter = "all" | OpenThermSide
type TypeFilter = "all" | number

const SIDES = {
  thermostat: {
    title: "Thermostat",
    role: "Master",
    icon: ThermometerIcon,
    dot: "bg-blue-500",
    text: "text-blue-600 dark:text-blue-400",
    iconColor: "text-blue-500",
    card: "border-blue-200 bg-blue-50/60 dark:border-blue-900 dark:bg-blue-950/30",
  },
  boiler: {
    title: "Boiler",
    role: "Slave",
    icon: FlameIcon,
    dot: "bg-orange-500",
    text: "",
    iconColor: "text-orange-500",
    card: "border-orange-200 bg-orange-50/60 dark:border-orange-900 dark:bg-orange-950/30",
  },
} as const

export default function OpenThermPage() {
  const { rows, missed, link, error, clear } = useOpenThermFrames()
  const [paused, setPaused] = useState<FrameRow[] | null>(null)
  const [traffic, setTraffic] = useState<TrafficFilter>("all")
  const [msgType, setMsgType] = useState<TypeFilter>("all")
  const [autoScroll, setAutoScroll] = useState(true)
  const now = useNow(500)

  // Paused means the VIEW stops; the hook keeps collecting underneath.
  const shown = paused ?? rows
  const visible = useMemo(
    () =>
      shown.filter(
        (r) =>
          (traffic === "all" || r.side === traffic) &&
          (msgType === "all" || r.decoded.msgType === msgType),
      ),
    [shown, traffic, msgType],
  )

  // Newest is on top, so following the traffic means staying at the top.
  const scroller = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (autoScroll) scroller.current?.scrollTo({ top: 0 })
  }, [visible, autoScroll])

  return (
    <div className="flex h-full flex-col gap-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <div className="flex items-center gap-3">
          <h1 className="text-3xl font-bold">OpenTherm</h1>
          <LinkBadge link={link} />
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

      <div className="grid shrink-0 gap-4 md:grid-cols-2">
        <DeviceCard side="thermostat" rows={rows} now={now} />
        <DeviceCard side="boiler" rows={rows} now={now} />
      </div>

      <Card className="min-h-0 flex-1 gap-0 overflow-hidden py-0">
        <div className="flex flex-wrap items-end justify-between gap-4 border-b p-4">
          <div className="flex flex-wrap items-end gap-6">
            <div className="flex flex-col gap-1.5">
              <span className="text-sm">Show traffic</span>
              <div className="flex overflow-hidden rounded-lg border">
                {(["all", "thermostat", "boiler"] as const).map((f) => (
                  <button
                    key={f}
                    type="button"
                    onClick={() => setTraffic(f)}
                    className={`border-r px-4 py-1.5 text-sm last:border-r-0 ${
                      traffic === f ? "bg-blue-600 text-white" : "hover:bg-muted"
                    }`}
                  >
                    {f === "all" ? "All traffic" : SIDES[f].title}
                  </button>
                ))}
              </div>
            </div>
            <div className="flex flex-col gap-1.5">
              <Label htmlFor="ot-type-filter" className="font-normal">
                Message type
              </Label>
              <select
                id="ot-type-filter"
                className={`${selectClass} w-52`}
                value={msgType}
                onChange={(e) =>
                  setMsgType(e.target.value === "all" ? "all" : Number(e.target.value))
                }
              >
                <option value="all">All types</option>
                {MSG_TYPES.map((t, i) => (
                  <option key={t} value={i}>
                    {t}
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
                <Th>Device</Th>
                <Th>Frame (hex)</Th>
                <Th>Type</Th>
                <Th>Data id</Th>
                <Th>Name</Th>
                <Th>Value</Th>
                <Th className="text-right">Δ ms</Th>
              </tr>
            </thead>
            <tbody>
              {visible.map((r) => (
                <FrameRowView key={r.seq + ":" + r.ms} row={r} />
              ))}
              {visible.length === 0 && (
                <tr>
                  <td colSpan={8} className="text-muted-foreground p-6 text-center">
                    {link?.phase === "ready"
                      ? "No frames yet. A thermostat on the module sends about once a second."
                      : "No frames - the link to the module is not up."}
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </Card>

      <Composer linkReady={link?.phase === "ready"} />
    </div>
  )
}

function DeviceCard({ side, rows, now }: { side: OpenThermSide; rows: FrameRow[]; now: number }) {
  const look = SIDES[side]
  const summary = useMemo(() => summariseLine(rows, side), [rows, side])
  const silentMs = summary.lastSeenAt === undefined ? undefined : now - summary.lastSeenAt
  const connected = silentMs !== undefined && silentMs < CONNECTED_WITHIN_MS

  const details = [
    summary.otVersion ? `OT ${summary.otVersion}` : "OT version unknown",
    look.role,
    summary.memberId === undefined ? "member id unknown" : `member id ${formatMemberId(summary.memberId)}`,
  ]

  return (
    <div className={`flex items-center gap-4 rounded-xl border p-4 ${look.card}`}>
      <div className="bg-background flex size-16 shrink-0 items-center justify-center rounded-lg border shadow-sm">
        <look.icon className={`size-8 ${look.iconColor}`} />
      </div>
      <div className="min-w-0 flex-1">
        <div className="flex flex-wrap items-center gap-3">
          <span className="text-lg font-semibold">{look.title}</span>
          <span className="flex items-center gap-1.5 text-sm">
            <span className={`size-2 rounded-full ${connected ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
            <span className={connected ? "text-emerald-700 dark:text-emerald-400" : "text-muted-foreground"}>
              {connected ? "Connected" : "Not connected"}
            </span>
          </span>
        </div>
        <div className="text-muted-foreground mt-1 truncate text-sm">{details.join("  |  ")}</div>
      </div>
      <div className="shrink-0 border-l pl-4 text-right">
        <div className="text-muted-foreground text-sm">Last seen</div>
        <div className="mt-1">
          {silentMs === undefined ? "never" : `${(Math.max(0, silentMs) / 1000).toFixed(1)} s ago`}
        </div>
      </div>
    </div>
  )
}

function FrameRowView({ row }: { row: FrameRow }) {
  const d = row.decoded
  const look = SIDES[row.side]
  return (
    <tr className={`border-b last:border-0 ${row.dir === "tx" ? "bg-muted/40" : ""}`}>
      <Td className="text-muted-foreground font-mono">{(row.ms / 1000).toFixed(3)}</Td>
      <Td className="whitespace-nowrap">
        {/* Who said it: off the line it is the device on that line; on it, the testbench. */}
        {row.dir === "rx" ? (
          <span className="flex items-center gap-2">
            <span className={`size-2.5 rounded-full ${look.dot}`} />
            <span className={look.text}>{look.title}</span>
          </span>
        ) : (
          <span className="flex items-center gap-2">
            <span className="bg-muted-foreground size-2.5 rounded-full" />
            Testbench → {look.title}
          </span>
        )}
      </Td>
      <Td className="font-mono">
        {d.hex}
        {!d.parityOk && (
          <Badge variant="destructive" className="ml-2">
            parity
          </Badge>
        )}
      </Td>
      <Td>
        <span className={`rounded-md px-2 py-0.5 whitespace-nowrap ${typePill(d.msgType)}`}>
          {d.msgTypeName}
        </span>
      </Td>
      <Td>{d.dataId}</Td>
      <Td className="whitespace-nowrap">{d.dataIdName}</Td>
      <Td className="font-mono">{d.valueText}</Td>
      <Td className="text-muted-foreground text-right">{row.deltaMs ?? ""}</Td>
    </tr>
  )
}

function typePill(msgType: number): string {
  switch (msgType) {
    case 0: // Read-Data
    case 1: // Write-Data
      return "bg-blue-100 text-blue-700 dark:bg-blue-950 dark:text-blue-300"
    case 4: // Read-Ack
    case 5: // Write-Ack
      return "bg-emerald-100 text-emerald-700 dark:bg-emerald-950 dark:text-emerald-300"
    case 3: // Reserved
      return "bg-muted text-muted-foreground"
    default: // Invalid-Data, Data-Invalid, Unknown-DataId
      return "bg-amber-100 text-amber-800 dark:bg-amber-950 dark:text-amber-300"
  }
}

function Composer({ linkReady }: { linkReady: boolean }) {
  const [side, setSide] = useState<OpenThermSide>("thermostat")
  const [msgType, setMsgType] = useState(0)
  const [dataId, setDataId] = useState("0")
  const [value, setValue] = useState("")
  const [busy, setBusy] = useState(false)

  const id = parseNumber(dataId)
  const val = value.trim() === "" ? 0 : parseHex(value)
  const idValid = id !== null && id <= 255
  const valueValid = val !== null && val <= 0xffff
  const frame = idValid && valueValid ? encodeFrame(msgType, id, val) : null

  const send = () => {
    if (frame === null) return
    setBusy(true)
    backend
      .sendModuleFrame(side, frame)
      .then((r) => {
        if (r.ok) toast.success(`Sent ${decodeFrame(frame).hex} on the ${side} line`)
        else toast.error("Not sent", { description: r.error })
      })
      .catch((e) => toast.error("Not sent", { description: errorMessage(e) }))
      .finally(() => setBusy(false))
  }

  return (
    <Card className="shrink-0 gap-4 p-4">
      <h2 className="text-xl font-semibold">Send a frame (manual injection)</h2>
      <div className="flex flex-wrap items-start gap-6">
        <fieldset className="bg-muted/50 flex flex-col gap-2 rounded-lg px-4 py-3">
          <legend className="sr-only">Send on</legend>
          <span className="text-sm">Send on</span>
          {(["thermostat", "boiler"] as const).map((s) => (
            <label key={s} className="flex cursor-pointer items-center gap-2">
              <input
                type="radio"
                name="ot-send-side"
                value={s}
                checked={side === s}
                onChange={() => setSide(s)}
                className="size-4 accent-blue-600"
              />
              {SIDES[s].title}
            </label>
          ))}
        </fieldset>
        <Field label="Frame type" htmlFor="ot-type">
          <select
            id="ot-type"
            className={`${selectClass} w-52`}
            value={msgType}
            onChange={(e) => setMsgType(Number(e.target.value))}
          >
            {MSG_TYPES.map((t, i) => (
              <option key={t} value={i}>
                {t}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Data id (8 bit)" htmlFor="ot-id" hint="e.g. 0 = Status, 3 = Slave config">
          <Input
            id="ot-id"
            className="w-60"
            value={dataId}
            onChange={(e) => setDataId(e.target.value)}
            aria-invalid={!idValid}
          />
        </Field>
        <Field
          label="Value (16 bit, hex)"
          htmlFor="ot-value"
          hint={frame !== null ? `Frame ${decodeFrame(frame).hex}, parity added` : "Hex, 0000 - FFFF"}
        >
          <Input
            id="ot-value"
            className="w-72 font-mono"
            placeholder="0x0000"
            value={value}
            onChange={(e) => setValue(e.target.value)}
            aria-invalid={!valueValid}
          />
        </Field>
        <Button className="mt-6 ml-auto" size="lg" onClick={send} disabled={frame === null || busy || !linkReady}>
          <SendIcon />
          Send frame
        </Button>
      </div>
    </Card>
  )
}

function LinkBadge({ link }: { link: ModuleLinkState | null }) {
  if (!link) return <Badge variant="outline">link …</Badge>
  const text: Record<ModuleLinkState["phase"], string> = {
    ready: "module linked",
    handshake: "waiting for module",
    paused: "module in bootloader",
    failed: "link failed",
  }
  if (link.phase !== "ready")
    return <Badge variant="destructive">{text[link.phase] ?? link.phase}</Badge>
  return (
    <span className="rounded-md bg-blue-100 px-2.5 py-1 text-sm text-blue-700 dark:bg-blue-950 dark:text-blue-300">
      {text.ready}
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

/** Hex, with or without 0x, at most 16 bits; null when it is not. */
function parseHex(text: string): number | null {
  const t = text.trim().replace(/^0x/i, "")
  return /^[0-9a-f]{1,4}$/i.test(t) ? parseInt(t, 16) : null
}

/** Decimal or 0x-hex, whole numbers only; null when it is neither. */
function parseNumber(text: string): number | null {
  const t = text.trim()
  if (/^0x[0-9a-f]+$/i.test(t)) return parseInt(t.slice(2), 16)
  if (/^\d+$/.test(t)) return parseInt(t, 10)
  return null
}
