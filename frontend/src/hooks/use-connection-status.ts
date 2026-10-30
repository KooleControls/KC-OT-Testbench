import { useEffect, useState } from "react"
import { backend, type ConnectionStatus } from "@/lib/backend"

export function useConnectionStatus(): ConnectionStatus {
  const [status, setStatus] = useState<ConnectionStatus>(backend.status)

  useEffect(() => {
    // Resync once after subscribing — covers the race where backend.status
    // changed between render and effect-setup (e.g. WS opened in between).
    setStatus(backend.status)
    return backend.onStatusChange(setStatus)
  }, [])

  return status
}
