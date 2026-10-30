import { useEffect, useState } from "react"
import { backend, type DeviceInfo } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"

export function useDeviceInfo() {
  const [info, setInfo] = useState<DeviceInfo | null>(null)
  const connection = useConnectionStatus()

  useEffect(() => {
    if (connection !== "connected") return

    backend.getInfo().then(setInfo).catch(() => {})

    // Refresh heap stats periodically
    const interval = setInterval(() => {
      backend.getInfo().then(setInfo).catch(() => {})
    }, 10000)

    return () => clearInterval(interval)
  }, [connection])

  return info
}
