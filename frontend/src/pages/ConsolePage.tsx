import { useEffect, useRef, useState } from "react"
import { backend } from "@/lib/backend"
import { useConnectionStatus } from "@/hooks/use-connection-status"
import { TerminalIcon, TrashIcon, ArrowDownIcon } from "lucide-react"
import { Button } from "@/components/ui/button"
import { toast } from "sonner"

export default function ConsolePage() {
  const connection = useConnectionStatus()
  const [lines, setLines] = useState<string[]>([])
  const [autoScroll, setAutoScroll] = useState(true)
  const bottomRef = useRef<HTMLDivElement>(null)
  const containerRef = useRef<HTMLDivElement>(null)

  // Fetch history on connect
  useEffect(() => {
    if (connection !== "connected") return
    backend.getLogs().then((r) => setLines(r.lines)).catch((e) =>
      toast.error("Failed to load log history", {
        description: e instanceof Error ? e.message : "Unknown error",
      }),
    )
  }, [connection])

  // Subscribe to live log broadcasts
  useEffect(() => {
    return backend.subscribe((msg) => {
      if (typeof msg.log === "string") {
        setLines((prev) => {
          const next = [...prev, msg.log as string]
          // Keep last 1000 lines in the UI
          return next.length > 1000 ? next.slice(-1000) : next
        })
      }
    })
  }, [])

  // Auto-scroll
  useEffect(() => {
    if (autoScroll && bottomRef.current) {
      bottomRef.current.scrollIntoView({ behavior: "smooth" })
    }
  }, [lines, autoScroll])

  // Detect manual scroll
  function handleScroll() {
    const el = containerRef.current
    if (!el) return
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40
    setAutoScroll(atBottom)
  }

  return (
    <div className="flex h-full flex-col">
      <div className="mb-4 flex items-center justify-between">
        <div className="flex items-center gap-2">
          <TerminalIcon className="size-5 text-muted-foreground" />
          <h1 className="text-2xl font-bold">Console</h1>
          <span className="text-sm text-muted-foreground">({lines.length} lines)</span>
        </div>
        <div className="flex gap-2">
          {!autoScroll && (
            <Button
              variant="outline"
              size="sm"
              onClick={() => {
                setAutoScroll(true)
                bottomRef.current?.scrollIntoView({ behavior: "smooth" })
              }}
            >
              <ArrowDownIcon className="mr-1.5 size-3.5" />
              Scroll to bottom
            </Button>
          )}
          <Button variant="outline" size="sm" onClick={() => setLines([])}>
            <TrashIcon className="mr-1.5 size-3.5" />
            Clear
          </Button>
        </div>
      </div>

      <div
        ref={containerRef}
        onScroll={handleScroll}
        className="min-h-0 flex-1 overflow-auto rounded-xl border bg-neutral-950 p-4 font-mono text-xs leading-5 text-neutral-300"
      >
        {lines.map((line, i) => (
          <LogLine key={i} line={line} />
        ))}
        <div ref={bottomRef} />
      </div>
    </div>
  )
}

// ESP-IDF log colors: E=red, W=yellow, I=green, D=cyan, V=white
function LogLine({ line }: { line: string }) {
  // ESP-IDF format: "X (timestamp) tag: message" where X is E/W/I/D/V
  const level = line.charAt(0)

  let color = "text-neutral-400"
  if (level === "E") color = "text-red-400"
  else if (level === "W") color = "text-yellow-300"
  else if (level === "I") color = "text-green-400"
  else if (level === "D") color = "text-cyan-400"
  else if (level === "V") color = "text-neutral-500"

  return <div className={color}>{line}</div>
}
