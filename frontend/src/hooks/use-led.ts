import { useCallback, useEffect, useRef, useState } from "react"
import { backend, type LedState } from "@/lib/backend"

// The relay link moves on its own, so the UI re-reads rather than trusting what it
// fetched when the page opened. Slow enough to be free on the LAN, and slow because
// through the relay every poll takes that device's request gate.
const POLL_MS = 3000

export function errorMessage(e: unknown): string {
  return e instanceof Error ? e.message : String(e)
}

/// Poll `led get`, and offer an optimistic `set`.
export function useLed() {
  const [state, setState] = useState<LedState | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  // A reply can land after unmount — the transport has no cancellation — so every
  // setState is guarded rather than assumed safe.
  const alive = useRef(true)
  useEffect(() => {
    alive.current = true
    return () => {
      alive.current = false
    }
  }, [])

  const refresh = useCallback(() => {
    backend
      .getLed()
      .then((s) => {
        if (!alive.current) return
        setState(s)
        setError(null)
      })
      .catch((e) => {
        if (!alive.current) return
        // Keep the last known state on the screen: a dropped poll is not news, and
        // blanking the page on every hiccup would be worse than being slightly stale.
        setError(errorMessage(e))
      })
  }, [])

  useEffect(() => {
    refresh()
    const id = setInterval(refresh, POLL_MS)
    return () => clearInterval(id)
  }, [refresh])

  /// Optimistic: the control follows the finger and the device's own reply is the
  /// correction, so a slow link does not make it feel broken. A failure rolls back
  /// rather than leaving the UI lying about the hardware.
  const setEnabled = useCallback(
    (next: boolean, onError?: (message: string) => void) => {
      const previous = state
      setState((s) => (s ? { ...s, enabled: next, on: next && s.connected } : s))
      setBusy(true)
      backend
        .setLed({ enabled: next })
        .then((s) => {
          if (alive.current) setState(s)
        })
        .catch((e) => {
          if (!alive.current) return
          setState(previous)
          onError?.(errorMessage(e))
        })
        .finally(() => {
          if (alive.current) setBusy(false)
        })
    },
    [state],
  )

  return { state, busy, error, setEnabled, refresh }
}

/// One sentence for whatever the board is doing.
export function describe(state: LedState | null): string {
  if (!state) return "Unknown"
  if (!state.enabled) return "Off — not showing the link"
  return state.connected ? "On — connected to the relay" : "Off — no relay connection"
}
