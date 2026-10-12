import { useCallback, useEffect, useRef, useState } from "react"
import { backend, type ModuleFrame, type ModuleLinkState, type OpenThermSide } from "@/lib/backend"
import { decodeFrame, type DecodedFrame } from "@/lib/opentherm"
import { errorMessage } from "@/hooks/use-led"

// A thermostat talks about once a second and the boiler answers each time, so twice a
// second keeps the table live without a backlog. The testbench holds the last 256
// frames, which is minutes of traffic: a slow poll misses nothing, it only lags.
const POLL_MS = 500
const LINK_POLL_MS = 3000

/** Rows kept in the browser, newest first. */
const KEEP = 1000

/** What the testbench sends back per request at most; a full page means ask again. */
const PAGE = 64

export interface FrameRow extends ModuleFrame {
  decoded: DecodedFrame
  /** Since the previous frame on the same line, in ms; absent for the first one. */
  deltaMs?: number
  /** When the frame was on the line, in browser time (Date.now()). Estimated: the
   *  newest frame of a poll is taken to be from that moment, the others are placed
   *  before it by the testbench's own clock. */
  receivedAt: number
}

/// Follow the frames crossing the PCB1246, by polling `module frames` with the last
/// seq seen. Keeps collecting while the view is paused, so pausing never loses frames.
export function useOpenThermFrames() {
  const [rows, setRows] = useState<FrameRow[]>([])
  const [missed, setMissed] = useState(0)
  const [link, setLink] = useState<ModuleLinkState | null>(null)
  const [error, setError] = useState<string | null>(null)

  const cursor = useRef(0)
  const previous = useRef<Record<string, ModuleFrame>>({})
  const inFlight = useRef(false)
  const alive = useRef(true)

  useEffect(() => {
    alive.current = true
    return () => {
      alive.current = false
    }
  }, [])

  const poll = useCallback(async () => {
    if (inFlight.current) return
    inFlight.current = true
    try {
      for (;;) {
        const r = await backend.getModuleFrames(cursor.current)
        if (!alive.current) return

        // The testbench restarted: its numbering did too, and the frames this page
        // holds are from before that. Keep them on screen, follow the new ones.
        if (r.latest < cursor.current) {
          cursor.current = 0
          previous.current = {}
          continue
        }

        const polledAt = Date.now()
        const newestMs = r.frames.length > 0 ? r.frames[r.frames.length - 1].ms : 0
        const fresh: FrameRow[] = r.frames.map((f) => {
          const prev = previous.current[f.side]
          previous.current[f.side] = f
          // The module's clock times line traffic exactly; the testbench's is all a
          // frame the testbench sent has, and it includes the UART both ways.
          let deltaMs: number | undefined
          if (prev)
            deltaMs =
              f.moduleMs !== undefined && prev.moduleMs !== undefined
                ? f.moduleMs - prev.moduleMs
                : f.ms - prev.ms
          const receivedAt = polledAt - (newestMs - f.ms)
          return { ...f, decoded: decodeFrame(f.frame), deltaMs, receivedAt }
        })

        if (r.frames.length > 0) cursor.current = r.frames[r.frames.length - 1].seq
        if (r.missed > 0) setMissed((m) => m + r.missed)
        if (fresh.length > 0) setRows((old) => [...fresh.reverse(), ...old].slice(0, KEEP))
        setError(null)

        if (r.frames.length < PAGE) break
      }
    } catch (e) {
      if (alive.current) setError(errorMessage(e))
    } finally {
      inFlight.current = false
    }
  }, [])

  const pollLink = useCallback(() => {
    backend
      .getModuleLink()
      .then((s) => {
        if (alive.current) setLink(s)
      })
      .catch(() => {
        /* the frame poll reports the connection; this one only decorates */
      })
  }, [])

  useEffect(() => {
    poll()
    pollLink()
    const a = setInterval(poll, POLL_MS)
    const b = setInterval(pollLink, LINK_POLL_MS)
    return () => {
      clearInterval(a)
      clearInterval(b)
    }
  }, [poll, pollLink])

  const clear = useCallback(() => {
    setRows([])
    setMissed(0)
  }, [])

  return { rows, missed, link, error, clear }
}

/** A device that has said nothing for this long is shown as gone. */
export const CONNECTED_WITHIN_MS = 5000

/** What the frames from one line say about the device on it. */
export interface DeviceSummary {
  lastSeenAt?: number
  otVersion?: string
  memberId?: number
}

// The thermostat announces itself in what it writes (124 its OpenTherm version,
// 2 its member id in the low byte); the boiler in what it answers (125, 3).
const IDENTITY = {
  thermostat: { msgType: 1, versionId: 124, configId: 2 },
  boiler: { msgType: 4, versionId: 125, configId: 3 },
} as const

/** Scan rows (newest first) for the latest word from the device on `side`. */
export function summariseLine(rows: FrameRow[], side: OpenThermSide): DeviceSummary {
  const identity = IDENTITY[side]
  const summary: DeviceSummary = {}
  for (const r of rows) {
    if (r.side !== side || r.dir !== "rx") continue
    summary.lastSeenAt ??= r.receivedAt
    const d = r.decoded
    if (d.msgType !== identity.msgType) continue
    if (d.dataId === identity.versionId) summary.otVersion ??= (d.value / 256).toFixed(1)
    if (d.dataId === identity.configId) summary.memberId ??= d.value & 0xff
  }
  return summary
}

export function formatMemberId(memberId: number): string {
  return "0x" + memberId.toString(16).toUpperCase().padStart(2, "0")
}

/** The current time, refreshed every `intervalMs`, for "last seen" to count up. */
export function useNow(intervalMs: number): number {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), intervalMs)
    return () => clearInterval(timer)
  }, [intervalMs])
  return now
}
