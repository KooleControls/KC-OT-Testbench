import { useCallback, useEffect, useMemo, useState } from "react"

import { backend, type CommandDesc, type CommandRegistry } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"

interface Discovery {
  commands: CommandDesc[]
  loading: boolean
  error: string | null
  reload: () => void
}

/** Everything the device can do, asked of the device.
 *
 *  `help` answers with one flat list — a command's full name, what it does, and
 *  each argument it declares — so there is nothing to assemble here beyond
 *  putting it in an order a reader can predict.
 *
 *  Refetched on every reconnect rather than cached across one: a device that came
 *  back may be running a different build, and a console listing commands the
 *  firmware no longer has is worse than a moment of loading. */
export function useCommands(): Discovery {
  const connection = useConnectionStatus()
  const [registry, setRegistry] = useState<CommandRegistry | null>(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [attempt, setAttempt] = useState(0)

  const reload = useCallback(() => setAttempt((n) => n + 1), [])

  useEffect(() => {
    if (connection !== "connected") return

    let cancelled = false
    setLoading(true)
    setError(null)

    backend
      .describeCommands()
      .then((r) => {
        if (cancelled) return
        setRegistry(r)
        setError(null)
      })
      .catch((e) => {
        if (cancelled) return
        setError(e instanceof Error ? e.message : "Unknown error")
      })
      .finally(() => {
        if (!cancelled) setLoading(false)
      })

    return () => {
      cancelled = true
    }
  }, [connection, attempt])

  // The device answers in chain order, which is the order managers happened to
  // register in. Alphabetical is the only order a reader can predict, and it puts
  // a command's siblings next to it because a name starts with its first word.
  const commands = useMemo(
    () => [...(registry?.commands ?? [])].sort((a, b) => a.name.localeCompare(b.name)),
    [registry],
  )

  return { commands, loading, error, reload }
}
