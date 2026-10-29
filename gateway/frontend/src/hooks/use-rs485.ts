import { useCallback, useEffect, useRef, useState } from "react"
import { backend, type BusFrame, type BusStatus, type CoolPlugUnit } from "@/lib/backend"
import { decodeFrame, hexToBytes, type DecodedFrame, type ReadRequest } from "@/lib/modbus"
import { errorMessage } from "@/hooks/use-led"

// A gateway polls a few registers per unit every few seconds, one request at a time,
// so twice a second keeps up with any bus without a backlog. The testbench holds the
// last 256 frames.
const POLL_MS = 500
const STATUS_POLL_MS = 3000

/** Rows kept in the browser, newest first. */
const KEEP = 1000

/** What the testbench sends back per request at most; a full page means ask again. */
const PAGE = 32

export interface BusRow extends BusFrame {
  bytes: number[]
  /** Absent for noise. */
  decoded?: DecodedFrame
  /** For a reply: how long after the request it answers, by the testbench's clock. */
  replyMs?: number
  /** When the frame was on the bus, in browser time (Date.now()); estimated as in
   *  use-opentherm: the newest frame of a poll is taken to be from that moment. */
  receivedAt: number
}

/** The last request to a unit, for the reply to it to be read with. */
interface Asked {
  ms: number
  read?: ReadRequest
}

/// Follow the frames on the RS485 bus, by polling `rs485 frames` with the last seq seen.
/// Keeps collecting while the view is paused, so pausing never loses frames.
export function useBusFrames() {
  const [rows, setRows] = useState<BusRow[]>([])
  const [missed, setMissed] = useState(0)
  const [status, setStatus] = useState<BusStatus | null>(null)
  const [error, setError] = useState<string | null>(null)

  const cursor = useRef(0)
  const asked = useRef(new Map<number, Asked>())
  const inFlight = useRef(false)
  const alive = useRef(true)

  useEffect(() => {
    alive.current = true
    return () => {
      alive.current = false
    }
  }, [])

  const toRow = useCallback((f: BusFrame, receivedAt: number): BusRow => {
    const bytes = hexToBytes(f.hex) ?? []
    if (f.kind === "noise") return { ...f, bytes, receivedAt }

    if (f.kind === "request") {
      const decoded = decodeFrame(bytes, "request", f.len)
      asked.current.set(decoded.unit, { ms: f.ms, read: decoded.read })
      return { ...f, bytes, decoded, receivedAt }
    }

    // A reply answers the last request to its unit, once.
    const request = asked.current.get(bytes[0] ?? 0)
    asked.current.delete(bytes[0] ?? 0)
    const decoded = decodeFrame(bytes, "reply", f.len, request?.read)
    return { ...f, bytes, decoded, replyMs: request ? f.ms - request.ms : undefined, receivedAt }
  }, [])

  const poll = useCallback(async () => {
    if (inFlight.current) return
    inFlight.current = true
    try {
      for (;;) {
        const r = await backend.getBusFrames(cursor.current)
        if (!alive.current) return

        // The testbench restarted: its numbering did too. Keep what is on screen,
        // follow the new ones.
        if (r.latest < cursor.current) {
          cursor.current = 0
          asked.current.clear()
          continue
        }

        const polledAt = Date.now()
        const newestMs = r.frames.length > 0 ? r.frames[r.frames.length - 1].ms : 0
        const fresh = r.frames.map((f) => toRow(f, polledAt - (newestMs - f.ms)))

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
  }, [toRow])

  const pollStatus = useCallback(() => {
    backend
      .getBusStatus()
      .then((s) => {
        if (alive.current) setStatus(s)
      })
      .catch(() => {
        /* the frame poll reports the connection; this one only decorates */
      })
  }, [])

  useEffect(() => {
    poll()
    pollStatus()
    const a = setInterval(poll, POLL_MS)
    const b = setInterval(pollStatus, STATUS_POLL_MS)
    return () => {
      clearInterval(a)
      clearInterval(b)
    }
  }, [poll, pollStatus])

  const clear = useCallback(() => {
    setRows([])
    setMissed(0)
  }, [])

  return { rows, missed, status, error, clear }
}

/// The simulated CoolPlugs, polled every `intervalMs`; null until the first answer.
export function useCoolPlugs(intervalMs: number) {
  const [units, setUnits] = useState<CoolPlugUnit[] | null>(null)

  const refresh = useCallback(() => {
    backend
      .getCoolPlugs()
      .then((r) => setUnits(r.units))
      .catch(() => {
        /* shown as unknown until it answers */
      })
  }, [])

  useEffect(() => {
    refresh()
    const timer = setInterval(refresh, intervalMs)
    return () => clearInterval(timer)
  }, [refresh, intervalMs])

  return { units, refresh, setUnits }
}

/** A unit that has answered within this long is shown as present. A gateway polls
 *  every 1-10 s (its MIV setting). */
export const ANSWERED_WITHIN_MS = 15000

/** What the frames say about one Modbus address on the bus. */
export interface UnitSummary {
  address: number
  requests: number
  replies: number
  exceptions: number
  lastAskedAt?: number
  lastAnsweredAt?: number
  /** The last reply time, testbench clock. */
  lastReplyMs?: number
  /** Whether the replies came from a simulated CoolPlug. */
  simulatedReplies: boolean
}

/** Every address asked on the bus, from the rows (newest first). Broadcasts are left out. */
export function summariseUnits(rows: readonly BusRow[]): Map<number, UnitSummary> {
  const units = new Map<number, UnitSummary>()
  const get = (address: number) => {
    let u = units.get(address)
    if (!u) {
      u = { address, requests: 0, replies: 0, exceptions: 0, simulatedReplies: false }
      units.set(address, u)
    }
    return u
  }
  for (const r of rows) {
    if (!r.decoded || r.decoded.unit === 0) continue
    const u = get(r.decoded.unit)
    if (r.kind === "request") {
      u.requests++
      u.lastAskedAt ??= r.receivedAt
    } else {
      u.replies++
      if (r.decoded.exception) u.exceptions++
      if (u.lastAnsweredAt === undefined) {
        u.lastAnsweredAt = r.receivedAt
        u.lastReplyMs = r.replyMs
        u.simulatedReplies = r.from === "sim"
      }
    }
  }
  return units
}
