import type { ReactNode } from "react"
import { CpuIcon } from "lucide-react"

import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from "@/components/ui/dialog"
import { PreReleaseBadge } from "@/components/PreReleaseBadge"
import { useDeviceInfo } from "@/hooks/use-device-info"

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  return `${(bytes / 1024).toFixed(1)} KB`
}

/**
 * What the device is, as a dialog behind the sidebar's footer.
 *
 * This used to be the first card on the home page, and that was the wrong place for
 * it. A home screen should be the *product* — what this device does — and a chip
 * name, a heap figure and a compile timestamp are reference material: read once when
 * something is wrong, never while using the thing. Leading with them pushed whatever
 * the firmware actually contributes below the fold on a device with one feature, and
 * made every product built from this template open on the same generic readout.
 *
 * A dialog rather than a page of its own, because it needs no nav entry: the footer
 * already shows the version and the link state, so it is where somebody looks when
 * they want to know more about those. Nothing else links here.
 *
 * `children` is the trigger, so the caller decides what the clickable thing looks
 * like and this file does not have to know about sidebar styling.
 */
export function DeviceInfoDialog({ children }: { children: ReactNode }) {
  const info = useDeviceInfo()

  return (
    <Dialog>
      <DialogTrigger asChild>{children}</DialogTrigger>
      <DialogContent className="sm:max-w-md">
        <DialogHeader>
          <DialogTitle className="flex items-center gap-2">
            <CpuIcon className="size-5 text-muted-foreground" />
            Device info
          </DialogTitle>
          <DialogDescription>
            {info ? info.name : "Reading the device…"}
          </DialogDescription>
        </DialogHeader>

        {!info ? (
          <p className="text-sm text-muted-foreground">Connecting…</p>
        ) : (
          <div className="grid gap-x-8 gap-y-3 text-sm sm:grid-cols-2">
            <Row label="Project" value={info.project} />
            <div className="flex justify-between">
              <span className="text-muted-foreground">Firmware</span>
              <span className="flex items-center gap-2 font-mono">
                {info.firmware}
                <PreReleaseBadge version={info.firmware} />
              </span>
            </div>
            <Row label="ESP-IDF" value={info.idf} />
            <Row label="Compiled" value={`${info.date} ${info.time}`} />
            <Row label="Chip" value={info.chip} />
            <Row label="CPU" value={info.cpu} />
            <Row label="IP address" value={info.ip || "No address"} />
            <Row label="Free heap" value={formatBytes(info.heapFree)} />
            <Row label="Min free heap" value={formatBytes(info.heapMin)} />
            <Row label="Device time" value={info.deviceTime} />
          </div>
        )}
      </DialogContent>
    </Dialog>
  )
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex justify-between gap-4">
      <span className="text-muted-foreground">{label}</span>
      <span className="truncate font-mono">{value}</span>
    </div>
  )
}
